import AVFoundation
import SwiftUI
import os

@main
struct RpcServerApp: App {
    init() {
        // A19 の GPU の行列計算の部品（Metal 4 の tensor）を使わない設定なら、Metal の装置を作る前に llama.cpp に伝える。
        // 使う・使わないで速さを比べるため。装置はここで作られるので、切り替えはアプリを開き直すと効く
        if UserDefaults.standard.object(forKey: "metalTensor") as? Bool == false {
            setenv("GGML_METAL_TENSOR_DISABLE", "1", 1)
        }
        #if MVK_RT
        // 試す用の IPA（レイトレーシングの入った作りかけの MoltenVK・PR #2771）だけ。MoltenVK は設定を最初の Vulkan の呼び出しで
        // 環境変数から読むので、受け手を始める前に立てる。切り替えはアプリを開き直すと効く。
        // 受け手は MoltenVK が拡張を名乗ったときだけ、PC にレイトレーシングを名乗る（記録の「レイトレーシングの土台」の行）
        if UserDefaults.standard.bool(forKey: "rtExperimental") {
            setenv("MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING", "1", 1)
        }
        #endif
        rpc_bridge_init()
        // 窓に出す絵を MetalFX で広げる関数を、受け手に先に渡しておく（PC が頼んだときだけ使う）
        FXUpscaler.register()
        // 録画の書き手を受け手に渡す（GameRecorder.swift）。PC が頼んだときだけ、iPhone が描いたゲームの絵をそのまま録画する
        GameRecorder.register()
        // 記録を PC から読めるようにする（USB の 50059 番。PC は app/pc/iphone_applog.py）
        let t = Thread { rpc_bridge_serve_log(50059) }
        t.start()
        // USB の通り道の速さを測る受け口（50058 番。PC は app/pc/usb_bw.py）
        vkr_bw_set_token(linkToken)
        let b = Thread { _ = vkr_bw_serve("127.0.0.1", 50058) }
        b.start()
        // Apple の機能（文字認識など）を Windows から使う受け口（50057 番。PC は app/pc/apple_ocr.py）
        AppleFeatures.shared.start()
        // 重い圧縮を PC から頼まれる口（Compress.swift。USB の 50065 番。PC は iphone_compress.py）
        CompressService.shared.start()
        // iPhone の写真と動画を PC へ送る口（PhotoTransfer.swift。USB の 50066 番はいつも待ち、Wi-Fi は画面の切り替えのとおり。PC は photo_rx.exe）
        PhotoTransfer.shared.start()
    }
    var body: some Scene { WindowGroup { ContentView() } }
}

// 後ろに回ったとき、PC が GPU につながっていれば、少しの間だけ動き続けさせてもらう（beginBackgroundTask）。
// その間に受け手が PC の次の命令へ「後ろに回った」と答え、PC のゲームが固まらずに終われるようにする。
// PC が切れたら・前に戻ったら・iOS が時間切れを告げたら終える。「バックグラウンドでも実行する」（Keeper）が入りなら、もともと止められない
final class GPUBackgroundGrace {
    static let shared = GPUBackgroundGrace()
    private var task: UIBackgroundTaskIdentifier = .invalid
    private var timer: Timer?

    func enteredBackground() {
        guard task == .invalid, vkr_clients() != 0 else { return }
        task = UIApplication.shared.beginBackgroundTask(withName: "PocketGPU GPU stop") { [weak self] in self?.finish() }
        guard task != .invalid else { return }
        vkr_note("後ろに回った：PC に GPU を止めたことを伝え終わるまで動かしてもらう")
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            if vkr_clients() == 0 { self?.finish() }
        }
    }

    func finish() {
        timer?.invalidate()
        timer = nil
        if task != .invalid {
            UIApplication.shared.endBackgroundTask(task)
            task = .invalid
        }
    }
}

// 後ろに回っても止められないように、音の出ない再生を流し続ける
final class Keeper {
    static let shared = Keeper()
    private var player: AVAudioPlayer?
    private var watching = false

