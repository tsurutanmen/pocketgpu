import AVFoundation
import Network
import SwiftUI
import UIKit
import os

// iPhone を、PC のスピーカーとマイクにする。PC は work/audio/pc/ipaudio.c（usbmuxd 越しに USB の 50063 番へつなぐ）。
// 約束は work/audio/DESIGN.md の「約束」と work/mic2/pc/proto.h。1 本の TCP で両方向を流す：
//   PC → iPhone：HELLO（40 バイト）→ 以降 [16 バイトの頭][PCM]（SPK）。PCM は s16le・2ch・PC の rate
//   iPhone → PC：ACK（40 バイト）→ 以降 [16 バイトの頭][PCM]（MIC）。PCM の rate・ch・形は ACK に書いた物
//   どちらからも PING / PONG（往復の測り）、iPhone からは 1 秒ごとに STATS、終わるときは BYE
// マイクの形と音の種類（mic2 で足した。古い PC は HELLO で 1ch・s16 を頼むので、前と同じ形で送る）：
//   ch：HELLO の mic_channels が 2 で、画面の「ステレオで送る」がオンで、入力が 2ch 取れたときだけ 2ch。それ以外は 1ch
//   形：HELLO の mic_format の 1 = s16le・2 = s24le（3 バイト）・3 = float32le。知らない値は 1
//   rate：HELLO の mic_rate（8000〜96000）。48000 より上は、iPhone の機器がその rate で動いたときだけ。だめなら 48000
//   音の種類：HELLO の 28 バイト目。0 は画面の設定・1 加工なし（mode の measurement）・2 標準・3 雑音とエコーを消す（音声処理）。
//            0 で flags に 0x4 があれば 3（前の PC の --voiceproc）
//   ACK の 32〜35 バイト目：使った音の種類・入力の種類・ステレオの印・入力の機器の ch 数。前の PC は読まない
//   一度 ACK に書いた rate・ch・形は、イヤホンの抜き差しで音を作り直しても変えない（足りない ch は AVAudioMixerNode が増やす）
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
    // マイクの PCM の形（HELLO と ACK の mic_format）
    static let fmtS16: UInt16 = 1, fmtS24: UInt16 = 2, fmtF32: UInt16 = 3
    // ACK の 34 バイト目
    static let a2StereoCapable: UInt8 = 0x01, a2StereoOn: UInt8 = 0x02
    // ACK の 33 バイト目：入力の種類
    static let kindBuiltIn: UInt8 = 1, kindHeadset: UInt8 = 2, kindUSB: UInt8 = 3, kindBluetooth: UInt8 = 4, kindOther: UInt8 = 5
}

// ---- マイクの設定（画面の MicSettingsView が書き、音を始めるときに読む）----

enum MicKey {
    static let mode = "audioMicMode"
    static let input = "audioMicInput"
    static let stereo = "audioMicStereo"
    static let orientation = "audioMicOrientation"
    // 前の版の「イヤホンのマイクを使う」（Bluetooth のマイクにする）。入力をまだ選んでいなければ、これがオンなら「Bluetooth」と同じ。
    // 書き換えて移すことはしない（Lite の PocketCamApp はこのスイッチのまま使っている）
    static let oldHeadset = "audioHeadsetMic"

    // 入力をまだ選んでいないときは、前の版のスイッチから決める
    static func effectiveInput(_ raw: String?, oldHeadset: Bool) -> MicInput {
        MicInput(rawValue: raw ?? "") ?? (oldHeadset ? .bluetooth : .auto)
    }
}

// マイクの音の種類
enum MicMode: Int, CaseIterable, Identifiable {
    case raw = 1, standard = 2, voice = 3
    var id: Int { rawValue }
    var label: String {
        switch self {
        case .raw: return L("加工なし", "Unprocessed")
        case .standard: return L("標準", "Standard")
        case .voice: return L("雑音とエコーを消す", "Reduce noise and echo")
        }
    }
    var note: String {
        switch self {
        case .raw: return L("iOS による音の処理を最小にして送ります。録画に向いています。この間は、iPhone から出る PC の音が小さくなります。", "Sends the mic with as little iOS processing as possible. Good for recording. PC audio from the iPhone is quieter while this is on.")
        case .standard: return L("iOS の標準の処理をした音を送ります。", "Sends the mic with the standard iOS processing.")
        case .voice: return L("通話に向いています。周りの雑音と、iPhone のスピーカーから戻るエコーを消します。", "Good for calls. Reduces background noise and the echo from the iPhone speaker.")
        }
    }
    fileprivate var logName: String {
        switch self {
        case .raw: return "加工なし・measurement"
        case .standard: return "標準・default"
        case .voice: return "雑音とエコーを消す・音声処理"
        }
    }
}

// マイクの入力。自動は、有線イヤホンが挿さっていればそのマイク、なければ本体のマイク。Bluetooth は選んだときだけ
enum MicInput: String, CaseIterable, Identifiable {
    case auto, builtIn, wired, usb, bluetooth
    var id: String { rawValue }
    var label: String {
        switch self {
        case .auto: return L("自動", "Auto")
        case .builtIn: return L("iPhone のマイク", "iPhone mic")
        case .wired: return L("有線イヤホン", "Wired earphones")
        case .usb: return L("USB デバイス", "USB device")
        case .bluetooth: return "Bluetooth"
        }
    }
}

