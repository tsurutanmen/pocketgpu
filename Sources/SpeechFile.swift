// 音声のファイルを iOS 26 の SpeechAnalyzer で文字にし、区切り（文）ごとの始まりと終わりの秒も返す。
// SpeechBridge.transcribe と同じ流れに、区切りの時刻（result.range）と、言葉ごとの時刻（audioTimeRange の属性）を足した物。
// OpenAI の /v1/audio/transcriptions の verbose_json・srt・vtt と、words の時刻に使う（work/ai_apps/openai_server.py）
//   {"op":"asr","lang":"ja-JP","ext":"wav","timestamps":true,"words":false} → {"text","segments":[{"text","start","end"}],"duration","engine","locale"}
#if compiler(>=6.2)
import AVFoundation
import CoreMedia
import Foundation
import Speech

@available(iOS 26.0, *)
enum SpeechFile {
    static func transcribe(url: URL, lang: String, words: Bool) -> [String: Any] {
        do {
            return try AIShared.blocking(timeout: 1800) { () async throws -> [String: Any] in
                guard let locale = await SpeechTranscriber.supportedLocale(equivalentTo: Locale(identifier: lang)) else {
                    throw AIError("\(lang) は新しい音声認識に無い")
                }
                let transcriber = SpeechTranscriber(locale: locale, transcriptionOptions: [], reportingOptions: [],
                                                    attributeOptions: words ? [.audioTimeRange] : [])
                if let r = try await AssetInventory.assetInstallationRequest(supporting: [transcriber]) {
                    try await r.downloadAndInstall()
                }
                let analyzer = SpeechAnalyzer(modules: [transcriber])
                // Task の答えは Sendable な型にする（[String: Any] は Sendable でない）
                let collect = Task { () throws -> [Seg] in
                    var segs: [Seg] = []
                    for try await r in transcriber.results {
                        segs.append(Seg(text: String(r.text.characters), start: AIShared.fin(r.range.start.seconds),
                                        end: AIShared.fin(r.range.end.seconds), words: words ? SpeechFile.wordTimes(r.text) : []))
                    }
                    return segs
                }
                let file = try AVAudioFile(forReading: url)
                let duration = Double(file.length) / file.fileFormat.sampleRate
                if let last = try await analyzer.analyzeSequence(from: file) {
                    try await analyzer.finalizeAndFinish(through: last)
                } else {
                    await analyzer.cancelAndFinishNow()
                }
                let segs = try await collect.value
                let list: [[String: Any]] = segs.map { s in
                    var d: [String: Any] = ["text": s.text, "start": s.start, "end": s.end]
                    if words { d["words"] = s.words.map { ["word": $0.word, "start": $0.start, "end": $0.end] as [String: Any] } }
                    return d
                }
                return ["text": segs.map { $0.text }.joined(), "segments": list,
                        "duration": AIShared.fin(duration), "engine": "SpeechAnalyzer", "locale": locale.identifier]
            }
        } catch {
            return ["error": "新しい音声認識が失敗：\(error.localizedDescription)"]
        }
    }

    struct Word: Sendable {
        let word: String
        let start: Double
        let end: Double
    }

    struct Seg: Sendable {
        let text: String
        let start: Double
        let end: Double
        let words: [Word]
    }

    // attributeOptions に .audioTimeRange を入れると、文の中の区切り（run）ごとに音の範囲が付く
    private static func wordTimes(_ text: AttributedString) -> [Word] {
        var out: [Word] = []
        for run in text.runs {
            guard let tr = run[AttributeScopes.SpeechAttributes.TimeRangeAttribute.self] else { continue }
            let w = String(text[run.range].characters)
            if w.trimmingCharacters(in: .whitespaces).isEmpty { continue }
            out.append(Word(word: w, start: AIShared.fin(tr.start.seconds), end: AIShared.fin(tr.end.seconds)))
        }
        return out
    }
}
#endif