    func start() {
        if player?.isPlaying == true { return }
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, options: [.mixWithOthers])
        try? session.setActive(true)
        if player == nil {
            player = try? AVAudioPlayer(data: Keeper.silentWav())
            player?.numberOfLoops = -1
            player?.volume = 0
        }
        player?.play()
        if watching { return }
        watching = true
        // 電話などで止まったら、終わったときにまた流す
        NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] n in
            let raw = n.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            if raw == AVAudioSession.InterruptionType.ended.rawValue {
                try? AVAudioSession.sharedInstance().setActive(true)
                self?.player?.play()
            }
        }
    }

    func stop() {
        player?.stop()
        try? AVAudioSession.sharedInstance().setActive(false, options: [.notifyOthersOnDeactivation])
    }

    // 1 秒ぶんの無音（16bit・8000Hz・モノラル）の WAV
    private static func silentWav() -> Data {
        let rate: UInt32 = 8000, samples = rate
        var d = Data()
        func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
        d.append(contentsOf: Array("RIFF".utf8)); u32(36 + samples * 2)
        d.append(contentsOf: Array("WAVEfmt ".utf8)); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16)
        d.append(contentsOf: Array("data".utf8)); u32(samples * 2)
        d.append(Data(count: Int(samples * 2)))
        return d
    }
}

struct Addr: Hashable { let name: String; let ip: String }

// インターネット共有の口の合言葉。起動のたびに作り、USB の口で PC のドライバにだけ渡す
let linkToken: String = (0..<16).map { _ in String(format: "%02x", UInt8.random(in: 0...255)) }.joined()

// インターネット共有の網の口（bridge100 の 172.20.10.1 など）を選ぶのは、受け手の C の側（vkr_hotspot_tick）。
// 前は「名前が bridge か、住所が 172.20.10.」で選んでいて、この iPhone がほかの iPhone の共有に Wi-Fi でつないだときの en0 も拾えた

// この iPhone の IPv4 の住所（Wi-Fi は en0、Tailscale は utun で 100.x）
func listAddresses() -> [Addr] {
    var out: [Addr] = []
    var head: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&head) == 0, let first = head else { return out }
    defer { freeifaddrs(head) }
    var p: UnsafeMutablePointer<ifaddrs>? = first
    while let cur = p {
        let ifa = cur.pointee
        if let sa = ifa.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) {
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                let name = String(cString: ifa.ifa_name), ip = String(cString: host)
                if name != "lo0" { out.append(Addr(name: name, ip: ip)) }
            }
        }
        p = ifa.ifa_next
    }
    return out
}

func readBridge(_ f: (UnsafeMutablePointer<CChar>, Int32) -> Int32) -> String {
    var buf = [CChar](repeating: 0, count: 20000)
    buf.withUnsafeMutableBufferPointer { b in _ = f(b.baseAddress!, Int32(b.count)) }
    return String(cString: buf)
}

