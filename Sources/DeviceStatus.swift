import SwiftUI
import UIKit

// 画面の「状態」と、受け手の記録に残す熱・電池・低電力の移り変わり。
// 熱で遅くなるか・切れる前に何が起きたかを、あとで PC から記録で確かめるため
final class DeviceStatus: ObservableObject {
    @Published private(set) var fps = 0.0
    @Published private(set) var connected = false
    @Published private(set) var thermal = ProcessInfo.ThermalState.nominal
    @Published private(set) var battery: Float = -1
    @Published private(set) var charging = false
    @Published private(set) var lowPower = false
    private var lastFrames: UInt64 = 0
    private var lastTime = Date()
    private var lastKey = ""
    private var lastNote = Date.distantPast

    init() {
        UIDevice.current.isBatteryMonitoringEnabled = true
        lastFrames = vkr_frames_total()
    }

    // 1 秒ごとに呼ぶ
    func tick() {
        let now = Date()
        let f = vkr_frames_total()
        let dt = now.timeIntervalSince(lastTime)
        if dt >= 0.5 {
            fps = Double(f &- lastFrames) / dt
            lastFrames = f
            lastTime = now
        }
        connected = vkr_clients() != 0
        thermal = ProcessInfo.processInfo.thermalState
        battery = UIDevice.current.batteryLevel
        let st = UIDevice.current.batteryState
        charging = st == .charging || st == .full
        lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
        // 変わったときと、PC とつながっている間は 1 分ごとに書く。電池は 10% きざみで変わったら
        let key = "\(thermal.rawValue)/\(charging)/\(lowPower)/\(battery < 0 ? -1 : Int(battery * 10))/\(connected)"
        if key != lastKey || (connected && now.timeIntervalSince(lastNote) >= 60) {
            lastKey = key
            lastNote = now
            noteLine().withCString { vkr_note($0) }
        }
    }

    // 受け手の記録の 1 行（記録は日本語のまま）
    private func noteLine() -> String {
        var s = "状態：熱 \(Self.thermalJa(thermal))・電池 \(battery < 0 ? "?" : "\(Int((battery * 100).rounded()))%")"
        if charging { s += "・充電中" }
        if lowPower { s += "・低電力モード" }
        s += connected ? "・PC とつながっている・毎秒 \(String(format: "%.1f", fps)) コマ" : "・PC を待っている"
        return s
    }

    static func thermalJa(_ t: ProcessInfo.ThermalState) -> String {
        switch t {
        case .nominal: return "ふつう"
        case .fair: return "やや熱い"
        case .serious: return "熱い"
        case .critical: return "とても熱い"
        @unknown default: return "?"
        }
    }

    var thermalText: String {
        switch thermal {
        case .nominal: return L("ふつう", "Normal")
        case .fair: return L("やや熱い", "Warm")
        case .serious: return L("熱い", "Hot")
        case .critical: return L("とても熱い", "Very hot")
        @unknown default: return "?"
        }
    }

    var thermalColor: Color {
        switch thermal {
        case .nominal: return .green
        case .fair: return .yellow
        case .serious: return .orange
        default: return .red
        }
    }

    var batteryText: String {
        let level = battery < 0 ? "?" : "\(Int((battery * 100).rounded()))%"
        return charging ? L("\(level)・充電中", "\(level), charging") : level
    }
}

// 「記録を送る」で渡す文。機種・iOS・アプリの版・いまの状態・受け手の記録
func diagnosticsText(status: DeviceStatus, log: String) -> String {
    var u = utsname()
    uname(&u)
    let machine = withUnsafeBytes(of: &u.machine) { raw in
        String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
    }
    let info = Bundle.main.infoDictionary
    let ver = info?["CFBundleShortVersionString"] as? String ?? "?"
    let build = info?["CFBundleVersion"] as? String ?? "?"
    let card = GPUCardInfo.current
    var s = "PocketGPU receiver diagnostics\n"
    s += "app: \(ver) (\(build))\n"
    s += "device: \(machine) / iOS \(UIDevice.current.systemVersion)\n"
    s += "gpu: \(card.gpuName) / \(card.memoryGB) GB\n"
    s += "thermal: \(status.thermal.rawValue) / battery: \(status.batteryText) / low power: \(status.lowPower)\n"
    s += "connected: \(status.connected) / fps: \(String(format: "%.1f", status.fps))\n"
    s += "---- log ----\n"
    s += log
    return s
}
