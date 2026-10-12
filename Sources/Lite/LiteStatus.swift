import AVFoundation
import SwiftUI
import UIKit

// PocketCam の「状態」：温度・電池・低電力モード・カメラとマイクの許可。画面が 1 秒ごとに tick を呼ぶ。
// 本体の DeviceStatus.swift は GPU の受け手（vkr_frames_total など）に頼るので、軽いアプリでは使わずにここで読む。
// 温度と電池の言葉は DeviceStatus.swift と同じ
final class LiteDeviceStatus: ObservableObject {
    static let shared = LiteDeviceStatus()

    @Published private(set) var thermal = ProcessInfo.ThermalState.nominal
    @Published private(set) var battery: Float = -1
    @Published private(set) var charging = false
    @Published private(set) var lowPower = false
    @Published private(set) var cameraDenied = false
    @Published private(set) var micDenied = false

    private init() {
        UIDevice.current.isBatteryMonitoringEnabled = true
        tick()
    }

    // 画面の糸から 1 秒ごとに呼ぶ。変わった値だけ書く（書くたびに画面が描き直されるため）
    func tick() {
        let t = ProcessInfo.processInfo.thermalState
        if t != thermal { thermal = t }
        let b = UIDevice.current.batteryLevel
        if b != battery { battery = b }
        let st = UIDevice.current.batteryState
        let c = st == .charging || st == .full
        if c != charging { charging = c }
        let lp = ProcessInfo.processInfo.isLowPowerModeEnabled
        if lp != lowPower { lowPower = lp }
        let cd = LitePermissions.cameraDenied
        if cd != cameraDenied { cameraDenied = cd }
        let md = LitePermissions.micDenied
        if md != micDenied { micDenied = md }
    }

    // 温度の文と色は、カメラタブと暗くした画面と同じ（ScreenDimmer.swift の ThermalSignal）。
    // 信号の 3 色：正常は緑・やや高いは黄・高いと危険は赤。危険は文字で分かるようにする
    var thermalText: String { ThermalSignal.text(thermal) }

    var thermalColor: Color { ThermalSignal.color(thermal) }

    var batteryText: String {
        let level = battery < 0 ? "?" : "\(Int((battery * 100).rounded()))%"
        return charging ? L("\(level)・充電中", "\(level), charging") : level
    }
}

// カメラとマイクの許可。マイクの許可を読む AVAudioApplication は iOS 17 から。それより前は AVAudioSession の同じ物を使う
// （AudioBridge.swift の MicPermission と同じ分け方。あちらは private なので、ここにも置く）
enum LitePermissions {
    static var cameraDenied: Bool {
        let s = AVCaptureDevice.authorizationStatus(for: .video)
        return s == .denied || s == .restricted
    }

    static var micDenied: Bool {
        if #available(iOS 17.0, *) {
            return AVAudioApplication.shared.recordPermission == .denied
        } else {
            return AVAudioSession.sharedInstance().recordPermission == .denied
        }
    }

    // CameraBridge は PC がつないできたときに許可を聞く。軽いアプリでは、切り替えをオンにした時点で先に聞いておく
    static func requestCameraIfNeeded() {
        if AVCaptureDevice.authorizationStatus(for: .video) == .notDetermined {
            AVCaptureDevice.requestAccess(for: .video) { _ in }
        }
    }

    static func openSettings() {
        if let u = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(u) }
    }
}

// この iPhone の Wi-Fi（en0）の IPv4 アドレス。つながっていなければ nil。
// 本体の RpcServerApp.swift の listAddresses と同じ読み方
func liteWifiAddress() -> String? {
    var head: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&head) == 0, let first = head else { return nil }
    defer { freeifaddrs(head) }
    var p: UnsafeMutablePointer<ifaddrs>? = first
    while let cur = p {
        let ifa = cur.pointee
        if let sa = ifa.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET), String(cString: ifa.ifa_name) == "en0" {
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 {
                return String(cString: host)
            }
        }
        p = ifa.ifa_next
    }
    return nil
}
