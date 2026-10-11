// 絵を作る（PC の /v1/images/generations・edits から。work/ai_apps/openai_server.py）
//   {"op":"image_styles"} → {"engines":{"playground":{"available","styles":[…]|"reason","code"},"sd":{"available","models":[…]}}}
//   {"op":"image_generate","engine":"playground","prompt","style":"animation","n":1,"source_image":false,"reply_bin":true}
//        中身＝元の絵（source_image のときだけ）。答え {"images":[{"width","height","fmt":"png","nbytes","b64"|"offset"}],"engine","style","ms"}
//        失敗は {"error","code"}。code は ImageCreator.Error の名前（backgroundCreationForbidden など）か unknown_style
//   {"op":"image_generate","engine":"sd","prompt","negative_prompt","steps":20,"seed":1,"guidance":7.5,"n":1,"model":"名前"}
//   {"op":"sd_install","name":"sd21b","parts":{"TextEncoder":"id","Unet":"id","VAEDecoder":"id",…},"vocab_len":n}
//        中身＝vocab.json と merges.txt をつないだ物。parts の id は coreml_put/coreml_compile で送った .mlmodelc（work/ane の CoreMLRemote）。
//        Application Support/ane_models/models/<id>/model.mlmodelc を Application Support/sd_models/<name>/<部品>.mlmodelc に移す
//        （CoreMLRemote の置き場は 8 GiB を超えると古い順に消すので、Stable Diffusion の部品はそこに置いたままにしない）
//
// Image Playground（ImageCreator）：iOS 18.4 から。Apple Intelligence が入っていて、アプリが画面の前にいるときだけ作れる
//   （後ろだと backgroundCreationForbidden）。絵柄は animation・illustration・sketch（availableStyles に出る物だけ）。
//   external（ChatGPT など外の絵柄・iOS 26）は文が外の会社に出ていくので、allow_external:true のときだけ
// Stable Diffusion：apple/ml-stable-diffusion 1.1.1 の Swift Package を入れて組んだときだけ（canImport・work/ai_pkg/stage2.patch）。Neural Engine で動かす。
//   使う API は 1.1.1 のソースで確かめた（work/ai_pkg/api_check.md）。空きメモリが足りなければ code low_memory で断る
import CoreGraphics
import CoreML
import Foundation
import UIKit
#if canImport(ImagePlayground)
import ImagePlayground
#endif
#if canImport(StableDiffusion)
import StableDiffusion
#endif

enum ImageGen {
    private static let lock = NSLock() // 絵は重いので 1 度に 1 つ

