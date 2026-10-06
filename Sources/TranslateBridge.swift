// iOS 26 の翻訳（Translation）で、画面を出さずに文を訳す。Xcode 26 で組んだときだけ入る。
// 使う言語は iPhone に入っている必要がある（設定 → アプリ → 翻訳 → ダウンロード済みの言語）。入っていなければ失敗を返す
#if compiler(>=6.2) && canImport(Translation)
import Foundation
import Translation

@available(iOS 26.0, *)
enum TranslateBridge {
    static func run(text: String, from: String, to: String) -> [String: Any] {
        let done = DispatchSemaphore(value: 0)
        var out: [String: Any] = [:]
        Task {
            do {
                let session = TranslationSession(installedSource: Locale.Language(identifier: from), target: Locale.Language(identifier: to))
                let r = try await session.translate(text)
                out = ["text": r.targetText]
            } catch {
                out = ["error": "訳せない：\(error)。iPhone の設定で \(from) と \(to) の言語をダウンロードしてあるか確かめる"]
            }
            done.signal()
        }
        if done.wait(timeout: .now() + 120) == .timedOut { return ["error": "2 分で終わらなかった"] }
        return out
    }
}
#endif
