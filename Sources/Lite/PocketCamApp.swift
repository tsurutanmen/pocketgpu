import AVFoundation
import Combine
import SwiftUI
import UIKit

// 古い iPhone（iOS 15・16。iPhone 8 など）用の軽いアプリ PocketCam。iPhone を PC のウェブカメラとマイクとしてだけ使う。
// GPU の機能（llama.cpp・Vulkan の受け手・MoltenVK）と AI のパッケージは入れない。
// 中身は本体と同じ CameraBridge.swift（USB の 50064 番・Wi-Fi は 6 桁のコードつき）と AudioBridge.swift（USB の 50063 番）。
// PC 側は本体と同じ（キットの Start-Mic-Camera.cmd・webcam_rx・ipaudio）で、ポートも通信も同じ。
// C の関数の代わりは LiteShims.swift、温度・電池・許可は LiteStatus.swift。画面の文は仮で、言葉は鶴田さんが決める
@main
struct PocketCamApp: App {
    var body: some Scene {
        WindowGroup { LiteContentView() }
    }
}

// 画面は 1 枚：状態・ウェブカメラ・スピーカーとマイク・画面・ログ。
// iOS 15 で使えない物は使わない（NavigationStack・LabeledContent・ShareLink・onChange の 2 引数の形など）
struct LiteContentView: View {
    // 切り替えの鍵の名前は本体と同じにしてある（別のアプリなので値は共有しない）
    @AppStorage("webcam") private var webcamOn = false
    @AppStorage(CameraBridge.wifiKey) private var webcamWifi = false
    @AppStorage("audioBridge") private var audioOn = false
    // AudioBridge が読む（iPhone につないだ Bluetooth のイヤホンのマイクを使う）
    @AppStorage("audioHeadsetMic") private var audioHeadsetMic = false
    // ウェブカメラがオフでも、アプリを開いている間は画面を自動で消さない
    @AppStorage("liteKeepScreenOn") private var keepScreenOn = true
    // 画面の言葉（自動・日本語・English）。Lang.swift の L が読む
    @AppStorage("lang") private var lang = AppLang.auto.rawValue
    // 画面を暗くする（ScreenDimmer.swift。本体と同じ部品）
    @ObservedObject private var dimmer = ScreenDimmer.shared
    @State private var showLog = false
    private let tick = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    var body: some View {
        ZStack {
            NavigationView {
                Form {
                    Section(L("状態", "Status")) {
                        LiteStatusRows(status: LiteDeviceStatus.shared, camera: CameraBridge.shared, audio: AudioBridgeModel.shared,
                                       webcamOn: webcamOn, audioOn: audioOn)
                    }
                    Section(L("ウェブカメラ", "Webcam")) {
                        Toggle(L("ウェブカメラとして使う", "Use this iPhone as a webcam"), isOn: Binding(get: { webcamOn }, set: { setWebcam($0) }))
                        if webcamOn {
                            LitePermissionNote(status: LiteDeviceStatus.shared, camera: true)
                            CameraBridgeStatusRow(model: CameraBridge.shared)
                            // カメラ・解像度・フレームレート・画質。PC が「おまかせ」にした項目に使う（CameraBridge.swift の設定画面をそのまま使う）
                            CameraSettingsView(model: CameraBridge.shared)
                            Toggle(L("Wi-Fi でもウェブカメラに接続できるようにする", "Webcam over Wi-Fi too"),
                                   isOn: Binding(get: { webcamWifi }, set: { v in webcamWifi = v; CameraBridge.shared.setWifi(v) }))
                            if webcamWifi {
                                LiteWifiRows(code: CameraBridge.shared.wifiCode)
                            }
                        }
                    }
                    Section(L("スピーカーとマイク", "Speaker and mic")) {
                        Toggle(L("PC のスピーカーとマイクとして使う", "Use as PC speaker and mic"), isOn: Binding(get: { audioOn }, set: { setAudio($0) }))
                        if audioOn {
                            LitePermissionNote(status: LiteDeviceStatus.shared, camera: false)
                            Toggle(L("イヤホンのマイクを使う。音質は下がります", "Use the earphone mic. The sound quality is lower."), isOn: $audioHeadsetMic)
                            AudioBridgeStatusRow(model: AudioBridgeModel.shared)
                        }
                    }
                    Section(L("画面", "Screen")) {
                        Toggle(L("画面を自動で消さない", "Keep the screen on"),
                               isOn: Binding(get: { keepScreenOn || webcamOn }, set: { v in keepScreenOn = v; applyIdleTimer() }))
                            .disabled(webcamOn)
                        if webcamOn {
                            Text(L("ウェブカメラがオンの間は、画面を自動で消しません。画面が消えると iOS がカメラを止めるためです。",
                                   "While the webcam is on, the screen stays on. iOS stops the camera when the screen turns off.")).font(.footnote)
                        }
                        Button(L("画面を暗くする", "Dim the screen")) { dimmer.setDim(true) }
                        Text(L("暗くしている間も、カメラとマイクは動作します。画面をタップすると元の明るさに戻ります。",
                               "The camera and mic keep working while the screen is dim. Tap the screen to restore the brightness.")).font(.footnote)
                        Picker(L("言語", "Language"), selection: $lang) {
                            ForEach(AppLang.allCases) { l in Text(l.label).tag(l.rawValue) }
                        }
                        .pickerStyle(.menu)
                    }
                    // DisclosureGroup の文字列の形は、Apple の資料で版が iOS 16 と書かれているので使わない
                    Section(L("ログ", "Log")) {
                        Toggle(L("ログを表示する", "Show the log"), isOn: $showLog)
                        if showLog {
                            LiteLogView(log: LiteLog.shared)
                        }
                        Text(Self.versionText).font(.footnote).foregroundColor(.secondary)
                    }
                }
                // 言語を変えたら、下の画面も含めて作り直す（L は描くときに読むため）
                .id(lang)
                .navigationTitle("PocketCam")
            }
            // 暗くしている間だけ出る。タップか、アプリが前からいなくなると元の明るさに戻す
            ScreenDimOverlay(connected: (webcamOn && CameraBridge.shared.connected) || (audioOn && AudioBridgeModel.shared.connected))
        }
        .statusBar(hidden: dimmer.dimmed)
        .onAppear {
            // 前にオンにしたまま閉じたなら、開いたときから待ち受ける
            CameraBridge.shared.setOn(webcamOn)
            if webcamOn { LitePermissions.requestCameraIfNeeded() }
            AudioBridgeModel.shared.setOn(audioOn)
            applyIdleTimer()
        }
        .onReceive(tick) { _ in
            LiteDeviceStatus.shared.tick()
            CameraBridge.shared.tick()
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didBecomeActiveNotification)) { _ in
            // マイクを前面でしか始められずに待っていたら、ここで始める
            AudioBridgeModel.shared.foregrounded()
            LiteDeviceStatus.shared.tick()
        }
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in
            // アプリが下がるときは明るさを戻す。iOS はアプリが変えた明るさを、画面をロックするまで戻さない
            dimmer.setDim(false)
        }
    }

    private func setWebcam(_ on: Bool) {
        webcamOn = on
        CameraBridge.shared.setOn(on)
        if on { LitePermissions.requestCameraIfNeeded() }
        applyIdleTimer()
    }

    private func setAudio(_ on: Bool) {
        audioOn = on
        AudioBridgeModel.shared.setOn(on)
        applyIdleTimer()
    }

    // カメラを使う間は必ず画面を消さない（iOS は、アプリが前面にいない間はカメラを使わせない）
    private func applyIdleTimer() {
        UIApplication.shared.isIdleTimerDisabled = webcamOn || keepScreenOn
    }

    private static var versionText: String {
        let info = Bundle.main.infoDictionary
        let ver = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "PocketCam \(ver) / \(build) / iOS \(UIDevice.current.systemVersion)"
    }
}

