import AVFoundation
import CoreMedia
import CoreVideo
import Network
import SwiftUI
import UIKit
import VideoToolbox

// iPhone のカメラを、USB でつないだ Windows の PC のウェブカメラにする送り手。
// PC は work/webcam/pc/webcam_rx.c（受けてほどいて窓に出し、仮想カメラへ流す）。約束は work/webcam/DESIGN.md と pc/wcam.h と同じ。
//
// つなぎ方：オンの間だけ、USB（iPhone の中の 127.0.0.1）の 50064 番で待つ（ScreenMirror の 50062 番と同じ作り）。一度に 1 本だけ受ける。
//   PC が HELLO（解像度・コマ数・カメラ・ビットレート）を送る → こちらがカメラを開いて ACK（決まった形）を返す
//   → カメラが渡す NV12 の絵を、そのまま VideoToolbox（H.264・低遅延・B フレームなし）に渡して、できた物を Annex B にして 1 コマずつ送る。
// カメラは iOS の決まりで、アプリが前にいる間しか使えない（後ろに回ると iOS が止める）。止まったら EVENT を送り、戻ったら続きを送る。
// Vulkan の受け手（50060）には触らない。
// 画面に出す文は仮。言葉は鶴田さんが決める
final class CameraBridge: NSObject, ObservableObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    static let shared = CameraBridge()
    static let port: UInt16 = 50064
    static let version: UInt32 = 1

    // メッセージの型（pc/wcam.h と同じ）
    private enum Msg {
        static let hello: UInt32 = 1, ack: UInt32 = 2, frame: UInt32 = 3, event: UInt32 = 4
        static let reqKey: UInt32 = 16, setKbps: UInt32 = 17, ping: UInt32 = 20, pong: UInt32 = 21
    }

    // 画面に見せる（画面の糸だけで触る）
    @Published private(set) var connected = false
    @Published private(set) var fps = 0.0
    @Published private(set) var detail = "" // 例：1920×1080・30 コマ/秒・背面
    // Wi-Fi でつなぐときの合言葉（起動のたびに変わる 6 けた）。PC は webcam_rx --addr iPhoneの住所:50064 --code これ
    let wifiCode = UInt32.random(in: 100_000...999_999)
    static let wifiKey = "webcamWifi"

    private var on = false
    private var lastTick = Date()
    private var shownCount: UInt64 = 0
    private var lastShown: UInt64 = 0

    // ここから下は q の上だけで触る
    private let q = DispatchQueue(label: "camera-bridge", qos: .userInitiated)
    private var listener: NWListener?
    private var wifiListener: NWListener? // 設定で入れたときだけ。Wi-Fi の口で待ち、合言葉を確かめる
    private var wifiMisses = 0              // 合言葉の違いが 5 回続いたら Wi-Fi の口を閉じる（当てずっぽうを止める）
    private var conn: NWConnection?
    private var connRemote = false
    private var connID = 0
    private var listening = false
    private var failNoted = false
    private var appActive = true
    private var session: AVCaptureSession?
    private var deviceInput: AVCaptureDeviceInput?
    private var venc: VTCompressionSession?
    private var curFps = 30, curKbps = 4000, curW = 0, curH = 0
    private var observers: [NSObjectProtocol] = []

    // カメラの糸・エンコーダの糸からも触る物（lock で守る）
    private let lock = NSLock()
    private var sendConn: NWConnection? // 送り先。つなぎが無い・止めているときは nil
    private var forceKey = true
    private var inflight = 0            // 送り始めて、まだ iOS が送り終えていないコマの数
    private var seq: UInt32 = 0
    private var sent: UInt64 = 0
    private var dropped: UInt64 = 0
    private let maxInflight = 4         // これより溜まったら、カメラの絵を捨てて追いつく（USB が詰まったとき、古いコマを送り続けない）

    private let captureQueue = DispatchQueue(label: "camera-capture", qos: .userInteractive)

    private override init() {
        super.init()
        let nc = NotificationCenter.default
        // アプリが前にいるか。iOS はカメラを後ろで使わせない
        observers.append(nc.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: nil) { [weak self] _ in self?.q.async { self?.appActive = true } })
        observers.append(nc.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: nil) { [weak self] _ in self?.q.async { self?.appActive = false } })
        observers.append(nc.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: nil) { [weak self] _ in
            let s = UInt32(ProcessInfo.processInfo.thermalState.rawValue)
            self?.q.async { self?.sendEvent(3, s) }
        })
    }

    private func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }

    // ---- 画面の糸から呼ぶ ----

    // on の間だけ 50064 番で待つ。off にしたらカメラを止め、口を閉じ、つなぎを切る
    func setOn(_ v: Bool) {
        guard v != on else { return }
        on = v
        if !v {
            connected = false
            fps = 0
            detail = ""
        }
        let active = UIApplication.shared.applicationState == .active
        q.async { [weak self] in
            guard let self else { return }
            self.appActive = active
            self.listening = v
            if v { self.listen(); self.listenWifi() } else { self.close() }
        }
    }

    // 1 秒ごとに呼ぶ。送ったコマの数の差で毎秒のコマ数を出す
    func tick() {
        let now = Date()
        let dt = now.timeIntervalSince(lastTick)
        guard dt >= 0.5 else { return }
        lock.lock(); let s = sent; lock.unlock()
        let f = Double(s &- lastShown) / dt
        lastShown = s
        lastTick = now
        if f != fps { fps = f }
    }

    // ---- ここから下は q の上 ----

    private func listen() {
        guard listening, listener == nil, let p = NWEndpoint.Port(rawValue: Self.port) else { return }
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: p) // USB（iPhone の中の 127.0.0.1）だけ。Wi-Fi には出さない
        params.allowLocalEndpointReuse = true
        let l: NWListener
        do { l = try NWListener(using: params) } catch {
            noteFailure("\(error)")
            retryLater()
            return
        }
        l.stateUpdateHandler = { [weak self, weak l] st in
            guard let self, let l, l === self.listener else { return }
            switch st {
            case .ready:
                self.failNoted = false
                self.log("ウェブカメラの口：127.0.0.1:\(Self.port) で待っている")
            case .failed(let e):
                self.noteFailure("\(e)")
                self.listener = nil
                l.cancel()
                self.retryLater()
            default: break
            }
        }
        l.newConnectionHandler = { [weak self, weak l] c in
            guard let self, let l, l === self.listener else { c.cancel(); return }
            self.accept(c)
        }
        listener = l
        l.start(queue: q)
    }

    private func noteFailure(_ why: String) {
        if !failNoted { log("ウェブカメラの口を開けない：\(why)。1 秒ごとに開き直す") }
        failNoted = true
    }

    private func retryLater() { q.asyncAfter(deadline: .now() + 1) { [weak self] in self?.listen() } }

    private func close() {
        if let c = conn { drop(c) }
        if let l = listener {
            listener = nil
            l.cancel()
            log("ウェブカメラの口を閉じた")
        }
        closeWifi()
        failNoted = false
    }

    // 画面の糸から呼ぶ。Wi-Fi でも待つかを切り替える（ウェブカメラがオンの間だけ効く）
    func setWifi(_ v: Bool) {
        q.async { [weak self] in
            guard let self else { return }
            if v && self.listening { self.wifiMisses = 0; self.listenWifi() } else if !v { self.closeWifi() }
        }
    }

    private func listenWifi() {
        guard listening, wifiListener == nil, UserDefaults.standard.bool(forKey: Self.wifiKey), let p = NWEndpoint.Port(rawValue: Self.port) else { return }
        let params = NWParameters.tcp
        params.requiredInterfaceType = .wifi // Wi-Fi の口だけ。合言葉の無い頼みは断る
        params.allowLocalEndpointReuse = true
        guard let l = try? NWListener(using: params, on: p) else { log("ウェブカメラ：Wi-Fi の口を開けない"); return }
        l.stateUpdateHandler = { [weak self, weak l] st in
            guard let self, let l, l === self.wifiListener else { return }
            if case .ready = st { self.log("ウェブカメラ：Wi-Fi の \(Self.port) 番でも待っている（合言葉つき）") }
            if case .failed = st { self.wifiListener = nil; l.cancel() }
        }
        l.newConnectionHandler = { [weak self, weak l] c in
            guard let self, let l, l === self.wifiListener else { c.cancel(); return }
            self.accept(c, remote: true)
        }
        wifiListener = l
        l.start(queue: q)
    }

    private func closeWifi() {
        if let l = wifiListener {
            wifiListener = nil
            l.cancel()
            log("ウェブカメラ：Wi-Fi の口を閉じた")
        }
    }

    // 一度に 1 本。新しいつなぎが来たら、古い方を切る（PC の橋渡しを開き直したとき、切れ残りを待たない）
    private func accept(_ c: NWConnection, remote: Bool = false) {
        if let old = conn { drop(old) }
        connID += 1
        conn = c
        connRemote = remote
        c.stateUpdateHandler = { [weak self, weak c] st in
            guard let self, let c else { return }
            switch st {
            case .failed, .cancelled: self.drop(c)
            default: break
            }
        }
        log("ウェブカメラ：PC がつながった")
        c.start(queue: q)
        readMessage(c)
    }

    // いまのつなぎなら、カメラとエンコーダを止めて切る
    private func drop(_ c: NWConnection) {
        guard c === conn else { return }
        conn = nil
        stopCapture()
        c.cancel()
        log("ウェブカメラ：切れた")
        DispatchQueue.main.async { [weak self] in
            self?.connected = false
            self?.detail = ""
        }
    }

    // [u32 型][u32 長さ][中身] を 1 つ読み、次を待つ
    private func readMessage(_ c: NWConnection) {
        readExactly(c, 8) { [weak self] h in
            guard let self, c === self.conn else { return }
            guard let h else { self.drop(c); return }
            let b = [UInt8](h)
            let type = Self.u32(b, 0), len = Int(Self.u32(b, 4))
            if len > 1 << 16 { self.log("ウェブカメラ：長さが \(len) バイトで上限を超えた。形が違うので切る"); self.drop(c); return }
            if len == 0 { self.handle(c, type, []); self.readMessage(c); return }
            self.readExactly(c, len) { [weak self] body in
                guard let self, c === self.conn else { return }
                guard let body else { self.drop(c); return }
                self.handle(c, type, [UInt8](body))
                self.readMessage(c)
            }
        }
    }

    private func readExactly(_ c: NWConnection, _ n: Int, _ done: @escaping (Data?) -> Void) {
        c.receive(minimumIncompleteLength: n, maximumLength: n) { data, _, _, err in
            if let data, data.count == n, err == nil { done(data) } else { done(nil) }
        }
    }

    private static func u32(_ b: [UInt8], _ at: Int) -> UInt32 {
        UInt32(b[at]) | UInt32(b[at + 1]) << 8 | UInt32(b[at + 2]) << 16 | UInt32(b[at + 3]) << 24
    }

    private func handle(_ c: NWConnection, _ type: UInt32, _ body: [UInt8]) {
        switch type {
        case Msg.hello where body.count >= 32:
            if connRemote {
                guard body.count >= 36, Self.u32(body, 32) == wifiCode else {
                    wifiMisses += 1
                    log("ウェブカメラ：Wi-Fi の合言葉が違う（\(wifiMisses) 回目）")
                    sendAck(status: 6, w: 0, h: 0, fps: 0, kbps: 0, camera: 0)
                    q.asyncAfter(deadline: .now() + 0.2) { [weak self] in if let self, c === self.conn { self.drop(c) } }
                    if wifiMisses >= 5 { closeWifi() }
                    return
                }
                wifiMisses = 0
            }
            let f = (0..<8).map { Self.u32(body, $0 * 4) } // version, width, height, fps, kbps, gop, camera, flags
            startSession(c, version: f[0], width: Int(f[1]), height: Int(f[2]), fps: Int(f[3]), kbps: Int(f[4]), gop: Int(f[5]), camera: f[6], flags: f[7])
        case Msg.reqKey:
            lock.lock(); forceKey = true; lock.unlock()
        case Msg.setKbps where body.count >= 4:
            setBitrate(Int(Self.u32(body, 0)))
        case Msg.ping where body.count >= 8:
            var d = Data()
            d.append(contentsOf: body[0..<8])
            Self.appendLE64(&d, Self.nowUs())
            sendMsg(Msg.pong, d)
        default:
            break // 知らない型は読み捨てる（先の版が足した物でも壊れない）
        }
    }

    // ---- 始める：権限 → カメラ → エンコーダ → ACK → 流す ----

    private func startSession(_ c: NWConnection, version: UInt32, width: Int, height: Int, fps: Int, kbps: Int, gop: Int, camera: UInt32, flags: UInt32) {
        func refuse(_ status: Int32, _ why: String) {
            log("ウェブカメラ：断った（\(status)・\(why)）")
            sendAck(status: status, w: 0, h: 0, fps: 0, kbps: 0, camera: camera)
            // ACK が出てから切る
            q.asyncAfter(deadline: .now() + 0.2) { [weak self] in if let self, c === self.conn { self.drop(c) } }
        }
        stopCapture() // 2 回目の HELLO（形を変えて頼み直す）のときは、前のカメラを止めてから
        let w = width == 0 ? 1280 : width, h = height == 0 ? 720 : height, fr = fps == 0 ? 30 : fps
        guard version == Self.version, w >= 64, h >= 64, w <= 4096, h <= 3072, fr >= 1, fr <= 120 else { refuse(5, "頼みが変"); return }
        guard appActive else { refuse(3, "アプリが前にいない（iOS はカメラを後ろで使わせない）"); return }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: break
        case .notDetermined:
            // 許可の画面を出す。答えが来たらもう一度ここへ（HELLO をそのまま使う）
            AVCaptureDevice.requestAccess(for: .video) { [weak self] ok in
                self?.q.async {
                    guard let self, c === self.conn else { return }
                    if ok { self.startSession(c, version: version, width: width, height: height, fps: fps, kbps: kbps, gop: gop, camera: camera, flags: flags) }
                    else { refuse(1, "カメラの許可が無い") }
                }
            }
            return
        default: refuse(1, "カメラの許可が無い"); return
        }
        guard let dev = Self.pickDevice(camera) else { refuse(2, "そのカメラが無い"); return }
        let fmt = Self.pickFormat(dev, w: w, h: h, fps: fr)
        let dims = fmt.map { CMVideoFormatDescriptionGetDimensions($0.formatDescription) }
        let aw = Int(dims?.width ?? Int32(w)), ah = Int(dims?.height ?? Int32(h))
        let kb = kbps > 0 ? kbps : Self.defaultKbps(w: aw, h: ah, fps: fr)
        let g = gop > 0 ? gop : fr * 2
        guard let enc = makeEncoder(w: aw, h: ah, fps: fr, kbps: kb, gop: g) else { refuse(4, "エンコーダを作れない"); return }
        let s = AVCaptureSession()
        s.beginConfiguration()
        s.sessionPreset = .inputPriority // 形は device.activeFormat で決める（プリセットだと 1080p は 30 コマまでのことがある）
        do {
            let input = try AVCaptureDeviceInput(device: dev)
            guard s.canAddInput(input) else { throw NSError(domain: "CameraBridge", code: 1) }
            s.addInput(input)
            deviceInput = input
            try dev.lockForConfiguration()
            if let fmt { dev.activeFormat = fmt }
            let d = CMTime(value: 1, timescale: CMTimeScale(fr))
            dev.activeVideoMinFrameDuration = d
            dev.activeVideoMaxFrameDuration = d
            // HDR の映像にしない（形式が変わり、色が合わなくなる）
            if dev.activeFormat.isVideoHDRSupported { dev.automaticallyAdjustsVideoHDREnabled = false; dev.isVideoHDREnabled = false }
            dev.unlockForConfiguration()
            let out = AVCaptureVideoDataOutput()
            out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            out.alwaysDiscardsLateVideoFrames = true // 遅れたコマは溜めずに捨てる
            out.setSampleBufferDelegate(self, queue: captureQueue)
            guard s.canAddOutput(out) else { throw NSError(domain: "CameraBridge", code: 2) }
            s.addOutput(out)
            if let cn = out.connection(with: .video) {
                // 向きは iPhone の持ち方に関わらず、横長（センサーの向き）で固定する。ウェブカメラの絵は 16:9 の横長
                if cn.isVideoRotationAngleSupported(0) { cn.videoRotationAngle = 0 }
                if cn.isVideoMirroringSupported {
                    cn.automaticallyAdjustsVideoMirroring = false
                    cn.isVideoMirrored = (flags & 1) != 0 // ふつうは反転しない。鏡のように見せるのは使う側のアプリ
                }
            }
        } catch {
            s.commitConfiguration()
            VTCompressionSessionInvalidate(enc)
            deviceInput = nil
            refuse(2, "カメラを開けない：\(error)")
            return
        }
        s.commitConfiguration()
        session = s
        venc = enc
        curFps = fr; curKbps = kb; curW = aw; curH = ah
        lock.lock(); sendConn = c; encCopy = enc; fpsCopy = fr; wCopy = aw; hCopy = ah; forceKey = true; inflight = 0; seq = 0; sent = 0; dropped = 0; lock.unlock()
        observeSession(s)
        sendAck(status: 0, w: aw, h: ah, fps: fr, kbps: kb, camera: camera)
        log("ウェブカメラ：始める \(aw)×\(ah)・\(fr) コマ/秒・\(kb) kbps・カメラ \(camera)（\(dev.localizedName)）")
        // startRunning は返ってくるまでに時間がかかるので、カメラの糸で
        captureQueue.async { s.startRunning() }
        let name = camera == 1 ? L("前面", "front") : L("背面", "back")
        DispatchQueue.main.async { [weak self] in
            self?.connected = true
            self?.detail = "\(aw)×\(ah)・\(fr)・\(name)"
        }
    }

    // カメラの選び方（0 背面の広角・1 前面・2 背面の超広角・3 背面の望遠）。無ければ nil
    private static func pickDevice(_ id: UInt32) -> AVCaptureDevice? {
        switch id {
        case 1: return AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front)
        case 2: return AVCaptureDevice.default(.builtInUltraWideCamera, for: .video, position: .back)
        case 3: return AVCaptureDevice.default(.builtInTelephotoCamera, for: .video, position: .back)
        default: return AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
        }
    }

    // 形の選び方：大きさがぴったりで、そのコマ数に対応する物。同じ大きさが何種類かあれば、いちばん広く写る物（videoFieldOfView が大きい）。
    // 無ければ、そのコマ数に対応する中で面積がいちばん近い物。それも無ければ nil（いまの形のまま）
    private static func pickFormat(_ dev: AVCaptureDevice, w: Int, h: Int, fps: Int) -> AVCaptureDevice.Format? {
        let want = Double(fps)
        let ok = dev.formats.filter { f in
            CMFormatDescriptionGetMediaSubType(f.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                && f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= want && want <= $0.maxFrameRate }
        }
        let exact = ok.filter { f in
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return Int(d.width) == w && Int(d.height) == h
        }
        if let best = exact.max(by: { $0.videoFieldOfView < $1.videoFieldOfView }) { return best }
        return ok.min(by: { a, b in
            let da = CMVideoFormatDescriptionGetDimensions(a.formatDescription), db = CMVideoFormatDescriptionGetDimensions(b.formatDescription)
            return abs(Int(da.width) * Int(da.height) - w * h) < abs(Int(db.width) * Int(db.height) - w * h)
        })
    }

    private static func defaultKbps(w: Int, h: Int, fps: Int) -> Int {
        let big = w * h >= 1920 * 1080
        return big ? (fps >= 60 ? 12000 : 8000) : (fps >= 60 ? 6000 : 4000)
    }

    // ---- エンコーダ（VideoToolbox・server.c の vid_open と同じ設定） ----

    private func makeEncoder(w: Int, h: Int, fps: Int, kbps: Int, gop: Int) -> VTCompressionSession? {
        var s: VTCompressionSession?
        let spec = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary
        let st = VTCompressionSessionCreate(allocator: nil, width: Int32(w), height: Int32(h), codecType: kCMVideoCodecType_H264, encoderSpecification: spec,
                                            imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        guard st == noErr, let s else { log("ウェブカメラ：エンコーダを作れない（\(st)）"); return nil }
        func set(_ k: CFString, _ v: CFTypeRef) { _ = VTSessionSetProperty(s, key: k, value: v) }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse) // B フレームなし
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue)
        if VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_ConstrainedHigh_AutoLevel) != noErr {
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel)
        }
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: kbps * 1000))
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: gop))
        VTCompressionSessionPrepareToEncodeFrames(s)
        return s
    }

    private func setBitrate(_ kbps: Int) {
        guard kbps >= 200, kbps <= 100_000, let s = venc else { return }
        _ = VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: kbps * 1000))
        curKbps = kbps
        log("ウェブカメラ：ビットレートを \(kbps) kbps に変えた")
    }

    // カメラとエンコーダを止める（つなぎ自体は切らない）
    private func stopCapture() {
        lock.lock(); sendConn = nil; encCopy = nil; lock.unlock()
        for o in sessionObservers { NotificationCenter.default.removeObserver(o) }
        sessionObservers.removeAll()
        if let s = session {
            session = nil
            deviceInput = nil
            captureQueue.async { // 走っている最中のコマの扱いが終わってから止める
                s.stopRunning()
                for i in s.inputs { s.removeInput(i) }
                for o in s.outputs { s.removeOutput(o) }
            }
        }
        if let e = venc {
            venc = nil
            VTCompressionSessionCompleteFrames(e, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(e)
        }
    }

    // カメラが止まった・戻った（アプリが後ろに回った・電話・他のアプリが使った）を PC に知らせる
    private var sessionObservers: [NSObjectProtocol] = []
    private func observeSession(_ s: AVCaptureSession) {
        let nc = NotificationCenter.default
        sessionObservers.append(nc.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: s, queue: nil) { [weak self] n in
            let why = (n.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int) ?? 0
            self?.q.async { self?.log("ウェブカメラ：カメラが止まった（理由 \(why)）"); self?.sendEvent(1, UInt32(why)) }
        })
        sessionObservers.append(nc.addObserver(forName: AVCaptureSession.interruptionEndedNotification, object: s, queue: nil) { [weak self] _ in
            self?.q.async {
                self?.log("ウェブカメラ：カメラが戻った")
                self?.lock.lock(); self?.forceKey = true; self?.lock.unlock() // 戻ったあとの最初のコマはキーフレームにする
                self?.sendEvent(2, 0)
            }
        })
        sessionObservers.append(nc.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: s, queue: nil) { [weak self, weak s] n in
            let e = n.userInfo?[AVCaptureSessionErrorKey] as? NSError
            self?.q.async {
                self?.log("ウェブカメラ：カメラの実行中の失敗 \(String(describing: e))")
                // メディアサービスが落ちたときなどは、もう一度走らせれば戻る
                if e?.code == AVError.mediaServicesWereReset.rawValue, let s { self?.captureQueue.async { s.startRunning() } }
            }
        })
    }

    // ---- カメラの糸：1 コマ来るたびに、エンコーダへ ----

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        lock.lock()
        let busy = inflight >= maxInflight
        let live = sendConn != nil
        if busy && live { dropped += 1; forceKey = true } // 捨てたあとの次のコマは、参照が切れているので、キーフレームにする
        let key = forceKey && !busy
        if key { forceKey = false }
        let n = seq
        if !busy && live { seq &+= 1 }
        lock.unlock()
        guard live, !busy, let enc = encoderForCallback(), let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let capUs = UInt64(max(0, CMTimeGetSeconds(pts)) * 1_000_000) // ホストの時計（単調）。PING の PONG と同じ時計
        let props = key ? ([kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary) : nil
        var info = VTEncodeInfoFlags()
        let st = VTCompressionSessionEncodeFrame(enc, imageBuffer: pb, presentationTimeStamp: pts, duration: CMTime(value: 1, timescale: CMTimeScale(curFpsForCallback())), frameProperties: props, infoFlagsOut: &info) { [weak self] status, flags, sb in
            self?.encoded(status, flags, sb, capUs, n)
        }
        if st != noErr { lock.lock(); forceKey = true; lock.unlock() }
    }

    // エンコーダと設定の値を、q の外（カメラの糸）から読む。q を待たない（待つと遅れる）ので、壊れても困らない値だけを lock の中の写しから読む
    private var encCopy: VTCompressionSession?
    private var fpsCopy = 30, wCopy = 0, hCopy = 0
    private func encoderForCallback() -> VTCompressionSession? { lock.lock(); defer { lock.unlock() }; return encCopy }
    private func curFpsForCallback() -> Int { lock.lock(); defer { lock.unlock() }; return fpsCopy }

    // ---- エンコーダの糸：できた 1 コマを Annex B にして送る ----

    private func encoded(_ status: OSStatus, _ info: VTEncodeInfoFlags, _ sb: CMSampleBuffer?, _ capUs: UInt64, _ seq: UInt32) {
        guard status == noErr, let sb, !info.contains(.frameDropped), let bb = CMSampleBufferGetDataBuffer(sb) else {
            lock.lock(); forceKey = true; lock.unlock() // 落とされた。次をキーフレームにする
            return
        }
        var key = true
        if let arr = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]], let a = arr.first, let notSync = a[kCMSampleAttachmentKey_NotSync] as? Bool, notSync {
            key = false
        }
        let sc: [UInt8] = [0, 0, 0, 1]
        var out = Data()
        out.reserveCapacity(40 + CMBlockBufferGetDataLength(bb) + 128)
        out.append(contentsOf: [UInt8](repeating: 0, count: 40)) // 頭（メッセージ 8 バイト＋コマ 32 バイト）は最後に埋める
        var nalLen = 4
        if let fd = CMSampleBufferGetFormatDescription(sb) {
            var count = 0
            var nl: Int32 = 4
            if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: &nl) == noErr {
                nalLen = Int(nl)
                if key {
                    for i in 0..<count { // SPS と PPS
                        var p: UnsafePointer<UInt8>?
                        var sz = 0
                        if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &p, parameterSetSizeOut: &sz, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr, let p {
                            out.append(contentsOf: sc)
                            out.append(p, count: sz)
                        }
                    }
                }
            }
        }
        let total = CMBlockBufferGetDataLength(bb)
        var raw = [UInt8](repeating: 0, count: total)
        guard total > 0, nalLen >= 1, nalLen <= 4, CMBlockBufferCopyDataBytes(bb, atOffset: 0, dataLength: total, destination: &raw) == kCMBlockBufferNoErr else {
            lock.lock(); forceKey = true; lock.unlock()
            return
        }
        var at = 0
        while at + nalLen <= total { // AVCC（長さ＋NAL）を Annex B（00 00 00 01 ＋ NAL）に直す
            var l = 0
            for k in 0..<nalLen { l = (l << 8) | Int(raw[at + k]) }
            at += nalLen
            if l > total - at { break }
            out.append(contentsOf: sc)
            out.append(contentsOf: raw[at..<(at + l)])
            at += l
        }
        // 頭：メッセージ（型・長さ）＋コマの頭（撮った時刻・送る時刻・番号・印・幅・高さ）
        let nowUs = Self.nowUs()
        lock.lock(); let conn = sendConn; let cw = wCopy, ch = hCopy; lock.unlock()
        guard let conn else { return }
        Self.putLE32(&out, 0, Msg.frame)
        Self.putLE32(&out, 4, UInt32(out.count - 8))
        Self.putLE64(&out, 8, capUs)
        Self.putLE64(&out, 16, nowUs)
        Self.putLE32(&out, 24, seq)
        Self.putLE32(&out, 28, key ? 1 : 0)
        Self.putLE32(&out, 32, UInt32(cw))
        Self.putLE32(&out, 36, UInt32(ch))
        lock.lock(); inflight += 1; lock.unlock()
        conn.send(content: out, completion: .contentProcessed { [weak self] err in
            guard let self else { return }
            self.lock.lock()
            self.inflight = max(0, self.inflight - 1)
            if err == nil { self.sent &+= 1 } else { self.forceKey = true }
            self.lock.unlock()
        })
    }

    // ---- 送る ----

    private func sendMsg(_ type: UInt32, _ body: Data) {
        guard let c = conn else { return }
        var d = Data()
        Self.appendLE32(&d, type)
        Self.appendLE32(&d, UInt32(body.count))
        d.append(body)
        c.send(content: d, completion: .contentProcessed { _ in })
    }

    private func sendAck(status: Int32, w: Int, h: Int, fps: Int, kbps: Int, camera: UInt32) {
        var d = Data()
        Self.appendLE32(&d, Self.version)
        Self.appendLE32(&d, UInt32(bitPattern: status))
        Self.appendLE32(&d, UInt32(w)); Self.appendLE32(&d, UInt32(h)); Self.appendLE32(&d, UInt32(fps)); Self.appendLE32(&d, UInt32(kbps))
        Self.appendLE32(&d, camera)
        Self.appendLE32(&d, 0)
        sendMsg(Msg.ack, d)
    }

    private func sendEvent(_ code: UInt32, _ arg: UInt32) {
        var d = Data()
        Self.appendLE32(&d, code)
        Self.appendLE32(&d, arg)
        sendMsg(Msg.event, d)
    }

    // iPhone の単調な時計（µs）。カメラの絵の時刻（ホストの時計）と同じ物
    private static func nowUs() -> UInt64 { UInt64(max(0, CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock()))) * 1_000_000) }

    private static func appendLE32(_ d: inout Data, _ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    private static func appendLE64(_ d: inout Data, _ v: UInt64) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    private static func putLE32(_ d: inout Data, _ at: Int, _ v: UInt32) { for i in 0..<4 { d[at + i] = UInt8((v >> (8 * UInt32(i))) & 0xff) } }
    private static func putLE64(_ d: inout Data, _ at: Int, _ v: UInt64) { for i in 0..<8 { d[at + i] = UInt8((v >> (8 * UInt64(i))) & 0xff) } }
}

// 「状態」に出す 1 行。つながっているか・毎秒のコマ数・形
struct CameraBridgeStatusRow: View {
    @ObservedObject var model: CameraBridge

    var body: some View {
        LabeledContent(L("ウェブカメラ", "Webcam"), value: text)
    }

    private var text: String {
        guard model.connected else { return L("PC の接続待ち", "Waiting for the PC") }
        return L("毎秒 \(String(format: "%.1f", model.fps)) コマ", "\(String(format: "%.1f", model.fps)) fps") + "・" + model.detail
    }
}
