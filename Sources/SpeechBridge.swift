// iOS 26 の新しい音声認識（SpeechAnalyzer と SpeechTranscriber）で、音声のファイルを文字にする。長い音声向け。
// Xcode 26 で組んだときだけ入る。初めての言語は、iPhone が言葉のモデルをダウンロードするので時間がかかる
#if compiler(>=6.2)
import AVFoundation
import Foundation
import Speech

@available(iOS 26.0, *)
enum SpeechBridge {
    static func transcribe(url: URL, lang: String) -> [String: Any] {
        let done = DispatchSemaphore(value: 0)
        var out: [String: Any] = [:]
        Task {
            do {
                guard let locale = await SpeechTranscriber.supportedLocale(equivalentTo: Locale(identifier: lang)) else {
                    out = ["error": "\(lang) は新しい音声認識に無い"]
                    done.signal()
                    return
                }
                let transcriber = SpeechTranscriber(locale: locale, preset: .transcription)
                if let r = try await AssetInventory.assetInstallationRequest(supporting: [transcriber]) {
                    try await r.downloadAndInstall()
                }
                let analyzer = SpeechAnalyzer(modules: [transcriber])
                // 結果は区切りごとに届くので、全部つなぐ
                let collect = Task { () throws -> String in
                    var s = ""
                    for try await r in transcriber.results { s += String(r.text.characters) }
                    return s
                }
                let file = try AVAudioFile(forReading: url)
                if let last = try await analyzer.analyzeSequence(from: file) {
                    try await analyzer.finalizeAndFinish(through: last)
                } else {
                    try? await analyzer.cancelAndFinishNow()
                }
                out = ["text": try await collect.value, "engine": "SpeechAnalyzer", "locale": locale.identifier]
            } catch {
                out = ["error": "新しい音声認識が失敗：\(error)"]
            }
            done.signal()
        }
        if done.wait(timeout: .now() + 900) == .timedOut { return ["error": "15 分で終わらなかった"] }
        return out
    }
}
#endif