// 画面は下のタブで 5 つに分ける（案 A に「PC に送る」を足した）：ホーム・PC に送る・カメラ・音と字幕・設定。
// 状態と切り替えの値はここにまとめて持ち、どのタブも同じ値を見る。ホームとタブの両方に同じ切り替えがあるので、
// 切り替えたときの処理（setWebcam など）は切り替えの所ではなく、タブの外側の onChange で 1 回だけ呼ぶ
struct ContentView: View {
    @State private var addrs = listAddresses()
    @State private var host = ""
    @State private var port = "50052"
    @State private var useGPU = true
    @State private var started: Set<String> = [] // 待ち受けを始めた住所
    @State private var log = ""
    @State private var devices = ""
    @State private var availMB = 0
    // 開いたら自動で受け手を始める（Tailscale の住所が見えたとき）
    @AppStorage("autoStart") private var autoStart = true
    // USB の通り道は iPhone の中の 127.0.0.1 に届く（PC 側は usb_forward.py）
    @AppStorage("usb") private var usb = true
    // 後ろに回っても続ける（その間は CPU で計算する）
    @AppStorage("keepAlive") private var keepAlive = true
    @AppStorage("metalTensor") private var metalTensor = true
    // 試す用の IPA だけ：MoltenVK のレイトレーシング（作りかけ）を有効にする。初めは切り。読むのは RpcServerApp.init
    @AppStorage("rtExperimental") private var rtExperimental = false
    // インターネット共有を USB でつないだ有線の網でも Vulkan と速さを測る口を待つ。初めは入り（共有が出ていなければ何も待たない）。合言葉は USB の口で PC に渡す
    @AppStorage("hotspotListen") private var hotspotListen = true
    @State private var onCPU = false
    @State private var vkStarted = false
    @State private var vkFailed = false
    @State private var mcRunning = false
    @State private var linkHost = "" // インターネット共有の口で待っている住所（空なら待っていない）
    @StateObject private var status = DeviceStatus()
    // PC から来た命令で描いた絵を、この iPhone の画面にも出す。出している間だけ受け手が 1 枚写す
    @AppStorage("showFrames") private var showFrames = false
    // 全画面にしたとき、絵を 90 度回して横向きで固定する（FramePreview.swift と同じ鍵）
    @AppStorage("fullLandscape") private var fullLandscape = true
    @StateObject private var preview = FramePreviewModel()
    // PC の画面そのものを、この iPhone に映す（ScreenMirror.swift。PC は app/pc/screen_mirror.py）。オンの間だけ USB の 50062 番で待つ。
    // コマが届くたびに描き直すのは絵の所だけにしたいので、ここでは見張らない
    @AppStorage("screenMirror") private var mirrorOn = false
    private let mirror = ScreenMirrorModel.shared
    // この iPhone を PC のスピーカーとマイクにする（AudioBridge.swift。PC は ipaudio.exe）。オンの間だけ USB の 50063 番で待つ
    @AppStorage("audioBridge") private var audioOn = false
    // iPhone につないだ Bluetooth のイヤホンのマイクを使う（音質が下がる）。AudioBridge が読む
    @AppStorage("audioHeadsetMic") private var audioHeadsetMic = false
    // iPhone のカメラを PC のウェブカメラにする（CameraBridge.swift。PC は webcam_rx.exe）。オンの間だけ USB の 50064 番で待つ
    @AppStorage("webcam") private var webcamOn = false
    // ウェブカメラを Wi-Fi でもつなげる（合言葉つき）。CameraBridge が読む
    @AppStorage(CameraBridge.wifiKey) private var webcamWifi = false
    private let webcam = CameraBridge.shared
    // GPU を使わずに、マイク・スピーカー・カメラだけに使う。オンの間は GPU の受け手（Vulkan の 50060・llama の rpc）を始めない。
    // 「起動時に自動で開始」もしない。マイク・スピーカー（50063）とカメラ（50064）の口は、それぞれの切り替えのとおり。
    // 画面ではホームの「GPU として使う」の逆（GPU として使うを切ると、このモード）
    @AppStorage("micCamOnly") private var micCamOnly = false
    // 画面の言葉（自動・日本語・English）。ここで読むと、変えたときに画面が描き直される
    @AppStorage("lang") private var lang = AppLang.auto.rawValue
    @Environment(\.scenePhase) private var scenePhase
    private let usbHost = "127.0.0.1"
    private let tick = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
    private var running: Bool { !started.isEmpty }