    static func styles() -> [String: Any] {
        var pg: [String: Any] = ["available": false]
        #if canImport(ImagePlayground)
        if #available(iOS 18.4, *) {
            do {
                let ids = try AIShared.blocking(timeout: 30) { () async throws -> [String] in
                    let creator = try await ImageCreator()
                    return creator.availableStyles.map { $0.id }
                }
                pg = ["available": true, "styles": ids]
            } catch {
                pg["reason"] = error.localizedDescription
                pg["code"] = playgroundCode(error)
            }
        } else {
            pg["reason"] = "Image Playground は iOS 18.4 から"
        }
        #else
        pg["reason"] = "この版は ImagePlayground を入れずに組んだ"
        #endif
        var sd: [String: Any] = ["available": false]
        #if canImport(StableDiffusion)
        sd = ["available": !sdModels().isEmpty, "models": sdModels()]
        #else
        sd["reason"] = "この版は Stable Diffusion の Swift Package を入れずに組んだ"
        #endif
        return ["engines": ["playground": pg, "sd": sd]]
    }

    static func generate(_ req: [String: Any], _ body: Data) -> [String: Any] {
        let engine = req["engine"] as? String ?? "playground"
        let replyBin = req["reply_bin"] as? Bool ?? false
        guard let prompt = req["prompt"] as? String, !prompt.isEmpty else { return ["error": "prompt が空"] }
        lock.lock()
        defer { lock.unlock() }
        let t0 = Date()
        var imgs: [CGImage] = []
        var info: [String: Any] = ["engine": engine]
        do {
            switch engine {
            case "playground":
                #if canImport(ImagePlayground)
                guard #available(iOS 18.4, *) else { return ["error": "Image Playground は iOS 18.4 から", "code": "notSupported"] }
                let (out, style) = try playground(req, prompt: prompt, body: body)
                imgs = out
                info["style"] = style
                #else
                return ["error": "この版は ImagePlayground を入れずに組んだ", "code": "notSupported"]
                #endif
            case "sd":
                #if canImport(StableDiffusion)
                let (out, model, filtered) = try stableDiffusion(req, prompt: prompt)
                imgs = out
                info["model"] = model
                info["filtered"] = filtered
                #else
                return ["error": "この版は Stable Diffusion の Swift Package を入れずに組んだ", "code": "not_installed"]
                #endif
            default:
                return ["error": "engine は playground か sd：\(engine)"]
            }
        } catch {
            var r: [String: Any] = ["error": "絵を作れない：\(error.localizedDescription)"]
            if let e = error as? AIError, let code = e.code { r["code"] = code }
            #if canImport(ImagePlayground)
            if #available(iOS 18.4, *), let code = playgroundCode(error) { r["code"] = code }
            #endif
            return r
        }
        if imgs.isEmpty { return ["error": "絵が 1 枚もできなかった", "code": "creationFailed"] }
        let (list, blob) = AIShared.packImages(imgs, replyBin: replyBin)
        var r = info
        r["images"] = list
        r["ms"] = Int(Date().timeIntervalSince(t0) * 1000)
        if replyBin {
            r["bin_len"] = blob.count
            r["_bin"] = blob // AppleFeatures.next（AppleFeatures_coreml.diff）が取り出して 2 枠目にする
        }
        return r
    }

    // ---- Image Playground ----
    #if canImport(ImagePlayground)
    @available(iOS 18.4, *)
    private static func playgroundCode(_ error: Error) -> String? {
        guard let e = error as? ImageCreator.Error else { return nil }
        // 例：backgroundCreationForbidden・notSupported・unavailable・unsupportedLanguage（PC はこの名前で HTTP の番号を決める）
        let s = String(describing: e)
        return s.split(separator: "(").first.map(String.init) ?? s
    }

    @available(iOS 18.4, *)
    private static func playground(_ req: [String: Any], prompt: String, body: Data) throws -> ([CGImage], String) {
        let n = max(1, min(4, (req["n"] as? NSNumber)?.intValue ?? 1))
        let want = req["style"] as? String ?? "animation"
        let allowExternal = req["allow_external"] as? Bool ?? false
        var source: CGImage?
        if req["source_image"] as? Bool == true {
            guard let ui = UIImage(data: body), let cg = ui.cgImage else { throw AIError("元の絵を読めない", code: "unsupportedInputImage") }
            source = cg
        }
        return try AIShared.blocking(timeout: 300) { () async throws -> ([CGImage], String) in
            let creator = try await ImageCreator()
            guard let style = ImageGen.pickStyle(want, creator.availableStyles, allowExternal: allowExternal) else {
                let have = creator.availableStyles.map { $0.id }.joined(separator: "・")
                throw AIError("この絵柄は使えない：\(want)（使える物：\(have)。外の絵柄は allow_external が要る）", code: "unknown_style")
            }
            // 長い文は、大事な言葉を取り出させる
            var concepts: [ImagePlaygroundConcept] = [prompt.count > 120 ? .extracted(from: prompt, title: nil) : .text(prompt)]
            if let source { concepts.append(.image(source)) }
            var out: [CGImage] = []
            for try await img in creator.images(for: concepts, style: style, limit: n) {
                out.append(img.cgImage)
            }
            return (out, style.id)
        }
    }

    @available(iOS 18.4, *)
    private static func pickStyle(_ want: String, _ avail: [ImagePlaygroundStyle], allowExternal: Bool) -> ImagePlaygroundStyle? {
        if want == "external" || want == "chatgpt" {
            guard allowExternal else { return nil }
            if #available(iOS 26.0, *) {
                let ext = ImagePlaygroundStyle.externalProvider
                return avail.first { $0.id == ext.id }
            }
            return nil
        }
        let known: [String: ImagePlaygroundStyle] = ["animation": .animation, "illustration": .illustration, "sketch": .sketch]
        if let s = known[want] { return avail.first { $0.id == s.id } }
        return avail.first { $0.id == want }
    }
    #endif

    // ---- Stable Diffusion（Core ML・Neural Engine）----
    #if canImport(StableDiffusion)
    private static var sdPipe: StableDiffusionPipeline?
    private static var sdName: String?
    // 生成の前に要る空き（仮）。SD 2.1 base のパレット化で Unet の重さが 650 MB、TextEncoder が 319 MB。
    // reduceMemory なので 1 度に載るのは 1 部品だが、Unet の展開と途中の計算の分を見ておく
    static let sdNeedMB = 2000

    private static func sdRoot() -> URL { AIShared.store("sd_models") }

    static func sdModels() -> [String] {
        let fm = FileManager.default
        let dirs = (try? fm.contentsOfDirectory(at: sdRoot(), includingPropertiesForKeys: nil)) ?? []
        return dirs.filter { fm.fileExists(atPath: $0.appendingPathComponent("TextEncoder.mlmodelc").path) }
            .map { $0.lastPathComponent }.sorted()
    }

    private static func stableDiffusion(_ req: [String: Any], prompt: String) throws -> ([CGImage], String, Int) {
        let models = sdModels()
        guard let name = (req["model"] as? String) ?? models.first, models.contains(name) else {
            throw AIError("Stable Diffusion のモデルが入っていない（PC の work/ai_apps/sd_push.py で入れる）", code: "not_installed")
        }
        // reduceMemory では生成のたびに部品を読み直すので、毎回見る
        try AIShared.checkMemory(needMB: sdNeedMB, what: "Stable Diffusion の絵づくり")
        if sdPipe == nil || sdName != name {
            sdPipe = nil
            let cfg = MLModelConfiguration()
            cfg.computeUnits = .cpuAndNeuralEngine // Neural Engine。GPU は本業（Vulkan の計算の受け手）に残す
            // reduceMemory：部品を使う直前に読み、使ったら手放す（Apple の README は iOS では true を勧める）
            let p = try StableDiffusionPipeline(resourcesAt: sdRoot().appendingPathComponent(name, isDirectory: true),
                                                controlNet: [], configuration: cfg, disableSafety: false, reduceMemory: true)
            try p.loadResources()
            sdPipe = p
            sdName = name
        }
        guard let pipe = sdPipe else { throw AIError("Stable Diffusion を用意できない") }
        var c = StableDiffusionPipeline.Configuration(prompt: prompt)
        c.negativePrompt = req["negative_prompt"] as? String ?? ""
        c.stepCount = max(1, min(50, (req["steps"] as? NSNumber)?.intValue ?? 20))
        c.seed = UInt32(truncatingIfNeeded: (req["seed"] as? NSNumber)?.int64Value ?? Int64.random(in: 0..<Int64(UInt32.max)))
        c.guidanceScale = (req["guidance"] as? NSNumber)?.floatValue ?? 7.5
        c.imageCount = max(1, min(4, (req["n"] as? NSNumber)?.intValue ?? 1))
        c.schedulerType = .dpmSolverMultistepScheduler // 少ない段数で絵になる
        c.disableSafety = false
        let out = try pipe.generateImages(configuration: c)
        let imgs = out.compactMap { $0 } // nil は安全確認で消された絵
        return (imgs, name, out.count - imgs.count)
    }
    #endif

    static func sdInstall(_ req: [String: Any], _ body: Data) -> [String: Any] {
        let fm = FileManager.default
        let okName = { (s: String) -> Bool in
            !s.isEmpty && s.count <= 64 && s.allSatisfy { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" || $0 == "." } && !s.hasPrefix(".")
        }
        guard let name = req["name"] as? String, okName(name) else { return ["error": "name は英数字と - _ . で 64 文字まで"] }
        guard let parts = req["parts"] as? [String: String] else { return ["error": "parts が無い"] }
        let allowed: Set<String> = ["TextEncoder", "Unet", "UnetChunk1", "UnetChunk2", "VAEDecoder", "VAEEncoder", "SafetyChecker"]
        let hasUnet = parts["Unet"] != nil || (parts["UnetChunk1"] != nil && parts["UnetChunk2"] != nil)
        guard parts["TextEncoder"] != nil, parts["VAEDecoder"] != nil, hasUnet else { return ["error": "TextEncoder・Unet（か UnetChunk1/2）・VAEDecoder が要る"] }
        let vocabLen = (req["vocab_len"] as? NSNumber)?.intValue ?? -1
        guard vocabLen > 0, vocabLen < body.count else { return ["error": "中身に vocab.json と merges.txt が無い"] }
        let ane = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("ane_models", isDirectory: true).appendingPathComponent("models", isDirectory: true)
        let dest = AIShared.store("sd_models").appendingPathComponent(name, isDirectory: true)
        do {
            try fm.createDirectory(at: dest, withIntermediateDirectories: true)
            for (part, id) in parts {
                guard allowed.contains(part) else { return ["error": "知らない部品：\(part)"] }
                guard id.count == 16, id.allSatisfy({ $0.isHexDigit }) else { return ["error": "\(part) の id が違う：\(id)"] }
                let src = ane.appendingPathComponent(id, isDirectory: true).appendingPathComponent("model.mlmodelc", isDirectory: true)
                let dst = dest.appendingPathComponent(part + ".mlmodelc", isDirectory: true)
                if !fm.fileExists(atPath: src.path) {
                    if fm.fileExists(atPath: dst.path) { continue } // もう移してある
                    return ["error": "\(part)（id \(id)）が iPhone に無い。先に coreml_remote.py push で送る"]
                }
                if fm.fileExists(atPath: dst.path) { try fm.removeItem(at: dst) }
                try fm.moveItem(at: src, to: dst)
                try? fm.removeItem(at: ane.appendingPathComponent(id, isDirectory: true)) // 空になった置き場（meta.json）も消す
            }
            try body.prefix(vocabLen).write(to: dest.appendingPathComponent("vocab.json"))
            try body.suffix(from: body.startIndex + vocabLen).write(to: dest.appendingPathComponent("merges.txt"))
        } catch {
            return ["error": "置けない：\(error.localizedDescription)"]
        }
        #if canImport(StableDiffusion)
        lock.lock()
        if sdName == name { sdPipe = nil; sdName = nil } // 入れ替えたので読み直す
        lock.unlock()
        #endif
        return ["ok": true, "name": name, "parts": parts.keys.sorted()]
    }
}
