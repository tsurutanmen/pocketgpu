// TTSKit（Argmax・MIT。argmax-oss-swift の中の 1 つ）で、Qwen3-TTS 0.6B（Apache-2.0）を Core ML で動かして読み上げる。日本語の話者は ono-anna。
// project.yml に argmax-oss-swift 1.1.1 を足して組んだときだけ入る（work/ai_pkg/stage1.patch）。使う API は v1.1.1 のソースで確かめた（work/ai_pkg/api_check.md）
//   {"op":"tts","engine":"ttskit","text":"…","speaker":"ono-anna","language":"japanese"}
//   → {"wav_b64"（16 ビット・モノラル）,"sample_rate","voice","engine","gen_ms","audio_s"}
// モデルは初回に Hugging Face の argmaxinc/ttskit-coreml から iPhone が落とす（0.6B の標準の 6 部品で 1,091 MB、ほかにトークナイザー）。
//   置き場は Application Support/ttskit（バックアップ対象外）。2 回目からは手元の物を確かめて使う（TTSKit が自分で見る。電波が無くてもよい）
// iOS 17 でも組めるが、読み上げの部品（SpeechDecoder・MultiCodeDecoder）は iOS 18 からなので、iOS 17 では先に断る
// 計算の場所は TTSKit の標準のまま（表引きは CPU、ほかは Neural Engine。GPU は使わない）
#if canImport(TTSKit)
import Foundation
import TTSKit

enum TTSKitBridge {
    static let needMB = 1400 // 読み込みの前に要る空き（仮。重さ 1,091 MB と仕上げの分）
    private static let lock = NSLock()
    private static var tts: TTSKit?

    static func speak(text: String, speaker: String?, language: String?) -> [String: Any] {
        guard #available(iOS 18.0, *) else { return ["error": "TTSKit は iOS 18 から", "code": "notSupported"] }
        lock.lock()
        defer { lock.unlock() }
        let voice = speaker ?? "ono-anna"
        let lang = language ?? "japanese"
        do {
            if tts == nil { try AIShared.checkMemory(needMB: needMB, what: "TTSKit の読み込み") }
            // 初回は 1,091 MB を落として仕上げるので長く待つ。PC が先に切っても、ここは鍵を持ったまま読み終えるまで待つので、
            // 次の頼みは鍵が空くのを待ってから読み込み済みを使う（二重に落とさない）
            return try AIShared.blocking(timeout: 1800) { () async throws -> [String: Any] in
                if TTSKitBridge.tts == nil {
                    // prewarm：部品を 1 つずつ仕上げてから読む。読み込みの山が低くなる
                    TTSKitBridge.tts = try await TTSKit(TTSKitConfig(model: .qwen3TTS_0_6b, downloadBase: AIShared.store("ttskit"), prewarm: true))
                }
                guard let k = TTSKitBridge.tts else { throw AIError("TTSKit を用意できない") }
                let t0 = Date()
                // 標準の concurrentWorkerCount 0 は、文に分けた塊を全部同時に作り、塊ごとに KV キャッシュを持つ。
                // 長い文だとメモリが膨らむので、Vulkan の本業と並ぶことを考えて 2 本までにする（仮）
                let r = try await k.generate(text: text, voice: voice, language: lang, options: GenerationOptions(concurrentWorkerCount: 2))
                let wav = AIShared.wav16(r.audio, rate: r.sampleRate)
                return ["wav_b64": wav.base64EncodedString(), "sample_rate": r.sampleRate, "voice": voice, "format": "s16",
                        "engine": "TTSKit Qwen3-TTS 0.6B", "gen_ms": Int(Date().timeIntervalSince(t0) * 1000),
                        "audio_s": AIShared.fin(r.audioDuration)]
            }
        } catch {
            return AIShared.failure("TTSKit で読み上げられない：", error)
        }
    }
}
#endif
