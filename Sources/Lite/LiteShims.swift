import Foundation

// 古い iPhone 用の軽いアプリ PocketCam だけで組むファイル（本体の RpcServer では project.yml の excludes で外す）。
//
// CameraBridge.swift と AudioBridge.swift は、本体では下の 2 つを C の関数として呼ぶ（Sources/rpc_bridge.h・vk_bridge.h）。
// 軽いアプリには C の部分（llama.cpp の受け手・Vulkan の受け手）を入れないので、同じ名前と形の Swift の関数で埋める。
// こうすると 2 本のファイルを書き換えずにそのまま使える。

// ログに 1 行足す。本体では受け手のログ（USB の 50059 番で PC からも読める）。ここでは画面の「ログ」に出す
func rpc_bridge_log_line(_ text: UnsafePointer<CChar>) {
    LiteLog.shared.add(String(cString: text))
}

// PC から届いた音を、GPU の受け手の録画に入れる（本体の GameRecorder）。軽いアプリは録画しないので何もしない。
// AudioBridge は無音の塊のときに nil を渡すので、pcm は nil を受ける形にしておく
func vkr_record_pcm(_ pcm: UnsafePointer<Int16>?, _ frames: UInt32, _ rate: UInt32, _ ch: UInt32, _ index: UInt64) {}

// 画面の「ログ」に出す行。ためるのは新しい方の 300 行だけ。
// 書くのはカメラ・音・画面のどの糸からでもよい。画面に出す文字は 0.2 秒ごとにまとめて main で更新する
final class LiteLog: ObservableObject {
    static let shared = LiteLog()
    @Published private(set) var text = ""

    private let lock = NSLock()
    private var lines: [String] = []
    private var scheduled = false
    private let maxLines = 300
    private let clock: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    private init() {}

    func add(_ s: String) {
        let body = s.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !body.isEmpty else { return }
        lock.lock()
        lines.append(clock.string(from: Date()) + " " + body)
        if lines.count > maxLines { lines.removeFirst(lines.count - maxLines) }
        let schedule = !scheduled
        scheduled = true
        lock.unlock()
        if schedule {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in self?.publish() }
        }
    }

    private func publish() {
        lock.lock()
        let t = lines.joined(separator: "\n")
        scheduled = false
        lock.unlock()
        text = t
    }
}