    var body: some View {
        TabView {
            homeTab
                .tabItem { Label(L("ホーム", "Home"), systemImage: "house") }
            // iPhone の写真と動画を PC へ送る（PhotoTransfer.swift）
            PhotoTransferView()
                .tabItem { Label(L("PC に送る", "Send to PC"), systemImage: "photo.on.rectangle.angled") }
            cameraTab
                .tabItem { Label(L("カメラ", "Camera"), systemImage: "camera") }
            audioTab
                .tabItem { Label(L("音と字幕", "Audio and captions"), systemImage: "mic") }
            settingsTab
                .tabItem { Label(L("設定", "Settings"), systemImage: "slider.horizontal.3") }
        }
        // 画面を暗くする（ScreenDimmer.swift）。カメラかマイクが接続中に操作が無いまま決めた時間がたつと、黒で覆って明るさを最小にする。
        // 暗い間は下の画面を描かず、iPhone に出力のプレビューも止める（戻したら元に戻す）
        .modifier(ScreenDimHost(connected: { CameraBridge.shared.connected || AudioBridgeModel.shared.connected },
                                onDim: { on in preview.setOn(on ? false : showFrames) }))
        // スイッチとタブの色は案 A の見本に合わせて青にする
        .tint(.blue)
        .onAppear {
            devices = readBridge { rpc_bridge_devices($0, $1) }
            host = addrs.first(where: { $0.ip.hasPrefix("100.") })?.ip ?? addrs.first(where: { $0.name == "en0" })?.ip ?? addrs.first?.ip ?? ""
            // 前にオンにしたまま閉じたなら、開いたときから出す
            preview.setOn(showFrames)
            setMirror(mirrorOn)
            AudioBridgeModel.shared.setOn(audioOn)
            setWebcam(webcamOn)
        }
        .onChange(of: scenePhase) { _, phase in
            // 後ろに回ると GPU が使えない。前に戻ったら GPU に戻す
            rpc_bridge_set_background(phase == .background ? 1 : 0)
            // Vulkan の受け手は、切り替えの途中（1）は GPU に仕事を出さずに待ち、後ろ（2）に回ったら PC に「GPU を失った」と返して接続を終える。
            // iOS は後ろに回ったアプリの Metal の仕事を捨てるので、待っていても PC のゲームが固まるだけだった
            vkr_set_paused(phase == .active ? 0 : phase == .inactive ? 1 : 2)
            if phase == .background { GPUBackgroundGrace.shared.enteredBackground() }
            if phase == .active {
                GPUBackgroundGrace.shared.finish()
                AudioBridgeModel.shared.foregrounded()
            }
        }
        // 切り替えたときの処理。前は切り替えの所に付けていた物を、ここにまとめた（呼ぶ物は同じ）
        .onChange(of: showFrames) { _, on in preview.setOn(on) }
        .onChange(of: mirrorOn) { _, on in setMirror(on) }
        .onChange(of: audioOn) { _, on in AudioBridgeModel.shared.setOn(on) }
        .onChange(of: webcamOn) { _, on in setWebcam(on) }
        .onChange(of: webcamWifi) { _, on in webcam.setWifi(on) }
        .onChange(of: micCamOnly) { _, on in setMicCamOnly(on) }
        .onChange(of: keepAlive) { _, on in if running { on ? Keeper.shared.start() : Keeper.shared.stop() } }
        // 熱の状態が変わったら、1 秒の tick を待たずにすぐ受け手へ渡す（PC のドライバは約 2 秒ごとに VR_GET_STATUS で聞く。
        // 使えるメモリ os_proc_available_memory は、聞かれたときに受け手（server.c）が自分で測って答える）
        .onReceive(NotificationCenter.default.publisher(for: ProcessInfo.thermalStateDidChangeNotification).receive(on: RunLoop.main)) { _ in
            vkr_set_thermal(Int32(ProcessInfo.processInfo.thermalState.rawValue))
        }
        .onReceive(tick) { _ in
            vkr_set_thermal(Int32(ProcessInfo.processInfo.thermalState.rawValue))
            // アクセスガイド中か（受け手の記録に出す。手動で始めたアクセスガイドで変わるかは、実機で確かめていない）
            vkr_set_guided(UIAccessibility.isGuidedAccessEnabled ? 1 : 0)
            status.tick()
            mirror.tick()
            webcam.tick()
            // 設定が入っていれば、インターネット共有の網の口が出たら、そこでも待つ（消えたら・設定を切ったら閉じる）
            if vkStarted && !vkFailed {
                let cap = 64
                var buf = [CChar](repeating: 0, count: cap)
                // 2 は Vulkan がまだ起きていない。次の 1 秒でもう一度
                if vkr_hotspot_tick(hotspotListen ? 1 : 0, &buf, Int32(cap)) != 2 {
                    let h = String(cString: buf)
                    if h != linkHost { linkHost = h }
                    // 速さを測る口は、開けなかったときのために毎回呼ぶ（同じ住所で開いていれば何もしない）
                    h.withCString { _ = vkr_bw_listen_extra($0, 50058) }
                }
            }
            log = readBridge { rpc_bridge_log($0, $1) }
            availMB = Int(os_proc_available_memory() / 1_048_576)
            onCPU = rpc_bridge_last_on_cpu() != 0
            // USB はすぐ、Tailscale はつながるのを待って、自動で始める。マイクとカメラだけのモードでは始めない
            if autoStart && !micCamOnly {
                if usb { start(usbHost) }
                if started.allSatisfy({ $0 == usbHost }) {
                    addrs = listAddresses()
                    if let ts = addrs.first(where: { $0.ip.hasPrefix("100.") }) {
                        host = ts.ip
                        start(ts.ip)
                    }
                }
            }
        }
    }

    // ---- ホーム：GPU のカード・状態・使う機能・iPhone の画面に表示 ----

