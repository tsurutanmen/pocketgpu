// iOS 26 の翻訳（Translation）で、画面を出さずに文を訳す。Xcode 26 で組んだときだけ入る。
// 使う言語は iPhone に入っている必要がある（設定 → アプリ → 翻訳 → ダウンロード済みの言語）。入っていなければ失敗を返す
// 0.7.60 で直した（work/fix1012）：
//   10/12 の実機で、字幕の最初の 1 文だけ unsupportedSourceLanguage で訳せず、2 文目からは訳せた。前は 1 文ごとに
//   TranslationSession を作り直していて、翻訳の準備が終わる前の 1 文目が失敗したとみる。そこで：
//   - 言語の組ごとに TranslationSession を 1 つ作って使い回す
//   - 初めて使う組は、先に準備する：isReady（iOS 26。言語が入っていて訳せる）が true になるのを待ち、短い文を 1 回訳して温める。
//     prepareTranslation（iOS 18）は、ダウンロードの許可を人に聞くための物で、画面を持たないこの使い方（installedSource）では役に立たない
//   - 準備中に来た文は、組ごとの錠（gate）で準備が終わるのを待ってから訳す
//   - 準備のあとでも、準備の遅れに見える失敗（unsupportedSourceLanguage など）は、0.5 秒・1.5 秒おいて訳し直す
//   - 失敗の答えは短くする：{"error": 短い理由, "code": 種類, "detail": 詳しい理由}。前の livecap は error をそのまま字幕に出していた
//   PC の livecap は字幕を始めるときに {"op":"translate_prepare"} を送り、最初の文より先に準備を済ませる（AppleFeatures.swift）
// 10/13 に直した（work/fix1013）：
//   時間切れ（訳すのは 2 分・準備は 60 秒）で待つのをやめたあとも、見捨てた Task は動き続け、gate を持たずに session と ready を書いていた。
//   それが次の頼み（gate を持っている）と競り合う。そこで組ごとに世代（gen）を持ち、時間切れで世代を進める。
//   中の値は錠（mu）の中で読み書きし、書くのは、その頼みを始めたときの世代が今の世代と同じ Task だけにする
#if compiler(>=6.2) && canImport(Translation)
import Foundation
import Translation

@available(iOS 26.0, *)
enum TranslateBridge {
    // 言語の組ごとの状態。頼みは gate で 1 つずつにする。中の値は mu の中で触り、書けるのは今の世代（gen）の Task だけ
    private final class Pair: @unchecked Sendable {
        let from: String, to: String
        let gate = DispatchSemaphore(value: 1)
        private let mu = NSLock()
        private var gen = 0
        private var cur: TranslationSession?
        private var ready = false
        private var notInstalledAt: Date? // 言語が入っていなかった時刻。そこから 30 秒は、準備を待たずにすぐ失敗を返す
        init(from: String, to: String) { self.from = from; self.to = to }

        // gate を持っている頼みが、Task を始める前に読む
        var generation: Int { mu.lock(); defer { mu.unlock() }; return gen }
        var isPrepared: Bool { mu.lock(); defer { mu.unlock() }; return ready }
        var notInstalledSince: Date? { mu.lock(); defer { mu.unlock() }; return notInstalledAt }

        // 時間切れ（gate を持ったまま呼ぶ）。世代を進めて見捨てた Task に書かせず、次は作り直して準備から
        func abandon() {
            mu.lock(); defer { mu.unlock() }
            gen += 1
            cur = nil
            ready = false
        }

        // その世代の TranslationSession。無ければ作る。見捨てた世代には nil を返す（もう何もしない）
        func sessionFor(_ g: Int) -> TranslationSession? {
            mu.lock(); defer { mu.unlock() }
            guard g == gen else { return nil }
            if let s = cur { return s }
            let s = TranslationSession(installedSource: Locale.Language(identifier: from), target: Locale.Language(identifier: to))
            cur = s
            return s
        }

        func setReady(_ g: Int) {
            mu.lock(); defer { mu.unlock() }
            if g == gen { ready = true }
        }

        func setNotInstalled(_ g: Int, _ at: Date?) {
            mu.lock(); defer { mu.unlock() }
            if g == gen { notInstalledAt = at }
        }

        // 言語が消された。次は作り直して準備から
        func reset(_ g: Int) {
            mu.lock(); defer { mu.unlock() }
            if g == gen { cur = nil; ready = false }
        }
    }

    private static let lock = NSLock()
    private static var pairs: [String: Pair] = [:]

    private static func pair(_ from: String, _ to: String) -> Pair {
        lock.lock(); defer { lock.unlock() }
        let key = from + ">" + to
        if let p = pairs[key] { return p }
        let p = Pair(from: from, to: to)
        pairs[key] = p
        return p
    }

    // 1 文を訳す。準備がまだなら、準備してから訳す（言語が入るのを 15 秒まで待つ）
    static func run(text: String, from: String, to: String) -> [String: Any] {
        let p = pair(from, to)
        p.gate.wait()
        defer { p.gate.signal() }
        let g = p.generation
        let done = DispatchSemaphore(value: 0)
        var out: [String: Any] = [:]
        Task {
            out = await translate(p, g, text)
            done.signal()
        }
        if done.wait(timeout: .now() + 120) == .timedOut {
            // 止まった物は使わない。世代を進めて、見捨てた Task がこのあとの頼みの状態を書かないようにし、次は作り直して準備から
            p.abandon()
            return fail(p, code: "timeout", L("時間内に終わりませんでした", "It did not finish in time"), detail: "2 分で終わらなかった")
        }
        return out
    }