// 「状態」の行：PC が接続中か・フレームレート・温度・電池。iOS 15 でも使えるように LabeledContent は使わない
struct LiteStatusRows: View {
    @ObservedObject var status: LiteDeviceStatus
    @ObservedObject var camera: CameraBridge
    @ObservedObject var audio: AudioBridgeModel
    let webcamOn: Bool
    let audioOn: Bool

    var body: some View {
        HStack {
            Text(L("PC", "PC"))
            Spacer()
            Text(pcText).foregroundColor(.secondary)
        }
        if webcamOn {
            HStack {
                Text(L("フレームレート", "Frame rate"))
                Spacer()
                Text(String(format: "%.1f fps", camera.fps)).foregroundColor(.secondary)
            }
        }
        HStack {
            Text(L("温度", "Temperature"))
            Spacer()
            Circle().fill(status.thermalColor).frame(width: 8, height: 8)
            Text(status.thermalText).foregroundColor(.secondary)
        }
        HStack {
            Text(L("電池", "Battery"))
            Spacer()
            Text(status.batteryText).foregroundColor(.secondary)
        }
        if status.lowPower {
            Text(L("低電力モードがオンです。処理が遅くなる場合があります", "Low Power Mode is on. It may run slower.")).font(.footnote)
        }
    }