// ステレオで録るときの iPhone の向き。左右がどちらになるかが決まる（AVAudioSession.StereoOrientation）
enum MicOrientation: Int, CaseIterable, Identifiable {
    case portrait = 1, portraitUpsideDown = 2, landscapeRight = 3, landscapeLeft = 4
    var id: Int { rawValue }
    var label: String {
        switch self {
        case .portrait: return L("縦・端子が下", "Portrait, connector at the bottom")
        case .portraitUpsideDown: return L("縦・端子が上", "Portrait, connector at the top")
        case .landscapeRight: return L("横・端子が右", "Landscape, connector on the right")
        case .landscapeLeft: return L("横・端子が左", "Landscape, connector on the left")
        }
    }
    var stereo: AVAudioSession.StereoOrientation {
        switch self {
        case .portrait: return .portrait
        case .portraitUpsideDown: return .portraitUpsideDown
        case .landscapeRight: return .landscapeRight
        case .landscapeLeft: return .landscapeLeft
        }
    }
}

private struct MicSettings {
    var mode: MicMode
    var input: MicInput
    var stereo: Bool
    var orientation: MicOrientation

    static func load() -> MicSettings {
        let d = UserDefaults.standard
        let input = MicKey.effectiveInput(d.string(forKey: MicKey.input), oldHeadset: d.bool(forKey: MicKey.oldHeadset))
        let mode = MicMode(rawValue: d.integer(forKey: MicKey.mode)) ?? .standard
        let o = MicOrientation(rawValue: d.integer(forKey: MicKey.orientation)) ?? .portrait
        return MicSettings(mode: mode, input: input, stereo: d.bool(forKey: MicKey.stereo), orientation: o)
    }
}

// ---- 小さな道具 ----

