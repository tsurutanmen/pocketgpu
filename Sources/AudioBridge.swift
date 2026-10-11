import AVFoundation
import Network
import SwiftUI
import UIKit
import os

// iPhone を、PC のスピーカーとマイクにする。PC は work/audio/pc/ipaudio.c（usbmuxd 越しに USB の 50063 番へつなぐ）。
// 約束は work/audio/DESIGN.md の「約束」。1 本の TCP で両方向を流す：
//   PC → iPhone：HELLO（40 バイト）→ 以降 [16 バイトの頭][PCM]（SPK）。PCM は s16le・2ch・PC の rate
//   iPhone → PC：ACK（40 バイト）→ 以降 [16 バイトの頭][PCM]（MIC）。PCM は s16le・1ch・HELLO で頼まれた rate
//   どちらからも PING / PONG（往復の測り）、iPhone からは 1 秒ごとに STATS、終わるときは BYE
// 受け取るのは後ろの糸で、音を出すのは AVAudioSourceNode の実時間の糸。間は「ため」（PlayoutRing）で渡す。
// ため方は work/audio/pc/proto.h の IpaPlayout と同じ：目標の長さまでためてから鳴らし、機器の時計とのずれは
// 読む速さを最大 ±0.5% 変えて吸収し（比例＋積分）、足りなければ無音で埋めてためなおし、多すぎれば古い方を捨てる。
// Vulkan の受け手（50060）・画面の映し（50062）とは別の口で、受け手の C の部分には触らない。
// 画面に出す文は仮。言葉は鶴田さんが決める

private enum IPA {
    static let tSpk: UInt8 = 1, tMic: UInt8 = 2, tPing: UInt8 = 3, tPong: UInt8 = 4, tStats: UInt8 = 5, tBye: UInt8 = 6
    static let fSpeaker: UInt16 = 0x0001, fMic: UInt16 = 0x0002, fVoiceProc: UInt16 = 0x0004, fExclusive: UInt16 = 0x0008
    static let aSpeakerOK: UInt16 = 0x0001, aMicOK: UInt16 = 0x0002, aMicNeedsForeground: UInt16 = 0x0004
    static let frSilent: UInt8 = 0x01
    static let maxPayload = 1 << 20
}

// ---- 小さな道具 ----

private final class UnfairLock {
    private let p = UnsafeMutablePointer<os_unfair_lock>.allocate(capacity: 1)
    init() { p.initialize(to: os_unfair_lock()) }
    deinit { p.deallocate() }
    @inline(__always) func lock() { os_unfair_lock_lock(p) }
    @inline(__always) func unlock() { os_unfair_lock_unlock(p) }
}

private func le16(_ d: Data, _ o: Int) -> UInt16 {
    UInt16(d[d.startIndex + o]) | UInt16(d[d.startIndex + o + 1]) << 8
}
private func le32(_ d: Data, _ o: Int) -> UInt32 {
    UInt32(le16(d, o)) | UInt32(le16(d, o + 2)) << 16
}
private func le64(_ d: Data, _ o: Int) -> UInt64 {
    UInt64(le32(d, o)) | UInt64(le32(d, o + 4)) << 32
}

private struct ByteOut {
    var d = Data()
    mutating func u8(_ v: UInt8) { d.append(v) }
    mutating func u16(_ v: UInt16) { d.append(UInt8(v & 0xff)); d.append(UInt8(v >> 8)) }
    mutating func u32(_ v: UInt32) { u16(UInt16(v & 0xffff)); u16(UInt16(v >> 16)) }
    mutating func u64(_ v: UInt64) { u32(UInt32(v & 0xffff_ffff)); u32(UInt32(v >> 32)) }
    mutating func zeros(_ n: Int) { d.append(Data(count: n)) }
}

private func frameHeader(type: UInt8, flags: UInt8 = 0, frames: UInt16 = 0, length: UInt32 = 0, index: UInt64 = 0) -> Data {
    var o = ByteOut()
    o.u8(type); o.u8(flags); o.u16(frames); o.u32(length); o.u64(index)
    return o.d
}

