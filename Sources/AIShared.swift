// work/ai_apps の足し分（LiveASR・SpeechFile・ImageGen・ArgmaxBridge）が共通で使う小道具：async を待つ・WAV・PNG・置き場
// 名前は CoreMLRemote（work/ane）の中の private な物とぶつからないように AI… にしている
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

final class AIBox<T>: @unchecked Sendable {
    var value: T?
    var error: Error?
}

struct AIError: Error, LocalizedError {
    let message: String
    let code: String?
    init(_ message: String, code: String? = nil) {
        self.message = message
        self.code = code
    }
    var errorDescription: String? { message }
}

enum AIShared {
    // 受け口（AppleFeatures.handle）は同期で答えるので、async の仕事をここで待つ（FMBridge・CoreMLRemote と同じ作り）
    static func blocking<T>(timeout: TimeInterval, _ work: @escaping () async throws -> T) throws -> T {
        let box = AIBox<T>()
        let sem = DispatchSemaphore(value: 0)
        Task {
            do { box.value = try await work() } catch { box.error = error }
            sem.signal()
        }
        if sem.wait(timeout: .now() + timeout) == .timedOut { throw AIError("\(Int(timeout)) 秒で終わらなかった", code: "timeout") }
        if let e = box.error { throw e }
        guard let v = box.value else { throw AIError("答えが空") }
        return v
    }

    // JSONSerialization は NaN と無限大で落ちる（例外なので try? でも防げない）。0 にして 1 ms に丸める
    static func fin(_ d: Double) -> Double {
        guard d.isFinite else { return 0 }
        return (d * 1000).rounded() / 1000
    }

    // [-1, 1] の float の並びを、16 ビット・モノラルの WAV に
    static func wav16(_ samples: [Float], rate: Int) -> Data {
        var pcm = Data(capacity: samples.count * 2)
        for s in samples {
            let v = Int16(max(-1, min(1, s)) * 32767)
            withUnsafeBytes(of: v.littleEndian) { pcm.append(contentsOf: $0) }
        }
        var d = Data()
        func u32(_ x: UInt32) { withUnsafeBytes(of: x.littleEndian) { d.append(contentsOf: $0) } }
        func u16(_ x: UInt16) { withUnsafeBytes(of: x.littleEndian) { d.append(contentsOf: $0) } }
        d.append(contentsOf: Array("RIFF".utf8)); u32(UInt32(36 + pcm.count))
        d.append(contentsOf: Array("WAVEfmt ".utf8)); u32(16); u16(1); u16(1); u32(UInt32(rate)); u32(UInt32(rate * 2)); u16(2); u16(16)
        d.append(contentsOf: Array("data".utf8)); u32(UInt32(pcm.count))
        d.append(pcm)
        return d
    }

    static func png(_ img: CGImage) -> Data? {
        let out = NSMutableData()
        guard let dst = CGImageDestinationCreateWithData(out as CFMutableData, UTType.png.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dst, img, nil)
        guard CGImageDestinationFinalize(dst) else { return nil }
        return out as Data
    }

    // Application Support の下の置き場（バックアップ対象外）
    static func store(_ name: String) -> URL {
        let fm = FileManager.default
        var u = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent(name, isDirectory: true)
        try? fm.createDirectory(at: u, withIntermediateDirectories: true)
        var v = URLResourceValues()
        v.isExcludedFromBackup = true
        try? u.setResourceValues(v)
        return u
    }

    // 絵を答えにする。reply_bin なら 2 枠目の生のバイト（AppleFeatures_coreml.diff の "_bin"）、無ければ base64
    static func packImages(_ imgs: [CGImage], replyBin: Bool) -> ([[String: Any]], Data) {
        var list: [[String: Any]] = []
        var blob = Data()
        for img in imgs {
            guard let p = png(img) else { continue }
            var d: [String: Any] = ["width": img.width, "height": img.height, "fmt": "png", "nbytes": p.count]
            if replyBin {
                d["offset"] = blob.count
                blob.append(p)
            } else {
                d["b64"] = p.base64EncodedString()
            }
            list.append(d)
        }
        return (list, blob)
    }
}
