// Windows から USB で、iPhone の Apple の機能を使うための受け口（iPhone の中の 127.0.0.1:50057。PC は app/pc/apple_ocr.py など）
// 送り方：[u32 長さ][JSON の頼み][u32 長さ][中身のバイト]。返し方：[u32 長さ][JSON の答え]。一つのつなぎで何回でも続けられる
//   {"op":"hello"}                          → できること・iOS の版・機種
//   {"op":"ocr","langs":["ja-JP","en-US"]}  → 中身は画像（PNG・JPEG・HEIC）。文字認識（Vision）の結果
//   {"op":"llm",…}・{"op":"llm_status"}     → Apple Intelligence の言葉のモデル（FMBridge.swift・Xcode 26 で組んだときだけ）
import Foundation
import Network
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

    private func log(_ s: String) { s.withCString { rpc_bridge_log_line($0) } }

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
            var features = ["ocr"]
            #if canImport(FoundationModels)
            features.append("llm")
            #endif
            return ["features": features, "ios": UIDevice.current.systemVersion, "model": model]
        case "ocr":
            return ocr(body, langs: req["langs"] as? [String] ?? ["ja-JP", "en-US"], fast: req["fast"] as? Bool ?? false)
        case "llm", "llm_status":
            return llm(req, statusOnly: op == "llm_status")
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