private struct Hello {
    var version: UInt16, flags: UInt16
    var spkRate: UInt32, spkChannels: UInt16, spkFormat: UInt16
    var micRate: UInt32, micChannels: UInt16, micFormat: UInt16
    var chunkFrames: UInt16, targetMs: UInt16

    init?(_ d: Data) {
        guard d.count >= 40, d.prefix(4).elementsEqual("IPAU".utf8) else { return nil }
        version = le16(d, 4); flags = le16(d, 6)
        spkRate = le32(d, 8); spkChannels = le16(d, 12); spkFormat = le16(d, 14)
        micRate = le32(d, 16); micChannels = le16(d, 20); micFormat = le16(d, 22)
        chunkFrames = le16(d, 24); targetMs = le16(d, 26)
    }
}

// ---- 鳴らす側の「ため」 ----
// 書くのは受け取りの糸、読むのは音の実時間の糸。錠はごく短い間だけ持つ（os_unfair_lock は優先度を引き継ぐ）

final class PlayoutRing {
    private let cap: Int
    private let buf: UnsafeMutablePointer<Float> // cap コマ × 2（L R の交互）
    private var wr: UInt64 = 0
    private var rd: Double = 0
    private let rate: Double
    let targetFrames: Double
    private var fs = 0.0, integ = 0.0, ratio = 1.0
    private var running = false
    private var underrunCount: UInt32 = 0, overrunCount: UInt32 = 0, gap: UInt32 = 0
    private let lock = UnfairLock()

    init(rate: Double, targetMs: Double) {
        self.rate = rate
        cap = Int(rate * 2)
        buf = UnsafeMutablePointer<Float>.allocate(capacity: cap * 2)
        buf.initialize(repeating: 0, count: cap * 2)
        targetFrames = rate * targetMs / 1000
    }
    deinit { buf.deallocate() }

    // s16le・2ch の PCM を frames コマ積む
    func write(_ pcm: UnsafeRawBufferPointer, frames: Int) {
        lock.lock(); defer { lock.unlock() }
        let n = min(frames, pcm.count / 4)
        for i in 0..<n {
            let l = Float(pcm.loadUnaligned(fromByteOffset: i * 4, as: Int16.self)) * (1.0 / 32768.0)
            let r = Float(pcm.loadUnaligned(fromByteOffset: i * 4 + 2, as: Int16.self)) * (1.0 / 32768.0)
            let at = Int(wr % UInt64(cap)) * 2
            buf[at] = l
            buf[at + 1] = r
            wr += 1
        }
        trimLocked()
    }

    func writeSilence(_ frames: Int) {
        lock.lock(); defer { lock.unlock() }
        var left = min(frames, cap)
        gap &+= UInt32(truncatingIfNeeded: frames)
        while left > 0 {
            let at = Int(wr % UInt64(cap)) * 2
            buf[at] = 0
            buf[at + 1] = 0
            wr += 1
            left -= 1
        }
        trimLocked()
    }

    // 多すぎる（止まりのあとの追いつき）なら、目標まで古い方を捨てる
    private func trimLocked() {
        let fill = Double(wr) - rd
        if fill > targetFrames * 2 + 0.05 * rate {
            rd = Double(wr) - targetFrames
            overrunCount &+= 1
        }
    }

    // 機器の時計で frames コマを out（L R の交互）に読む。足りない所は無音
    func read(frames: Int, into out: UnsafeMutablePointer<Float>) {
        lock.lock(); defer { lock.unlock() }
        let fill = Double(wr) - rd
        if !running {
            if fill >= targetFrames { running = true; fs = fill } else {
                out.update(repeating: 0, count: frames * 2)
                return
            }
        }
        fs += 0.02 * (fill - fs)
        let err = fs - targetFrames
        let dt = Double(frames) / rate
        let adjP = err / (4.0 * rate)
        integ = max(-0.005, min(0.005, integ + adjP * dt / 20.0))
        let adj = max(-0.005, min(0.005, adjP + integ))
        ratio = 1.0 + adj
        for i in 0..<frames {
            let i0 = Int64(rd.rounded(.down))
            if Double(i0 + 1) >= Double(wr) { // 足りない。残りは無音にして、ためなおしを待つ
                (out + i * 2).update(repeating: 0, count: (frames - i) * 2)
                running = false
                underrunCount &+= 1
                return
            }
            let fr = Float(rd - Double(i0))
            let a = Int(i0 % Int64(cap)) * 2
            let b = Int((i0 + 1) % Int64(cap)) * 2
            out[i * 2] = buf[a] + (buf[b] - buf[a]) * fr
            out[i * 2 + 1] = buf[a + 1] + (buf[b + 1] - buf[a + 1]) * fr
            rd += ratio
        }
    }

