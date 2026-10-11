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
        rpc_bridge_init()
        // 窓に出す絵を MetalFX で広げる関数を、受け手に先に渡しておく（PC が頼んだときだけ使う）
        FXUpscaler.register()
        // 記録を PC から読めるようにする（USB の 50059 番。PC は app/pc/iphone_applog.py）
        let t = Thread { rpc_bridge_serve_log(50059) }
        t.start()
        // USB の通り道の速さを測る受け口（50058 番。PC は app/pc/usb_bw.py）
        vkr_bw_set_token(linkToken)
        let b = Thread { _ = vkr_bw_serve("127.0.0.1", 50058) }
        b.start()
        // Apple の機能（文字認識など）を Windows から使う受け口（50057 番。PC は app/pc/apple_ocr.py）
        AppleFeatures.shared.start()
    }
    var body: some Scene { WindowGroup { ContentView() } }
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
    // インターネット共有を USB でつないだ有線の網でも Vulkan と速さを測る口を待つ。初めは切り。合言葉は USB の口で PC に渡す
    @AppStorage("hotspotListen") private var hotspotListen = false
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
    // 画面の言葉（自動・日本語・English）。ここで読むと、変えたときに画面が描き直される
    @AppStorage("lang") private var lang = AppLang.auto.rawValue
    @Environment(\.scenePhase) private var scenePhase
    private let usbHost = "127.0.0.1"
    private let tick = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
    private var running: Bool { !started.isEmpty }

    var body: some View {
        NavigationStack {
            Form {
                // GPU のカード：ファンの数は機種で、回る速さは PC から来る仕事の量で変わる
                Section { GPUCardView() }
                Section(L("状態", "Status")) {
                    LabeledContent(L("PC", "PC"), value: status.connected ? L("接続中", "Connected") : L("接続待ち", "Waiting"))
                    LabeledContent(L("毎秒のコマ数", "Frames per second"), value: String(format: "%.1f", status.fps))
                    LabeledContent(L("温度", "Temperature")) {
                        HStack(spacing: 6) {
                            Circle().fill(status.thermalColor).frame(width: 8, height: 8)
                            Text(status.thermalText)
                        }
                    }
                    LabeledContent(L("電池", "Battery"), value: status.batteryText)
                    if status.lowPower {
                        Text(L("低電力モードがオン。遅くなることがある", "Low Power Mode is on. It may run slower.")).font(.footnote)
                    }
                    Toggle(L("iPhone に出力", "Output to this iPhone"), isOn: $showFrames)
                        .onChange(of: showFrames) { _, on in preview.setOn(on) }
                    if showFrames {
                        Toggle(L("全画面は横向きで固定", "Full screen: lock to landscape"), isOn: $fullLandscape)
                        FramePreviewView(model: preview, status: status)
                    }
                    Toggle(L("PC の画面をミラー", "Mirror the PC screen"), isOn: $mirrorOn)
                        .onChange(of: mirrorOn) { _, on in setMirror(on) }
                    if mirrorOn {
                        if !showFrames {
                            Toggle(L("全画面は横向きで固定", "Full screen: lock to landscape"), isOn: $fullLandscape)
                        }
                        ScreenMirrorStatusRow(model: mirror)
                        ScreenMirrorView(model: mirror)
                    }
                    ShareLink(item: diagnosticsText(status: status, log: log)) {
                        Label(L("ログを送信", "Send logs"), systemImage: "square.and.arrow.up")
                    }
                }
                Section(L("この iPhone", "This iPhone")) {
                    Text(devices.isEmpty ? L("読み込み中", "Loading") : devices).font(.footnote.monospaced())
                    Text(L("アプリが使えるメモリの残り：\(availMB) MB", "Memory available to the app: \(availMB) MB")).font(.footnote)
                    Text(onCPU ? L("いまの計算：CPU", "Computing on: CPU") : L("いまの計算：GPU", "Computing on: GPU")).font(.footnote)
                }
                Section {
                    Picker(L("アドレス", "Address"), selection: $host) {
                        ForEach(addrs, id: \.ip) { a in
                            Text("\(a.ip) (\(a.name.hasPrefix("utun") ? "Tailscale" : a.name == "en0" ? "Wi-Fi" : a.name))").tag(a.ip)
                        }
                    }
                    TextField(L("ポート", "Port"), text: $port).keyboardType(.numberPad)
                    Toggle(L("GPU（Metal）で計算する", "Compute on the GPU (Metal)"), isOn: $useGPU)
                    Toggle(L("USB でも待ち受ける", "Also listen over USB"), isOn: $usb)
                    Toggle(L("インターネット共有でも待ち受ける", "Also listen on Personal Hotspot"), isOn: $hotspotListen)
                    Toggle(L("バックグラウンドでも動かす", "Keep running in the background"), isOn: $keepAlive)
                        .onChange(of: keepAlive) { _, on in if running { on ? Keeper.shared.start() : Keeper.shared.stop() } }
                    Toggle(L("起動時に自動で開始", "Start automatically on launch"), isOn: $autoStart)
                    Toggle(L("Metal 4 の tensor を使う・開き直すと効く", "Use Metal 4 tensors (applies after relaunch)"), isOn: $metalTensor)
                    Picker(L("言語", "Language"), selection: $lang) {
                        ForEach(AppLang.allCases) { l in Text(l.label).tag(l.rawValue) }
                    }
                } header: { Text(L("待ち受け", "Listening")) }
                Section {
                    Button(running ? L("実行中・停止はアプリを終了", "Running. Quit the app to stop") : L("開始", "Start")) {
                        if !host.isEmpty { start(host) }
                        if usb { start(usbHost) }
                    }
                    .disabled(running || (host.isEmpty && !usb))
                    if started.contains(host) {
                        Text(L("PC で：", "On the PC: ") + "llama-cli … --rpc \(host):\(port)").font(.footnote.monospaced()).textSelection(.enabled)
                    }
                    if started.contains(usbHost) {
                        Text(L("USB：usb_forward.py のあと", "USB: after usb_forward.py,") + " --rpc 127.0.0.1:50053").font(.footnote.monospaced()).textSelection(.enabled)
                    }
                    if vkFailed {
                        Text(L("Vulkan：起動に失敗（下のログの vk: を見る）", "Vulkan: failed to start (see the vk: lines in the log below)")).font(.footnote)
                    } else if vkStarted {
                        Text(L("Vulkan：USB の 50060 番で待ち受け中", "Vulkan: listening on USB port 50060")).font(.footnote.monospaced()).textSelection(.enabled)
                        if !linkHost.isEmpty {
                            Text(L("Vulkan：インターネット共有の口 \(linkHost):50060 でも待ち受け中", "Vulkan: also listening on the tethering link \(linkHost):50060")).font(.footnote.monospaced()).textSelection(.enabled)
                        }
                    }
                }
                #if PUMPKIN
                Section {
                    Button(mcRunning ? "実行中・停止はアプリを終了" : "マイクラのサーバーを始める") {
                        MinecraftServer.shared.start()
                        mcRunning = true
                    }
                    .disabled(mcRunning)
                    if mcRunning, let wifi = addrs.first(where: { $0.name == "en0" })?.ip {
                        Text("Java 版：\(wifi):25565（同じ Wi-Fi から）").font(.footnote.monospaced()).textSelection(.enabled)
                    }
                    Text("中身は Pumpkin（GPL-3.0）。世界は書類の minecraft/ に置く").font(.footnote)
                } header: { Text("マイクラのサーバー") }
                #endif
                Section(L("ログ", "Log")) {
                    Text(log).font(.caption2.monospaced()).textSelection(.enabled)
                }
            }
            .navigationTitle(L("GPU", "GPU"))
        }
        .onAppear {
            devices = readBridge { rpc_bridge_devices($0, $1) }
            host = addrs.first(where: { $0.ip.hasPrefix("100.") })?.ip ?? addrs.first(where: { $0.name == "en0" })?.ip ?? addrs.first?.ip ?? ""
            // 前にオンにしたまま閉じたなら、開いたときから出す
            preview.setOn(showFrames)
            setMirror(mirrorOn)
        }
        .onChange(of: scenePhase) { _, phase in
            // 後ろに回ると GPU が使えない。前に戻ったら GPU に戻す
            rpc_bridge_set_background(phase == .background ? 1 : 0)
            // Vulkan の受け手は、前にいない間（切り替えの途中も含む）は GPU に仕事を出さずに待つ
            vkr_set_paused(phase == .active ? 0 : 1)
        }
        .onReceive(tick) { _ in
            vkr_set_thermal(Int32(ProcessInfo.processInfo.thermalState.rawValue))
            status.tick()
            mirror.tick()
            // 設定が入っていれば、インターネット共有の網の口が出たら、そこでも待つ（消えたら・設定を切ったら閉じる）
            if vkStarted && !vkFailed {
                let cap = 64
                var buf = [CChar](repeating: 0, count: cap)
                // 2 は Vulkan がまだ起きていない。次の 1 秒でもう一度
                if vkr_hotspot_tick(hotspotListen ? 1 : 0, &buf, Int32(cap)) != 2 {
                    let h = String(cString: buf)
                    if h != linkHost {
                        linkHost = h
                        h.withCString { _ = vkr_bw_listen_extra($0, 50058) }
                    }
                }
            }
            log = readBridge { rpc_bridge_log($0, $1) }
            availMB = Int(os_proc_available_memory() / 1_048_576)
            onCPU = rpc_bridge_last_on_cpu() != 0
            // USB はすぐ、Tailscale はつながるのを待って、自動で始める
            if autoStart {
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

    private func setMirror(_ on: Bool) {
        mirror.setOn(on)
        // 映している間は画面を消さない。受け手が動いている間は受け手がつけたままにしているので、そのままにする
        if on {
            UIApplication.shared.isIdleTimerDisabled = true
        } else if !running {
            UIApplication.shared.isIdleTimerDisabled = false
        }
    }

    private func start(_ h: String) {
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
