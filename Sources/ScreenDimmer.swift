import Combine
import SwiftUI
import UIKit
import UIKit.UIGestureRecognizerSubclass

// 画面を暗くする（熱と電池の対策）。本体の PocketGPU と、古い iPhone 用の PocketCam の両方に入れる（project.yml）。
// iOS はアプリが前にいないとカメラを使わせないので、画面を本当に消す（ロックする）とカメラが止まる。代わりに、
// 画面を黒で覆って明るさを一番下にする。有機 EL の iPhone（17 Pro など）は、黒い画素がほとんど電気を使わない。
// タップするか、アプリが前からいなくなる（コントロールセンター・ホームに戻るなど）と元の明るさに戻す
// （iOS は、アプリが変えた明るさを画面をロックするまで戻さないため。PocketCam の「画面を暗くする」と同じ決まり）。
// 暗い間は、アプリが描く物を止める：覆いの下の画面は透明にして描かない（ScreenDimHost）。カメラの 1 秒ごとの表示の更新も止める（CameraBridge.tick）。
// 温度の信号の色（ThermalSignal・ThermalBadge）もここに置く。両方のアプリとホームの温度の行で、同じ 3 色にする
final class ScreenDimmer: ObservableObject {
    static let shared = ScreenDimmer()
    @Published private(set) var dimmed = false
    let changes = PassthroughSubject<Bool, Never>() // 暗くした（true）・戻した（false）

    // 自動で暗くする設定（本体のカメラタブで選ぶ）
    static let autoKey = "screenAutoDim", secondsKey = "screenAutoDimSeconds"
    static let secondChoices = [15, 30, 60, 120, 300]

    // ここから下は画面の糸だけで触る
    private var savedBrightness: CGFloat?
    private var lastActivity = Date()
    private var hold = false // 画面のライトを出している間は、自動で暗くしない
    private var watcherInstalled = false

    private init() {
        UserDefaults.standard.register(defaults: [Self.autoKey: true, Self.secondsKey: 30])
    }

    // 暗くする・戻す。戻すときは前の明るさに戻す
    func setDim(_ on: Bool) {
        guard on != dimmed else { return }
        if on {
            if let s = Self.currentScreen() {
                savedBrightness = s.brightness
                s.brightness = 0
            }
        } else {
            if let s = Self.currentScreen(), let b = savedBrightness { s.brightness = b }
            savedBrightness = nil
            lastActivity = Date()
        }
        dimmed = on
        changes.send(on)
    }

    func setHold(_ on: Bool) {
        hold = on
        lastActivity = Date()
    }

    // 画面に触れた
    func noteActivity() { lastActivity = Date() }

    // 1 秒ごと。接続中（カメラかマイク）に、操作が無いまま決めた秒数がたったら暗くする
    func autoTick(connected: Bool) {
        installWatcher()
        let d = UserDefaults.standard
        guard connected else { lastActivity = Date(); return } // つないでから数え始める
        guard !dimmed, !hold, d.bool(forKey: Self.autoKey), UIApplication.shared.applicationState == .active else { return }
        let secs = max(5, d.integer(forKey: Self.secondsKey))
        if Date().timeIntervalSince(lastActivity) >= Double(secs) { setDim(true) }
    }

    // 画面に触れたことを知るため、ウィンドウに見張りの部品を付ける（触れたらすぐ失敗して、ほかの操作を邪魔しない）
    private func installWatcher() {
        guard !watcherInstalled else { return }
        let windows = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.flatMap { $0.windows }
        guard let w = windows.first(where: { $0.isKeyWindow }) ?? windows.first else { return }
        w.addGestureRecognizer(ActivityRecognizer { [weak self] in self?.noteActivity() })
        watcherInstalled = true
    }

    // 今の画面（明るさを変える先）
    static func currentScreen() -> UIScreen? {
        UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first?.screen
    }
}

// 触れたことだけを知らせる部品。すぐ失敗するので、ボタンやスクロールの邪魔をしない
private final class ActivityRecognizer: UIGestureRecognizer {
    private let onTouch: () -> Void

    init(_ onTouch: @escaping () -> Void) {
        self.onTouch = onTouch
        super.init(target: nil, action: nil)
        cancelsTouchesInView = false
        delaysTouchesBegan = false
        delaysTouchesEnded = false
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        onTouch()
        state = .failed
    }
}

// 本体の画面のいちばん外に付ける：暗い間は下の画面を透明にして描かず、黒い覆いを重ねる。1 秒ごとに自動で暗くするかを見る。
// connected は「カメラかマイクが接続中か」。onDim は暗くした・戻したときに呼ぶ（iPhone に出力のプレビューを止めるなど）
struct ScreenDimHost: ViewModifier {
    let connected: () -> Bool
    let onDim: (Bool) -> Void
    @ObservedObject private var dimmer = ScreenDimmer.shared
    private let tick = Timer.publish(every: 1, on: .main, in: .common).autoconnect()

    func body(content: Content) -> some View {
        content
            .opacity(dimmer.dimmed ? 0 : 1)
            .overlay(ScreenDimOverlay(connected: connected()))
            .onReceive(tick) { _ in dimmer.autoTick(connected: connected()) }
            .onReceive(dimmer.changes) { on in onDim(on) }
    }
}

