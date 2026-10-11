// Windows から USB で、任意の Core ML モデルを iPhone の Neural Engine で動かす受け口（下書き。まだ Xcode で組んでいない）
// AppleFeatures の 50057 番に相乗りする。頼みの名前が "coreml_" で始まるものをここに回す（AppleFeatures_coreml.diff）。
// PC は work/ane/coreml_remote.py。試験用の偽の受け手は work/ane/fake_ane_server.py（同じ約束を話す）
//
// 枠の約束は AppleFeatures と同じ：送り [u32 長さ][JSON の頼み][u32 長さ][中身]、返し [u32 長さ][JSON の答え]
//   例外が 1 つ：答えの JSON に "bin_len" があれば、そのあとにもう 1 枠 [u32 長さ][バイト] が続く（reply_bin:true と頼んだときだけ）
//   1 枠の上限は 256 MiB（AppleFeatures.readFrame）。だから大きいモデルは 64 MiB ごとに分けて送る
//
// 頼み（答えには必ず "server_ms" が付く。失敗は {"error":"…"}）
//   coreml_hello                                  → できること・上限・Neural Engine のコア数・空き容量
//   coreml_has     {sha256}                       → {uploaded, received, compiled, id}  途中から続けるための問い合わせ
//   coreml_put     {sha256,total,offset,final,name} 中身=モデルの入れ物の一部 → {received, ready}
//   coreml_compile {sha256}                       → 入れ物をほどいて MLModel.compileModel。{id, compile_ms, inputs, outputs}
//   coreml_info    {id}                           → 入出力の説明
//   coreml_plan    {id, compute}                  → MLComputePlan で「どの演算がどの回路で走る見込みか」（iOS 17.4 から）
//   coreml_predict {id, compute, inputs, iters, warmup, reply_bin} 中身=入力のバイトを並べたもの → {outputs, ms, …}
//   coreml_unload / coreml_delete {id}・coreml_list・coreml_status
//   coreml_probe_apple                            → Vision の文字認識が使える回路を iPhone に聞く（iOS 17 の supportedComputeStageDevices）
//
// モデルの入れ物（ANEPKG1）：Windows で .mlpackage（や .mlmodelc）のフォルダを 1 本のバイト列にしたもの。sha256 はこの全体
//   [8 バイト "ANEPKG1\n"][u32 ヘッダ長][ヘッダ JSON {"root":"X.mlpackage","files":[{"p":"相対パス","n":バイト数},…]}][ファイルの中身を順に]
//   p が "" なら root 自体が 1 ファイル（.mlmodel）。id は sha256 の頭 16 文字
//
// 入力の書き方（inputs の 1 つ）
//   {"name","kind":"tensor","dtype":"float16|float32|float64|int32","shape":[…],"offset","nbytes"}  C 順・リトルエンディアン・隙間なし
//   {"name","kind":"image","scale":"fill|fit|crop","offset","nbytes"}                               PNG・JPEG・HEIC のバイト
//   {"name","kind":"string","value":"…"}・{"name","kind":"int","value":3}・{"name","kind":"double","value":0.5}
// 出力の書き方（outputs の 1 つ）
//   tensor：{"name","kind":"tensor","dtype","shape","nbytes", "b64" か "offset"}   stride の隙間は詰めて返す
//   image ：{"name","kind":"image","fmt":"png"|"raw","width","height","fourcc",…}  BGRA は PNG、L008・L00h・L00f は生（行を詰めて）
//   string・int・double・dict（確率の上位 50）
//
// 計算の回路：compute は "cpuAndNeuralEngine"（標準）・"all"・"cpuAndGPU"・"cpuOnly"。標準は GPU を使わないので、
//   このアプリの本業（GPU の計算の受け手）と取り合わない
//
// 上限：1 回の put 64 MiB・モデル 1 本 4 GiB・置き場の合計 8 GiB（古い順に消す）・空きは 2 GiB 残す・1 回の入出力 128 MiB・メモリに載せるモデルは 2 本
// 置き場：Application Support/ane_models/models/<id>/model.mlmodelc（バックアップ対象外）。送りかけは ane_models/uploads
// 同じモデルは sha256 が同じなら二度と送らない。MLModel は (id, compute) ごとにメモリに残し、1 本ずつ順番に使う（Apple の決まり）
//
// 確かめていないこと（実機で）：初回ロードの長さ（Neural Engine 用の仕上げが入る）・画面ロック中やバックグラウンドで Neural Engine が使えるか・
//   Xcode 26.3 で警告なく組めるか。API の名前は Apple の資料（developer.apple.com/documentation/coreml）で確かめたが、
//   Swift の宣言そのものを読めなかった物が 2 つ：MLModelStructure.Program.Operation.operatorName・NeuralNetwork.Layer.type（plan だけで使う）
//   Swift 5 の言語モードで、Task の中から var を書き換える作りは FMBridge などと同じ（Box で包んである）
import CoreImage
import CoreML
import CoreVideo
import CryptoKit
import Foundation
import UIKit
import Vision

enum CoreMLRemote {
    static let protoVersion = 1
    static let maxChunk = 64 << 20
    static let maxModelBytes: Int64 = 4 << 30
    static let maxStoreBytes: Int64 = 8 << 30
    static let minFreeBytes: Int64 = 2 << 30
    static let maxIOBytes = 128 << 20
    static let maxLoaded = 2