// マイクの許可。AVAudioApplication は iOS 17 から。それより前は AVAudioSession の同じ物（iOS 17 で非推奨）を使う
// （あとで作る iOS 15 からのカメラとマイクだけの軽い版で、このファイルをそのまま使うため）
private enum MicPermission {
    case granted, denied, undetermined
    static var current: MicPermission {
        if #available(iOS 17.0, *) {
            switch AVAudioApplication.shared.recordPermission {
            case .granted: return .granted
            case .denied: return .denied
            default: return .undetermined
            }
        } else {
            switch AVAudioSession.sharedInstance().recordPermission {
            case .granted: return .granted
            case .denied: return .denied
            default: return .undetermined
            }
        }
    }
    static func request() {
        if #available(iOS 17.0, *) {
            AVAudioApplication.requestRecordPermission { _ in }
        } else {
            AVAudioSession.sharedInstance().requestRecordPermission { _ in }
        }
    }
}

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
    var pcMicMode: UInt8 // 28 バイト目。前の PC は 0（予約の 0）

    init?(_ d: Data) {
        guard d.count >= 40, d.prefix(4).elementsEqual("IPAU".utf8) else { return nil }
        version = le16(d, 4); flags = le16(d, 6)
        spkRate = le32(d, 8); spkChannels = le16(d, 12); spkFormat = le16(d, 14)
        micRate = le32(d, 16); micChannels = le16(d, 20); micFormat = le16(d, 22)
        chunkFrames = le16(d, 24); targetMs = le16(d, 26)
        pcMicMode = d[d.startIndex + 28]
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
// Float（-1〜1）のまま、ch の交互で持つ。送るときに PC の頼んだ形（s16・s24・float）にする

final class MicRing {
    let channels: Int
    private let cap: Int
    private let buf: UnsafeMutablePointer<Float>
    private var wr: UInt64 = 0, rd: UInt64 = 0
    private let lock = UnfairLock()

    init(rate: Int, channels: Int) {
        self.channels = max(1, min(2, channels))
        cap = max(rate, 8000) // 1 秒
        buf = UnsafeMutablePointer<Float>.allocate(capacity: cap * self.channels)
        buf.initialize(repeating: 0, count: cap * self.channels)
    }
    deinit { buf.deallocate() }

    // sink が渡す塊を積む。形は ch ごとに分かれた Float（AVAudioFormat の標準の形）。あふれたら古い方を捨てる
    func write(_ list: UnsafeMutableAudioBufferListPointer, frames: Int) {
        guard list.count > 0, let p0 = list[0].mData?.assumingMemoryBound(to: Float.self) else { return }
        let p1 = list.count > 1 ? list[1].mData?.assumingMemoryBound(to: Float.self) : nil
        lock.lock(); defer { lock.unlock() }
        let ch = channels
        for i in 0..<frames {
            let at = Int(wr % UInt64(cap)) * ch
            buf[at] = p0[i]
            if ch == 2 { buf[at + 1] = p1?[i] ?? p0[i] }
            wr += 1
        }
        let over = Int(wr - rd) - cap
        if over > 0 { rd += UInt64(over) }
    }

    var available: Int {
        lock.lock(); defer { lock.unlock() }
        return Int(wr - rd)
    }

    // frames コマ（frames × ch 個）を out に取り出す。足りなければ false
    func pop(_ out: UnsafeMutablePointer<Float>, frames: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        guard Int(wr - rd) >= frames else { return false }
        let ch = channels
        for i in 0..<frames {
            let at = Int((rd + UInt64(i)) % UInt64(cap)) * ch
            for c in 0..<ch { out[i * ch + c] = buf[at + c] }
        }
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
    // 画面に出す、いま使っている入力と出力の名前。マイクの形（例：48 kHz・ステレオ・32 ビット）
    @Published private(set) var inputName = ""
    @Published private(set) var outputName = ""
    @Published private(set) var micDetail = ""
    // PC が音の種類を決めているときの知らせ。決めていなければ空
    @Published private(set) var pcMicNote = ""

    // main の上だけで触る
    private var on = false
    private var engine: AVAudioEngine?
    private var observers: [NSObjectProtocol] = []
    private var lastInputsSig = ""
    private var rebuildTimes: [Date] = []
    private var stereoTouched = false // ステレオのために、本体のマイクの向きの設定を変えた
    private var channelsTouched = false // USB の機器などで 2ch を頼んだ

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
    private var micPromise: MicPromise? // ACK で PC に伝えたマイクの形
    private var micIndex: UInt64 = 0
    private var micSent: UInt64 = 0, micDropped: UInt64 = 0
    private var pendingSends = 0
    private var wantMicLater = false // 前に出ていなくてマイクを始められなかった
    private var rebuildSeq = 0

    private init() {}

    private func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }

    // 画面の糸から呼ぶ。on の間だけ 50063 番で待つ
    func setOn(_ v: Bool) {
        guard v != on else { return }
        on = v
        if v, MicPermission.current == .undetermined {
            // 許可の確認は前にいる間に出す。許可されなければマイクは使えないがスピーカーは使える
            MicPermission.request()
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
            guard let self, self.wantMicLater, let h = self.hello, let p = self.micPromise else { return }
            let r = self.ring // スピーカーなしのときは nil
            self.wantMicLater = false
            self.log("音：前に出たので、マイクを始める")
            DispatchQueue.main.async { [weak self] in
                self?.startEngine(h, ring: r, promise: p) { micRing, _ in
                    self?.queue.async { self?.attachMic(micRing) }
                }
            }
        }
    }

    // 画面でマイクの設定を変えたときに呼ぶ。接続中なら、その場で音を作り直す
    func settingsChanged() {
        requestRebuild("マイクの設定を変えた", onlyIfStopped: false)
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
        ring = nil; micRing = nil; hello = nil; micPromise = nil
        wantMicLater = false
        rebuildSeq += 1 // 待っている作り直しを取り消す
        c.cancel()
        log("音：切れた")
        DispatchQueue.main.async { [weak self] in
            self?.stopEngine()
            self?.connected = false
            self?.speakerActive = false
            self?.micActive = false
            self?.micWaitsForeground = false
            self?.fillMs = 0
            self?.inputName = ""
            self?.outputName = ""
            self?.micDetail = ""
            self?.pcMicNote = ""
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
        log("音：あいさつ flags=0x\(String(h.flags, radix: 16)) スピーカー \(h.spkRate)Hz マイク \(h.micRate)Hz・\(h.micChannels)ch・形 \(h.micFormat)・種類 \(h.pcMicMode) ため \(h.targetMs)ms")
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.startEngine(h, ring: r, promise: nil) { micRing, info in
                self.queue.async {
                    guard c === self.conn, id == self.connID else { return }
                    self.micRing = micRing
                    self.micPromise = MicPromise(rate: info.micRate, channels: info.micChannels, format: info.micFormat)
                    self.sendAck(c, info)
                    self.startTimers(c)
                    self.readFrame(c)
                    DispatchQueue.main.async { self.connected = true }
                }
            }
        }
    }

    // ---- 音の機器（main の上）----

    // ACK で PC に伝えたマイクの形。音を作り直しても変えない
    private struct MicPromise { var rate: Int; var channels: Int; var format: UInt16 }

    private struct EngineInfo {
        var speakerOK = false, micOK = false, needsForeground = false
        var hwRate: UInt32 = 0, ioUs: UInt32 = 0, outUs: UInt32 = 0, inUs: UInt32 = 0
        var micRate = 48000, micChannels = 1
        var micFormat: UInt16 = IPA.fmtS16
        var micMode = MicMode.standard
        var inputKind: UInt8 = 0, flags2: UInt8 = 0, hwInChannels: UInt8 = 0
    }

    private struct SessionInfo { var inputKind: UInt8 = 0; var stereoCapable = false; var inputMissing = false }

    // PC が決めた音の種類。0 なら画面の設定
    private static func micMode(_ h: Hello, _ st: MicSettings) -> MicMode {
        if let m = MicMode(rawValue: Int(h.pcMicMode)) { return m }
        if h.flags & IPA.fVoiceProc != 0 { return .voice }
        return st.mode
    }

    private static func micFormat(_ h: Hello) -> UInt16 {
        switch h.micFormat {
        case IPA.fmtS24, IPA.fmtF32: return h.micFormat
        default: return IPA.fmtS16
        }
    }

    private static func askedMicRate(_ h: Hello) -> Int {
        h.micRate == 0 ? 48000 : Int(min(max(h.micRate, 8000), 96000))
    }

    private static func kindCode(_ t: AVAudioSession.Port) -> UInt8 {
        switch t {
        case .builtInMic: return IPA.kindBuiltIn
        case .headsetMic: return IPA.kindHeadset
        case .usbAudio: return IPA.kindUSB
        case .bluetoothHFP: return IPA.kindBluetooth
        default: return IPA.kindOther
        }
    }

    // 画面に出す機器の名前
    static func portLabel(_ p: AVAudioSessionPortDescription) -> String {
        switch p.portType {
        case .builtInMic: return L("iPhone のマイク", "iPhone mic")
        case .builtInSpeaker: return L("iPhone のスピーカー", "iPhone speaker")
        case .builtInReceiver: return L("iPhone の受話口", "iPhone earpiece")
        case .headsetMic, .headphones: return L("有線イヤホン", "Wired earphones")
        case .usbAudio: return L("USB・", "USB: ") + p.portName
        case .bluetoothHFP, .bluetoothA2DP, .bluetoothLE: return L("Bluetooth・", "Bluetooth: ") + p.portName
        default: return p.portName
        }
    }

    private static func inputsSignature() -> String {
        (AVAudioSession.sharedInstance().availableInputs ?? []).map { $0.portType.rawValue + ":" + $0.uid }.sorted().joined(separator: ",")
    }

    private static func formatName(_ f: UInt16) -> String {
        switch f {
        case IPA.fmtF32: return "f32"
        case IPA.fmtS24: return "s24"
        default: return "s16"
        }
    }

    private func configureSession(mic: Bool, exclusive: Bool, mode: MicMode, settings st: MicSettings, wantStereo: Bool, preferredRate: Double) throws -> SessionInfo {
        let s = AVAudioSession.sharedInstance()
        if !mic {
            try s.setCategory(.playback, mode: .default, options: exclusive ? [] : [.mixWithOthers])
            try? s.setPreferredSampleRate(48000)
            try? s.setPreferredIOBufferDuration(0.005)
            try s.setActive(true)
            return SessionInfo()
        }
        // iPhone につないだイヤホンがあれば、そこへ出す（AirPods などは A2DP で音質を保つ）。
        // Bluetooth のマイクを選んだときだけ、Bluetooth の通話の形（HFP）を許す。そのときは音質が下がる
        var o: AVAudioSession.CategoryOptions = [.allowBluetoothA2DP]
        if st.input == .bluetooth { o.insert(.allowBluetooth) }
        if !exclusive { o.insert(.mixWithOthers) }
        let m: AVAudioSession.Mode = mode == .raw ? .measurement : .default
        try s.setCategory(.playAndRecord, mode: m, options: o)
        // 先に頼む（有効にしてから値を読む）。mixWithOthers のままだと短いバッファが通りにくい。EXCLUSIVE ならほかの音と混ぜない
        try? s.setPreferredSampleRate(preferredRate)
        try? s.setPreferredIOBufferDuration(0.005)
        try s.setActive(true)
        // ほかに出す所が無いと受話口になる。そのときだけスピーカーにする。
        // defaultToSpeaker は、付けたあとに有線イヤホンを挿しても切り替わらないと Apple の資料にあるので、受話口のときだけ付ける。
        // 抜き差しのときは routeChangeNotification から作り直して、ここをもう一度通る
        if s.currentRoute.outputs.contains(where: { $0.portType == .builtInReceiver }) {
            try s.setCategory(.playAndRecord, mode: m, options: o.union(.defaultToSpeaker))
        }
        return selectInput(s, st, wantStereo: wantStereo)
    }

    // 入力を選び、ステレオにする。setPreferredInput などは、カテゴリと mode を決めて有効にしたあとに呼ぶ（Apple の資料）
    private func selectInput(_ s: AVAudioSession, _ st: MicSettings, wantStereo: Bool) -> SessionInfo {
        var info = SessionInfo()
        let inputs = s.availableInputs ?? []
        func first(_ t: AVAudioSession.Port) -> AVAudioSessionPortDescription? { inputs.first(where: { $0.portType == t }) }
        var pick: AVAudioSessionPortDescription?
        switch st.input {
        case .auto: pick = first(.headsetMic) ?? first(.usbAudio) ?? first(.builtInMic)
        case .builtIn: pick = first(.builtInMic)
        case .wired: pick = first(.headsetMic) ?? first(.usbAudio)
        case .usb: pick = first(.usbAudio)
        case .bluetooth: pick = first(.bluetoothHFP)
        }
        if pick == nil {
            // 選んだ機器が挿さっていない。本体のマイクに戻す
            info.inputMissing = st.input != .auto
            pick = first(.builtInMic) ?? inputs.first
            if info.inputMissing { log("音：選んだ入力「\(st.input.label)」が見つからないので、本体のマイクを使う") }
        }
        guard let port = pick else { return info }
        info.inputKind = Self.kindCode(port.portType)
        if s.preferredInput?.uid != port.uid || s.currentRoute.inputs.first?.uid != port.uid {
            do { try s.setPreferredInput(port) } catch { log("音：入力を選べない：\(error)") }
        }
        let stereoPattern = AVAudioSession.PolarPattern.stereo
        let sources = port.dataSources ?? []
        let canStereo = sources.filter { $0.supportedPolarPatterns?.contains(stereoPattern) == true }
        info.stereoCapable = !canStereo.isEmpty
        if port.portType == .builtInMic {
            if wantStereo, !canStereo.isEmpty {
                // 前面 → 背面 → ほかの順。ステレオにできる物だけから選ぶ
                let ds = canStereo.first(where: { $0.orientation == AVAudioSession.Orientation.front })
                    ?? canStereo.first(where: { $0.orientation == AVAudioSession.Orientation.back })
                    ?? canStereo[0]
                do {
                    if ds.preferredPolarPattern != stereoPattern { try ds.setPreferredPolarPattern(stereoPattern) }
                    if port.preferredDataSource?.dataSourceID != ds.dataSourceID { try port.setPreferredDataSource(ds) }
                    if s.inputOrientation != st.orientation.stereo { try s.setPreferredInputOrientation(st.orientation.stereo) }
                    stereoTouched = true
                } catch { log("音：ステレオにできない：\(error)") }
            } else if stereoTouched {
                // 前にステレオにした。元に戻す（データソースと向きは iOS に任せる）
                for ds in sources where ds.preferredPolarPattern == stereoPattern { try? ds.setPreferredPolarPattern(nil) }
                try? port.setPreferredDataSource(nil)
                stereoTouched = false
            }
        } else if wantStereo, s.maximumInputNumberOfChannels >= 2 {
            // USB の機器などで 2ch 取れるなら 2ch で
            do { try s.setPreferredInputNumberOfChannels(2); channelsTouched = true } catch { log("音：入力を 2ch にできない：\(error)") }
        }
        if !wantStereo, channelsTouched {
            try? s.setPreferredInputNumberOfChannels(1)
            channelsTouched = false
        }
        return info
    }

    // ring が nil ならスピーカーなし。promise は ACK で伝えた形（最初は nil で、ここで決める）。done(マイクの輪, 状態) は main で呼ぶ
    private func startEngine(_ h: Hello, ring: PlayoutRing?, promise: MicPromise?, done: @escaping (MicRing?, EngineInfo) -> Void) {
        stopEngine()
        let wantMic = h.flags & IPA.fMic != 0
        let exclusive = h.flags & IPA.fExclusive != 0
        let fg = UIApplication.shared.applicationState == .active
        let perm = MicPermission.current
        // マイクは前に出ているときしか始められない（後ろから新しく録りはじめるのは OS が断る）
        let micAllowed = wantMic && fg && perm == .granted
        let st = MicSettings.load()
        let mode = Self.micMode(h, st)
        let askRate = Self.askedMicRate(h)
        let wantStereo = promise.map { $0.channels >= 2 } ?? (h.micChannels >= 2 && st.stereo)
        var info = EngineInfo()
        info.needsForeground = wantMic && !fg
        info.micRate = promise?.rate ?? min(askRate, 48000)
        info.micChannels = promise?.channels ?? (wantStereo ? 2 : 1)
        info.micFormat = promise?.format ?? Self.micFormat(h)
        info.micMode = mode
        let pcChose = h.pcMicMode != 0 || h.flags & IPA.fVoiceProc != 0
        if pcChose && mode != st.mode {
            pcMicNote = L("PC が「\(mode.label)」を指定しています。", "The PC asked for \u{201C}\(mode.label)\u{201D}.")
        } else {
            pcMicNote = ""
        }
        if wantMic && perm == .denied { log("音：マイクが許可されていない。設定アプリで許可すると使える") }
        if wantMic && perm == .undetermined { MicPermission.request() }
        if wantMic && !fg { queue.async { [weak self] in self?.wantMicLater = true }; micWaitsForeground = true }
        let sess: SessionInfo
        do {
            sess = try configureSession(mic: micAllowed, exclusive: exclusive, mode: mode, settings: st, wantStereo: wantStereo,
                                        preferredRate: Double(max(48000, promise?.rate ?? askRate)))
        } catch {
            log("音：セッションを始められない：\(error)")
            done(nil, info)
            return
        }
        let s = AVAudioSession.sharedInstance()
        if promise == nil, micAllowed, askRate > 48000 {
            // 48 kHz より上は、機器がその rate で動いたときだけ
            info.micRate = s.sampleRate >= Double(askRate) - 1 ? askRate : 48000
        }
        info.inputKind = sess.inputKind
        if sess.stereoCapable { info.flags2 |= IPA.a2StereoCapable }
        lastInputsSig = micAllowed ? Self.inputsSignature() : ""
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
        var inDesc = "-"
        if micAllowed {
            if mode == .voice { do { try e.inputNode.setVoiceProcessingEnabled(true) } catch { log("音：音声処理を入れられない：\(error)") } }
            let inFmt = e.inputNode.outputFormat(forBus: 0)
            inDesc = "\(inFmt.channelCount)ch・\(Int(inFmt.sampleRate))Hz"
            if inFmt.sampleRate > 0 && inFmt.channelCount > 0 {
                info.hwInChannels = UInt8(min(255, inFmt.channelCount))
                if promise == nil { info.micChannels = wantStereo && inFmt.channelCount >= 2 ? 2 : 1 }
                if info.micChannels >= 2 && inFmt.channelCount >= 2 { info.flags2 |= IPA.a2StereoOn }
                let outFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: Double(info.micRate),
                                           channels: AVAudioChannelCount(info.micChannels), interleaved: false)!
                let mr = MicRing(rate: info.micRate, channels: info.micChannels)
                // 入力 →（rate と ch を合わせる。ch が足りなければ増やし、多ければ混ぜる）→ sink。sink は機器の塊のまま渡す
                let mix = AVAudioMixerNode()
                e.attach(mix)
                e.connect(e.inputNode, to: mix, format: inFmt)
                let sink = AVAudioSinkNode { _, frameCount, abl -> OSStatus in
                    mr.write(UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: abl)), frames: Int(frameCount))
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
        info.hwRate = UInt32(s.sampleRate)
        info.ioUs = UInt32(s.ioBufferDuration * 1e6)
        info.outUs = UInt32(s.outputLatency * 1e6)
        info.inUs = UInt32(s.inputLatency * 1e6)
        ioMs = s.ioBufferDuration * 1000
        speakerActive = info.speakerOK
        micActive = info.micOK
        if info.micOK { micWaitsForeground = false }
        micDetail = info.micOK ? Self.detailText(info) : ""
        updateRouteNames()
        log("音：始めた スピーカー=\(info.speakerOK) マイク=\(info.micOK) hw=\(info.hwRate)Hz io=\(String(format: "%.1f", s.ioBufferDuration * 1000))ms out=\(String(format: "%.1f", s.outputLatency * 1000))ms in=\(String(format: "%.1f", s.inputLatency * 1000))ms")
        if info.micOK {
            // 親が実機で見る 1 行：実際の機器の rate と ch、使っている入力の機器、送る形
            let route = s.currentRoute
            let ip = route.inputs.first, op = route.outputs.first
            let ds = ip?.selectedDataSource
            log("音：マイクの実際 hw=\(Int(s.sampleRate))Hz 入力の機器の ch=\(s.inputNumberOfChannels) エンジンの入力=\(inDesc) 機器=\(ip?.portName ?? "-")・\(ip?.portType.rawValue ?? "-") データソース=\(ds?.dataSourceName ?? "-")・\(ds?.selectedPolarPattern?.rawValue ?? "-") 向き=\(s.inputOrientation.rawValue) 送る形=\(info.micRate)Hz・\(info.micChannels)ch・\(Self.formatName(info.micFormat)) 種類=\(mode.logName) 出力=\(op?.portName ?? "-")・\(op?.portType.rawValue ?? "-")")
        }
        done(micRing, info)
    }

    private static func detailText(_ i: EngineInfo) -> String {
        let khz = i.micRate % 1000 == 0 ? "\(i.micRate / 1000) kHz" : String(format: "%.1f kHz", Double(i.micRate) / 1000)
        let ch = i.micChannels >= 2 ? L("ステレオ", "stereo") : L("モノラル", "mono")
        let bits: String
        switch i.micFormat {
        case IPA.fmtF32: bits = L("32 ビット", "32-bit")
        case IPA.fmtS24: bits = L("24 ビット", "24-bit")
        default: bits = L("16 ビット", "16-bit")
        }
        return [khz, ch, bits].joined(separator: L("・", ", "))
    }

    private func updateRouteNames() {
        let r = AVAudioSession.sharedInstance().currentRoute
        inputName = micActive ? (r.inputs.first.map(Self.portLabel) ?? "") : ""
        outputName = speakerActive ? (r.outputs.first.map(Self.portLabel) ?? "") : ""
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

    // 経路が変わった（イヤホンの抜き差しなど）・機器の形が変わった・電話などで止められた。
    // 経路と形の変化は、通知の中では何もせず、作り直しを頼むだけにする（Apple の注意：通知の中でエンジンを捨てると止まることがある）
    private func observe(_ e: AVAudioEngine) {
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: .AVAudioEngineConfigurationChange, object: e, queue: nil) { [weak self] _ in
            // 機器の rate か ch が変わると、エンジンは止まる。止まっていたら作り直す
            self?.requestRebuild("音の機器の形が変わった", onlyIfStopped: true)
        })
        observers.append(nc.addObserver(forName: AVAudioSession.routeChangeNotification, object: nil, queue: nil) { [weak self] n in
            let raw = n.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt
            let reason = raw.flatMap { AVAudioSession.RouteChangeReason(rawValue: $0) }
            switch reason {
            case .newDeviceAvailable?, .oldDeviceUnavailable?:
                self?.requestRebuild("イヤホンなどを抜き差しした", onlyIfStopped: false)
            default:
                DispatchQueue.main.async { self?.updateRouteNames() }
            }
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

    // 続けて来た頼みは 0.4 秒まとめて 1 回にする。10 秒に 5 回を超えたら、2 秒おいてからにする
    private func requestRebuild(_ why: String, onlyIfStopped: Bool) {
        queue.async { [weak self] in
            guard let self, self.conn != nil, self.micPromise != nil else { return }
            self.rebuildSeq += 1
            let seq = self.rebuildSeq
            self.queue.asyncAfter(deadline: .now() + 0.4) { [weak self] in
                guard let self, seq == self.rebuildSeq, let c = self.conn, let h = self.hello, let p = self.micPromise else { return }
                let r = self.ring, id = self.connID
                DispatchQueue.main.async { [weak self] in
                    guard let self else { return }
                    if onlyIfStopped, let e = self.engine, e.isRunning { return }
                    let now = Date()
                    self.rebuildTimes = self.rebuildTimes.filter { now.timeIntervalSince($0) < 10 }
                    if self.rebuildTimes.count >= 5 {
                        self.log("音：作り直しが続いたので、2 秒おいてからにする")
                        self.queue.asyncAfter(deadline: .now() + 2) { [weak self] in self?.requestRebuild(why, onlyIfStopped: onlyIfStopped) }
                        return
                    }
                    self.rebuildTimes.append(now)
                    self.log("音：\(why)。音を作り直す")
                    self.startEngine(h, ring: r, promise: p) { [weak self] micRing, _ in
                        self?.queue.async { [weak self] in
                            guard let self, c === self.conn, id == self.connID else { return }
                            self.micRing = micRing
                        }
                    }
                }
            }
        }
    }

    // 1 秒ごと（main）：画面の名前を更新し、入力の機器が増えたり減ったりしていたら作り直す。
    // defaultToSpeaker を付けている間は、有線イヤホンを挿しても経路が変わらず通知も来ないことがあるので、ここでも見る
    private func pollRoute() {
        updateRouteNames()
        guard micActive, engine != nil else { return }
        let sig = Self.inputsSignature()
        if !lastInputsSig.isEmpty, sig != lastInputsSig {
            lastInputsSig = sig
            requestRebuild("入力の機器が変わった", onlyIfStopped: false)
        }
    }

    // 前に出てからマイクだけ足したとき
    private func attachMic(_ mr: MicRing?) {
        guard conn != nil else { return }
        micRing = mr
        DispatchQueue.main.async { [weak self] in self?.micWaitsForeground = false }
    }

    // ---- 送る（queue の上）----

    private func sendAck(_ c: NWConnection, _ i: EngineInfo) {
        var o = ByteOut()
        o.d.append(contentsOf: Array("IPAK".utf8))
        o.u16(1)
        var f: UInt16 = 0
        if i.speakerOK { f |= IPA.aSpeakerOK }
        if i.micOK { f |= IPA.aMicOK }
        if i.needsForeground { f |= IPA.aMicNeedsForeground }
        o.u16(f)
        o.u32(UInt32(i.micRate))   // 前に出ていなくても、あとで始めたら必ずこの rate・ch・形で送る
        o.u16(UInt16(i.micChannels)); o.u16(i.micFormat)
        o.u32(i.hwRate); o.u32(i.ioUs); o.u32(i.outUs); o.u32(i.inUs)
        o.u8(UInt8(i.micMode.rawValue)); o.u8(i.inputKind); o.u8(i.flags2); o.u8(i.hwInChannels)
        o.zeros(4)
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
        guard c === conn, let mr = micRing, let p = micPromise else { return }
        let chunk = p.rate / 100
        let ch = mr.channels
        guard ch == p.channels else { return } // 形が合わない輪は使わない（起きない見込み）
        let bps = p.format == IPA.fmtF32 ? 4 : p.format == IPA.fmtS24 ? 3 : 2
        var pcm = [Float](repeating: 0, count: chunk * ch)
        while mr.available >= chunk {
            if pendingSends > 20 { // PC が受けていない。古い方を捨て、番号だけ進める（PC は抜けとして無音で埋める）
                mr.discard(chunk)
                micDropped += UInt64(chunk)
                micIndex += UInt64(chunk)
                continue
            }
            guard pcm.withUnsafeMutableBufferPointer({ mr.pop($0.baseAddress!, frames: chunk) }) else { break }
            let n = chunk * ch
            var bytes = [UInt8](repeating: 0, count: n * bps)
            switch p.format {
            case IPA.fmtF32:
                for i in 0..<n {
                    let b = pcm[i].bitPattern
                    bytes[i * 4] = UInt8(truncatingIfNeeded: b); bytes[i * 4 + 1] = UInt8(truncatingIfNeeded: b >> 8)
                    bytes[i * 4 + 2] = UInt8(truncatingIfNeeded: b >> 16); bytes[i * 4 + 3] = UInt8(truncatingIfNeeded: b >> 24)
                }
            case IPA.fmtS24:
                for i in 0..<n {
                    let v = Int32(max(-1.0, min(1.0, pcm[i])) * 8388607.0)
                    bytes[i * 3] = UInt8(truncatingIfNeeded: v); bytes[i * 3 + 1] = UInt8(truncatingIfNeeded: v >> 8); bytes[i * 3 + 2] = UInt8(truncatingIfNeeded: v >> 16)
                }
            default:
                for i in 0..<n {
                    let v = Int16(max(-1.0, min(1.0, pcm[i])) * 32767.0)
                    bytes[i * 2] = UInt8(truncatingIfNeeded: v); bytes[i * 2 + 1] = UInt8(truncatingIfNeeded: v >> 8)
                }
            }
            var out = frameHeader(type: IPA.tMic, frames: UInt16(chunk), length: UInt32(bytes.count), index: micIndex)
            out.append(contentsOf: bytes)
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
        DispatchQueue.main.async { [weak self] in
            self?.fillMs = fillMs
            self?.pollRoute()
        }
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

// 「音と字幕」の一番上に出す行。PC の音の状態と、いま使っている入力と出力の名前。
// iOS 15 でも使えるように LabeledContent（iOS 16 から）は使わない
struct AudioBridgeStatusRow: View {
    @ObservedObject var model: AudioBridgeModel

    var body: some View {
        row(L("PC の音", "PC audio"), text)
        if model.connected && model.micActive && !model.inputName.isEmpty {
            row(L("マイク", "Mic"), model.micDetail.isEmpty ? model.inputName : model.inputName + L("・", ", ") + model.micDetail)
        }
        if model.connected && model.speakerActive && !model.outputName.isEmpty {
            row(L("出力", "Output"), model.outputName)
        }
    }

    private func row(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title)
            Spacer()
            Text(value).foregroundColor(.secondary).multilineTextAlignment(.trailing)
        }
    }

    private var text: String {
        guard model.connected else { return L("接続待ち", "Waiting") }
        var parts: [String] = []
        if model.speakerActive { parts.append(L("スピーカーのバッファ \(Int(model.fillMs)) ms", "Speaker \(Int(model.fillMs)) ms buffered")) }
        if model.micActive { parts.append(L("マイク", "Mic")) }
        if model.micWaitsForeground { parts.append(L("マイクはアプリを前面に表示すると開始します", "Mic starts when the app is in front")) }
        return parts.isEmpty ? L("接続中", "Connected") : parts.joined(separator: L("・", ", "))
    }
}

// 「音と字幕」の「マイク」に出す設定。入力・音の種類・ステレオ・iPhone の向き。
// 変えたら、接続中ならその場で音を作り直す。iOS 15 でも使えるように、onChange（iOS 17 の形）を使わず、選んだときに Binding の set で知らせる
struct MicSettingsView: View {
    @ObservedObject var model: AudioBridgeModel
    @AppStorage(MicKey.input) private var inputRaw = "" // 空はまだ選んでいない
    @AppStorage(MicKey.oldHeadset) private var oldHeadset = false
    @AppStorage(MicKey.mode) private var mode = MicMode.standard.rawValue
    @AppStorage(MicKey.stereo) private var stereo = false
    @AppStorage(MicKey.orientation) private var orientation = MicOrientation.portrait.rawValue

    var body: some View {
        Picker(L("入力", "Input"), selection: inputBinding) {
            ForEach(MicInput.allCases) { m in Text(m.label).tag(m.rawValue) }
        }
        .pickerStyle(.menu)
        Text(inputNote).font(.footnote)
        Picker(L("音の種類", "Sound processing"), selection: bind($mode)) {
            ForEach(MicMode.allCases) { m in Text(m.label).tag(m.rawValue) }
        }
        .pickerStyle(.menu)
        Text((MicMode(rawValue: mode) ?? .standard).note).font(.footnote)
        if !model.pcMicNote.isEmpty {
            Text(model.pcMicNote).font(.footnote)
        }
        Toggle(L("ステレオで送る", "Send in stereo"), isOn: bind($stereo))
        if stereo {
            Picker(L("iPhone の向き", "iPhone orientation"), selection: bind($orientation)) {
                ForEach(MicOrientation.allCases) { o in Text(o.label).tag(o.rawValue) }
            }
            .pickerStyle(.menu)
            Text(L("本体のマイクがステレオに対応している機種で使えます。加工なしでは、モノラルになる場合があります。左右は、ここで選んだ向きで決まります。", "Works on models whose built-in mic supports stereo. Unprocessed may be mono. Left and right follow the orientation you choose here.")).font(.footnote)
        }
    }

    private var input: MicInput { MicKey.effectiveInput(inputRaw, oldHeadset: oldHeadset) }

    private var inputBinding: Binding<String> {
        Binding(get: { input.rawValue }, set: { v in
            inputRaw = v
            model.settingsChanged()
        })
    }

    private var inputNote: String {
        switch input {
        case .auto: return L("有線イヤホンを挿している間は、そのマイクを使います。抜くと iPhone のマイクに戻ります。", "Uses the wired earphone mic while it is plugged in, and the iPhone mic after you unplug it.")
        case .bluetooth: return L("Bluetooth のマイクを使う間は、音質が下がります。", "The sound quality is lower while the Bluetooth mic is in use.")
        case .builtIn, .wired, .usb: return L("選んだデバイスが接続されていないときは、iPhone のマイクを使います。", "If the selected device is not connected, the iPhone mic is used.")
        }
    }

    private func bind<T>(_ b: Binding<T>) -> Binding<T> {
        Binding(get: { b.wrappedValue }, set: { v in
            b.wrappedValue = v
            model.settingsChanged()
        })
    }
}