    struct Snapshot { var fill: Double; var target: Double; var under: UInt32; var over: UInt32; var gap: UInt32; var ratioPPM: Int32 }
    func snapshot() -> Snapshot {
        lock.lock(); defer { lock.unlock() }
        return Snapshot(fill: Double(wr) - rd, target: targetFrames, under: underrunCount, over: overrunCount, gap: gap, ratioPPM: Int32((ratio - 1.0) * 1e6))
    }
}

// 実時間の糸が使う作業用の置き場。ノードが消えるときに一緒に解放される
private final class Scratch {
    let p = UnsafeMutablePointer<Float>.allocate(capacity: 8192 * 2)
    init() { p.initialize(repeating: 0, count: 8192 * 2) }
    deinit { p.deallocate() }
}

// ---- マイクの側の輪（音の実時間の糸が書き、送りの糸が読む）----

final class MicRing {
    private let cap: Int
    private let buf: UnsafeMutablePointer<Int16>
    private var wr: UInt64 = 0, rd: UInt64 = 0
    private var droppedFrames: UInt64 = 0
    private let lock = UnfairLock()

    init(rate: Int) {
        cap = rate // 1 秒
        buf = UnsafeMutablePointer<Int16>.allocate(capacity: cap)
        buf.initialize(repeating: 0, count: cap)
    }
    deinit { buf.deallocate() }

    // Float（-1〜1）を Int16 にして積む。あふれたら古い方を捨てる
    func write(_ x: UnsafePointer<Float>, frames: Int) {
        lock.lock(); defer { lock.unlock() }
        for i in 0..<frames {
            let v = max(-1.0, min(1.0, x[i])) * 32767.0
            buf[Int(wr % UInt64(cap))] = Int16(v)
            wr += 1
        }
        let over = Int(wr - rd) - cap
        if over > 0 { rd += UInt64(over); droppedFrames += UInt64(over) }
    }

    var available: Int {
        lock.lock(); defer { lock.unlock() }
        return Int(wr - rd)
    }

    // frames コマを out に取り出す。足りなければ false
    func pop(_ out: UnsafeMutablePointer<Int16>, frames: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard Int(wr - rd) >= frames else { return false }
        for i in 0..<frames { out[i] = buf[Int((rd + UInt64(i)) % UInt64(cap))] }
        rd += UInt64(frames)
        return true
    }

    func discard(_ frames: Int) {
        lock.lock(); defer { lock.unlock() }
        let n = min(frames, Int(wr - rd))
        rd += UInt64(n)
    }
}

// ---- 本体 ----

final class AudioBridgeModel: ObservableObject {
    static let shared = AudioBridgeModel()
    static let port: UInt16 = 50063

    @Published private(set) var connected = false
    @Published private(set) var speakerActive = false
    @Published private(set) var micActive = false
    @Published private(set) var micWaitsForeground = false
    @Published private(set) var fillMs = 0.0
    @Published private(set) var ioMs = 0.0

    // main の上だけで触る
    private var on = false
    private var engine: AVAudioEngine?
    private var observers: [NSObjectProtocol] = []

    private let queue = DispatchQueue(label: "audio-bridge", qos: .userInitiated)
    // ここから下は queue の上だけで触る
    private var listening = false
    private var listener: NWListener?
    private var conn: NWConnection?
    private var connID = 0
    private var failNoted = false
    private var hello: Hello?
    private var ring: PlayoutRing?
    private var micRing: MicRing?
    private var spkExpected: UInt64 = 0
    private var micTimer: DispatchSourceTimer?
    private var statsTimer: DispatchSourceTimer?
    private var micRate = 48000
    private var micIndex: UInt64 = 0
    private var micSent: UInt64 = 0, micDropped: UInt64 = 0
    private var pendingSends = 0
    private var wantMicLater = false // 前に出ていなくてマイクを始められなかった

