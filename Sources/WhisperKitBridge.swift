// WhisperKit（Argmax・MIT。今は argmax-oss-swift の中の 1 つ）で、音声のファイルを Whisper large-v3 turbo などで文字にする。
// project.yml に argmax-oss-swift 1.1.1 を足して組んだときだけ入る（work/ai_pkg/stage1.patch）。使う API は v1.1.1 のソースで確かめた（work/ai_pkg/api_check.md）
//   {"op":"asr","engine":"whisperkit","lang":"ja-JP","ext":"wav","model":"large-v3-v20240930_626MB","words":false}
//   → {"text","segments":[{"text","start","end"(,"words")}],"engine":"WhisperKit","model","duration","load_ms","source"}
//   source：hub＝Hugging Face から落とした・local＝手元の置き場から読んだ・loaded＝読み込み済みを使った
// モデルは初回に Hugging Face の argmaxinc/whisperkit-coreml から iPhone が落とす（large-v3-v20240930_626MB は 627 MB、ほかにトークナイザーが数 MB）。
//   置き場は Application Support/whisperkit/models/argmaxinc/whisperkit-coreml/openai_whisper-<名前>（バックアップ対象外）。
//   WhisperKit は model の名前だけを渡すと、毎回 Hugging Face にファイルの一覧を聞く（電波が無いと失敗する）。2 回目からは手元の置き場を直に渡す
// 計算の場所：音の特徴（MelSpectrogram）は CPU、AudioEncoder と TextDecoder は Neural Engine。GPU は Vulkan の本業に残す（標準は Mel が GPU）
// 長い音声（32 MB を超えるファイル）は 120 秒ずつ読む（v1.1.0 の .incremental。1 時間の音声を全部 float にすると 230 MB）
// 流しっぱなしの字幕には使わない（LiveASR.swift の SpeechAnalyzer を使う）。WhisperKit は「あとで清書」と議事録の文字起こし向け
#if canImport(WhisperKit)
import CoreML
import Foundation
import WhisperKit

enum WhisperKitBridge {
    static let defaultModel = "large-v3-v20240930_626MB"
    static let needMB = 1000 // 読み込みの前に要る空き（仮。重さ 627 MB と Neural Engine 向けの仕上げの分）
    private static let lock = NSLock()
    private static var pipe: WhisperKit?
    private static var loaded: String?

    // 前に落とし終えた物の置き場。3 つの部品の重さのファイルがそろっているときだけ使う
    // （落とし途中のファイルは別の場所に置かれ、終わってから移されるので、ここにあれば揃っている）
    static func localFolder(_ base: URL, _ name: String) -> URL? {
        let repo = base.appendingPathComponent("models", isDirectory: true)
            .appendingPathComponent("argmaxinc", isDirectory: true)
            .appendingPathComponent("whisperkit-coreml", isDirectory: true)
        let fm = FileManager.default
        // 置き場の名前は openai_whisper-<名前>（distil の物は distil-whisper_<名前>）
        for dir in ["openai_whisper-\(name)", "distil-whisper_\(name)", name] {
            let d = repo.appendingPathComponent(dir, isDirectory: true)
            let ok = ["MelSpectrogram", "AudioEncoder", "TextDecoder"].allSatisfy {
                fm.fileExists(atPath: d.appendingPathComponent("\($0).mlmodelc/weights/weight.bin").path)
            }
            if ok { return d }
        }
        return nil
    }

    static func transcribe(url: URL, lang: String, model: String?, words: Bool) -> [String: Any] {
        lock.lock()
        defer { lock.unlock() }
        let name = model ?? defaultModel
        let code = String(lang.prefix(2)).lowercased() // WhisperKit の言語は "ja"・"en"
        let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        do {
            if pipe == nil || loaded != name { try AIShared.checkMemory(needMB: needMB, what: "WhisperKit の読み込み") }
            return try AIShared.blocking(timeout: 3600) { () async throws -> [String: Any] in
                let t0 = Date()
                var loadMs = 0
                var source = "loaded"
                if WhisperKitBridge.pipe == nil || WhisperKitBridge.loaded != name {
                    WhisperKitBridge.pipe = nil
                    WhisperKitBridge.loaded = nil
                    let base = AIShared.store("whisperkit")
                    let local = WhisperKitBridge.localFolder(base, name)
                    source = local == nil ? "hub" : "local"
                    // prewarm：部品を 1 つずつ仕上げてから読む。読み込みの山が低くなる（仕上げ済みなら 1 秒ほど延びるだけ）
                    let cfg = WhisperKitConfig(model: name, downloadBase: base, modelFolder: local?.path,
                                               computeOptions: ModelComputeOptions(melCompute: .cpuOnly),
                                               prewarm: true, load: true, download: local == nil)
                    WhisperKitBridge.pipe = try await WhisperKit(cfg)
                    WhisperKitBridge.loaded = name
                    loadMs = Int(Date().timeIntervalSince(t0) * 1000)
                }
                guard let wk = WhisperKitBridge.pipe else { throw AIError("WhisperKit を用意できない") }
                // skipSpecialTokens：<|ja|> などの印を文に残さない。vad：無音で区切って長い音声を流す
                let opts = DecodingOptions(language: code, skipSpecialTokens: true, wordTimestamps: words, chunkingStrategy: .vad)
                // 少しずつ読んでも、区切りは全部読んだときと同じ（README）。秒は音声の頭からの秒に直して返ってくる
                let mode: AudioInputOptions.AudioLoadingMode =
                    size > (32 << 20) ? .incremental(chunkDurationSeconds: 120, maxBufferedChunks: 2) : .fullFile
                let results = try await wk.transcribe(audioPath: url.path, audioInputOptions: AudioInputOptions(audioLoadingMode: mode),
                                                      decodeOptions: opts)
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
                return ["text": text, "segments": segs, "engine": "WhisperKit", "model": name, "duration": duration,
                        "load_ms": loadMs, "source": source]
            }
        } catch {
            return AIShared.failure("WhisperKit で文字にできない：", error)
        }
    }
}
#endif