    private var pcText: String {
        if !webcamOn && !audioOn { return L("オフ", "Off") }
        if (webcamOn && camera.connected) || (audioOn && audio.connected) { return L("接続中", "Connected") }
        return L("PC の接続待ち", "Waiting for the PC")
    }
}

// カメラかマイクの許可が無いときだけ出す。設定アプリから戻ると、1 秒ごとの tick で消える
struct LitePermissionNote: View {
    @ObservedObject var status: LiteDeviceStatus
    let camera: Bool

    var body: some View {
        if camera ? status.cameraDenied : status.micDenied {
            Text(camera ? L("カメラへのアクセスが許可されていません。設定アプリで許可してください。", "Camera access is not allowed. Allow it in the Settings app.")
                        : L("マイクへのアクセスが許可されていません。設定アプリで許可してください。", "Microphone access is not allowed. Allow it in the Settings app."))
                .font(.footnote)
            Button(L("設定アプリを開く", "Open the Settings app")) { LitePermissions.openSettings() }
        }
    }
}

// Wi-Fi で接続するときの 6 桁のコードと、PC で実行するコマンド。コードは起動のたびに変わる（CameraBridge.wifiCode）
struct LiteWifiRows: View {
    let code: UInt32

    var body: some View {
        let ip = liteWifiAddress()
        HStack {
            Text(L("接続コード", "Connection code"))
            Spacer()
            Text(String(code)).font(.title2.monospaced()).textSelection(.enabled)
        }
        if let ip {
            HStack {
                Text(L("iPhone のアドレス", "iPhone address"))
                Spacer()
                Text(ip).foregroundColor(.secondary).textSelection(.enabled)
            }
            Text(L("PC で実行：", "On the PC: ") + "webcam_rx --addr \(ip):50064 --code \(code)").font(.footnote.monospaced()).textSelection(.enabled)
        } else {
            Text(L("Wi-Fi に接続されていません。", "Not connected to Wi-Fi.")).font(.footnote)
        }
        Text(L("接続コードは、アプリを起動するたびに変わります。", "The code changes each time the app starts.")).font(.footnote)
    }
}

// ログ。長押しで選んでコピーできる。ボタンでも全部をコピーできる
struct LiteLogView: View {
    @ObservedObject var log: LiteLog

    var body: some View {
        Button(L("ログをコピー", "Copy the log")) { UIPasteboard.general.string = log.text }
            .disabled(log.text.isEmpty)
        Text(log.text.isEmpty ? L("ログはまだありません。", "No log yet.") : log.text)
            .font(.caption2.monospaced())
            .textSelection(.enabled)
    }
}
