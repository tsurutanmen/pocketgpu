import Metal
import SwiftUI

// 画面の上の「GPU のカード」。
// ファンの数は、この iPhone の GPU とメモリで決める（Pro の GPU で 12 GB 以上は 3 つ、8 GB 以上は 2 つ、それより小さいと 1 つ）。
// 回る速さは、PC から来る Vulkan の命令の多さで決める。PC がつながっていなければ止まる（本物の GPU の「回さない」と同じ）。
// 画面に出す文は仮。言葉は鶴田さんが決める
struct GPUCardInfo {
    let gpuName: String // Metal が答える名前（例 Apple A19 Pro GPU）
    let memoryGB: Int
    let fans: Int

    static let current: GPUCardInfo = {
        let name = MTLCreateSystemDefaultDevice()?.name ?? "Apple GPU"
        let gb = Int((Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824.0).rounded())
        let fans = (name.contains("Pro") && gb >= 12) ? 3 : (gb >= 8 ? 2 : 1)
        return GPUCardInfo(gpuName: name, memoryGB: gb, fans: fans)
    }()
}

// 命令の数を少しずつ読んで、ファンの回る速さ（1 秒あたりの回転）をなめらかに変える
final class FanModel: ObservableObject {
    @Published private(set) var rpm = 0
    @Published private(set) var connected = false
    @Published private(set) var hot = false
    // 角度と速さは絵を描くたびに進めるので、画面の更新の知らせは出さない
    private var angle = 0.0
    private var speed = 0.0
    private var target = 0.0
    private var lastFrame = Date()
    private var lastOps: UInt64 = 0
    private var lastSample = Date()
    private var started = false

    // 0.25 秒ごとに呼ぶ
    func sample() {
        let now = Date()
        let ops = vkr_ops_total()
        if !started { started = true; lastOps = ops; lastSample = now; return }
        let dt = max(now.timeIntervalSince(lastSample), 0.05)
        let rate = Double(ops &- lastOps) / dt
        lastOps = ops
        lastSample = now
        connected = vkr_clients() != 0
        let thermal = ProcessInfo.processInfo.thermalState
        hot = thermal == .serious || thermal == .critical
        // 仕事が多いほど速く。上限は 1 秒に 12 回転
        var t = connected ? 0.6 + min(1.0, log10(1.0 + rate) / 4.0) * 11.4 : 0.0
        if hot { t = max(t, 10.0) }
        target = t
        rpm = Int(speed * 60.0)
    }

    // 絵を描くたびに角度を進めて返す。急には速さが変わらない
    func advance(to now: Date) -> Double {
        let dt = min(max(now.timeIntervalSince(lastFrame), 0.0), 0.1)
        lastFrame = now
        speed += (target - speed) * min(1.0, dt * 1.5)
        angle = (angle + speed * 360.0 * dt).truncatingRemainder(dividingBy: 360.0)
        return angle
    }
}

// ファン 1 つ。枠と 9 枚の羽根と軸
struct FanView: View {
    let angle: Double
    let hot: Bool

    var body: some View {
        Canvas { ctx, size in
            let r = min(size.width, size.height) / 2.0
            let c = CGPoint(x: size.width / 2.0, y: size.height / 2.0)
            func pt(_ deg: Double, _ k: Double) -> CGPoint {
                let a = deg * Double.pi / 180.0
                return CGPoint(x: c.x + CGFloat(cos(a)) * r * CGFloat(k), y: c.y + CGFloat(sin(a)) * r * CGFloat(k))
            }
            let rim = Path(ellipseIn: CGRect(x: c.x - r + 1, y: c.y - r + 1, width: 2 * r - 2, height: 2 * r - 2))
            ctx.fill(rim, with: .color(Color(white: 0.06)))
            ctx.stroke(rim, with: .color(hot ? Color.orange : Color(white: 0.30)), lineWidth: 2)
            var blades = Path()
            for i in 0..<9 {
                let t = angle + Double(i) * 40.0
                blades.move(to: pt(t, 0.30))
                blades.addQuadCurve(to: pt(t + 28.0, 0.92), control: pt(t - 6.0, 0.70))
                blades.addLine(to: pt(t + 40.0, 0.90))
                blades.addQuadCurve(to: pt(t + 22.0, 0.30), control: pt(t + 20.0, 0.65))
                blades.closeSubpath()
            }
            ctx.fill(blades, with: .color(Color(white: 0.26)))
            let hub = Path(ellipseIn: CGRect(x: c.x - r * 0.30, y: c.y - r * 0.30, width: r * 0.60, height: r * 0.60))
            ctx.fill(hub, with: .color(Color(white: 0.13)))
            ctx.stroke(hub, with: .color(Color(white: 0.30)), lineWidth: 1)
        }
    }
}

struct GPUCardView: View {
    @StateObject private var model = FanModel()
    private let info = GPUCardInfo.current
    private let tick = Timer.publish(every: 0.25, on: .main, in: .common).autoconnect()

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            TimelineView(.animation) { tl in
                let a = model.advance(to: tl.date)
                HStack(spacing: 10) {
                    ForEach(0..<info.fans, id: \.self) { i in
                        // 隣どうしで羽根の向きを少しずらす
                        FanView(angle: a + Double(i) * 13.0, hot: model.hot)
                            .frame(width: 92, height: 92)
                    }
                }
                .frame(maxWidth: .infinity)
            }
            .padding(.vertical, 12)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color(white: 0.10)))
            .overlay(RoundedRectangle(cornerRadius: 10).stroke(Color(white: 0.24), lineWidth: 1))
            HStack {
                Text(info.gpuName).font(.footnote.weight(.semibold))
                Spacer()
                Text("\(info.memoryGB) GB").font(.footnote.monospaced())
            }
            HStack(spacing: 6) {
                Circle().fill(model.connected ? Color.green : Color(white: 0.45)).frame(width: 7, height: 7)
                Text("\(model.rpm) rpm").font(.caption.monospaced())
                if model.hot {
                    Image(systemName: "thermometer.high").font(.caption).foregroundStyle(Color.orange)
                }
                Spacer()
            }
        }
        .onReceive(tick) { _ in model.sample() }
    }
}
