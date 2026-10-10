import Foundation

// 画面の言葉。設定の「言語」で 自動・日本語・English を選ぶ。自動は iPhone の言語に合わせる。
// 記録（vk: の行など）は日本語のまま。画面の言葉は仮で、鶴田さんがあとで直す
enum AppLang: String, CaseIterable, Identifiable {
    case auto, ja, en
    var id: String { rawValue }
    var label: String {
        switch self {
        case .auto: return L("自動", "Auto")
        case .ja: return "日本語"
        case .en: return "English"
        }
    }
}

func isJapanese() -> Bool {
    switch AppLang(rawValue: UserDefaults.standard.string(forKey: "lang") ?? "auto") ?? .auto {
    case .ja: return true
    case .en: return false
    case .auto: return (Locale.preferredLanguages.first ?? "en").hasPrefix("ja")
    }
}

func L(_ ja: String, _ en: String) -> String { isJapanese() ? ja : en }
