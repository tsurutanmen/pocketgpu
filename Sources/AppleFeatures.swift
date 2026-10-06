// Windows から USB で、iPhone の Apple の機能を使うための受け口（iPhone の中の 127.0.0.1:50057。PC は app/pc/apple_ocr.py など）
// 送り方：[u32 長さ][JSON の頼み][u32 長さ][中身のバイト]。返し方：[u32 長さ][JSON の答え]。一つのつなぎで何回でも続けられる
//   {"op":"hello"}                          → できること・iOS の版・機種
//   {"op":"ocr","langs":["ja-JP","en-US"]}  → 中身は画像（PNG・JPEG・HEIC）。文字認識（Vision）の結果
//   {"op":"asr","lang":"ja-JP","ext":"wav"} → 中身は音声のファイル。音声認識の結果（標準は iOS 26 の SpeechAnalyzer）
//   {"op":"llm",…}・{"op":"llm_status"}     → Apple Intelligence の言葉のモデル（FMBridge.swift・Xcode 26 で組んだときだけ）
//   {"op":"translate","text":"…","from":"ja","to":"en"} → 翻訳（TranslateBridge.swift・言語が iPhone に入っていること）
//   {"op":"tts","text":"…","lang":"ja-JP"}  → 読み上げた音声の WAV を base64 で。{"op":"voices"} で声の一覧
import AVFoundation
import Foundation
import Network
import Speech
import UIKit
import Vision