    private init() {}

    private func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }

    // 画面の糸から呼ぶ。on の間だけ 50063 番で待つ
    func setOn(_ v: Bool) {
        guard v != on else { return }
        on = v
        if v, AVAudioApplication.shared.recordPermission == .undetermined {
            // 許可の確認は前にいる間に出す。許可されなければマイクは使えないがスピーカーは使える
            AVAudioApplication.requestRecordPermission { _ in }
        }
        queue.async { [weak self] in
            guard let self else { return }
            self.listening = v
            if v { self.listen() } else { self.close() }
        }
        if !v { DispatchQueue.main.async { [weak self] in self?.stopEngine() } }
    }

    // アプリが前に出たときに呼ぶ。マイクを始められずに待っていたら始め直す
    func foregrounded() {
        queue.async { [weak self] in
            guard let self, self.wantMicLater, let h = self.hello else { return }
            let r = self.ring // スピーカーなしのときは nil
            self.wantMicLater = false
            self.log("音：前に出たので、マイクを始める")
            DispatchQueue.main.async { [weak self] in
                self?.startEngine(h, ring: r) { micRing, _ in
                    self?.queue.async { self?.attachMic(micRing) }
                }
            }
        }
    }

    // ---- 口（queue の上）----

    private func listen() {
        guard listening, listener == nil, let p = NWEndpoint.Port(rawValue: Self.port) else { return }
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: p) // USB（iPhone の中の 127.0.0.1）だけ。Wi-Fi には出さない
        params.allowLocalEndpointReuse = true
        let l: NWListener
        do { l = try NWListener(using: params) } catch {
            if !failNoted { log("音の口を開けない：\(error)。1 秒ごとに開き直す") }
            failNoted = true
            queue.asyncAfter(deadline: .now() + 1) { [weak self] in self?.listen() }
            return
        }
        l.stateUpdateHandler = { [weak self, weak l] st in
            guard let self, let l, l === self.listener else { return }
            switch st {
            case .ready:
                self.failNoted = false
                self.log("音の口：127.0.0.1:\(Self.port) で待っている")
            case .failed(let e):
                if !self.failNoted { self.log("音の口を開けない：\(e)。1 秒ごとに開き直す") }
                self.failNoted = true
                self.listener = nil
                l.cancel()
                self.queue.asyncAfter(deadline: .now() + 1) { [weak self] in self?.listen() }
            default: break
            }
        }
        l.newConnectionHandler = { [weak self, weak l] c in
            guard let self, let l, l === self.listener else { c.cancel(); return }
            self.accept(c)
        }
        listener = l
        l.start(queue: queue)
    }

    private func close() {
        if let c = conn { drop(c) }
        if let l = listener { listener = nil; l.cancel(); log("音の口を閉じた") }
        failNoted = false
    }

    // 一度に 1 本。新しいつなぎが来たら古い方を切る
    private func accept(_ c: NWConnection) {
        if let old = conn { drop(old) }
        connID += 1
        let id = connID
        conn = c
        c.stateUpdateHandler = { [weak self, weak c] st in
            guard let self, let c else { return }
            switch st {
            case .failed, .cancelled: self.drop(c)
            default: break
            }
        }
        c.start(queue: queue)
        log("音：つながった")
        readExactly(c, 40) { [weak self] d in
            guard let self, c === self.conn else { return }
            guard let d, let h = Hello(d) else { self.log("音：あいさつの形が違う。切る"); self.drop(c); return }
            self.begin(c, h, id)
        }
    }

    private func drop(_ c: NWConnection) {
        guard c === conn else { return }
        conn = nil
        micTimer?.cancel(); micTimer = nil
        statsTimer?.cancel(); statsTimer = nil
        ring = nil; micRing = nil; hello = nil
        wantMicLater = false
        c.cancel()
        log("音：切れた")
        DispatchQueue.main.async { [weak self] in
            self?.stopEngine()
            self?.connected = false
            self?.speakerActive = false
            self?.micActive = false
            self?.micWaitsForeground = false
            self?.fillMs = 0
        }
    }

    private func begin(_ c: NWConnection, _ h: Hello, _ id: Int) {
        hello = h
        spkExpected = 0
        micIndex = 0; micSent = 0; micDropped = 0; pendingSends = 0
        let wantSpk = h.flags & IPA.fSpeaker != 0
        var r: PlayoutRing?
        if wantSpk {
            let t = h.targetMs == 0 ? 40 : Double(min(max(Int(h.targetMs), 10), 300))
            r = PlayoutRing(rate: Double(h.spkRate), targetMs: t)
        }
        ring = r
        micRate = Int(h.micRate == 0 ? 48000 : min(max(h.micRate, 8000), 48000))
        log("音：あいさつ flags=0x\(String(h.flags, radix: 16)) スピーカー \(h.spkRate)Hz マイク \(micRate)Hz ため \(h.targetMs)ms")
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.startEngine(h, ring: r) { micRing, info in
                self.queue.async {
                    guard c === self.conn, id == self.connID else { return }
                    self.micRing = micRing
                    self.sendAck(c, h, info)
                    self.startTimers(c)
                    self.readFrame(c)
                    DispatchQueue.main.async { self.connected = true }
                }
            }
        }
    }

    // ---- 音の機器（main の上）----

    private struct EngineInfo { var speakerOK: Bool; var micOK: Bool; var needsForeground: Bool; var hwRate: UInt32; var ioUs: UInt32; var outUs: UInt32; var inUs: UInt32 }

    private func configureSession(mic: Bool, exclusive: Bool) throws {
        let s = AVAudioSession.sharedInstance()
        if mic {
            // iPhone につないだイヤホンがあれば、そこへ出す（AirPods などは A2DP で音質を保つ）。
            // 「イヤホンのマイクを使う」なら Bluetooth の通話の形（HFP）にして、イヤホンのマイクを取る。そのときは音質が下がる
            var o: AVAudioSession.CategoryOptions = [.defaultToSpeaker, .allowBluetoothA2DP]
            if UserDefaults.standard.bool(forKey: "audioHeadsetMic") { o.insert(.allowBluetooth) }
            if !exclusive { o.insert(.mixWithOthers) }
            try s.setCategory(.playAndRecord, mode: .default, options: o)
        } else {
            try s.setCategory(.playback, mode: .default, options: exclusive ? [] : [.mixWithOthers])
        }
        // 先に頼む（有効にしてから値を読む）。mixWithOthers のままだと短いバッファが通りにくい。EXCLUSIVE ならほかの音と混ぜない
        try? s.setPreferredSampleRate(48000)
        try? s.setPreferredIOBufferDuration(0.005)
        try s.setActive(true)
    }

    // ring が nil ならスピーカーなし。done(マイクの輪, 状態) は main で呼ぶ
    private func startEngine(_ h: Hello, ring: PlayoutRing?, done: @escaping (MicRing?, EngineInfo) -> Void) {
        stopEngine()
        let wantMic = h.flags & IPA.fMic != 0
        let exclusive = h.flags & IPA.fExclusive != 0
        let fg = UIApplication.shared.applicationState == .active
        let perm = AVAudioApplication.shared.recordPermission
        // マイクは前に出ているときしか始められない（後ろから新しく録りはじめるのは OS が断る）
        let micAllowed = wantMic && fg && perm == .granted
        var info = EngineInfo(speakerOK: false, micOK: false, needsForeground: wantMic && !fg, hwRate: 0, ioUs: 0, outUs: 0, inUs: 0)
        if wantMic && perm == .denied { log("音：マイクが許可されていない。設定アプリで許可すると使える") }
        if wantMic && perm == .undetermined { AVAudioApplication.requestRecordPermission { _ in } }
        if wantMic && !fg { queue.async { [weak self] in self?.wantMicLater = true }; micWaitsForeground = true }
        do { try configureSession(mic: micAllowed, exclusive: exclusive) } catch {
            log("音：セッションを始められない：\(error)")
            done(nil, info)
            return
        }
        let e = AVAudioEngine()
        var micRing: MicRing?
        if let ring {
            let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: Double(h.spkRate), channels: 2, interleaved: false)!
            let scratch = Scratch()
            let src = AVAudioSourceNode(format: fmt) { _, _, frameCount, abl -> OSStatus in
                let list = UnsafeMutableAudioBufferListPointer(abl)
                var pos = 0
                let total = Int(frameCount)
                let tmp = scratch.p
                while pos < total {
                    let n = min(8192, total - pos)
                    ring.read(frames: n, into: tmp)
                    if list.count >= 2, let l = list[0].mData?.assumingMemoryBound(to: Float.self), let r = list[1].mData?.assumingMemoryBound(to: Float.self) {
                        for i in 0..<n { l[pos + i] = tmp[i * 2]; r[pos + i] = tmp[i * 2 + 1] }
                    } else if list.count == 1, let p = list[0].mData?.assumingMemoryBound(to: Float.self) {
                        (p + pos * 2).update(from: tmp, count: n * 2) // 交互の形で来たとき
                    }
                    pos += n
                }
                return noErr
            }
            e.attach(src)
            e.connect(src, to: e.mainMixerNode, format: fmt)
            info.speakerOK = true
        }
        if micAllowed {
            if h.flags & IPA.fVoiceProc != 0 { do { try e.inputNode.setVoiceProcessingEnabled(true) } catch { log("音：音声処理を入れられない：\(error)") } }
            let inFmt = e.inputNode.outputFormat(forBus: 0)
            if inFmt.sampleRate > 0 && inFmt.channelCount > 0 {
                let outFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: Double(micRate), channels: 1, interleaved: false)!
                let mr = MicRing(rate: micRate)
                // 入力 →（rate と ch を合わせる）→ sink。sink はバッファの長さを自分で決めず、機器の塊のまま渡す
                let mix = AVAudioMixerNode()
                e.attach(mix)
                e.connect(e.inputNode, to: mix, format: inFmt)
                let sink = AVAudioSinkNode { _, frameCount, abl -> OSStatus in
                    let list = UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: abl))
                    if let p = list.first?.mData?.assumingMemoryBound(to: Float.self) { mr.write(p, frames: Int(frameCount)) }
                    return noErr
                }
                e.attach(sink)
                e.connect(mix, to: sink, format: outFmt)
                micRing = mr
                info.micOK = true
            }
        }
        e.prepare()
        do { try e.start() } catch {
            log("音：エンジンを始められない：\(error)")
            info.speakerOK = false; info.micOK = false
            done(nil, info)
            return
        }
        engine = e
        observe(e)
        let s = AVAudioSession.sharedInstance()
        info.hwRate = UInt32(s.sampleRate)
        info.ioUs = UInt32(s.ioBufferDuration * 1e6)
        info.outUs = UInt32(s.outputLatency * 1e6)
        info.inUs = UInt32(s.inputLatency * 1e6)
        ioMs = s.ioBufferDuration * 1000
        speakerActive = info.speakerOK
        micActive = info.micOK
        if info.micOK { micWaitsForeground = false }
        log("音：始めた スピーカー=\(info.speakerOK) マイク=\(info.micOK) hw=\(info.hwRate)Hz io=\(String(format: "%.1f", s.ioBufferDuration * 1000))ms out=\(String(format: "%.1f", s.outputLatency * 1000))ms in=\(String(format: "%.1f", s.inputLatency * 1000))ms")
        done(micRing, info)
    }

    private func stopEngine() {
        for o in observers { NotificationCenter.default.removeObserver(o) }
        observers.removeAll()
        if let e = engine {
            e.stop()
            engine = nil
            // 受け手の「後ろに回っても続ける」の再生（Keeper）の設定に戻す
            let s = AVAudioSession.sharedInstance()
            try? s.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            try? s.setActive(true)
        }
        speakerActive = false
        micActive = false
    }

    // 経路が変わった（イヤホンの抜き差しなど）・電話などで止められた：止まっていたら動かしなおす
    private func observe(_ e: AVAudioEngine) {
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .AVAudioEngineConfigurationChange, object: e, queue: .main) { [weak self, weak e] _ in
            guard let self, let e, e === self.engine, !e.isRunning else { return }
            do { try e.start(); self.log("音：経路が変わったので動かしなおした") } catch { self.log("音：動かしなおせない：\(error)") }
        })
        observers.append(nc.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] n in
            guard let self, let e = self.engine else { return }
            let raw = n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            if raw == AVAudioSession.InterruptionType.ended.rawValue {
                try? AVAudioSession.sharedInstance().setActive(true)
                if !e.isRunning { try? e.start() }
                self.log("音：中断が終わったので続ける")
            }
        })
    }

    // 前に出てからマイクだけ足したとき
    private func attachMic(_ mr: MicRing?) {
        guard conn != nil else { return }
        micRing = mr
        DispatchQueue.main.async { [weak self] in self?.micWaitsForeground = false }
    }

    // ---- 送る（queue の上）----

    private func sendAck(_ c: NWConnection, _ h: Hello, _ i: EngineInfo) {
        var o = ByteOut()
        o.d.append(contentsOf: Array("IPAK".utf8))
        o.u16(1)
        var f: UInt16 = 0
        if i.speakerOK { f |= IPA.aSpeakerOK }
        if i.micOK { f |= IPA.aMicOK }
        if i.needsForeground { f |= IPA.aMicNeedsForeground }
        o.u16(f)
        o.u32(UInt32(micRate))   // 前に出ていなくても、あとで始めたら必ずこの rate・1ch で送る
        o.u16(1); o.u16(1)
        o.u32(i.hwRate); o.u32(i.ioUs); o.u32(i.outUs); o.u32(i.inUs)
        o.zeros(8)
        c.send(content: o.d, completion: .contentProcessed({ _ in }))
    }

    private func startTimers(_ c: NWConnection) {
        // マイク：5 ms ごとに、たまった分を 10 ms の塊にして送る
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + .milliseconds(5), repeating: .milliseconds(5), leeway: .milliseconds(1))
        t.setEventHandler { [weak self] in self?.pumpMic(c) }
        micTimer = t
        t.resume()
        // 1 秒ごと：状態を PC に送り、画面の数字も更新する
        let s = DispatchSource.makeTimerSource(queue: queue)
        s.schedule(deadline: .now() + 1, repeating: 1)
        s.setEventHandler { [weak self] in self?.sendStats(c) }
        statsTimer = s
        s.resume()
    }

    private func pumpMic(_ c: NWConnection) {
        guard c === conn, let mr = micRing else { return }
        let chunk = micRate / 100
        while mr.available >= chunk {
            if pendingSends > 20 { // PC が受けていない。古い方を捨て、番号だけ進める（PC は抜けとして無音で埋める）
                mr.discard(chunk)
                micDropped += UInt64(chunk)
                micIndex += UInt64(chunk)
                continue
            }
            var out = frameHeader(type: IPA.tMic, frames: UInt16(chunk), length: UInt32(chunk * 2), index: micIndex)
            var pcm = [Int16](repeating: 0, count: chunk)
            guard pcm.withUnsafeMutableBufferPointer({ mr.pop($0.baseAddress!, frames: chunk) }) else { break }
            pcm.withUnsafeBytes { out.append(contentsOf: $0) }
            pendingSends += 1
            c.send(content: out, completion: .contentProcessed({ [weak self] _ in self?.queue.async { self?.pendingSends -= 1 } }))
            micIndex += UInt64(chunk)
            micSent += UInt64(chunk)
        }
    }

    private func sendStats(_ c: NWConnection) {
        guard c === conn else { return }
        var o = ByteOut()
        var fillMs = 0.0
        if let r = ring {
            let s = r.snapshot()
            o.u32(UInt32(max(0, s.fill))); o.u32(UInt32(s.target)); o.u32(s.under); o.u32(s.over); o.u32(s.gap)
            o.u32(UInt32(bitPattern: s.ratioPPM))
            fillMs = s.fill * 1000 / Double(hello?.spkRate ?? 48000)
        } else {
            o.zeros(24)
        }
        o.u32(UInt32(truncatingIfNeeded: micSent)); o.u32(UInt32(truncatingIfNeeded: micDropped))
        c.send(content: frameHeader(type: IPA.tStats, length: 32) + o.d, completion: .contentProcessed({ _ in }))
        DispatchQueue.main.async { [weak self] in self?.fillMs = fillMs }
    }

    // ---- 受ける（queue の上）----

    private func readFrame(_ c: NWConnection) {
        readExactly(c, 16) { [weak self] h in
            guard let self, c === self.conn else { return }
            guard let h else { self.drop(c); return }
            let type = h[h.startIndex]
            let flags = h[h.startIndex + 1]
            let frames = Int(le16(h, 2))
            let length = Int(le32(h, 4))
            let index = le64(h, 8)
            if length > IPA.maxPayload { self.log("音：長さ \(length) が上限を超えた。形が違うので切る"); self.drop(c); return }
            if length == 0 {
                self.handle(c, type, flags, frames, index, Data())
                self.readFrame(c)
                return
            }
            self.readExactly(c, length) { [weak self] body in
                guard let self, c === self.conn else { return }
                guard let body else { self.drop(c); return }
                self.handle(c, type, flags, frames, index, body)
                self.readFrame(c)
            }
        }
    }

    private func handle(_ c: NWConnection, _ type: UInt8, _ flags: UInt8, _ frames: Int, _ index: UInt64, _ body: Data) {
        switch type {
        case IPA.tSpk:
            guard let r = ring else { return }
            if index > spkExpected { // 抜け。無音で埋めて、時間の流れを保つ
                r.writeSilence(Int(min(index - spkExpected, 96_000)))
                spkExpected = index
            } else if index < spkExpected {
                return // 古い塊
            }
            if flags & IPA.frSilent != 0 { r.writeSilence(frames) } else {
                body.withUnsafeBytes { r.write($0, frames: frames) }
            }
            // 録画している間は、PC の音も録画に入れる（GameRecorder・vkr_record_pcm。録画していなければ何もしない）
            let rate = hello?.spkRate ?? 48000, ch = UInt32(hello?.spkChannels ?? 2)
            if flags & IPA.frSilent != 0 { vkr_record_pcm(nil, UInt32(frames), rate, ch, index) }
            else { body.withUnsafeBytes { vkr_record_pcm($0.baseAddress?.assumingMemoryBound(to: Int16.self), UInt32(frames), rate, ch, index) } }
            spkExpected += UInt64(frames)
        case IPA.tPing:
            c.send(content: frameHeader(type: IPA.tPong, index: index), completion: .contentProcessed({ _ in }))
        case IPA.tBye:
            drop(c)
        default:
            break
        }
    }

    // 切れたときは nil。足りないまま終わったときも nil
    private func readExactly(_ c: NWConnection, _ n: Int, _ done: @escaping (Data?) -> Void) {
        c.receive(minimumIncompleteLength: n, maximumLength: n) { data, _, _, err in
            if let data, data.count == n, err == nil { done(data) } else { done(nil) }
        }
    }
}

