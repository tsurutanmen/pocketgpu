// iPhone の中でマイクラのサーバー（Pumpkin・Rust 製・GPL-3.0）を動かす。mc/pumpkin-ios の pumpkin_ios_run を呼ぶ。
// 世界と設定はアプリの書類の場所の minecraft/ に置く。Pumpkin の記録（標準出力）は、アプリの記録（50059 番）へ流す。
// Java 版は 25565 番で待つ。同じ Wi-Fi の PC やスマホから、この iPhone の住所:25565 で入れる
#if PUMPKIN
import Foundation

final class MinecraftServer {
    static let shared = MinecraftServer()
    private(set) var running = false
    private var thread: Thread?

    var folder: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("minecraft", isDirectory: true)
    }

    func start() {
        guard !running else { return }
        running = true
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        forwardOutput()
        let dir = folder.path
        let t = Thread {
            log("マイクラのサーバーを始める：\(dir)")
            let code = dir.withCString { pumpkin_ios_run($0) }
            log("マイクラのサーバーが止まった（\(code)）")
            DispatchQueue.main.async { self.running = false }
        }
        t.stackSize = 16 << 20
        t.start()
        thread = t
    }

    // Pumpkin は標準出力に書くので、それを読んでアプリの記録へ回す
    private var forwarding = false
    private func forwardOutput() {
        guard !forwarding else { return }
        forwarding = true
        var fds: [Int32] = [0, 0]
        guard pipe(&fds) == 0 else { return }
        setvbuf(stdout, nil, _IOLBF, 0)
        dup2(fds[1], STDOUT_FILENO)
        dup2(fds[1], STDERR_FILENO)
        let r = fds[0]
        let t = Thread {
            var buf = [UInt8](repeating: 0, count: 4096)
            var line = Data()
            while true {
                let n = read(r, &buf, buf.count)
                if n <= 0 { break }
                for b in buf[0..<n] {
                    if b == 10 {
                        // 色の制御文字を取り除く
                        let s = String(decoding: line, as: UTF8.self).replacingOccurrences(of: "\u{1B}\\[[0-9;]*m", with: "", options: .regularExpression)
                        log("mc: " + s)
                        line.removeAll(keepingCapacity: true)
                    } else {
                        line.append(b)
                    }
                }
            }
        }
        t.start()
    }
}

private func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }
#endif