// 暗くしている間の覆い。黒の上に、小さく暗い色で温度と「接続中です。タップすると戻ります。」を出す。
// 焼き付きを防ぐため、20 秒ごとに場所を動かす
struct ScreenDimOverlay: View {
    let connected: Bool
    @ObservedObject private var dimmer = ScreenDimmer.shared
    @State private var spot = 0
    private let move = Timer.publish(every: 20, on: .main, in: .common).autoconnect()
    private static let spots: [CGPoint] = [CGPoint(x: 0.3, y: 0.08), CGPoint(x: 0.7, y: 0.92), CGPoint(x: 0.7, y: 0.1), CGPoint(x: 0.3, y: 0.9),
                                           CGPoint(x: 0.5, y: 0.12), CGPoint(x: 0.5, y: 0.88), CGPoint(x: 0.35, y: 0.5), CGPoint(x: 0.65, y: 0.5)]

    var body: some View {
        if dimmer.dimmed {
            GeometryReader { g in
                ZStack {
                    Color.black
                    VStack(spacing: 6) {
                        ThermalBadge(dim: true)
                        Text((connected ? L("接続中です。", "Connected. ") : "") + L("タップすると戻ります。", "Tap to return."))
                            .font(.caption2)
                            .foregroundColor(Color(white: 0.28))
                    }
                    .position(x: g.size.width * Self.spots[spot].x, y: g.size.height * Self.spots[spot].y)
                }
            }
            .ignoresSafeArea()
            .contentShape(Rectangle())
            .onTapGesture { dimmer.setDim(false) }
            .statusBar(hidden: true)
            .onReceive(move) { _ in spot = (spot + 1) % Self.spots.count }
            .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in dimmer.setDim(false) }
        }
    }
}

// 画面を暗くする設定（本体のカメラタブ）
struct ScreenDimSettingsView: View {
    @AppStorage(ScreenDimmer.autoKey) private var auto = true
    @AppStorage(ScreenDimmer.secondsKey) private var seconds = 30

    var body: some View {
        Toggle(L("接続中は自動で画面を暗くする", "Dim the screen automatically while connected"), isOn: $auto)
        if auto {
            Picker(L("暗くするまでの時間", "Dim after"), selection: $seconds) {
                ForEach(ScreenDimmer.secondChoices, id: \.self) { s in Text(Self.secondsText(s)).tag(s) }
            }
            .pickerStyle(.menu)
        }
        Button(L("今すぐ暗くする", "Dim the screen now")) { ScreenDimmer.shared.setDim(true) }
        Text(L("カメラかマイクが接続中に、操作が無いまま決めた時間がたつと、画面を黒くして明るさを最小にします。カメラとマイクは動き続けます。画面をタップすると元に戻ります。",
               "While the camera or mic is connected and the screen is not touched for the chosen time, the screen turns black at the lowest brightness. The camera and mic keep working. Tap the screen to return.")).font(.footnote)
        Text(L("カメラは、アプリが前にあるときしか使えません。画面はロックしないでください。マイクとスピーカーだけを使うときは、PC と接続したあとなら、画面をロックしても使えます。",
               "The camera works only while the app is in front, so do not lock the screen. If you use only the mic and speaker, you can lock the screen after the PC connects.")).font(.footnote)
    }

    private static func secondsText(_ s: Int) -> String {
        s < 60 ? L("\(s) 秒", "\(s) seconds") : L("\(s / 60) 分", "\(s / 60) min")
    }
}

// 温度の信号の色。ProcessInfo.thermalState の 4 段階を 3 色にする：正常は緑・やや高いは黄・高いと危険は赤。
// 危険は文字で「危険」と出し、点を点滅させて、高いと区別する
enum ThermalSignal {
    static func color(_ t: ProcessInfo.ThermalState) -> Color {
        switch t {
        case .nominal: return .green
        case .fair: return .yellow
        default: return .red // serious と critical
        }
    }

    static func text(_ t: ProcessInfo.ThermalState) -> String {
        switch t {
        case .nominal: return L("正常", "Normal")
        case .fair: return L("やや高い", "Warm")
        case .serious: return L("高い", "Hot")
        case .critical: return L("危険", "Critical")
        @unknown default: return "?"
        }
    }
}

// 温度の点と文字。温度が変わったら自分で描き直す。危険のときは点が点滅する
struct ThermalBadge: View {
    var dim = false // 暗くしている間の覆いに出すときは、暗い色にする
    @State private var state = ProcessInfo.processInfo.thermalState
    @State private var blinkOff = false
    private let blink = Timer.publish(every: 0.6, on: .main, in: .common).autoconnect()

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(ThermalSignal.color(state))
                .frame(width: 8, height: 8)
                .opacity(state == .critical && blinkOff ? 0.1 : (dim ? 0.5 : 1))
            Text(ThermalSignal.text(state))
                .fontWeight(state == .critical ? Font.Weight.bold : Font.Weight.regular)
                .foregroundColor(state == .critical ? Color.red.opacity(dim ? 0.6 : 1) : (dim ? Color(white: 0.28) : Color.secondary))
        }
        .onAppear { state = ProcessInfo.processInfo.thermalState }
        .onReceive(NotificationCenter.default.publisher(for: ProcessInfo.thermalStateDidChangeNotification).receive(on: RunLoop.main)) { _ in
            state = ProcessInfo.processInfo.thermalState
        }
        .onReceive(blink) { _ in
            if state == .critical { blinkOff.toggle() } else if blinkOff { blinkOff = false }
        }
    }
}