    // 準備だけする（livecap が字幕を始めるときに呼ぶ）。言語が入るのを 20 秒まで待つ
    static func prepare(from: String, to: String) -> [String: Any] {
        let p = pair(from, to)
        p.gate.wait()
        defer { p.gate.signal() }
        let g = p.generation
        let done = DispatchSemaphore(value: 0)
        var out: [String: Any] = [:]
        Task {
            out = await warmUp(p, g, wait: 20)
            done.signal()
        }
        if done.wait(timeout: .now() + 60) == .timedOut {
            p.abandon()
            return fail(p, code: "timeout", L("時間内に終わりませんでした", "It did not finish in time"), detail: "準備が 60 秒で終わらなかった")
        }
        return out
    }

    // 見捨てた世代の Task が、続きをやめるときに返す（頼んだ側はもう待っていないので、誰にも届かない）
    private static func abandoned(_ p: Pair) -> [String: Any] {
        fail(p, code: "timeout", L("時間内に終わりませんでした", "It did not finish in time"), detail: "時間切れのあとで止めた")
    }

    // 言語が入って訳せる状態（isReady）になるのを wait 秒まで待ち、短い文を 1 回訳して温める
    private static func warmUp(_ p: Pair, _ g: Int, wait: Double) async -> [String: Any] {
        let t0 = Date()
        guard let s = p.sessionFor(g) else { return abandoned(p) }
        var installed = await s.isReady
        while !installed && Date().timeIntervalSince(t0) < wait {
            try? await Task.sleep(nanoseconds: 500_000_000)
            installed = await s.isReady
        }
        if !installed {
            p.setNotInstalled(g, Date())
            return fail(p, code: "not_installed", L("言語データがダウンロードされていません", "The language data is not downloaded"),
                        detail: "isReady が \(Int(wait)) 秒 false のまま。\(p.from) と \(p.to) の言語が入っていない")
        }
        p.setNotInstalled(g, nil)
        // 温める。ここで失敗しても準備は終わったことにする（本番の文は translate が訳し直す）
        var warm = "ok"
        for delay in [0.0, 0.5, 1.0] {
            if delay > 0 { try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000)) }
            do {
                _ = try await s.translate(sample(p.from))
                warm = "ok"
                break
            } catch {
                warm = "\(error)"
                if !transient(error) { break }
            }
        }
        p.setReady(g)
        return ["ok": true, "ready": true, "warm": warm, "wait_ms": Int(Date().timeIntervalSince(t0) * 1000)]
    }

    private static func translate(_ p: Pair, _ g: Int, _ text: String) async -> [String: Any] {
        if !p.isPrepared {
            if let at = p.notInstalledSince, Date().timeIntervalSince(at) < 30 {
                return fail(p, code: "not_installed", L("言語データがダウンロードされていません", "The language data is not downloaded"),
                            detail: "言語が入っていない。\(Int(Date().timeIntervalSince(at))) 秒前にチェックした")
            }
            let r = await warmUp(p, g, wait: 15)
            if r["error"] != nil { return r }
        }
        guard let s = p.sessionFor(g) else { return abandoned(p) }
        var last: Error?
        for delay in [0.0, 0.5, 1.5] {
            if delay > 0 { try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000)) }
            do {
                let r = try await s.translate(text)
                return ["text": r.targetText]
            } catch {
                last = error
                if !transient(error) { break }
            }
        }
        if let e = last, TranslationError.notInstalled ~= e {
            // 言語が消された。次は作り直して準備から（見捨てた世代なら何もしない）
            p.reset(g)
        }
        return failure(p, last)
    }

    // 準備の遅れで起きうる失敗。少し待って訳し直す
    private static func transient(_ e: Error) -> Bool {
        TranslationError.unsupportedSourceLanguage ~= e || TranslationError.unsupportedLanguagePairing ~= e
            || TranslationError.unsupportedTargetLanguage ~= e || TranslationError.notInstalled ~= e
            || TranslationError.internalError ~= e
    }

    // 温めに訳す短い文。元の言語の文にする（言語の見分けで、ほかの言語と取られないように）
    private static func sample(_ lang: String) -> String {
        let base = String(lang.split(separator: "-").first ?? Substring(lang)) // ja-JP → ja
        switch base {
        case "ja": return "こんにちは。"
        case "zh": return "你好。"
        case "ko": return "안녕하세요."
        case "fr": return "Bonjour."
        case "de": return "Guten Tag."
        case "es": return "Hola."
        case "it": return "Ciao."
        case "pt": return "Olá."
        default: return "Hello."
        }
    }

    private static func failure(_ p: Pair, _ e: Error?) -> [String: Any] {
        guard let e else {
            return fail(p, code: "failed", L("翻訳の処理でエラーが起きました", "An error occurred in translation"), detail: "")
        }
        if TranslationError.notInstalled ~= e {
            return fail(p, code: "not_installed", L("言語データがダウンロードされていません", "The language data is not downloaded"), detail: "\(e)")
        }
        if TranslationError.nothingToTranslate ~= e {
            return fail(p, code: "empty", L("翻訳する文字がありません", "There is no text to translate"), detail: "\(e)")
        }
        let unsupported = TranslationError.unsupportedSourceLanguage ~= e || TranslationError.unsupportedLanguagePairing ~= e
            || TranslationError.unsupportedTargetLanguage ~= e
        return fail(p, code: unsupported ? "unsupported" : "failed", L("翻訳の処理でエラーが起きました", "An error occurred in translation"), detail: "\(e)")
    }

    // error は短い理由だけ。前の livecap はこれを「翻訳できませんでした: 」のあとに出す。詳しい理由は detail に入れ、記録に書く
    private static func fail(_ p: Pair, code: String, _ msg: String, detail: String) -> [String: Any] {
        ["error": msg, "code": code, "detail": detail, "from": p.from, "to": p.to]
    }
}
#endif
