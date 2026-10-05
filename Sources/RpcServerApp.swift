import SwiftUI
import os

@main
struct RpcServerApp: App {
    init() { rpc_bridge_init() }
    var body: some Scene { WindowGroup { ContentView() } }
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
    private let usbHost = "127.0.0.1"
    private let tick = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
    private var running: Bool { !started.isEmpty }

    var body: some View {
        NavigationStack {
            Form {
                Section("この iPhone") {
                    Text(devices.isEmpty ? "読み込み中" : devices).font(.footnote.monospaced())
                    Text("アプリが使えるメモリの残り：\(availMB) MB").font(.footnote)
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
                    Toggle("開いたら自動で始める", isOn: $autoStart)
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
                }
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
        .onReceive(tick) { _ in
            log = readBridge { rpc_bridge_log($0, $1) }
            availMB = Int(os_proc_available_memory() / 1_048_576)
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
        UIApplication.shared.isIdleTimerDisabled = true // 画面が消えると止まるので、つけたままにする
        let gpu: Int32 = useGPU ? 1 : 0
        let threads = Int32(max(1, ProcessInfo.processInfo.activeProcessorCount - 1))
        let t = Thread { h.withCString { rpc_bridge_serve($0, p, threads, gpu) } }
        t.stackSize = 8 << 20
        t.start()
    }
}
