// WhisperKit（Argmax・MIT。今は argmax-oss-swift の中の 1 つ）で、音声のファイルを Whisper large-v3 turbo などで文字にする。
// project.yml に argmax-oss-swift の Swift Package を足して組んだときだけ入る（work/ai_apps/project_yml.patch）。
//   {"op":"asr","engine":"whisperkit","lang":"ja-JP","ext":"wav","model":"large-v3-v20240930_626MB","words":false}
//   → {"text","segments":[{"text","start","end"(,"words")}],"engine":"WhisperKit","model","duration"}
// モデルは初回に Hugging Face の argmaxinc/whisperkit-coreml から iPhone が落とす（large-v3-v20240930_626MB で約 0.6 GB）。
//   置き場は Application Support/whisperkit（バックアップ対象外）。2 回目からは落とさない。最初の読み込みは Neural Engine 用の仕上げで長い
// 流しっぱなしの字幕には使わない（LiveASR.swift の SpeechAnalyzer を使う）。WhisperKit は「あとで清書」と議事録の文字起こし向け
#if canImport(WhisperKit)
import Foundation
import WhisperKit

enum WhisperKitBridge {
    static let defaultModel = "large-v3-v20240930_626MB"
    private static let lock = NSLock()
    private static var pipe: WhisperKit?
    private static var loaded: String?

    static func transcribe(url: URL, lang: String, model: String?, words: Bool) -> [String: Any] {
        lock.lock()
        defer { lock.unlock() }
        let name = model ?? defaultModel
        let code = String(lang.prefix(2)).lowercased() // WhisperKit の言語は "ja"・"en"
        do {
            return try AIShared.blocking(timeout: 3600) { () async throws -> [String: Any] in
                let t0 = Date()
                var loadMs = 0
                if WhisperKitBridge.pipe == nil || WhisperKitBridge.loaded != name {
                    WhisperKitBridge.pipe = nil
                    WhisperKitBridge.pipe = try await WhisperKit(WhisperKitConfig(model: name, downloadBase: AIShared.store("whisperkit")))
                    WhisperKitBridge.loaded = name
                    loadMs = Int(Date().timeIntervalSince(t0) * 1000)
                }
                guard let wk = WhisperKitBridge.pipe else { throw AIError("WhisperKit を用意できない") }
                // skipSpecialTokens：<|ja|> などの印を文に残さない。vad：無音で区切って長い音声を流す
                let opts = DecodingOptions(language: code, skipSpecialTokens: true, wordTimestamps: words, chunkingStrategy: .vad)
                let results = try await wk.transcribe(audioPath: url.path, decodeOptions: opts)
                var segs: [[String: Any]] = []
                for r in results {
                    for s in r.segments {
                        var d: [String: Any] = ["text": s.text.trimmingCharacters(in: .whitespaces),
                                                "start": AIShared.fin(Double(s.start)), "end": AIShared.fin(Double(s.end))]
                        if words, let ws = s.words {
                            d["words"] = ws.map { ["word": $0.word, "start": AIShared.fin(Double($0.start)), "end": AIShared.fin(Double($0.end))] as [String: Any] }
                        }
                        segs.append(d)
                    }
                }
                let text = results.map { $0.text.trimmingCharacters(in: .whitespaces) }.joined(separator: code == "ja" ? "" : " ")
                let duration = (segs.last?["end"] as? Double) ?? 0
                return ["text": text, "segments": segs, "engine": "WhisperKit", "model": name, "duration": duration, "load_ms": loadMs]
            }
        } catch {
            return ["error": "WhisperKit で文字にできない：\(error.localizedDescription)"]
        }
    }
}
#endif