final class AppleFeatures {
    static let shared = AppleFeatures()
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "apple-features", attributes: .concurrent)

    func start(port: UInt16 = 50057) {
        guard listener == nil, let p = NWEndpoint.Port(rawValue: port) else { return }
        let params = NWParameters.tcp
        // USB（iPhone の中の 127.0.0.1）だけで待つ。Wi-Fi には出さない
        params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: p)
        params.allowLocalEndpointReuse = true
        guard let l = try? NWListener(using: params) else { log("Apple の機能の受け口を作れない"); return }
        l.newConnectionHandler = { [weak self] c in self?.serve(c) }
        l.start(queue: queue)
        listener = l
        log("Apple の機能の受け口：127.0.0.1:\(port) で待っている")
    }

    private func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }

    private func serve(_ c: NWConnection) {
        c.start(queue: queue)
        next(c)
    }

    // 頼みを 1 つ読んで答え、次を待つ
    private func next(_ c: NWConnection) {
        readFrame(c) { [weak self] head in
            guard let self, let head else { c.cancel(); return }
            self.readFrame(c) { body in
                guard let body else { c.cancel(); return }
                let req = (try? JSONSerialization.jsonObject(with: head)) as? [String: Any] ?? [:]
                let res = self.handle(req, body)
                let out = (try? JSONSerialization.data(withJSONObject: res)) ?? Data("{}".utf8)
                var n = UInt32(out.count).littleEndian
                var frame = Data(bytes: &n, count: 4)
                frame.append(out)
                c.send(content: frame, completion: .contentProcessed { err in
                    if err != nil { c.cancel() } else { self.next(c) }
                })
            }
        }
    }

    private func readExactly(_ c: NWConnection, _ n: Int, _ done: @escaping (Data?) -> Void) {
        if n == 0 { done(Data()); return }
        c.receive(minimumIncompleteLength: n, maximumLength: n) { data, _, _, err in
            if let data, data.count == n, err == nil { done(data) } else { done(nil) }
        }
    }

    private func readFrame(_ c: NWConnection, _ done: @escaping (Data?) -> Void) {
        readExactly(c, 4) { [weak self] h in
            guard let self, let h else { done(nil); return }
            let n = Int(h.withUnsafeBytes { $0.load(as: UInt32.self) }.littleEndian)
            if n > 256 << 20 { done(nil); return }
            self.readExactly(c, n, done)
        }
    }

    private func handle(_ req: [String: Any], _ body: Data) -> [String: Any] {
        let op = req["op"] as? String ?? ""
        switch op {
        case "hello":
            var sys = utsname()
            uname(&sys)
            let model = withUnsafePointer(to: &sys.machine) { $0.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) } }
            var features = ["ocr", "asr", "tts", "translate"]
            #if canImport(FoundationModels)
            features.append("llm")
            #endif
            return ["features": features, "ios": UIDevice.current.systemVersion, "model": model]
        case "ocr":
            return ocr(body, langs: req["langs"] as? [String] ?? ["ja-JP", "en-US"], fast: req["fast"] as? Bool ?? false)
        case "llm", "llm_status":
            return llm(req, statusOnly: op == "llm_status")
        case "translate":
            let text = req["text"] as? String ?? ""
            let from = req["from"] as? String ?? "ja"
            let to = req["to"] as? String ?? "en"
            #if compiler(>=6.2) && canImport(Translation)
            if #available(iOS 26.0, *) {
                let t0 = Date()
                var r = TranslateBridge.run(text: text, from: from, to: to)
                r["ms"] = Int(Date().timeIntervalSince(t0) * 1000)
                return r
            }
            #endif
            return ["error": "この版では翻訳が使えない"]
        case "tts":
            return tts(text: req["text"] as? String ?? "", lang: req["lang"] as? String ?? "ja-JP",
                       rate: req["rate"] as? Double, voice: req["voice"] as? String)
        case "voices":
            return ["voices": AVSpeechSynthesisVoice.speechVoices().map {
                ["id": $0.identifier, "name": $0.name, "lang": $0.language, "quality": $0.quality.rawValue]
            }]
        case "asr":
            return asr(body, lang: req["lang"] as? String ?? "ja-JP", ext: req["ext"] as? String ?? "wav",
                       onDevice: req["on_device"] as? Bool ?? true, engine: req["engine"] as? String ?? "new")
        default:
            return ["error": "知らない頼み：\(op)"]
        }
    }

    // ---- Apple Intelligence の言葉のモデル（Foundation Models・iOS 26） ----
    // Xcode 26 で組んだときだけ入る。Xcode 16 で組むと「この版には無い」と答える
    private func llm(_ req: [String: Any], statusOnly: Bool) -> [String: Any] {
        #if canImport(FoundationModels)
        if #available(iOS 26.0, *) {
            return FMBridge.run(req, statusOnly: statusOnly)
        }
        return ["error": "iOS 26 より前なので使えない"]
        #else
        return ["error": "この版は Xcode 26 で組んでいないので、Foundation Models が入っていない"]
        #endif
    }

    // ---- 音声認識 ----
    // 中身は音声のファイル（wav・m4a など）。一度ファイルに書いてから読ませる。初めてのときは iPhone の画面で許可が要る。
    // engine：new は iOS 26 の SpeechAnalyzer（長い音声向け・標準）、legacy は SFSpeechRecognizer
    private func asr(_ data: Data, lang: String, ext: String, onDevice: Bool, engine: String) -> [String: Any] {
        let auth = DispatchSemaphore(value: 0)
        var status = SFSpeechRecognizer.authorizationStatus()
        if status == .notDetermined {
            SFSpeechRecognizer.requestAuthorization { s in status = s; auth.signal() }
            auth.wait()
        }
        guard status == .authorized else { return ["error": "音声認識が許可されていない。iPhone の設定で「計算の受け手」の音声認識を許可する"] }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("asr-\(UUID().uuidString).\(ext)")
        do { try data.write(to: url) } catch { return ["error": "音声を書けない"] }
        defer { try? FileManager.default.removeItem(at: url) }
        let t0 = Date()
        var out: [String: Any]
        #if compiler(>=6.2)
        if engine != "legacy", #available(iOS 26.0, *) {
            out = SpeechBridge.transcribe(url: url, lang: lang)
        } else {
            out = legacyASR(url: url, lang: lang, onDevice: onDevice)
        }
        #else
        out = legacyASR(url: url, lang: lang, onDevice: onDevice)
        #endif
        let ms = Int(Date().timeIntervalSince(t0) * 1000)
        out["ms"] = ms
        log("音声認識：\(ms) ms・\(out["engine"] as? String ?? "?")")
        return out
    }

    // 長い音声は区切られて、区切りごとに確定が届く。全部つないで、仕事が終わるまで待つ
    private func legacyASR(url: URL, lang: String, onDevice: Bool) -> [String: Any] {
        guard let rec = SFSpeechRecognizer(locale: Locale(identifier: lang)), rec.isAvailable else { return ["error": "\(lang) の音声認識が使えない"] }
        let req = SFSpeechURLRecognitionRequest(url: url)
        req.shouldReportPartialResults = false
        req.requiresOnDeviceRecognition = onDevice && rec.supportsOnDeviceRecognition
        req.addsPunctuation = true
        let lock = NSLock()
        var finals: [String] = []
        var failure: String?
        var ended = false
        let task = rec.recognitionTask(with: req) { result, error in
            lock.lock()
            defer { lock.unlock() }
            if let result, result.isFinal { finals.append(result.bestTranscription.formattedString) }
            if let error, finals.isEmpty { failure = error.localizedDescription }
            if error != nil { ended = true }
        }
        let deadline = Date().addingTimeInterval(600)
        while Date() < deadline {
            lock.lock()
            let stop = ended || task.state == .completed || task.state == .canceling
            lock.unlock()
            if stop { break }
            Thread.sleep(forTimeInterval: 0.1)
        }
        if task.state == .running || task.state == .starting { task.cancel() }
        lock.lock()
        defer { lock.unlock() }
        if finals.isEmpty, let failure { return ["error": "音声認識が失敗：\(failure)", "engine": "SFSpeechRecognizer"] }
        return ["text": finals.joined(), "engine": "SFSpeechRecognizer", "on_device": req.requiresOnDeviceRecognition, "segments": finals.count]
    }

    // ---- 読み上げ（AVSpeechSynthesizer） ----
    // 文を音声にして、WAV のバイトを base64 で返す。iPhone のスピーカーからは鳴らさない
    private let synthLock = NSLock()
    private var synths: [AVSpeechSynthesizer] = [] // 書き終わるまで手放さない

    private func tts(text: String, lang: String, rate: Double?, voice: String?) -> [String: Any] {
        let u = AVSpeechUtterance(string: text)
        if let voice, let v = AVSpeechSynthesisVoice(identifier: voice) { u.voice = v } else { u.voice = AVSpeechSynthesisVoice(language: lang) }
        if let rate { u.rate = Float(rate) }
        let synth = AVSpeechSynthesizer()
        synthLock.lock(); synths.append(synth); synthLock.unlock()
        defer { synthLock.lock(); synths.removeAll { $0 === synth }; synthLock.unlock() }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("tts-\(UUID().uuidString).wav")
        defer { try? FileManager.default.removeItem(at: url) }
        let done = DispatchSemaphore(value: 0)
        var file: AVAudioFile?
        var failure: String?
        let t0 = Date()
        synth.write(u) { buf in
            guard let pcm = buf as? AVAudioPCMBuffer else { return }
            if pcm.frameLength == 0 { done.signal(); return } // 終わりの印
            do {
                if file == nil {
                    file = try AVAudioFile(forWriting: url, settings: pcm.format.settings,
                                           commonFormat: pcm.format.commonFormat, interleaved: pcm.format.isInterleaved)
                }
                try file?.write(from: pcm)
            } catch {
                failure = "音声を書けない：\(error.localizedDescription)"
                done.signal()
            }
        }
        if done.wait(timeout: .now() + 120) == .timedOut { return ["error": "2 分で終わらなかった"] }
        file = nil // 閉じて書き切る
        if let failure { return ["error": failure] }
        guard let data = try? Data(contentsOf: url) else { return ["error": "音声を読めない"] }
        let ms = Int(Date().timeIntervalSince(t0) * 1000)
        log("読み上げ：\(text.count) 文字・\(ms) ms")
        return ["wav_b64": data.base64EncodedString(), "ms": ms, "voice": u.voice?.identifier ?? ""]
    }

    // ---- 文字認識（Vision の VNRecognizeTextRequest） ----
    private func ocr(_ data: Data, langs: [String], fast: Bool) -> [String: Any] {
        guard let ui = UIImage(data: data), let cg = ui.cgImage else { return ["error": "画像を読めない"] }
        let t0 = Date()
        let req = VNRecognizeTextRequest()
        req.recognitionLevel = fast ? .fast : .accurate
        req.recognitionLanguages = langs
        req.usesLanguageCorrection = true
        let orient = CGImagePropertyOrientation(ui.imageOrientation)
        do {
            try VNImageRequestHandler(cgImage: cg, orientation: orient, options: [:]).perform([req])
        } catch {
            return ["error": "文字認識が失敗：\(error.localizedDescription)"]
        }
        var lines: [[String: Any]] = []
        for o in req.results ?? [] {
            guard let top = o.topCandidates(1).first else { continue }
            let b = o.boundingBox
            lines.append(["text": top.string, "conf": top.confidence, "box": [b.minX, 1 - b.maxY, b.width, b.height]])
        }
        let ms = Int(Date().timeIntervalSince(t0) * 1000)
        log("文字認識：\(lines.count) 行・\(ms) ms・\(cg.width)x\(cg.height)")
        return ["text": lines.map { $0["text"] as? String ?? "" }.joined(separator: "\n"), "lines": lines, "ms": ms,
                "width": cg.width, "height": cg.height]
    }
}

extension CGImagePropertyOrientation {
    init(_ o: UIImage.Orientation) {
        switch o {
        case .up: self = .up
        case .down: self = .down
        case .left: self = .left
        case .right: self = .right
        case .upMirrored: self = .upMirrored
        case .downMirrored: self = .downMirrored
        case .leftMirrored: self = .leftMirrored
        case .rightMirrored: self = .rightMirrored
        @unknown default: self = .up
        }
    }
}