    private var homeTab: some View {
        NavigationStack {
            Form {
                if micCamOnly {
                    // マイクとカメラだけのモード：何が動いているかを一番上に出す
                    Section(L("マイクとカメラだけのモード", "Mic and camera only")) {
                        MicCamSummaryView(audio: AudioBridgeModel.shared, camera: webcam, audioOn: audioOn, webcamOn: webcamOn, gpuRunning: running)
                    }
                } else {
                    // GPU のカード：ファンの数は機種で、回る速さは PC から来る仕事の量で変わる
                    Section { GPUCardView() }
                }
                Section(L("状態", "Status")) {
                    if !micCamOnly {
                        LabeledContent(L("PC", "PC"), value: status.connected ? L("接続中", "Connected") : L("接続待ち", "Waiting"))
                        LabeledContent(L("フレームレート", "Frame rate"), value: String(format: "%.1f", status.fps))
                    }
                    LabeledContent(L("温度", "Temperature")) {
                        HStack(spacing: 6) {
                            Circle().fill(status.thermalColor).frame(width: 8, height: 8)
                            Text(status.thermalText)
                        }
                    }
                    LabeledContent(L("電池", "Battery"), value: status.batteryText)
                    if status.lowPower {
                        Text(L("低電力モードがオンです。処理が遅くなる場合があります", "Low Power Mode is on. It may run slower.")).font(.footnote)
                    }
                }
                Section(L("使う機能", "Features")) {
                    Toggle(L("GPU として使う", "Use as a GPU"), isOn: gpuOn)
                    if micCamOnly {
                        Text(running
                             ? L("GPU の接続の受け付けは、アプリを終了すると止まります。次に開いたときは、マイクとカメラだけで始まります。", "GPU listening stops when you quit the app. Next time, only the mic and camera start.")
                             : L("GPU の接続は受け付けません。マイク・スピーカー・カメラの接続だけを受け付けます。PC では Start-Mic-Camera.cmd を起動してください。", "GPU connections are not accepted. Only the mic, speaker, and camera accept connections. On the PC, open Start-Mic-Camera.cmd."))
                            .font(.footnote)
                    }
                    Toggle(L("ウェブカメラとして使う", "Use this iPhone as a webcam"), isOn: $webcamOn)
                    Toggle(L("スピーカーとマイクとして使う", "Use as PC speaker and mic"), isOn: $audioOn)
                }
                Section(L("iPhone の画面に表示", "Show on the iPhone screen")) {
                    Toggle(L("iPhone に出力", "Output to this iPhone"), isOn: $showFrames)
                    if showFrames {
                        Toggle(L("全画面表示を横向きに固定する", "Full screen: lock to landscape"), isOn: $fullLandscape)
                        FramePreviewView(model: preview, status: status)
                    }
                    Toggle(L("PC の画面をミラー", "Mirror the PC screen"), isOn: $mirrorOn)
                    if mirrorOn {
                        if !showFrames {
                            Toggle(L("全画面表示を横向きに固定する", "Full screen: lock to landscape"), isOn: $fullLandscape)
                        }
                        ScreenMirrorStatusRow(model: mirror)
                        ScreenMirrorView(model: mirror)
                    }
                }
            }
            .navigationTitle("PocketGPU")
        }
    }

    // 「GPU として使う」はマイクとカメラだけのモードの逆。切ると、このモードに入る（setMicCamOnly は onChange から）
    private var gpuOn: Binding<Bool> {
        Binding(get: { !micCamOnly }, set: { micCamOnly = !$0 })
    }

    // ---- カメラ：ウェブカメラ・映像の設定・Wi-Fi ----

