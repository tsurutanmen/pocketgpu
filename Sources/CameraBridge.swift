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
//
// camset で足した所：
//   アプリの画面の設定（カメラ・解像度・フレームレート・画質）。PC の HELLO が 0（おまかせ）の項目はこの設定を使う。
//   PC が値を決めた項目は PC を使う。ただし画面の「PC からの指定を優先する」を切ると、画面の設定を使う。
//   つないだまま画面で設定を変えたら、その場でカメラとエンコーダを作り直し、ACK（flags に CHANGED）を送ってキーフレームから流す。
//   ただし PC が WC_HELLO_LIVE を立てていない（古い webcam_rx。途中で大きさが変わると壊れる）ときは、大きさかコマ数が変わる変更は次の接続まで待つ。
//   PC は同じつなぎのまま HELLO を送り直してよい（仮想カメラを開いたアプリが形を選んだとき）。
// iOS 15 でも動く書き方にしてある（あとで作るカメラとマイクだけの軽い版で使うため）。iOS 17 からの API は #available で分けた。
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
    // HELLO と ACK の flags（pc/wcam.h と同じ）
    private enum Flag {
        static let mirror: UInt32 = 1, cameraSet: UInt32 = 2, live: UInt32 = 4
        static let ackAppSettings: UInt32 = 1, ackAppPriority: UInt32 = 2, ackLive: UInt32 = 4, ackChanged: UInt32 = 8
    }

    // アプリの画面の設定（UserDefaults の鍵）
    enum Key {
        static let camera = "webcamCamera"     // 0 背面の広角・1 前面・2 背面の超広角・3 背面の望遠
        static let height = "webcamHeight"     // 720・1080・2160（幅は 16:9）
        static let fps = "webcamFps"           // 30・60
        static let quality = "webcamQuality"   // 1 標準・2 高画質・3 最高
        static let preferPC = "webcamPreferPC" // PC が値を決めた項目は PC を使う
    }
    // 初めの値（画面の @AppStorage の初めの値と同じにしておく）
    static let defaultCamera = 0, defaultHeight = 1080, defaultFps = 30, defaultQuality = 2, defaultPreferPC = true

    // 画面に見せる（画面の糸だけで触る）
    @Published private(set) var connected = false
    @Published private(set) var fps = 0.0
    @Published private(set) var detail = ""      // 例：1920×1080・30 fps・背面カメラ
    @Published private(set) var pcNote = ""      // PC が指定した項目。画面の設定より優先しているとき
    @Published private(set) var pendingNote = false // 画面の変更が次の接続まで待ちになっている
    // Wi-Fi でつなぐときの合言葉（起動のたびに変わる 6 けた）。PC は webcam_rx --addr iPhoneの住所:50064 --code これ
    let wifiCode = UInt32.random(in: 100_000...999_999)
    static let wifiKey = "webcamWifi"

    private var on = false
    private var lastTick = Date()
    private var shownCount: UInt64 = 0
    private var lastShown: UInt64 = 0

    // PC の頼み（HELLO の中身）
    private struct Hello {
        var version: UInt32, width: Int, height: Int, fps: Int, kbps: Int, gop: Int, camera: UInt32, flags: UInt32, quality: Int
    }
    // 頼みと画面の設定を混ぜて決めた形（実際に撮る大きさは pickFormat で決まる）
    private struct Resolved: Equatable {
        var camera: UInt32, w: Int, h: Int, fps: Int, quality: Int, kbps: Int, gop: Int, mirror: Bool, ackFlags: UInt32
        var pcSize: Bool, pcFps: Bool, pcCam: Bool
    }

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
    private var lastHello: Hello?         // いまのつなぎで最後に受けた HELLO。画面の設定が変わったら、これと混ぜて作り直す
    private var curReq: Resolved?          // いま流している形の元（nil は流していない）
    private var settingsGen = 0
    private var observers: [NSObjectProtocol] = []

    // カメラの糸・エンコーダの糸からも触る物（lock で守る）
    private let lock = NSLock()
    private var sendConn: NWConnection? // 送り先。つなぎが無い・止めているときは nil
    private var forceKey = true
    private var inflight = 0            // 送り始めて、まだ iOS が送り終えていないコマの数
    private var seq: UInt32 = 0
    private var sent: UInt64 = 0
    private var dropped: UInt64 = 0
    private var sessionGen: UInt64 = 0  // カメラを作り直すたびに増やす。前のカメラのコマを、新しい形の後ろに送らないため
    private let maxInflight = 4         // これより溜まったら、カメラの絵を捨てて追いつく（USB が詰まったとき、古いコマを送り続けない）

    private let captureQueue = DispatchQueue(label: "camera-capture", qos: .userInteractive)

    private override init() {
        super.init()
        UserDefaults.standard.register(defaults: [Key.camera: Self.defaultCamera, Key.height: Self.defaultHeight, Key.fps: Self.defaultFps,
                                                  Key.quality: Self.defaultQuality, Key.preferPC: Self.defaultPreferPC])
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
            pcNote = ""
            pendingNote = false
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

    // 画面でカメラの設定を変えた。つないでいれば、その場で切り替える。続けて変えたときは、最後の 1 回だけ作り直す
    func settingsChanged() {
        q.async { [weak self] in
            guard let self else { return }
            self.settingsGen += 1
            let g = self.settingsGen
            self.q.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                guard let self, g == self.settingsGen else { return }
                self.applySettings()
            }
        }
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
        lastHello = nil
        stopCapture()
        c.cancel()
        log("ウェブカメラ：切れた")
        DispatchQueue.main.async { [weak self] in
            self?.connected = false
            self?.detail = ""
            self?.pcNote = ""
            self?.pendingNote = false
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
                    sendAck(status: 6, w: 0, h: 0, fps: 0, kbps: 0, camera: 0, flags: 0)
                    q.asyncAfter(deadline: .now() + 0.2) { [weak self] in if let self, c === self.conn { self.drop(c) } }
                    if wifiMisses >= 5 { closeWifi() }
                    return
                }
                wifiMisses = 0
            }
            let f = (0..<8).map { Self.u32(body, $0 * 4) } // version, width, height, fps, kbps, gop, camera, flags
            // 36 バイト目からは画質の段（camset の PC だけが送る。古い PC は 32 か 36 バイト）
            let quality = body.count >= 40 ? Int(Self.u32(body, 36)) : 0
            let hi = Hello(version: f[0], width: Int(f[1]), height: Int(f[2]), fps: Int(f[3]), kbps: Int(f[4]), gop: Int(f[5]), camera: f[6], flags: f[7], quality: quality)
            lastHello = hi
            startSession(c, hi, changed: curReq != nil)
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

    // ---- 頼みと画面の設定を混ぜる ----

    private func resolve(_ hi: Hello) -> Resolved? {
        let d = UserDefaults.standard
        let preferPC = d.bool(forKey: Key.preferPC)
        let appCam = UInt32(max(0, min(3, d.integer(forKey: Key.camera))))
        let (appW, appH) = Self.size(forHeight: d.integer(forKey: Key.height))
        let appFps = d.integer(forKey: Key.fps) > 0 ? d.integer(forKey: Key.fps) : Self.defaultFps
        let appQ = max(1, min(3, d.integer(forKey: Key.quality)))
        let pcSize = hi.width > 0 && hi.height > 0, pcFps = hi.fps > 0
        let pcCam = hi.camera != 0 || hi.flags & Flag.cameraSet != 0
        let pcRate = hi.kbps > 0 || hi.quality > 0
        var flags: UInt32 = 0
        if !(pcSize && pcFps && pcCam && pcRate) { flags |= Flag.ackAppSettings }
        if !preferPC && (pcSize || pcFps || pcCam || pcRate) { flags |= Flag.ackAppPriority | Flag.ackAppSettings }
        let useSize = pcSize && preferPC, useFps = pcFps && preferPC, useCam = pcCam && preferPC, useRate = pcRate && preferPC
        // 無いカメラを頼まれたら：画面の設定のカメラ → 背面の広角 → 前面の順に代える（ACK の camera で、実際に使ったカメラを知らせる）
        var cam = useCam ? min(hi.camera, 3) : appCam
        for alt in [cam, appCam, 0, 1] where Self.device(alt) != nil { cam = alt; break }
        guard Self.device(cam) != nil else { return nil }
        return Resolved(camera: cam, w: useSize ? hi.width : appW, h: useSize ? hi.height : appH, fps: useFps ? hi.fps : appFps,
                        quality: useRate && hi.quality > 0 ? min(hi.quality, 3) : appQ, kbps: useRate ? hi.kbps : 0, gop: hi.gop,
                        mirror: hi.flags & Flag.mirror != 0, ackFlags: flags, pcSize: useSize, pcFps: useFps, pcCam: useCam)
    }

    // 画面で設定が変わった：同じつなぎのまま作り直す（できるとき）
    private func applySettings() {
        guard let c = conn, let hi = lastHello, let cur = curReq, let r = resolve(hi) else { return } // つないでいなければ、次の HELLO のときに使う
        if r == cur { return }
        // 形が同じで画質だけ違う：作り直さずにビットレートだけ変える（どの PC でもよい）
        var onlyRate = r
        onlyRate.quality = cur.quality
        onlyRate.kbps = cur.kbps
        if onlyRate == cur {
            let k = r.kbps > 0 ? r.kbps : Self.qualityKbps(r.quality, w: curW, h: curH, fps: curFps)
            setBitrate(k)
            curReq = r
            return
        }
        // 大きさとコマ数が今と同じになるなら、古い PC でも作り直してよい（絵の大きさが変わらない）
        var sameSize = false
        if let dev = Self.device(r.camera), let p = Self.pickFormat(dev, w: r.w, h: r.h, fps: r.fps) {
            let d = CMVideoFormatDescriptionGetDimensions(p.0.formatDescription)
            sameSize = Int(d.width) == curW && Int(d.height) == curH && p.2 == curFps
        }
        if hi.flags & Flag.live != 0 || sameSize {
            log("ウェブカメラ：画面で設定が変わったので、カメラとエンコーダを作り直す")
            startSession(c, hi, changed: true)
        } else {
            log("ウェブカメラ：画面で設定が変わった。PC が途中の切り替えに対応しないので、次の接続から使う")
            DispatchQueue.main.async { [weak self] in self?.pendingNote = true }
        }
    }

    // ---- 始める：権限 → カメラ → エンコーダ → ACK → 流す ----

    private func startSession(_ c: NWConnection, _ hi: Hello, changed: Bool) {
        func refuse(_ status: Int32, _ why: String) {
            log("ウェブカメラ：断った（\(status)・\(why)）")
            sendAck(status: status, w: 0, h: 0, fps: 0, kbps: 0, camera: hi.camera, flags: Flag.ackLive)
            // ACK が出てから切る
            q.asyncAfter(deadline: .now() + 0.2) { [weak self] in if let self, c === self.conn { self.drop(c) } }
        }
        stopCapture() // 2 回目の HELLO（形を変えて頼み直す）・画面の設定の変更のときは、前のカメラを止めてから
        let sizeOK = (hi.width == 0 && hi.height == 0) || (hi.width >= 64 && hi.height >= 64 && hi.width <= 4096 && hi.height <= 3072)
        guard hi.version == Self.version, sizeOK, hi.fps >= 0, hi.fps <= 120 else { refuse(5, "頼みが変"); return }
        guard appActive else { refuse(3, "アプリが前にいない（iOS はカメラを後ろで使わせない）"); return }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: break
        case .notDetermined:
            // 許可の画面を出す。答えが来たらもう一度ここへ（HELLO をそのまま使う）
            AVCaptureDevice.requestAccess(for: .video) { [weak self] ok in
                self?.q.async {
                    guard let self, c === self.conn else { return }
                    if ok { self.startSession(c, hi, changed: changed) } else { refuse(1, "カメラの許可が無い") }
                }
            }
            return
        default: refuse(1, "カメラの許可が無い"); return
        }
        guard let r = resolve(hi), let dev = Self.device(r.camera) else { refuse(2, "使えるカメラが無い"); return }
        guard let picked = Self.pickFormat(dev, w: r.w, h: r.h, fps: r.fps) else { refuse(2, "そのカメラに使える形が無い"); return }
        let (fmt, frameDur, fr) = picked
        let dims = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription)
        let aw = Int(dims.width), ah = Int(dims.height)
        let kb = r.kbps > 0 ? r.kbps : Self.qualityKbps(r.quality, w: aw, h: ah, fps: fr)
        let g = r.gop > 0 ? r.gop : fr * 2
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
            dev.activeFormat = fmt
            // frameDur は pickFormat が fmt の videoSupportedFrameRateRanges の中から選んだ物（範囲の外を入れると例外で落ちる）
            dev.activeVideoMinFrameDuration = frameDur
            dev.activeVideoMaxFrameDuration = frameDur
            // HDR の映像にしない（形式が変わり、色が合わなくなる）
            if fmt.isVideoHDRSupported { dev.automaticallyAdjustsVideoHDREnabled = false; dev.isVideoHDREnabled = false }
            dev.unlockForConfiguration()
            let out = AVCaptureVideoDataOutput()
            out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            out.alwaysDiscardsLateVideoFrames = true // 遅れたコマは溜めずに捨てる
            out.setSampleBufferDelegate(self, queue: captureQueue)
            guard s.canAddOutput(out) else { throw NSError(domain: "CameraBridge", code: 2) }
            s.addOutput(out)
            if let cn = out.connection(with: .video) {
                // 向きは iPhone の持ち方に関わらず、横長（センサーの向き）で固定する。ウェブカメラの絵は 16:9 の横長。
                // iOS 17 からは回す角度 0 を明示する。それより前は、AVCaptureVideoDataOutput の既定（センサーの向きのまま）に任せる
                if #available(iOS 17.0, *) {
                    if cn.isVideoRotationAngleSupported(0) { cn.videoRotationAngle = 0 }
                }
                if cn.isVideoMirroringSupported {
                    cn.automaticallyAdjustsVideoMirroring = false
                    cn.isVideoMirrored = r.mirror // ふつうは反転しない。鏡のように見せるのは使う側のアプリ
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
        curReq = r
        lock.lock(); sendConn = c; encCopy = enc; fpsCopy = fr; wCopy = aw; hCopy = ah; forceKey = true; inflight = 0; if !changed { seq = 0; sent = 0; dropped = 0 }; lock.unlock()
        observeSession(s)
        let ackFlags = r.ackFlags | Flag.ackLive | (changed ? Flag.ackChanged : 0)
        sendAck(status: 0, w: aw, h: ah, fps: fr, kbps: kb, camera: r.camera, flags: ackFlags)
        log("ウェブカメラ：\(changed ? "切り替えた" : "始める") \(aw)×\(ah)・\(fr) コマ/秒・\(kb) kbps・カメラ \(r.camera)（\(dev.localizedName)）・ACK flags \(ackFlags)")
        // startRunning は返ってくるまでに時間がかかるので、カメラの糸で（前のカメラの stopRunning のあとに並ぶ）
        captureQueue.async { s.startRunning() }
        let name = Self.cameraName(r.camera)
        let note = Self.pcNoteText(r, w: aw, h: ah, fps: fr)
        DispatchQueue.main.async { [weak self] in
            self?.connected = true
            self?.detail = "\(aw)×\(ah)・\(fr) fps・\(name)・\(String(format: "%.0f", Double(kb) / 1000)) Mbps"
            self?.pcNote = note
            self?.pendingNote = false
        }
    }

    // ---- カメラと形 ----

    // カメラ（0 背面の広角・1 前面・2 背面の超広角・3 背面の望遠）。無い機種では入れない
    private static let deviceTable: [UInt32: AVCaptureDevice] = {
        func find(_ type: AVCaptureDevice.DeviceType, _ pos: AVCaptureDevice.Position) -> AVCaptureDevice? {
            AVCaptureDevice.DiscoverySession(deviceTypes: [type], mediaType: .video, position: pos).devices.first
        }
        var t: [UInt32: AVCaptureDevice] = [:]
        t[0] = find(.builtInWideAngleCamera, .back)
        t[1] = find(.builtInWideAngleCamera, .front)
        t[2] = find(.builtInUltraWideCamera, .back)
        t[3] = find(.builtInTelephotoCamera, .back)
        return t
    }()
    static func device(_ id: UInt32) -> AVCaptureDevice? { deviceTable[id] }
    static func cameraName(_ id: UInt32) -> String {
        switch id {
        case 1: return L("前面カメラ", "Front camera")
        case 2: return L("超広角", "Ultra wide")
        case 3: return L("望遠", "Telephoto")
        default: return L("背面カメラ", "Back camera")
        }
    }
    static func size(forHeight h: Int) -> (Int, Int) {
        switch h {
        case 720: return (1280, 720)
        case 2160: return (3840, 2160)
        default: return (1920, 1080)
        }
    }

    // 画面に出す選び肢。4K は 30 fps まで（Windows の H.264 のデコーダの資料は level 5.1 まで。4K の 60 fps は level 5.2 が要る）
    static let heightChoices = [720, 1080, 2160]
    static let fpsChoices = [30, 60]
    static func availableCameras() -> [UInt32] { [UInt32(0), 1, 2, 3].filter { device($0) != nil } }
    static func heights(for cam: UInt32) -> [Int] { heightChoices.filter { !rates(for: cam, height: $0).isEmpty } }
    static func rates(for cam: UInt32, height: Int) -> [Int] {
        fpsChoices.filter { f in !(height >= 2160 && f > 30) && supports(camera: cam, height: height, fps: f) }
    }
    // そのカメラが、その大きさ（16:9）とコマ数を NV12（420v）で撮れるか
    static func supports(camera: UInt32, height: Int, fps: Int) -> Bool {
        guard let dev = device(camera) else { return false }
        let (w, h) = size(forHeight: height)
        let want = Double(fps)
        return dev.formats.contains { f in
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return CMFormatDescriptionGetMediaSubType(f.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                && Int(d.width) == w && Int(d.height) == h
                && f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= want && want <= $0.maxFrameRate }
        }
    }

    // 撮る形を選ぶ。返すのは形・1 コマの長さ・実際のコマ数。
    //   1. 大きさがぴったりで、そのコマ数を撮れる形。同じ大きさが何種類かあれば、いちばん広く写る物（videoFieldOfView が大きい）
    //   2. そのコマ数を撮れる中で、面積がいちばん近い形
    //   3. どの形もそのコマ数を撮れない：大きさがぴったりの形（無ければ面積がいちばん近い形）を、その形の最高のコマ数で
    // activeVideoMinFrameDuration に範囲の外の値を入れると例外で落ちるので、1 コマの長さは必ず範囲の中の値にする
    private static func pickFormat(_ dev: AVCaptureDevice, w: Int, h: Int, fps: Int) -> (AVCaptureDevice.Format, CMTime, Int)? {
        let nv = dev.formats.filter { CMFormatDescriptionGetMediaSubType($0.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange }
        let want = Double(fps)
        func area(_ f: AVCaptureDevice.Format) -> Int {
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return Int(d.width) * Int(d.height)
        }
        func exact(_ f: AVCaptureDevice.Format) -> Bool {
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return Int(d.width) == w && Int(d.height) == h
        }
        func canDo(_ f: AVCaptureDevice.Format) -> Bool { fps > 0 && f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= want && want <= $0.maxFrameRate } }
        func widest(_ list: [AVCaptureDevice.Format]) -> AVCaptureDevice.Format? { list.max { $0.videoFieldOfView < $1.videoFieldOfView } }
        func nearest(_ list: [AVCaptureDevice.Format]) -> AVCaptureDevice.Format? { list.min { abs(area($0) - w * h) < abs(area($1) - w * h) } }
        if let f = widest(nv.filter { exact($0) && canDo($0) }) ?? nearest(nv.filter(canDo)) {
            return (f, CMTime(value: 1, timescale: CMTimeScale(fps)), fps)
        }
        guard let f = widest(nv.filter(exact)) ?? nearest(nv),
              let r = f.videoSupportedFrameRateRanges.max(by: { $0.maxFrameRate < $1.maxFrameRate }) else { return nil }
        return (f, r.minFrameDuration, max(1, Int(r.maxFrameRate.rounded(.down))))
    }

    // 画質の段とビットレート（kbps）。表は [段][720p・1080p・4K][30・60 fps]
    //   最高は USB（毎秒 50 MB 以上）の太さを生かす値。1080p60 で 36 Mbps（H.264 の level 4.2 の上限は High で 62.5 Mbps）、
    //   4K30 で 50 Mbps（level 5.1 の上限は 300 Mbps）。PC のほどく側（Windows の H.264 デコーダ）は level 5.1 までと資料にある。
    //   PC の work/camset/pc/fake_iphone.c の quality_kbps と同じ表
    static func qualityKbps(_ q: Int, w: Int, h: Int, fps: Int) -> Int {
        let table: [[[Int]]] = [[[4000, 6000], [8000, 12000], [20000, 30000]],
                                [[6000, 9000], [12000, 18000], [30000, 45000]],
                                [[12000, 18000], [24000, 36000], [50000, 60000]]]
        let tier = max(1, min(3, q)) - 1
        let size = w * h >= 3840 * 2160 ? 2 : w * h >= 1920 * 1080 ? 1 : 0
        return table[tier][size][fps >= 50 ? 1 : 0]
    }

    // PC が指定した項目を画面に出す文。画面の設定を優先しているときもそう書く
    private static func pcNoteText(_ r: Resolved, w: Int, h: Int, fps: Int) -> String {
        if r.ackFlags & Flag.ackAppPriority != 0 { return L("PC からの指定ではなく、この画面の設定を使っています。", "Using the settings on this screen, not the ones chosen on the PC.") }
        var items: [String] = []
        if r.pcSize { items.append("\(r.w)×\(r.h)") }
        if r.pcFps { items.append("\(r.fps) fps") }
        if r.pcCam { items.append(cameraName(r.camera)) }
        if items.isEmpty { return "" }
        let s = items.joined(separator: L("・", ", "))
        return L("PC が \(s) を指定しています。この画面の設定より優先されます。", "The PC asked for \(s). This is used instead of the settings on this screen.")
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
        setPeak(s, kbps)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: gop))
        VTCompressionSessionPrepareToEncodeFrames(s)
        return s
    }

    // 1 秒あたりの上限を平均の 1.5 倍に（動きの多い場面で USB が詰まらないように）。エンコーダが受けなければ、そのまま（返り値は見ない）
    private func setPeak(_ s: VTCompressionSession, _ kbps: Int) {
        let bytesPerSec = kbps * 1000 / 8 * 3 / 2
        _ = VTSessionSetProperty(s, key: kVTCompressionPropertyKey_DataRateLimits, value: [NSNumber(value: bytesPerSec), NSNumber(value: 1)] as CFArray)
    }

    private func setBitrate(_ kbps: Int) {
        guard kbps >= 200, kbps <= 100_000, let s = venc else { return }
        _ = VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: kbps * 1000))
        setPeak(s, kbps)
        curKbps = kbps
        log("ウェブカメラ：ビットレートを \(kbps) kbps に変えた")
        let w = curW, h = curH, f = curFps
        let name = Self.cameraName(curReq?.camera ?? 0)
        DispatchQueue.main.async { [weak self] in
            if self?.connected == true { self?.detail = "\(w)×\(h)・\(f) fps・\(name)・\(String(format: "%.0f", Double(kbps) / 1000)) Mbps" }
        }
    }

    // カメラとエンコーダを止める（つなぎ自体は切らない）
    private func stopCapture() {
        lock.lock(); sendConn = nil; encCopy = nil; sessionGen &+= 1; lock.unlock()
        curReq = nil
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
        let gen = sessionGen
        lock.unlock()
        guard live, !busy, let enc = encoderForCallback(), let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let capUs = UInt64(max(0, CMTimeGetSeconds(pts)) * 1_000_000) // ホストの時計（単調）。PING の PONG と同じ時計
        let props = key ? ([kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary) : nil
        var info = VTEncodeInfoFlags()
        let st = VTCompressionSessionEncodeFrame(enc, imageBuffer: pb, presentationTimeStamp: pts, duration: CMTime(value: 1, timescale: CMTimeScale(curFpsForCallback())), frameProperties: props, infoFlagsOut: &info) { [weak self] status, flags, sb in
            self?.encoded(status, flags, sb, capUs, n, gen)
        }
        if st != noErr { lock.lock(); forceKey = true; lock.unlock() }
    }

    // エンコーダと設定の値を、q の外（カメラの糸）から読む。q を待たない（待つと遅れる）ので、壊れても困らない値だけを lock の中の写しから読む
    private var encCopy: VTCompressionSession?
    private var fpsCopy = 30, wCopy = 0, hCopy = 0
    private func encoderForCallback() -> VTCompressionSession? { lock.lock(); defer { lock.unlock() }; return encCopy }
    private func curFpsForCallback() -> Int { lock.lock(); defer { lock.unlock() }; return fpsCopy }

    // ---- エンコーダの糸：できた 1 コマを Annex B にして送る ----

    private func encoded(_ status: OSStatus, _ info: VTEncodeInfoFlags, _ sb: CMSampleBuffer?, _ capUs: UInt64, _ seq: UInt32, _ gen: UInt64) {
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
        // カメラを作り直したあとに、前のカメラのコマを送らない（新しい形の ACK の後ろに、古い大きさのコマが並ばないように）
        lock.lock(); let conn = sendConn; let cw = wCopy, ch = hCopy; let same = gen == sessionGen
        if conn != nil && same { inflight += 1 }
        lock.unlock()
        guard let conn, same else { return }
        Self.putLE32(&out, 0, Msg.frame)
        Self.putLE32(&out, 4, UInt32(out.count - 8))
        Self.putLE64(&out, 8, capUs)
        Self.putLE64(&out, 16, nowUs)
        Self.putLE32(&out, 24, seq)
        Self.putLE32(&out, 28, key ? 1 : 0)
        Self.putLE32(&out, 32, UInt32(cw))
        Self.putLE32(&out, 36, UInt32(ch))
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

    private func sendAck(status: Int32, w: Int, h: Int, fps: Int, kbps: Int, camera: UInt32, flags: UInt32) {
        var d = Data()
        Self.appendLE32(&d, Self.version)
        Self.appendLE32(&d, UInt32(bitPattern: status))
        Self.appendLE32(&d, UInt32(w)); Self.appendLE32(&d, UInt32(h)); Self.appendLE32(&d, UInt32(fps)); Self.appendLE32(&d, UInt32(kbps))
        Self.appendLE32(&d, camera)
        Self.appendLE32(&d, flags)
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

// 「状態」に出す 1 行。接続中か・毎秒のコマ数・形。iOS 15 でも使えるように LabeledContent は使わない
struct CameraBridgeStatusRow: View {
    @ObservedObject var model: CameraBridge

    var body: some View {
        HStack {
            Text(L("ウェブカメラ", "Webcam"))
            Spacer()
            Text(text).foregroundColor(.secondary).multilineTextAlignment(.trailing)
        }
        if !model.pcNote.isEmpty {
            Text(model.pcNote).font(.footnote)
        }
        if model.pendingNote {
            Text(L("変更した設定は、PC を接続し直すと反映されます。", "The new settings apply when the PC reconnects.")).font(.footnote)
        }
    }

    private var text: String {
        guard model.connected else { return L("PC の接続待ち", "Waiting for the PC") }
        return String(format: "%.1f fps", model.fps) + "・" + model.detail
    }
}

// 「ウェブカメラとして使う」の下に出す、カメラの設定。PC が「おまかせ」にした項目はここの設定を使う。
// その iPhone に無いカメラ、その組み合わせで撮れない解像度とフレームレートは出さない。変えたら、接続中ならその場で切り替える
// iOS 15 でも使えるように、onChange（iOS 17 の形）を使わず、選んだときに Binding の set で知らせる
struct CameraSettingsView: View {
    @ObservedObject var model: CameraBridge
    @AppStorage(CameraBridge.Key.camera) private var camera = CameraBridge.defaultCamera
    @AppStorage(CameraBridge.Key.height) private var height = CameraBridge.defaultHeight
    @AppStorage(CameraBridge.Key.fps) private var fps = CameraBridge.defaultFps
    @AppStorage(CameraBridge.Key.quality) private var quality = CameraBridge.defaultQuality
    @AppStorage(CameraBridge.Key.preferPC) private var preferPC = CameraBridge.defaultPreferPC

    var body: some View {
        let cams = CameraBridge.availableCameras()
        if cams.isEmpty {
            Text(L("この iPhone ではカメラが見つかりません。", "No camera was found on this iPhone.")).font(.footnote)
        } else {
            Picker(L("カメラ", "Camera"), selection: bind($camera)) {
                ForEach(cams, id: \.self) { c in Text(CameraBridge.cameraName(c)).tag(Int(c)) }
            }
            .pickerStyle(.menu)
            Picker(L("解像度", "Resolution"), selection: bind($height)) {
                ForEach(CameraBridge.heights(for: UInt32(clamping: camera)), id: \.self) { h in Text(Self.heightName(h)).tag(h) }
            }
            .pickerStyle(.menu)
            Picker(L("フレームレート", "Frame rate"), selection: bind($fps)) {
                ForEach(CameraBridge.rates(for: UInt32(clamping: camera), height: height), id: \.self) { f in Text("\(f) fps").tag(f) }
            }
            .pickerStyle(.menu)
            Picker(L("画質", "Quality"), selection: bind($quality)) {
                Text(L("標準", "Standard")).tag(1)
                Text(L("高画質", "High")).tag(2)
                Text(L("最高", "Maximum")).tag(3)
            }
            .pickerStyle(.menu)
            Text(Self.qualityNote(quality, height: height, fps: fps)).font(.footnote)
            Toggle(L("PC からの指定を優先する", "Use the settings chosen on the PC first"), isOn: bind($preferPC))
            if height >= 2160 {
                Text(L("4K は 30 fps までです。カメラを使うアプリには、PC で 1920×1080 に縮めて渡します。", "4K is up to 30 fps. Apps on the PC get it scaled to 1920×1080.")).font(.footnote)
            }
        }
    }

    // 選んだら：組み合わせを撮れる物に直してから、接続中なら切り替える
    private func bind<T>(_ b: Binding<T>) -> Binding<T> {
        Binding(get: { b.wrappedValue }, set: { v in
            b.wrappedValue = v
            normalize()
            model.settingsChanged()
        })
    }

    private func normalize() {
        let cams = CameraBridge.availableCameras()
        if !cams.contains(UInt32(max(0, camera))), let first = cams.first { camera = Int(first) }
        let hs = CameraBridge.heights(for: UInt32(clamping: camera))
        if !hs.contains(height) { height = hs.last(where: { $0 <= height }) ?? hs.first ?? 1080 }
        let rs = CameraBridge.rates(for: UInt32(clamping: camera), height: height)
        if !rs.contains(fps) { fps = rs.contains(30) ? 30 : (rs.first ?? 30) }
    }

    private static func heightName(_ h: Int) -> String { h >= 2160 ? "4K" : "\(h)p" }

    private static func qualityNote(_ q: Int, height: Int, fps: Int) -> String {
        let (w, h) = CameraBridge.size(forHeight: height)
        let mbps = Double(CameraBridge.qualityKbps(q, w: w, h: h, fps: fps)) / 1000
        let s = String(format: "%.0f", mbps)
        if q >= 3 { return L("約 \(s) Mbps です。USB で接続しているときに向いています。Wi-Fi では映像が止まることがあります。", "About \(s) Mbps. Best over USB. Over Wi-Fi the video may stall.") }
        return L("約 \(s) Mbps です。", "About \(s) Mbps.")
    }
}
