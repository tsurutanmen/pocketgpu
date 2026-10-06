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
        // 記録を PC から読めるようにする（USB の 50059 番。PC は app/pc/iphone_applog.py）
        let t = Thread { rpc_bridge_serve_log(50059) }
        t.start()
        // USB の通り道の速さを測る受け口（50058 番。PC は app/pc/usb_bw.py）
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
    @State private var onCPU = false
    @State private var vkStarted = false
    @State private var vkFailed = false
    @State private var mcRunning = false
    @Environment(\.scenePhase) private var scenePhase
    private let usbHost = "127.0.0.1"
    private let tick = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
    private var running: Bool { !started.isEmpty }

    var body: some View {
        NavigationStack {
            Form {
                Section("この iPhone") {
                    Text(devices.isEmpty ? "読み込み中" : devices).font(.footnote.monospaced())
                    Text("アプリが使えるメモリの残り：\(availMB) MB").font(.footnote)
                    Text(onCPU ? "いまの計算：CPU" : "いまの計算：GPU").font(.footnote)
                }
                Section {
                    Picker("待ち受ける住所", selection: $host) {
                        ForEach(addrs, id: \.ip) { a in
                            Text("\(a.ip)（\(a.name.hasPrefix("utun") ? "Tailscale" : a.name == "en0" ? "Wi-Fi" : a.name)）").tag(a.ip)
                        }
                    }
                    TextField("番号", text: $port).keyboardType(.numberPad)
                    Toggle("GPU（Metal）で計算する", isOn: $useGPU)
                    Toggle("USB でも待ち受ける", isOn: $usb)
                    Toggle("後ろに回っても続ける", isOn: $keepAlive)
                        .onChange(of: keepAlive) { _, on in if running { on ? Keeper.shared.start() : Keeper.shared.stop() } }
                    Toggle("開いたら自動で始める", isOn: $autoStart)
                    Toggle("Metal 4 の tensor を使う・開き直すと効く", isOn: $metalTensor)
                } header: { Text("待ち受け") }
                Section {
                    Button(running ? "動いている（止めるときはアプリを閉じる）" : "受け手を始める") {
                        if !host.isEmpty { start(host) }
                        if usb { start(usbHost) }
                    }
                    .disabled(running || (host.isEmpty && !usb))
                    if started.contains(host) {
                        Text("PC で：llama-cli … --rpc \(host):\(port)").font(.footnote.monospaced()).textSelection(.enabled)
                    }
                    if started.contains(usbHost) {
                        Text("USB：usb_forward.py のあと --rpc 127.0.0.1:50053").font(.footnote.monospaced()).textSelection(.enabled)
                    }
                    if vkFailed {
                        Text("Vulkan：起動に失敗（下の記録の vk: を見る）").font(.footnote)
                    } else if vkStarted {
                        Text("Vulkan：USB の 50060 番で待ち受け中").font(.footnote.monospaced()).textSelection(.enabled)
                    }
                }
                #if PUMPKIN
                Section {
                    Button(mcRunning ? "動いている（止めるときはアプリを閉じる）" : "マイクラのサーバーを始める") {
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
                Section("記録") {
                    Text(log).font(.caption2.monospaced()).textSelection(.enabled)
                }
            }
            .navigationTitle("計算の受け手")
        }
        .onAppear {
            devices = readBridge { rpc_bridge_devices($0, $1) }
            host = addrs.first(where: { $0.ip.hasPrefix("100.") })?.ip ?? addrs.first(where: { $0.name == "en0" })?.ip ?? addrs.first?.ip ?? ""
        }
        .onChange(of: scenePhase) { _, phase in
            // 後ろに回ると GPU が使えない。前に戻ったら GPU に戻す
            rpc_bridge_set_background(phase == .background ? 1 : 0)
            // Vulkan の受け手は、前にいない間（切り替えの途中も含む）は GPU に仕事を出さずに待つ
            vkr_set_paused(phase == .active ? 0 : 1)
        }
        .onReceive(tick) { _ in
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
            let v = Thread {
                // 待ち受けている間は戻ってこない。戻ってきたら起動に失敗した
                _ = vkr_serve("127.0.0.1", 50060)
                DispatchQueue.main.async { vkFailed = true }
            }
            v.stackSize = 8 << 20
            v.start()
        }
    }
}
