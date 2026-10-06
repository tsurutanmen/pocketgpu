// Apple Intelligence の言葉のモデル（Foundation Models）を、Windows からの頼みで動かす。Xcode 26 で組んだときだけ入る
//   {"op":"llm_status"}                                         → 使えるか・使えない理由
//   {"op":"llm","prompt":"…","instructions":"…","temperature":0.7,"max_tokens":300} → 答えの文
// 一つの頼みごとに新しい会話（LanguageModelSession）を作る。前の頼みは覚えていない
#if canImport(FoundationModels)
import Foundation
import FoundationModels

@available(iOS 26.0, *)
enum FMBridge {
    static func run(_ req: [String: Any], statusOnly: Bool) -> [String: Any] {
        let model = SystemLanguageModel.default
        var status: [String: Any] = ["available": false]
        switch model.availability {
        case .available:
            status["available"] = true
        case .unavailable(let reason):
            status["reason"] = String(describing: reason)
        @unknown default:
            status["reason"] = "わからない"
        }
        if statusOnly || status["available"] as? Bool != true { return status }

        let prompt = req["prompt"] as? String ?? ""
        let instructions = req["instructions"] as? String
        var opts = GenerationOptions()
        if let t = req["temperature"] as? Double { opts.temperature = t }
        if let n = req["max_tokens"] as? Int { opts.maximumResponseTokens = n }

        // 受け口は同期で答えるので、ここで答えが出るまで待つ
        let done = DispatchSemaphore(value: 0)
        var out: [String: Any] = [:]
        let t0 = Date()
        Task {
            do {
                let session = instructions.map { LanguageModelSession(instructions: $0) } ?? LanguageModelSession()
                let r = try await session.respond(to: prompt, options: opts)
                out = ["text": r.content]
            } catch {
                out = ["error": "答えを作れない：\(error)"]
            }
            done.signal()
        }
        done.wait()
        out["ms"] = Int(Date().timeIntervalSince(t0) * 1000)
        out["available"] = true
        return out
    }
}
#endif