    private static let fm = FileManager.default
    private static let ciContext = CIContext()
    private static let stateLock = NSLock()
    private static let uploadLock = NSLock()
    private static var loaded: [String: Loaded] = [:]
    private static var activity: [String: Any] = [:]

    private enum CRError: Error, LocalizedError {
        case msg(String)
        var errorDescription: String? { if case .msg(let s) = self { return s }; return nil }
    }

    private final class Box<T>: @unchecked Sendable {
        var value: T?
        var error: Error?
    }

    private final class Loaded {
        let id: String
        let compute: String
        let model: MLModel
        let lock = NSLock()
        var last = Date()
        init(id: String, compute: String, model: MLModel) { self.id = id; self.compute = compute; self.model = model }
    }

    // ---- 入口 ----
    static func handle(_ op: String, _ req: [String: Any], _ body: Data) -> [String: Any] {
        let t0 = Date()
        var r: [String: Any]
        switch op {
        case "coreml_hello": r = hello()
        case "coreml_has": r = has(req)
        case "coreml_put": r = put(req, body)
        case "coreml_compile": r = compile(req)
        case "coreml_info": r = info(req)
        case "coreml_plan": r = plan(req)
        case "coreml_predict": r = predict(req, body)
        case "coreml_unload": r = unload(req)
        case "coreml_delete": r = delete(req)
        case "coreml_list": r = list()
        case "coreml_status": r = status()
        case "coreml_probe_apple": r = probeApple()
        default: r = err("知らない頼み：\(op)")
        }
        r["server_ms"] = Int(Date().timeIntervalSince(t0) * 1000)
        return r
    }

    // ---- 置き場 ----
    private static let root: URL = {
        let base = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("ane_models", isDirectory: true)
        try? fm.createDirectory(at: base, withIntermediateDirectories: true)
        var b = base
        var v = URLResourceValues()
        v.isExcludedFromBackup = true
        try? b.setResourceValues(v)
        return base
    }()
    private static func uploadsDir() -> URL {
        let d = root.appendingPathComponent("uploads", isDirectory: true)
        try? fm.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }
    private static func modelsDir() -> URL {
        let d = root.appendingPathComponent("models", isDirectory: true)
        try? fm.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }
    private static func partURL(_ sha: String) -> URL { uploadsDir().appendingPathComponent(sha + ".part") }
    private static func pkgURL(_ sha: String) -> URL { uploadsDir().appendingPathComponent(sha + ".pkg") }
    private static func modelDir(_ id: String) -> URL { modelsDir().appendingPathComponent(id, isDirectory: true) }
    private static func compiledURL(_ id: String) -> URL { modelDir(id).appendingPathComponent("model.mlmodelc", isDirectory: true) }