    private var cameraTab: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle(L("ウェブカメラとして使う", "Use this iPhone as a webcam"), isOn: $webcamOn)
                    if webcamOn {
                        CameraBridgeStatusRow(model: webcam)
                    }
                }
                // カメラ・解像度・フレームレート・画質・PC からの指定を優先する。PC が「おまかせ」にした項目に使う（CameraBridge.swift）。
                // 切っている間も、先に決めておけるように出す。変えた値は次につないだときに使う
                Section(L("映像", "Video")) {
                    CameraSettingsView(model: webcam)
                }
                // 向き・ズームと切り抜き・ピント・明るさ・色・ライト・手ぶれ補正・背景ぼかし（CameraBridge.swift の CameraControlsView）。
                // 接続中に変えると、作り直さずにその場で効く物と、カメラを作り直す物がある。どちらも次の接続でも同じ値を使う
                CameraControlsView(model: webcam)
                // 画面を暗くする（熱の対策。ScreenDimmer.swift）
                Section(L("画面", "Screen")) {
                    ScreenDimSettingsView()
                }
                Section("Wi-Fi") {
                    Toggle(L("Wi-Fi でもウェブカメラに接続できるようにする", "Webcam over Wi-Fi too"), isOn: $webcamWifi)
                    if webcamOn && webcamWifi {
                        let ip = addrs.first(where: { $0.name == "en0" })?.ip ?? "?"
                        Text(L("PC で実行：", "On the PC: ") + "webcam_rx --addr \(ip):50064 --code \(webcam.wifiCode)").font(.footnote.monospaced()).textSelection(.enabled)
                    }
                }
                Section { BetaNote() }
            }
            .navigationTitle(L("カメラ", "Camera"))
        }
    }

    // ---- 音と字幕：PC のスピーカーとマイク・字幕 ----

    private var audioTab: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle(L("スピーカーとマイクとして使う", "Use as PC speaker and mic"), isOn: $audioOn)
                    if audioOn {
                        AudioBridgeStatusRow(model: AudioBridgeModel.shared)
                    }
                }
                // 入力・音の種類・ステレオ（AudioBridge.swift の MicSettingsView）。接続中に変えたら、その場で音を作り直す。
                // 前の「イヤホンのマイクを使う」（audioHeadsetMic）は、入力の「Bluetooth」に移した
                Section(L("マイク", "Microphone")) {
                    MicSettingsView(model: AudioBridgeModel.shared)
                }
                // 字幕は PC のアプリが始める（AppleFeatures の 50057 番の live_asr・LiveASR.swift）。ここでは使えるかだけを出す
                Section(L("字幕", "Captions")) {
                    LabeledContent(L("文字起こし", "Transcription"), value: liveCaptionAvailable ? L("使用できます", "Available") : L("iOS 26 以降で使用できます", "Needs iOS 26 or later"))
                    Text(L("PC の音声をこの iPhone で文字に起こします。字幕は PC の PocketGPU アプリで開始します。", "Transcribes PC audio on this iPhone. Start captions in the PocketGPU app on the PC.")).font(.footnote)
                }
                Section { BetaNote() }
            }
            .navigationTitle(L("音と字幕", "Audio and captions"))
        }
    }

    // 流しっぱなしの文字起こし（LiveASR.swift）が使えるか。AppleFeatures の hello が live_asr を名乗るときと同じ決め方
    private var liveCaptionAvailable: Bool {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) { return true }
        #endif
        return false
    }

    // ---- 設定：接続の受け付け・計算・この iPhone・PC から使う機能・言語・ログ ----

    private var settingsTab: some View {
        NavigationStack {
            Form {
                Section {
                    Picker(L("アドレス", "Address"), selection: $host) {
                        ForEach(addrs, id: \.ip) { a in
                            Text(a.ip + L("・", ", ") + Self.addrKind(a)).tag(a.ip)
                        }
                    }
                    TextField(L("ポート", "Port"), text: $port).keyboardType(.numberPad)
                    Toggle(L("USB でも接続を受け付ける", "Also listen over USB"), isOn: $usb)
                    Toggle(L("インターネット共有でも接続を受け付ける", "Also listen on Personal Hotspot"), isOn: $hotspotListen)
                    Toggle(L("バックグラウンドでも実行する", "Keep running in the background"), isOn: $keepAlive)
                    Toggle(L("起動時に自動で開始", "Start automatically on launch"), isOn: $autoStart)
                        .disabled(micCamOnly) // マイクとカメラだけのモードでは、自動でも GPU を始めない
                    // 「起動時に自動で開始」を切ったときは、ここで始める。始めたら、止めるにはアプリを終了する
                    Button(running ? L("実行中です。停止するにはアプリを終了してください", "Running. Quit the app to stop")
                                   : micCamOnly ? L("マイクとカメラだけのモードでは GPU を開始しません", "The GPU does not start in mic and camera only mode") : L("開始", "Start")) {
                        if !host.isEmpty { start(host) }
                        if usb { start(usbHost) }
                    }
                    .disabled(running || micCamOnly || (host.isEmpty && !usb))
                } header: { Text(L("接続の受け付け", "Listening")) }
                if running {
                    // 始めたあとに出す：PC で打つコマンドと、Vulkan の受け付け
                    Section {
                        if started.contains(host) {
                            Text(L("PC で実行：", "On the PC: ") + "llama-cli … --rpc \(host):\(port)").font(.footnote.monospaced()).textSelection(.enabled)
                        }
                        if started.contains(usbHost) {
                            Text(L("USB の場合は usb_forward.py の起動後に", "USB: after usb_forward.py,") + " --rpc 127.0.0.1:50053").font(.footnote.monospaced()).textSelection(.enabled)
                        }
                        if vkFailed {
                            Text(L("Vulkan の起動に失敗しました。下のログの vk: の行をチェックしてください", "Vulkan failed to start. Check the vk: lines in the log below.")).font(.footnote)
                        } else if vkStarted {
                            Text(L("Vulkan：USB のポート 50060 で接続を受け付けています", "Vulkan: listening on USB port 50060")).font(.footnote.monospaced()).textSelection(.enabled)
                            if !linkHost.isEmpty {
                                Text(L("Vulkan：インターネット共有の \(linkHost):50060 でも接続を受け付けています", "Vulkan: also listening on the tethering link \(linkHost):50060")).font(.footnote.monospaced()).textSelection(.enabled)
                            }
                        }
                    }
                }
                Section(L("計算", "Computing")) {
                    Toggle(L("Metal の GPU で計算する", "Compute on the Metal GPU"), isOn: $useGPU)
                    Toggle(L("Metal 4 の tensor を使用する。アプリの再起動後に有効になります", "Use Metal 4 tensors. Applies after the app restarts."), isOn: $metalTensor)
                    #if MVK_RT
                    Toggle(L("試験中のレイトレーシングを使用する。アプリの再起動後に有効になります", "Use experimental ray tracing. Applies after the app restarts."), isOn: $rtExperimental)
                    if rtExperimental {
                        Text(L("試験用のビルドです。使用できるかどうかはログの vk: の行で確認できます", "Test build. Check the vk: lines in the log to see whether it is available.")).font(.footnote)
                    }
                    #endif
                }
                Section(L("この iPhone", "This iPhone")) {
                    Text(devices.isEmpty ? L("読み込み中", "Loading") : devices).font(.footnote.monospaced())
                    Text(L("アプリが使用できる残りのメモリ：\(availMB) MB", "Memory available to the app: \(availMB) MB")).font(.footnote)
                    Text(onCPU ? L("現在の計算：CPU", "Computing on: CPU") : L("現在の計算：GPU", "Computing on: GPU")).font(.footnote)
                }
                // 起動したときから USB で待っている受け口。画面での操作は無いので、何があるかだけを出す
                Section(L("PC から使う機能", "Used from the PC")) {
                    LabeledContent(L("AI の機能", "AI features"), value: L("USB のポート 50057", "USB port 50057"))
                    LabeledContent(L("圧縮", "Compression"), value: L("USB のポート 50065", "USB port 50065"))
                    LabeledContent(L("録画", "Recording"), value: L("USB のポート 50060", "USB port 50060"))
                    Text(L("どれも PC の PocketGPU アプリから使います。この iPhone で操作する必要はありません。", "All of these are used from the PocketGPU app on the PC. Nothing needs to be done on this iPhone.")).font(.footnote)
                }
                Section {
                    Picker(L("言語", "Language"), selection: $lang) {
                        ForEach(AppLang.allCases) { l in Text(l.label).tag(l.rawValue) }
                    }
                }
                #if PUMPKIN
                Section {
                    Button(mcRunning ? L("実行中です。停止するにはアプリを終了してください", "Running. Quit the app to stop") : L("Minecraft のサーバーを開始", "Start the Minecraft server")) {
                        MinecraftServer.shared.start()
                        mcRunning = true
                    }
                    .disabled(mcRunning)
                    if mcRunning, let wifi = addrs.first(where: { $0.name == "en0" })?.ip {
                        Text(L("Java 版：\(wifi):25565。同じ Wi-Fi から接続できます", "Java Edition: \(wifi):25565. Connect from the same Wi-Fi.")).font(.footnote.monospaced()).textSelection(.enabled)
                    }
                    Text(L("サーバーは Pumpkin で、ライセンスは GPL-3.0 です。ワールドは書類の minecraft フォルダに保存します。", "The server is Pumpkin, licensed under GPL-3.0. The world is saved in the minecraft folder in Documents.")).font(.footnote)
                } header: { Text(L("Minecraft のサーバー", "Minecraft server")) }
                #endif
                Section(L("ログ", "Log")) {
                    ShareLink(item: diagnosticsText(status: status, log: log)) {
                        Label(L("ログを送信", "Send logs"), systemImage: "square.and.arrow.up")
                    }
                    Text(log).font(.caption2.monospaced()).textSelection(.enabled)
                }
            }
            .navigationTitle(L("設定", "Settings"))
        }
    }

    // アドレスの種類の名前。Tailscale・Wi-Fi・それ以外は口の名前のまま
    private static func addrKind(_ a: Addr) -> String {
        a.name.hasPrefix("utun") ? "Tailscale" : a.name == "en0" ? "Wi-Fi" : a.name
    }

    private func setMirror(_ on: Bool) {
        mirror.setOn(on)
        // 映している間は画面を消さない。受け手が動いている間は受け手がつけたままにしているので、そのままにする
        if on {
            UIApplication.shared.isIdleTimerDisabled = true
        } else if !running && !webcamOn {
            UIApplication.shared.isIdleTimerDisabled = false
        }
    }

    private func setWebcam(_ on: Bool) {
        webcam.setOn(on)
        // カメラを使う間は画面を消さない（iOS は、アプリが前にいない間はカメラを使わせない）
        if on {
            UIApplication.shared.isIdleTimerDisabled = true
        } else if !running && !mirrorOn {
            UIApplication.shared.isIdleTimerDisabled = false
        }
    }

    // マイクとカメラだけのモードに入れたら、スピーカーとマイク・ウェブカメラもオンにする（GPU の受け手は始めない）。
    // 切ったときは何も止めない（オンにした物は、それぞれの切り替えで止める）
    private func setMicCamOnly(_ on: Bool) {
        guard on else { return }
        if !audioOn { audioOn = true; AudioBridgeModel.shared.setOn(true) }
        if !webcamOn { webcamOn = true; setWebcam(true) }
    }

    private func start(_ h: String) {
        guard !micCamOnly else { return } // マイクとカメラだけのモードでは、GPU の受け手（llama の rpc・Vulkan）を始めない
        guard let p = Int32(port), !h.isEmpty, !started.contains(h) else { return }
        started.insert(h)
        UIApplication.shared.isIdleTimerDisabled = true // GPU を使えるのは前にいる間だけなので、画面はつけたままにする
        if keepAlive { Keeper.shared.start() }
        let gpu: Int32 = useGPU ? 1 : 0
        let threads = Int32(max(1, ProcessInfo.processInfo.activeProcessorCount - 1))
        let t = Thread { h.withCString { rpc_bridge_serve($0, p, threads, gpu) } }
        t.stackSize = 8 << 20
        t.start()
        // Vulkan の受け手は USB（iPhone の中の 127.0.0.1）だけで待つ。命令をそのまま GPU に流すので、Wi-Fi には出さない
        if h == usbHost && !vkStarted {
            vkStarted = true
            vkr_set_token(linkToken)
            // 受け手の pipeline cache を Library/Caches に残す。容量が足りないとシステムが消すことがあるが、消えても空から始まるだけ
            if let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first {
                caches.path.withCString { vkr_set_pcache_dir($0) }
            }
            let v = Thread {
                // 待ち受けている間は戻ってこない。戻ってきたら起動に失敗した
                _ = vkr_serve("127.0.0.1", 50060)
                DispatchQueue.main.async { vkFailed = true }
            }
            v.stackSize = 8 << 20
            v.qualityOfService = .userInteractive // 省電力のコアに回されると、同じゲームが半分の速さになる
            v.start()
        }
    }
}