// ---- 画面（仮）----

// 「状態」に出す 1 行
struct AudioBridgeStatusRow: View {
    @ObservedObject var model: AudioBridgeModel

    var body: some View {
        LabeledContent(L("PC の音", "PC audio"), value: text)
    }

    private var text: String {
        guard model.connected else { return L("接続待ち", "Waiting") }
        var parts: [String] = []
        if model.speakerActive { parts.append(L("スピーカーのバッファ \(Int(model.fillMs)) ms", "Speaker \(Int(model.fillMs)) ms buffered")) }
        if model.micActive { parts.append(L("マイク", "Mic")) }
        if model.micWaitsForeground { parts.append(L("マイクはアプリを前面に表示すると開始します", "Mic starts when the app is in front")) }
        // いま音が出ている所と、マイクの音を取っている所（イヤホンの名前など）
        let route = AVAudioSession.sharedInstance().currentRoute
        if model.speakerActive, let out = route.outputs.first?.portName { parts.append(L("出力：\(out)", "Out: \(out)")) }
        if model.micActive, let inp = route.inputs.first?.portName { parts.append(L("マイク：\(inp)", "Mic: \(inp)")) }
        return parts.isEmpty ? L("接続中", "Connected") : parts.joined(separator: L("・", ", "))
    }
}