    // ---- 小道具 ----
    private static func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }

    private static func err(_ s: String, _ extra: [String: Any] = [:]) -> [String: Any] {
        log("coreml：\(s)")
        var d = extra
        d["error"] = s
        return d
    }

    private static func num(_ v: Any?) -> Int64? { (v as? NSNumber)?.int64Value }

    private static func validSha(_ v: Any?) -> String? {
        let hex = Set("0123456789abcdef")
        guard let s = v as? String, s.count == 64, s.allSatisfy({ hex.contains($0) }) else { return nil }
        return s
    }

    private static func fileSize(_ u: URL) -> Int64? {
        guard let a = try? fm.attributesOfItem(atPath: u.path) else { return nil }
        return (a[.size] as? NSNumber)?.int64Value
    }

    private static func dirSize(_ u: URL) -> Int64 {
        guard let e = fm.enumerator(at: u, includingPropertiesForKeys: [.totalFileAllocatedSizeKey, .isRegularFileKey]) else { return 0 }
        var total: Int64 = 0
        for case let f as URL in e {
            let v = try? f.resourceValues(forKeys: [.totalFileAllocatedSizeKey, .isRegularFileKey])
            if v?.isRegularFile == true { total += Int64(v?.totalFileAllocatedSize ?? 0) }
        }
        return total
    }

    private static func freeBytes() -> Int64 {
        let v = try? root.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
        return v?.volumeAvailableCapacityForImportantUsage ?? 0
    }

    // JSONSerialization は NaN と無限大で落ちる（例外なので try? でも防げない）。nil の代わりに NSNull を返す
    private static func jsonSafe(_ d: Double) -> Any {
        if d.isFinite { return d }
        return NSNull()
    }

    private static func fourcc(_ t: OSType) -> String {
        let b = [UInt8((t >> 24) & 255), UInt8((t >> 16) & 255), UInt8((t >> 8) & 255), UInt8(t & 255)]
        return String(bytes: b, encoding: .ascii) ?? String(t)
    }

    private static func sha256Hex(of url: URL) -> String? {
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        var hasher = SHA256()
        while true {
            guard let d = try? h.read(upToCount: 8 << 20), !d.isEmpty else { break }
            hasher.update(data: d)
        }
        return hasher.finalize().map { String(format: "%02x", $0) }.joined()
    }

    // 受け口の窓口は同期なので、async の仕事をここで待つ（FMBridge などと同じ作り）
    private static func blocking<T>(timeout: TimeInterval, _ work: @escaping () async throws -> T) throws -> T {
        let box = Box<T>()
        let sem = DispatchSemaphore(value: 0)
        Task {
            do { box.value = try await work() } catch { box.error = error }
            sem.signal()
        }
        if sem.wait(timeout: .now() + timeout) == .timedOut { throw CRError.msg("\(Int(timeout)) 秒で終わらなかった") }
        if let e = box.error { throw e }
        guard let v = box.value else { throw CRError.msg("答えが空") }
        return v
    }

    private static func setActivity(_ what: String, _ id: String) {
        stateLock.lock(); defer { stateLock.unlock() }
        activity = ["what": what, "id": id, "since": Date().timeIntervalSince1970]
    }
    private static func clearActivity() {
        stateLock.lock(); defer { stateLock.unlock() }
        activity = [:]
    }

    private static func units(_ s: String) throws -> MLComputeUnits {
        switch s {
        case "cpuAndNeuralEngine": return .cpuAndNeuralEngine
        case "all": return .all
        case "cpuAndGPU": return .cpuAndGPU
        case "cpuOnly": return .cpuOnly
        default: throw CRError.msg("compute が違う：\(s)（cpuAndNeuralEngine・all・cpuAndGPU・cpuOnly）")
        }
    }

    // ---- hello・status・list ----
    private static func hello() -> [String: Any] {
        var devs: [String] = []
        var aneCores = 0
        for d in MLComputeDevice.allComputeDevices {
            switch d {
            case .cpu: devs.append("cpu")
            case .gpu: devs.append("gpu")
            case .neuralEngine(let n):
                devs.append("neuralEngine")
                aneCores = n.totalCoreCount
            @unknown default: devs.append("other")
            }
        }
        var sys = utsname()
        uname(&sys)
        let machine = withUnsafePointer(to: &sys.machine) { $0.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) } }
        var canPlan = false
        if #available(iOS 17.4, *) { canPlan = true }
        return ["ok": true, "proto": protoVersion, "ios": UIDevice.current.systemVersion, "model": machine,
                "devices": devs, "ane_cores": aneCores, "compute_plan": canPlan,
                "limits": ["chunk": maxChunk, "model": maxModelBytes, "store": maxStoreBytes, "min_free": minFreeBytes,
                           "io": maxIOBytes, "loaded": maxLoaded],
                "free_bytes": freeBytes(), "store_bytes": dirSize(root)]
    }

    private static func status() -> [String: Any] {
        stateLock.lock()
        let act = activity
        let ls = loaded.values.map { ["id": $0.id, "compute": $0.compute, "idle_s": Int(Date().timeIntervalSince($0.last))] as [String: Any] }
        stateLock.unlock()
        return ["activity": act, "loaded": ls, "store_bytes": dirSize(root), "free_bytes": freeBytes()]
    }

    private static func list() -> [String: Any] {
        let dirs = (try? fm.contentsOfDirectory(at: modelsDir(), includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        var out: [[String: Any]] = []
        for d in dirs {
            let id = d.lastPathComponent
            var m = readMeta(id)
            m["id"] = id
            m["bytes"] = dirSize(d)
            out.append(m)
        }
        return ["models": out]
    }

    private static func readMeta(_ id: String) -> [String: Any] {
        let u = modelDir(id).appendingPathComponent("meta.json")
        guard let d = try? Data(contentsOf: u), let j = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] else { return [:] }
        return j
    }

    // ---- 送る ----
    private static func has(_ req: [String: Any]) -> [String: Any] {
        guard let sha = validSha(req["sha256"]) else { return err("sha256 は 64 文字の 16 進（小文字）") }
        let id = String(sha.prefix(16))
        let pkg = pkgURL(sha)
        let received: Int64 = fileSize(pkg) ?? fileSize(partURL(sha)) ?? 0
        return ["id": id, "compiled": fm.fileExists(atPath: compiledURL(id).path),
                "uploaded": fm.fileExists(atPath: pkg.path), "received": received]
    }

    private static func put(_ req: [String: Any], _ body: Data) -> [String: Any] {
        guard let sha = validSha(req["sha256"]) else { return err("sha256 は 64 文字の 16 進（小文字）") }
        guard let total = num(req["total"]), total > 12 else { return err("total が無い") }
        if total > maxModelBytes { return err("モデルが上限（\(maxModelBytes >> 20) MiB）を超える", ["limit": maxModelBytes]) }
        if body.count > maxChunk { return err("1 回の中身が上限（\(maxChunk >> 20) MiB）を超える", ["limit": maxChunk]) }
        let offset = num(req["offset"]) ?? 0
        let id = String(sha.prefix(16))
        uploadLock.lock(); defer { uploadLock.unlock() }
        if fm.fileExists(atPath: compiledURL(id).path) || fm.fileExists(atPath: pkgURL(sha).path) {
            return ["received": total, "ready": true]
        }
        let part = partURL(sha)
        if offset == 0 { try? fm.removeItem(at: part) } // 最初から送り直し
        let have = fileSize(part) ?? 0
        if offset != have { return err("つなぎ目が合わない（iPhone にあるのは \(have) バイト）", ["received": have]) }
        if freeBytes() < (total - have) + minFreeBytes { return err("iPhone の空きが足りない", ["free": freeBytes()]) }
        if !fm.fileExists(atPath: part.path) { fm.createFile(atPath: part.path, contents: nil) }
        do {
            let h = try FileHandle(forWritingTo: part)
            defer { try? h.close() }
            _ = try h.seekToEnd()
            try h.write(contentsOf: body)
        } catch {
            return err("書けない：\(error.localizedDescription)", ["received": have])
        }
        let now = have + Int64(body.count)
        if now > total {
            try? fm.removeItem(at: part)
            return err("total を超えた", ["received": 0])
        }
        let isFinal = (req["final"] as? Bool ?? false) || now == total
        if !isFinal { return ["received": now, "ready": false] }
        if now != total { return err("final なのに total に足りない", ["received": now]) }
        guard let got = sha256Hex(of: part), got == sha else {
            try? fm.removeItem(at: part)
            return err("sha256 が合わない。最初から送り直す", ["received": 0])
        }
        do {
            try? fm.removeItem(at: pkgURL(sha))
            try fm.moveItem(at: part, to: pkgURL(sha))
        } catch {
            return err("置けない：\(error.localizedDescription)")
        }
        log("coreml：\(total >> 20) MiB 受け取った（\(id)）")
        return ["received": now, "ready": true]
    }

    // ---- 入れ物をほどく ----
    private static func checkRel(_ p: String) throws {
        if p.hasPrefix("/") || p.contains("\\") || p.contains("\0") { throw CRError.msg("入れ物の中の名前が危ない：\(p)") }
        for c in p.split(separator: "/", omittingEmptySubsequences: false) where c.isEmpty || c == "." || c == ".." {
            throw CRError.msg("入れ物の中の名前が危ない：\(p)")
        }
    }

    private static func extract(pkg: URL, to dir: URL) throws -> URL {
        let h = try FileHandle(forReadingFrom: pkg)
        defer { try? h.close() }
        guard let head = try h.read(upToCount: 12), head.count == 12, head.prefix(8) == Data("ANEPKG1\n".utf8) else {
            throw CRError.msg("入れ物の頭が違う")
        }
        let hlen = Int(head.subdata(in: 8..<12).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }.littleEndian)
        guard hlen > 0, hlen < (4 << 20), let hd = try h.read(upToCount: hlen), hd.count == hlen,
              let j = try JSONSerialization.jsonObject(with: hd) as? [String: Any],
              let rootName = j["root"] as? String, let files = j["files"] as? [[String: Any]] else {
            throw CRError.msg("入れ物のヘッダが読めない")
        }
        let ext = (rootName as NSString).pathExtension
        guard ["mlpackage", "mlmodelc", "mlmodel"].contains(ext) else { throw CRError.msg("root の名前は .mlpackage・.mlmodelc・.mlmodel のどれか：\(rootName)") }
        if rootName.contains("/") { throw CRError.msg("root の名前に / は使えない") }
        try checkRel(rootName)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        let rootURL = dir.appendingPathComponent(rootName)
        for f in files {
            guard let p = f["p"] as? String, let n = num(f["n"]), n >= 0 else { throw CRError.msg("入れ物のファイル表が読めない") }
            var dest = rootURL
            if !p.isEmpty {
                try checkRel(p)
                dest = rootURL.appendingPathComponent(p)
            }
            try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
            fm.createFile(atPath: dest.path, contents: nil)
            let out = try FileHandle(forWritingTo: dest)
            var left = n
            while left > 0 {
                guard let d = try h.read(upToCount: Int(min(left, 8 << 20))), !d.isEmpty else { throw CRError.msg("入れ物が途中で終わっている") }
                try out.write(contentsOf: d)
                left -= Int64(d.count)
            }
            try out.close()
        }
        return rootURL
    }

    // ---- コンパイル ----
    private static func compile(_ req: [String: Any]) -> [String: Any] {
        guard let sha = validSha(req["sha256"]) else { return err("sha256 は 64 文字の 16 進（小文字）") }
        let id = String(sha.prefix(16))
        let dest = compiledURL(id)
        let t0 = Date()
        var compiledNow = false
        if !fm.fileExists(atPath: dest.path) {
            let pkg = pkgURL(sha)
            guard fm.fileExists(atPath: pkg.path) else { return err("まだ送られていない", ["uploaded": false]) }
            setActivity("compile", id)
            defer { clearActivity() }
            let work = root.appendingPathComponent("work-\(id)", isDirectory: true)
            try? fm.removeItem(at: work)
            defer { try? fm.removeItem(at: work) }
            do {
                let src = try extract(pkg: pkg, to: work)
                try fm.createDirectory(at: modelDir(id), withIntermediateDirectories: true)
                if src.pathExtension == "mlmodelc" {
                    try fm.moveItem(at: src, to: dest) // もう仕上がっている物はそのまま置く
                } else {
                    // Apple の決まり：compileModel は一時の場所に書くので、残すなら Application Support などへ移す
                    let compiled = try blocking(timeout: 1800) { try await MLModel.compileModel(at: src) }
                    _ = try fm.replaceItemAt(dest, withItemAt: compiled)
                }
                let name = (req["name"] as? String) ?? (src.lastPathComponent)
                let meta: [String: Any] = ["name": name, "sha256": sha, "created": Date().timeIntervalSince1970,
                                           "source_ext": src.pathExtension]
                if let d = try? JSONSerialization.data(withJSONObject: meta) { try d.write(to: modelDir(id).appendingPathComponent("meta.json")) }
                try? fm.removeItem(at: pkg)
                compiledNow = true
            } catch {
                try? fm.removeItem(at: modelDir(id))
                return err("コンパイルできない：\(error.localizedDescription)")
            }
            evictIfNeeded(keep: id)
        }
        var r = describe(id)
        r["id"] = id
        r["compiled_now"] = compiledNow
        r["compile_ms"] = Int(Date().timeIntervalSince(t0) * 1000)
        r["bytes"] = dirSize(modelDir(id))
        log("coreml：\(id) \(compiledNow ? "コンパイルした" : "もうある")・\(r["compile_ms"] ?? 0) ms")
        return r
    }

    // 置き場の合計が上限を超えたら、使われていない古い物から消す
    private static func evictIfNeeded(keep: String) {
        while dirSize(modelsDir()) > maxStoreBytes {
            let dirs = ((try? fm.contentsOfDirectory(at: modelsDir(), includingPropertiesForKeys: [.contentModificationDateKey])) ?? [])
                .filter { $0.lastPathComponent != keep }
            let oldest = dirs.min { a, b in
                let da = (try? a.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                let db = (try? b.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                return da < db
            }
            guard let victim = oldest else { return }
            dropLoaded(victim.lastPathComponent)
            try? fm.removeItem(at: victim)
            log("coreml：置き場がいっぱいなので \(victim.lastPathComponent) を消した")
        }
    }

    private static func dropLoaded(_ id: String) {
        stateLock.lock(); defer { stateLock.unlock() }
        for k in loaded.keys where k.hasPrefix(id + "|") { loaded[k] = nil }
    }

    // ---- 入出力の説明 ----
    private static func describe(_ id: String) -> [String: Any] {
        let url = compiledURL(id)
        guard fm.fileExists(atPath: url.path) else { return err("そのモデルは無い（id \(id)）") }
        do {
            let asset = try MLModelAsset(url: url)
            let box = Box<MLModelDescription>()
            let sem = DispatchSemaphore(value: 0)
            asset.modelDescription { d, e in
                box.value = d
                box.error = e
                sem.signal()
            }
            if sem.wait(timeout: .now() + 60) == .timedOut { return err("モデルの説明を読めない（60 秒）") }
            if let e = box.error { return err("モデルの説明を読めない：\(e.localizedDescription)") }
            guard let d = box.value else { return err("モデルの説明が空") }
            return ["inputs": d.inputDescriptionsByName.keys.sorted().compactMap { d.inputDescriptionsByName[$0] }.map(featureJSON),
                    "outputs": d.outputDescriptionsByName.keys.sorted().compactMap { d.outputDescriptionsByName[$0] }.map(featureJSON)]
        } catch {
            return err("モデルの説明を読めない：\(error.localizedDescription)")
        }
    }

    private static func dtypeName(_ t: MLMultiArrayDataType) -> String {
        switch t {
        case .float16: return "float16"
        case .float32: return "float32"
        case .float64: return "float64"
        case .int32: return "int32"
        default: return "unknown(\(t.rawValue))"
        }
    }

    private static func dtype(_ s: String) -> MLMultiArrayDataType? {
        switch s {
        case "float16": return .float16
        case "float32": return .float32
        case "float64": return .float64
        case "int32": return .int32
        default: return nil
        }
    }

    private static func elemSize(_ t: MLMultiArrayDataType) -> Int {
        switch t {
        case .float16: return 2
        case .float32, .int32: return 4
        case .float64: return 8
        default: return 0
        }
    }

    private static func featureJSON(_ f: MLFeatureDescription) -> [String: Any] {
        var d: [String: Any] = ["name": f.name, "optional": f.isOptional]
        switch f.type {
        case .multiArray:
            d["kind"] = "tensor"
            if let c = f.multiArrayConstraint {
                d["dtype"] = dtypeName(c.dataType)
                d["shape"] = c.shape.map { $0.intValue }
                switch c.shapeConstraint.type {
                case .enumerated: d["shapes"] = c.shapeConstraint.enumeratedShapes.map { $0.map { $0.intValue } }
                case .range: d["ranges"] = c.shapeConstraint.sizeRangeForDimension.map { [$0.rangeValue.location, $0.rangeValue.length] }
                default: break
                }
            }
        case .image:
            d["kind"] = "image"
            if let c = f.imageConstraint {
                d["width"] = c.pixelsWide
                d["height"] = c.pixelsHigh
                d["fourcc"] = fourcc(c.pixelFormatType)
            }
        case .string: d["kind"] = "string"
        case .int64: d["kind"] = "int"
        case .double: d["kind"] = "double"
        case .dictionary: d["kind"] = "dict"
        case .sequence: d["kind"] = "sequence"
        default: d["kind"] = "other"
        }
        return d
    }

    private static func info(_ req: [String: Any]) -> [String: Any] {
        guard let id = req["id"] as? String, !id.contains("/"), !id.contains("..") else { return err("id が無い") }
        var r = describe(id)
        if r["error"] != nil { return r }
        for (k, v) in readMeta(id) { r[k] = v }
        r["id"] = id
        r["bytes"] = dirSize(modelDir(id))
        return r
    }

    private static func unload(_ req: [String: Any]) -> [String: Any] {
        guard let id = req["id"] as? String else { return err("id が無い") }
        dropLoaded(id)
        return ["ok": true]
    }

    private static func delete(_ req: [String: Any]) -> [String: Any] {
        guard let id = req["id"] as? String, !id.contains("/"), !id.contains("..") else { return err("id が無い") }
        dropLoaded(id)
        try? fm.removeItem(at: modelDir(id))
        return ["ok": true]
    }

    // ---- メモリに載せる ----
    private static func loadModel(id: String, compute: String) throws -> (Loaded, Bool) {
        let key = "\(id)|\(compute)"
        stateLock.lock()
        if let l = loaded[key] {
            l.last = Date()
            stateLock.unlock()
            return (l, true)
        }
        stateLock.unlock()
        let url = compiledURL(id)
        guard fm.fileExists(atPath: url.path) else { throw CRError.msg("そのモデルは無い（id \(id)）。先に送ってコンパイルする") }
        let cfg = MLModelConfiguration()
        cfg.computeUnits = try units(compute)
        setActivity("load", id)
        defer { clearActivity() }
        // 初めて cpuAndNeuralEngine で読むときは、Neural Engine 用の仕上げが入るので長くかかることがある
        let m = try blocking(timeout: 1800) { try await MLModel.load(contentsOf: url, configuration: cfg) }
        let l = Loaded(id: id, compute: compute, model: m)
        stateLock.lock(); defer { stateLock.unlock() }
        loaded[key] = l
        while loaded.count > maxLoaded {
            guard let victim = loaded.filter({ $0.key != key }).min(by: { $0.value.last < $1.value.last })?.key else { break }
            loaded[victim] = nil
        }
        try? fm.setAttributes([.modificationDate: Date()], ofItemAtPath: modelDir(id).path)
        return (l, false)
    }

    // ---- 予測 ----
    private static func predict(_ req: [String: Any], _ body: Data) -> [String: Any] {
        guard let id = req["id"] as? String, !id.contains("/"), !id.contains("..") else { return err("id が無い") }
        let compute = req["compute"] as? String ?? "cpuAndNeuralEngine"
        let iters = Int(max(1, min(1000, num(req["iters"]) ?? 1)))
        let warmup = Int(max(0, min(100, num(req["warmup"]) ?? 0)))
        let replyBin = req["reply_bin"] as? Bool ?? false
        if body.count > maxIOBytes { return err("入力が上限（\(maxIOBytes >> 20) MiB）を超える") }
        do {
            let tLoad = Date()
            let (l, wasCached) = try loadModel(id: id, compute: compute)
            let loadMs = Int(Date().timeIntervalSince(tLoad) * 1000)
            let provider = try makeInputs(req["inputs"], body, l.model.modelDescription)
            l.lock.lock() // MLModel は 1 度に 1 つの糸からだけ（Apple の決まり）
            defer { l.lock.unlock() }
            var times: [Double] = []
            var out: MLFeatureProvider?
            for i in 0..<(warmup + iters) {
                let t = DispatchTime.now().uptimeNanoseconds
                try autoreleasepool { out = try l.model.prediction(from: provider) }
                let ms = Double(DispatchTime.now().uptimeNanoseconds - t) / 1e6
                if i >= warmup { times.append(ms) }
            }
            guard let result = out else { return err("答えが空") }
            let (outputs, blob) = try packOutputs(result, replyBin: replyBin)
            var r: [String: Any] = ["outputs": outputs, "ms": times.map { ($0 * 100).rounded() / 100 }, "compute": compute,
                                    "cached": wasCached, "load_ms": loadMs]
            if replyBin {
                r["bin_len"] = blob.count
                r["_bin"] = blob // AppleFeatures.next が取り出して 2 枠目にする
            }
            return r
        } catch {
            return err("予測できない：\(error.localizedDescription)")
        }
    }

    private static func makeInputs(_ spec: Any?, _ body: Data, _ desc: MLModelDescription) throws -> MLFeatureProvider {
        guard let list = spec as? [[String: Any]], !list.isEmpty else { throw CRError.msg("inputs が無い") }
        var dict: [String: MLFeatureValue] = [:]
        for s in list {
            guard let name = s["name"] as? String else { throw CRError.msg("入力に name が無い") }
            guard let fd = desc.inputDescriptionsByName[name] else {
                throw CRError.msg("入力 \(name) はこのモデルに無い。ある名前：\(desc.inputDescriptionsByName.keys.sorted().joined(separator: "・"))")
            }
            let kind = s["kind"] as? String ?? "tensor"
            switch kind {
            case "tensor":
                guard let dts = s["dtype"] as? String, let dt = dtype(dts) else { throw CRError.msg("\(name)：dtype は float16・float32・float64・int32") }
                guard let shape = s["shape"] as? [Int], !shape.isEmpty, shape.allSatisfy({ $0 > 0 }) else { throw CRError.msg("\(name)：shape が違う") }
                let (off, n) = try slice(s, body)
                let arr = try MLMultiArray(shape: shape.map { NSNumber(value: $0) }, dataType: dt)
                guard n == arr.count * elemSize(dt) else { throw CRError.msg("\(name)：バイト数 \(n) が shape \(shape)・\(dts) と合わない") }
                let denseStrides = cStrides(shape)
                body.withUnsafeBytes { raw in
                    let src = raw.baseAddress!.advanced(by: off)
                    arr.withUnsafeMutableBytes { dst, strides in
                        copyStrided(src: src, srcStrides: denseStrides, dst: dst.baseAddress!, dstStrides: strides, shape: shape, elem: elemSize(dt))
                    }
                }
                dict[name] = MLFeatureValue(multiArray: arr)
            case "image":
                guard let ic = fd.imageConstraint else { throw CRError.msg("\(name) は画像の入力ではない") }
                let (off, n) = try slice(s, body)
                guard let ui = UIImage(data: body.subdata(in: off..<(off + n))), let cg = ui.cgImage else { throw CRError.msg("\(name)：画像を読めない") }
                let scale = s["scale"] as? String ?? "fill"
                let opt: VNImageCropAndScaleOption = scale == "fit" ? .scaleFit : (scale == "crop" ? .centerCrop : .scaleFill)
                // 向きは直さない。PC で EXIF の向きを直してから送る
                dict[name] = try MLFeatureValue(cgImage: cg, constraint: ic, options: [.cropAndScale: opt.rawValue])
            case "string":
                guard let v = s["value"] as? String else { throw CRError.msg("\(name)：value が無い") }
                dict[name] = MLFeatureValue(string: v)
            case "int":
                guard let v = num(s["value"]) else { throw CRError.msg("\(name)：value が無い") }
                dict[name] = MLFeatureValue(int64: v)
            case "double":
                guard let v = (s["value"] as? NSNumber)?.doubleValue else { throw CRError.msg("\(name)：value が無い") }
                dict[name] = MLFeatureValue(double: v)
            default:
                throw CRError.msg("\(name)：kind が違う：\(kind)")
            }
        }
        for (k, fd) in desc.inputDescriptionsByName where !fd.isOptional && dict[k] == nil {
            throw CRError.msg("入力 \(k) が足りない")
        }
        return try MLDictionaryFeatureProvider(dictionary: dict)
    }

    private static func slice(_ s: [String: Any], _ body: Data) throws -> (Int, Int) {
        let off = Int(num(s["offset"]) ?? 0), n = Int(num(s["nbytes"]) ?? 0)
        guard off >= 0, n >= 0, off + n <= body.count else { throw CRError.msg("offset・nbytes が中身の外に出ている") }
        return (off, n)
    }

    // ---- stride の違う 2 つの並びの間でコピーする ----
    // Neural Engine のモデルの出力は、最後の次元が 32 バイトに揃えられて隙間が入ることがある。だから stride を見て詰める
    private static func cStrides(_ shape: [Int]) -> [Int] {
        var s = [Int](repeating: 1, count: shape.count)
        if shape.count >= 2 { for i in stride(from: shape.count - 2, through: 0, by: -1) { s[i] = s[i + 1] * shape[i + 1] } }
        return s
    }

    private static func copyStrided(src: UnsafeRawPointer, srcStrides: [Int], dst: UnsafeMutableRawPointer, dstStrides: [Int],
                                    shape: [Int], elem: Int) {
        let nd = shape.count
        let total = shape.reduce(1, *)
        if nd == 0 || total == 0 { return }
        if srcStrides == dstStrides && srcStrides == cStrides(shape) {
            memcpy(dst, src, total * elem)
            return
        }
        let inner = shape[nd - 1]
        let rows = total / inner
        let innerContig = srcStrides[nd - 1] == 1 && dstStrides[nd - 1] == 1
        var idx = [Int](repeating: 0, count: max(nd - 1, 0))
        for _ in 0..<rows {
            var so = 0, dofs = 0
            for d in 0..<(nd - 1) {
                so += idx[d] * srcStrides[d]
                dofs += idx[d] * dstStrides[d]
            }
            if innerContig {
                memcpy(dst + dofs * elem, src + so * elem, inner * elem)
            } else {
                for k in 0..<inner {
                    memcpy(dst + (dofs + k * dstStrides[nd - 1]) * elem, src + (so + k * srcStrides[nd - 1]) * elem, elem)
                }
            }
            var d = nd - 2
            while d >= 0 {
                idx[d] += 1
                if idx[d] < shape[d] { break }
                idx[d] = 0
                d -= 1
            }
        }
    }

    // ---- 出力を詰める ----
    private static func packOutputs(_ out: MLFeatureProvider, replyBin: Bool) throws -> ([[String: Any]], Data) {
        var list: [[String: Any]] = []
        var blob = Data()
        func attach(_ bytes: Data, _ d: inout [String: Any]) {
            d["nbytes"] = bytes.count
            if replyBin {
                d["offset"] = blob.count
                blob.append(bytes)
            } else {
                d["b64"] = bytes.base64EncodedString()
            }
        }
        for name in out.featureNames.sorted() {
            guard let v = out.featureValue(for: name) else { continue }
            var d: [String: Any] = ["name": name]
            switch v.type {
            case .multiArray:
                guard let a = v.multiArrayValue else { continue }
                let es = elemSize(a.dataType)
                guard es > 0, a.count > 0 else { throw CRError.msg("出力 \(name)：この型は未対応（\(dtypeName(a.dataType))）") }
                let shape = a.shape.map { $0.intValue }
                let strides = a.strides.map { $0.intValue }
                var dense = Data(count: a.count * es)
                a.withUnsafeBytes { raw in
                    dense.withUnsafeMutableBytes { dst in
                        copyStrided(src: raw.baseAddress!, srcStrides: strides, dst: dst.baseAddress!, dstStrides: cStrides(shape), shape: shape, elem: es)
                    }
                }
                d["kind"] = "tensor"
                d["dtype"] = dtypeName(a.dataType)
                d["shape"] = shape
                attach(dense, &d)
            case .image:
                guard let pb = v.imageBufferValue else { continue }
                let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
                let fmt = CVPixelBufferGetPixelFormatType(pb)
                d["kind"] = "image"
                d["width"] = w
                d["height"] = h
                d["fourcc"] = fourcc(fmt)
                if fmt == kCVPixelFormatType_32BGRA {
                    guard let cs = CGColorSpace(name: CGColorSpace.sRGB),
                          let png = ciContext.pngRepresentation(of: CIImage(cvPixelBuffer: pb), format: .RGBA8, colorSpace: cs) else {
                        throw CRError.msg("出力 \(name)：PNG にできない")
                    }
                    d["fmt"] = "png"
                    attach(png, &d)
                } else {
                    let px: Int
                    switch fmt {
                    case kCVPixelFormatType_OneComponent8: px = 1
                    case kCVPixelFormatType_OneComponent16Half: px = 2
                    case kCVPixelFormatType_OneComponent32Float: px = 4
                    default: throw CRError.msg("出力 \(name)：この画素の形は未対応（\(fourcc(fmt))）")
                    }
                    CVPixelBufferLockBaseAddress(pb, .readOnly)
                    defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
                    guard let base = CVPixelBufferGetBaseAddress(pb) else { throw CRError.msg("出力 \(name)：画素を読めない") }
                    let bpr = CVPixelBufferGetBytesPerRow(pb)
                    var raw = Data(capacity: w * h * px)
                    for y in 0..<h { raw.append(Data(bytes: base + y * bpr, count: w * px)) } // 行の端の隙間は捨てる
                    d["fmt"] = "raw"
                    attach(raw, &d)
                }
            case .string:
                d["kind"] = "string"
                d["value"] = v.stringValue
            case .int64:
                d["kind"] = "int"
                d["value"] = v.int64Value
            case .double:
                d["kind"] = "double"
                d["value"] = jsonSafe(v.doubleValue)
            case .dictionary:
                d["kind"] = "dict"
                let top = v.dictionaryValue.map { (String(describing: $0.key), $0.value.doubleValue) }
                    .sorted { $0.1 > $1.1 }.prefix(50)
                d["value"] = top.map { ["key": $0.0, "value": jsonSafe($0.1)] as [String: Any] }
            default:
                d["kind"] = "unsupported"
            }
            list.append(d)
        }
        return (list, blob)
    }

    // ---- どの回路で走る見込みか ----
    private static func plan(_ req: [String: Any]) -> [String: Any] {
        guard #available(iOS 17.4, *) else { return err("MLComputePlan は iOS 17.4 から") }
        guard let id = req["id"] as? String, !id.contains("/"), !id.contains("..") else { return err("id が無い") }
        let compute = req["compute"] as? String ?? "cpuAndNeuralEngine"
        let url = compiledURL(id)
        guard fm.fileExists(atPath: url.path) else { return err("そのモデルは無い（id \(id)）") }
        do {
            let cfg = MLModelConfiguration()
            cfg.computeUnits = try units(compute)
            return try blocking(timeout: 600) { () async throws -> [String: Any] in
                let cp = try await MLComputePlan.load(contentsOf: url, configuration: cfg)
                var ane = 0, gpu = 0, cpu = 0, aneCapable = 0, total = 0, noDevice = 0
                var cpuOps: [String: Int] = [:]
                func visit(_ u: MLComputePlan.DeviceUsage?, _ opName: String) {
                    total += 1
                    guard let u else { noDevice += 1; return } // const など、回路を持たない物
                    switch u.preferred {
                    case .neuralEngine: ane += 1
                    case .gpu: gpu += 1
                    case .cpu:
                        cpu += 1
                        cpuOps[opName, default: 0] += 1
                    @unknown default: break
                    }
                    if u.supported.contains(where: { if case .neuralEngine = $0 { return true } else { return false } }) { aneCapable += 1 }
                }
                var kind = "other"
                switch cp.modelStructure {
                case .program(let p):
                    kind = "mlprogram"
                    for (_, f) in p.functions { for op in f.block.operations { visit(cp.deviceUsage(for: op), op.operatorName) } }
                case .neuralNetwork(let nn):
                    kind = "neuralnetwork"
                    for layer in nn.layers { visit(cp.deviceUsage(for: layer), layer.type) }
                default:
                    break // パイプラインなど
                }
                let worst = cpuOps.sorted { $0.value > $1.value }.prefix(10).map { ["op": $0.key, "count": $0.value] as [String: Any] }
                return ["structure": kind, "compute": compute, "ops": total, "no_device": noDevice,
                        "prefer_ane": ane, "prefer_gpu": gpu, "prefer_cpu": cpu, "ane_capable": aneCapable, "cpu_ops_top": worst]
            }
        } catch {
            return err("計画を読めない：\(error.localizedDescription)")
        }
    }

    // ---- Apple の機能が使える回路を聞く ----
    // 文字認識（Vision）は iOS 17 から、段ごとに使える回路を聞ける。音声認識（Speech）と言葉のモデル（Foundation Models）には同じ問い合わせが無い
    private static func probeApple() -> [String: Any] {
        var out: [String: Any] = [:]
        let levels: [(String, VNRequestTextRecognitionLevel)] = [("fast", .fast), ("accurate", .accurate)]
        for (label, level) in levels {
            let req = VNRecognizeTextRequest()
            req.recognitionLevel = level
            do {
                let m = try req.supportedComputeStageDevices
                var d: [String: [String]] = [:]
                for (stage, devs) in m { d[stage.rawValue] = devs.map { String(describing: $0) } }
                out["vision_text_\(label)"] = d
            } catch {
                out["vision_text_\(label)"] = "聞けない：\(error.localizedDescription)"
            }
        }
        out["all_devices"] = MLComputeDevice.allComputeDevices.map { String(describing: $0) }
        return out
    }
}