// マイクとカメラだけのモードで、一番上に出す「何が動いているか」。音とカメラの接続の変化で描き直す
struct MicCamSummaryView: View {
    @ObservedObject var audio: AudioBridgeModel
    @ObservedObject var camera: CameraBridge
    let audioOn: Bool
    let webcamOn: Bool
    let gpuRunning: Bool

    var body: some View {
        LabeledContent(L("GPU", "GPU"), value: gpuRunning ? L("アプリを終了するまで動作します", "Runs until you quit the app") : L("使用していません", "Not in use"))
        LabeledContent(L("スピーカーとマイク", "Speaker and mic"), value: !audioOn ? L("オフ", "Off") : audio.connected ? L("接続中", "Connected") : L("PC の接続待ち", "Waiting for the PC"))
        LabeledContent(L("ウェブカメラ", "Webcam"), value: !webcamOn ? L("オフ", "Off") : camera.connected ? camera.detail : L("PC の接続待ち", "Waiting for the PC"))
    }
}

// カメラ・音と字幕・PC に送るのタブの下に出す。テスターに先に配る、試験中の機能だと分かるようにする
struct BetaNote: View {
    var body: some View {
        Text(L("試験中の機能です。うまく動かないときは、Discord で教えてください。", "This feature is still being tested. If it does not work well, please tell us on Discord."))
            .font(.footnote)
            .foregroundStyle(.secondary)
    }
}
