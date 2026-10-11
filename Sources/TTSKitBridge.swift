// TTSKit（Argmax・MIT。argmax-oss-swift の中の 1 つ）で、Qwen3-TTS 0.6B（Apache-2.0）を Core ML で動かして読み上げる。日本語の話者は ono-anna。
// project.yml に argmax-oss-swift の Swift Package を足して組んだときだけ入る（work/ai_apps/project_yml.patch）。
//   {"op":"tts","engine":"ttskit","text":"…","speaker":"ono-anna","language":"japanese"}
//   → {"wav_b64"（16 ビット・モノラル）,"sample_rate","voice","engine","gen_ms","audio_s"}
// モデルは初回に Hugging Face の argmaxinc/ttskit-coreml から iPhone が落とす（README では 0.6B で約 1 GB）。TTSKit の README は iOS 18 から
// 置き場は Application Support/ttskit（バックアップ対象外）
#if canImport(TTSKit)
import Foundation
import TTSKit

enum TTSKitBridge {
    private static let lock = NSLock()
    private static var tts: TTSKit?

    static func speak(text: String, speaker: String?, language: String?) -> [String: Any] {
        guard #available(iOS 18.0, *) else { return ["error": "TTSKit は iOS 18 から"] }
        lock.lock()
        defer { lock.unlock() }
        let voice = speaker ?? "ono-anna"
        let lang = language ?? "japanese"
        do {
            return try AIShared.blocking(timeout: 900) { () async throws -> [String: Any] in
                if TTSKitBridge.tts == nil {
                    TTSKitBridge.tts = try await TTSKit(TTSKitConfig(model: .qwen3TTS_0_6b, downloadBase: AIShared.store("ttskit")))
                }
                guard let k = TTSKitBridge.tts else { throw AIError("TTSKit を用意できない") }
                let t0 = Date()
                let r = try await k.generate(text: text, voice: voice, language: lang)
                let wav = AIShared.wav16(r.audio, rate: r.sampleRate)
                return ["wav_b64": wav.base64EncodedString(), "sample_rate": r.sampleRate, "voice": voice, "format": "s16",
                        "engine": "TTSKit Qwen3-TTS 0.6B", "gen_ms": Int(Date().timeIntervalSince(t0) * 1000),
                        "audio_s": AIShared.fin(r.audioDuration)]
            }
        } catch {
            return ["error": "TTSKit で読み上げられない：\(error.localizedDescription)"]
        }
    }
}
#endif
