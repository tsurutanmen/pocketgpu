// Windows から USB で、iPhone に圧縮と解凍をさせるための受け口（iPhone の中の 127.0.0.1:50065。PC は work/compress/iphone_compress.py）
// 弱いノート PC の代わりに、重い圧縮（xz の 9 や zstd の 19 など、遅い物ほど効く）を iPhone の 6 コアにさせる。
//
// 作りは AppleFeatures.swift と同じ：iPhone の中の 127.0.0.1 だけで待つ。届け物は [u32 長さ][JSON の頭][u32 長さ][中身のバイト]（リトルエンディアン）。
// ちがうのは、返事が頼みと 1 対 1 ではないこと。塊ごとに別の糸で圧縮するので、out があとから順不同で来る。一つのつなぎで何度でも続けられる。
//   PC → iPhone  hello / ping / bench / open / put / end / cancel
//   iPhone → PC  hello / pong / bench / open / out / progress / done / error
// 全文は iphone_compress.py の頭にある。要点：
//   open で方式（algo）・段階・糸の数・塊の大きさを決める。返事の window が、PC が「送ったが out が来ていない」塊の上限（iPhone のメモリを守る）。
//   put の中身 1 つが 1 つの塊。圧縮なら元のバイト、解凍なら 1 本の .xz（zstd なら 1 つのフレーム）と raw_len（元の大きさ）。
//   out の中身は、その塊だけで解ける 1 本の .xz（zstd なら 1 つのフレーム）。PC がつなぐだけで正しいファイルになる。
//
// 方式（algo）
//   "xz"   liblzma を組み込んだとき（コンパイル印 CZ_LZMA）は段階 0〜9 と extreme。
//          組み込んでいないときは Apple の LZMA（段階 6 固定・xz の入れ物）で代用し、open の返事の note で知らせる。
//   "zstd" libzstd を組み込んだとき（コンパイル印 CZ_ZSTD）だけ。段階 1〜22。
//   "apple:lzfse" "apple:lz4" "apple:lz4_raw" "apple:zlib" "apple:lzma" "apple:brotli"  Compression フレームワーク。段階は選べない。
//          apple:zlib はヘッダの無い deflate（PC は gzip の入れ物をかぶせる）。lzfse などは PC では解けないので、速さを測る（bench）用。
//
// 組み込み方（まだ組んでいない・Codemagic で組む前の下書き）
//   1. RpcServerApp.swift の AppleFeatures.shared.start() の次の行に CompressService.shared.start() を足す。
//   2. xz・zstd の段階を使うなら、liblzma（0BSD）と libzstd（BSD-3-Clause）の C のソースを vendor/ に置いて静的ライブラリにし、
//      Bridging-Header.h に #if __has_include("lzma.h") と #if __has_include("zstd.h") で include を足し、
//      project.yml の SWIFT_ACTIVE_COMPILATION_CONDITIONS に CZ_LZMA・CZ_ZSTD を足す（PUMPKIN_SWIFT_FLAG と同じやり方）。
//      入れなくても、Apple の部品だけで動く（xz は段階 6 固定）。
//   3. 速さは bench で測る。糸は DispatchQueue / OperationQueue の userInitiated で、性能コアから先に使われる。
//
// 気をつけること（実機で確かめる）
//   - メモリ：xz の 9 は辞書 64 MiB で 1 本あたり 674 MiB ほど食う。塊が辞書より小さければ辞書を塊の大きさに下げる（縮み方は変わらない）。
//     糸の数は os_proc_available_memory で抑える。increased-memory-limit の権限は付いている。
//   - 熱：全コアを何分も回すと熱で遅くなる。progress と bench に thermal を載せる。critical のときは糸を 2 本まで減らす。
//   - 後ろに回ったとき：音の再生で生きている（Keeper）が、CPU を使い続けるとき iOS が止めないかは実機で見る。
import Compression
import Foundation
import Network
import os

enum CZError: Error {
    case msg(String)
}

// ---- 方式ごとの圧縮・解凍 ----
struct CZCodec {
    let algo: String
    let engine: String
    let note: String
    let encode: (Data) throws -> Data
    let decode: (Data, Int) throws -> Data // 塊, 元の大きさ
    let memMB: (Int) -> Int // 塊の大きさ（バイト）→ 1 本の糸が使うメモリ（MB）の見積もり。実測ではない

    // xz の辞書の大きさ（preset 0〜9）と、xz のマニュアルにある 1 本あたりの圧縮メモリ（MiB。辞書が既定のとき）
    static let xzDictMB: [Double] = [0.25, 1, 2, 4, 4, 8, 8, 16, 32, 64]

    static func make(algo: String, level: Int, extreme: Bool) throws -> CZCodec {
        switch algo {
        case "xz":
            #if CZ_LZMA
            guard (0...9).contains(level) else { throw CZError.msg("xz の段階は 0〜9") }
            return CZCodec(algo: algo, engine: "liblzma", note: "",
                           encode: { try CZCodec.xzEncode($0, level: level, extreme: extreme) },
                           decode: { try CZCodec.xzDecode($0, rawLen: $1) },
                           memMB: { chunk in
                               // 辞書は塊の大きさで頭打ち。BT4 の探索は辞書の 10.5 倍ほど。入力と出力の置き場を足す
                               let dict = min(CZCodec.xzDictMB[level], Double(chunk) / 1048576)
                               return Int(dict * 10.5 + 3 + Double(chunk) * 2 / 1048576)
                           })
            #else
            return try appleCodec(algo: algo, apple: COMPRESSION_LZMA, engine: "apple-lzma",
                                  note: "liblzma が入っていないので Apple の LZMA（段階 6 固定）で代用", mem: { _ in 120 })
            #endif
        case "zstd":
            #if CZ_ZSTD
            guard (1...22).contains(level) else { throw CZError.msg("zstd の段階は 1〜22") }
            return CZCodec(algo: algo, engine: "libzstd", note: "",
                           encode: { try CZCodec.zstdEncode($0, level: level) },
                           decode: { try CZCodec.zstdDecode($0, rawLen: $1) },
                           memMB: { chunk in
                               let base = level <= 3 ? 40 : (level <= 9 ? 100 : (level <= 19 ? 250 : 1500))
                               return base + chunk * 2 / 1048576
                           })
            #else
            throw CZError.msg("この版には libzstd が入っていない（CZ_ZSTD が無い）")
            #endif
        case "apple:lzfse": return try appleCodec(algo: algo, apple: COMPRESSION_LZFSE, engine: "apple", note: "", mem: { $0 * 2 / 1048576 + 16 })
        case "apple:lz4": return try appleCodec(algo: algo, apple: COMPRESSION_LZ4, engine: "apple", note: "", mem: { $0 * 2 / 1048576 + 8 })
        case "apple:lz4_raw": return try appleCodec(algo: algo, apple: COMPRESSION_LZ4_RAW, engine: "apple", note: "", mem: { $0 * 2 / 1048576 + 8 })
        case "apple:zlib": return try appleCodec(algo: algo, apple: COMPRESSION_ZLIB, engine: "apple", note: "ヘッダの無い deflate・段階 5", mem: { $0 * 2 / 1048576 + 8 })
        case "apple:lzma": return try appleCodec(algo: algo, apple: COMPRESSION_LZMA, engine: "apple", note: "段階 6 固定・xz の入れ物", mem: { _ in 120 })
        case "apple:brotli": return try appleCodec(algo: algo, apple: COMPRESSION_BROTLI, engine: "apple", note: "", mem: { $0 * 2 / 1048576 + 64 })
        default:
            throw CZError.msg("知らない方式：\(algo)")
        }
    }

    // ---- Apple の Compression フレームワーク ----
    private static func appleCodec(algo: String, apple: compression_algorithm, engine: String, note: String, mem: @escaping (Int) -> Int) throws -> CZCodec {
        CZCodec(algo: algo, engine: engine, note: note,
                encode: { try CZCodec.appleEncode(apple, $0) },
                decode: { try CZCodec.appleDecode(apple, $0, rawLen: $1) },
                memMB: mem)
    }

    private static func appleEncode(_ alg: compression_algorithm, _ src: Data) throws -> Data {
        let cap = src.count + (src.count >> 3) + 4096
        var out = Data(count: cap)
        let n: Int = out.withUnsafeMutableBytes { (o: UnsafeMutableRawBufferPointer) -> Int in
            src.withUnsafeBytes { (s: UnsafeRawBufferPointer) -> Int in
                compression_encode_buffer(o.bindMemory(to: UInt8.self).baseAddress!, cap,
                                          s.bindMemory(to: UInt8.self).baseAddress!, s.count, nil, alg)
            }
        }
        if n == 0 { throw CZError.msg("Apple の圧縮が 0 バイトを返した（失敗か、出力先に入りきらない）") }
        return Data(out.prefix(n))
    }

    private static func appleDecode(_ alg: compression_algorithm, _ src: Data, rawLen: Int) throws -> Data {
        var out = Data(count: max(rawLen, 1))
        let n: Int = out.withUnsafeMutableBytes { (o: UnsafeMutableRawBufferPointer) -> Int in
            src.withUnsafeBytes { (s: UnsafeRawBufferPointer) -> Int in
                compression_decode_buffer(o.bindMemory(to: UInt8.self).baseAddress!, rawLen,
                                          s.bindMemory(to: UInt8.self).baseAddress!, s.count, nil, alg)
            }
        }
        if n != rawLen { throw CZError.msg("Apple の解凍の大きさが合わない（\(n) ≠ \(rawLen)）") }
        return Data(out.prefix(rawLen))
    }

    // ---- liblzma（組み込んだときだけ） ----
    #if CZ_LZMA
    private static func xzEncode(_ src: Data, level: Int, extreme: Bool) throws -> Data {
        var opt = lzma_options_lzma()
        let preset = UInt32(level) | (extreme ? 0x8000_0000 : 0) // LZMA_PRESET_EXTREME
        if lzma_lzma_preset(&opt, preset) != 0 { throw CZError.msg("xz の段階が不正") }
        // 塊より大きい辞書は使い道が無い（メモリだけ増える）。塊の大きさで頭打ちにする
        opt.dict_size = UInt32(max(4096, min(Int(opt.dict_size), src.count)))
        let cap = Int(lzma_stream_buffer_bound(src.count))
        var out = Data(count: cap)
        var outPos = 0
        let ret: lzma_ret = withUnsafeMutablePointer(to: &opt) { (optPtr: UnsafeMutablePointer<lzma_options_lzma>) -> lzma_ret in
            // LZMA_FILTER_LZMA2 = 0x21・LZMA_VLI_UNKNOWN = UINT64_MAX（どちらも関数の形の印なので Swift には見えない）
            var filters = [lzma_filter(id: 0x21, options: UnsafeMutableRawPointer(optPtr)),
                           lzma_filter(id: UInt64.max, options: nil)]
            return out.withUnsafeMutableBytes { (o: UnsafeMutableRawBufferPointer) -> lzma_ret in
                src.withUnsafeBytes { (s: UnsafeRawBufferPointer) -> lzma_ret in
                    lzma_stream_buffer_encode(&filters, LZMA_CHECK_CRC64, nil,
                                              s.bindMemory(to: UInt8.self).baseAddress, s.count,
                                              o.bindMemory(to: UInt8.self).baseAddress, &outPos, cap)
                }
            }
        }
        if ret != LZMA_OK { throw CZError.msg("xz の圧縮が失敗（\(ret.rawValue)）") }
        return Data(out.prefix(outPos))
    }

    private static func xzDecode(_ src: Data, rawLen: Int) throws -> Data {
        var out = Data(count: max(rawLen, 1))
        var inPos = 0
        var outPos = 0
        var memlimit = UInt64.max
        let ret: lzma_ret = out.withUnsafeMutableBytes { (o: UnsafeMutableRawBufferPointer) -> lzma_ret in
            src.withUnsafeBytes { (s: UnsafeRawBufferPointer) -> lzma_ret in
                lzma_stream_buffer_decode(&memlimit, 0, nil,
                                          s.bindMemory(to: UInt8.self).baseAddress, &inPos, s.count,
                                          o.bindMemory(to: UInt8.self).baseAddress, &outPos, rawLen)
            }
        }
        if ret != LZMA_OK || outPos != rawLen { throw CZError.msg("xz の解凍が失敗（\(ret.rawValue)・\(outPos) / \(rawLen)）") }
        return Data(out.prefix(rawLen))
    }
    #endif

    // ---- libzstd（組み込んだときだけ） ----
    #if CZ_ZSTD
    private static func zstdEncode(_ src: Data, level: Int) throws -> Data {
        guard let cctx = ZSTD_createCCtx() else { throw CZError.msg("zstd の入れ物を作れない") }
        defer { ZSTD_freeCCtx(cctx) }
        _ = ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, Int32(level))
        _ = ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1)
        let cap = ZSTD_compressBound(src.count)
        var out = Data(count: cap)
        let n: Int = out.withUnsafeMutableBytes { (o: UnsafeMutableRawBufferPointer) -> Int in
            src.withUnsafeBytes { (s: UnsafeRawBufferPointer) -> Int in
                ZSTD_compress2(cctx, o.baseAddress, cap, s.baseAddress, s.count)
            }
        }
        if ZSTD_isError(n) != 0 { throw CZError.msg("zstd の圧縮が失敗") }
        return Data(out.prefix(n))
    }

    private static func zstdDecode(_ src: Data, rawLen: Int) throws -> Data {
        var out = Data(count: max(rawLen, 1))
        let n: Int = out.withUnsafeMutableBytes { (o: UnsafeMutableRawBufferPointer) -> Int in
            src.withUnsafeBytes { (s: UnsafeRawBufferPointer) -> Int in
                ZSTD_decompress(o.baseAddress, rawLen, s.baseAddress, s.count)
            }
        }
        if ZSTD_isError(n) != 0 || n != rawLen { throw CZError.msg("zstd の解凍が失敗") }
        return Data(out.prefix(rawLen))
    }
    #endif
}

private func thermalName() -> String {
    switch ProcessInfo.processInfo.thermalState {
    case .nominal: return "nominal"
    case .fair: return "fair"
    case .serious: return "serious"
    case .critical: return "critical"
    @unknown default: return "unknown"
    }
}

// bench の糸たちが結果を足していく入れ物（@Sendable の中で外の var は書き換えられないので、鍵つきの class にする）
private final class CZTally {
    private let mu = NSLock()
    private(set) var total = 0
    private(set) var outLen = 0
    private(set) var lastEnd = Date.distantPast
    private(set) var failure: String?
    func add(_ n: Int, outLen o: Int) {
        mu.lock(); total += n; outLen = o; lastEnd = max(lastEnd, Date()); mu.unlock()
    }
    func fail(_ s: String) { mu.lock(); failure = s; mu.unlock() }
}

private func sysctlInt(_ name: String) -> Int? {
    var v: Int32 = 0
    var size = MemoryLayout<Int32>.size
    return sysctlbyname(name, &v, &size, nil, 0) == 0 ? Int(v) : nil
}

// ---- 受け口 ----
final class CompressService {
    static let shared = CompressService()
    static let maxHead = 1 << 20
    static let maxBody = 80 << 20 // 塊の上限は 64 MiB。圧縮が膨らむ分の余裕を足す
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "compress-listener", attributes: .concurrent)
    private let lock = NSLock()
    fileprivate var sessions: [ObjectIdentifier: CZSession] = [:]

    func start(port: UInt16 = 50065) {
        guard listener == nil, let p = NWEndpoint.Port(rawValue: port) else { return }
        let params = NWParameters.tcp
        // USB（iPhone の中の 127.0.0.1）だけで待つ。Wi-Fi には出さない
        params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: p)
        params.allowLocalEndpointReuse = true
        guard let l = try? NWListener(using: params) else { log("圧縮の受け口を作れない"); return }
        l.newConnectionHandler = { [weak self] c in
            guard let self else { c.cancel(); return }
            let s = CZSession(c, service: self)
            self.lock.lock(); self.sessions[ObjectIdentifier(s)] = s; self.lock.unlock()
            s.start()
        }
        l.start(queue: queue)
        listener = l
        log("圧縮の受け口：127.0.0.1:\(port) で待っている")
    }

    // 画面に出す用：いま動いている仕事の数
    var activeJobs: Int {
        lock.lock(); defer { lock.unlock() }
        return sessions.values.filter { $0.hasJob }.count
    }

    fileprivate func finished(_ s: CZSession) {
        lock.lock(); sessions.removeValue(forKey: ObjectIdentifier(s)); lock.unlock()
    }

    fileprivate func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }
}

// つなぎ 1 本ぶん。状態に触るのは q の上だけ（NWConnection の返しも、糸の仕事の終わりも q に戻してから触る）
fileprivate final class CZSession {
    private let c: NWConnection
    private let q = DispatchQueue(label: "compress-session")
    private unowned let service: CompressService
    private var closed = false
    private var benching = false
    private var job: CZJob?
    private var hasJobFlag = false
    var hasJob: Bool { hasJobFlag } // 画面に出す数を数えるだけ。q の外から読むので、一瞬ずれてもよい

    init(_ c: NWConnection, service: CompressService) {
        self.c = c
        self.service = service
    }

    func start() {
        c.stateUpdateHandler = { [weak self] st in
            if case .failed = st { self?.q.async { self?.close() } }
            if case .cancelled = st { self?.q.async { self?.close() } }
        }
        c.start(queue: q)
        readMessage()
    }

    // ---- 読む ----
    private func readExactly(_ n: Int, _ done: @escaping (Data?) -> Void) {
        if n == 0 { done(Data()); return }
        c.receive(minimumIncompleteLength: n, maximumLength: n) { data, _, _, err in
            if let data, data.count == n, err == nil { done(data) } else { done(nil) }
        }
    }

    private func readFrame(limit: Int, _ done: @escaping (Data?) -> Void) {
        readExactly(4) { [weak self] h in
            guard let self, let h else { done(nil); return }
            let n = Int(h.withUnsafeBytes { $0.load(as: UInt32.self) }.littleEndian)
            if n > limit { done(nil); return }
            self.readExactly(n, done)
        }
    }

    private func readMessage() {
        readFrame(limit: CompressService.maxHead) { [weak self] head in
            guard let self, let head else { self?.close(); return }
            self.readFrame(limit: CompressService.maxBody) { [weak self] body in
                guard let self, let body else { self?.close(); return }
                self.handle(head, body)
                if !self.closed { self.readMessage() } // すぐ次を読む。返事は別の糸の仕事が終わったときに出る
            }
        }
    }

    // ---- 書く（q の上だけで呼ぶ。呼んだ順に出る） ----
    private func send(_ head: [String: Any], _ body: Data = Data()) {
        if closed { return }
        let h = (try? JSONSerialization.data(withJSONObject: head)) ?? Data("{}".utf8)
        var frame = Data(capacity: 8 + h.count + body.count)
        var a = UInt32(h.count).littleEndian
        frame.append(Data(bytes: &a, count: 4))
        frame.append(h)
        var b = UInt32(body.count).littleEndian
        frame.append(Data(bytes: &b, count: 4))
        frame.append(body)
        c.send(content: frame, completion: .contentProcessed { [weak self] err in
            if err != nil { self?.q.async { self?.close() } }
        })
    }

    private func fail(_ text: String, seq: Int? = nil) {
        var h: [String: Any] = ["op": "error", "error": text]
        if let seq { h["seq"] = seq }
        send(h)
        service.log("圧縮：\(text)")
    }

    private func close() {
        if closed { return }
        closed = true
        job?.cancel()
        job = nil
        hasJobFlag = false
        c.cancel()
        service.finished(self)
    }

    // ---- 頼みを 1 つ受ける ----
    private func handle(_ headData: Data, _ body: Data) {
        let h = (try? JSONSerialization.jsonObject(with: headData)) as? [String: Any] ?? [:]
        switch h["op"] as? String ?? "" {
        case "hello": hello()
        case "ping": send(["op": "pong", "got": body.count], Data(count: min(h["reply_bytes"] as? Int ?? 0, CompressService.maxBody)))
        case "bench": bench(h, body)
        case "open": open(h)
        case "put": put(h, body)
        case "end": end()
        case "cancel": job?.cancel(); job = nil; hasJobFlag = false
        default: fail("知らない頼み：\(h["op"] as? String ?? "?")")
        }
    }

    private func hello() {
        var sys = utsname()
        uname(&sys)
        let model = withUnsafePointer(to: &sys.machine) { $0.withMemoryRebound(to: CChar.self, capacity: 256) { String(cString: $0) } }
        let v = ProcessInfo.processInfo.operatingSystemVersion
        var algos = ["xz", "apple:lzfse", "apple:lz4", "apple:lz4_raw", "apple:zlib", "apple:lzma", "apple:brotli"]
        var engines = ["xz": "apple-lzma"]
        #if CZ_LZMA
        engines["xz"] = "liblzma"
        #endif
        #if CZ_ZSTD
        algos.append("zstd")
        engines["zstd"] = "libzstd"
        #endif
        send(["op": "hello", "proto": 1, "algos": algos, "engines": engines, "model": model,
              "ios": "\(v.majorVersion).\(v.minorVersion).\(v.patchVersion)",
              "cores": ProcessInfo.processInfo.activeProcessorCount, "perf_cores": sysctlInt("hw.perflevel0.logicalcpu") ?? 0,
              "mem_avail_mb": os_proc_available_memory() >> 20, "thermal": thermalName(), "max_chunk_mb": 64])
    }

    // ---- 速さを測る：試料を、糸の数ごとに seconds 秒、繰り返し圧縮する ----
    private func bench(_ h: [String: Any], _ sample: Data) {
        if job != nil || benching { return fail("ほかの仕事の最中") }
        if sample.isEmpty { return fail("bench に試料が無い") }
        let codec: CZCodec
        do {
            codec = try CZCodec.make(algo: h["algo"] as? String ?? "xz", level: h["level"] as? Int ?? 6, extreme: h["extreme"] as? Bool ?? false)
        } catch {
            return fail("\(error)")
        }
        let steps = (h["threads"] as? [Int]) ?? [1]
        let seconds = h["seconds"] as? Double ?? 2
        benching = true
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var results: [[String: Any]] = []
            var failure: String?
            for n in steps where n >= 1 && n <= 16 {
                let stopAt = Date().addingTimeInterval(seconds)
                let tally = CZTally()
                let t0 = Date()
                let group = DispatchGroup()
                for _ in 0..<n {
                    DispatchQueue.global(qos: .userInitiated).async(group: group) {
                        var mine = 0
                        var o = 0
                        repeat {
                            do { o = try codec.encode(sample).count } catch { tally.fail("\(error)"); return }
                            mine += sample.count
                        } while Date() < stopAt
                        tally.add(mine, outLen: o)
                    }
                }
                group.wait()
                if let f = tally.failure { failure = f; break }
                let wall = max(tally.lastEnd.timeIntervalSince(t0), 0.001)
                results.append(["threads": n, "mbps": Double(tally.total) / 1048576 / wall,
                                "ratio": Double(tally.outLen) / Double(sample.count), "thermal": thermalName()])
            }
            self?.q.async {
                guard let self else { return }
                self.benching = false
                if let failure { self.fail("bench が失敗：\(failure)") } else { self.send(["op": "bench", "results": results]) }
            }
        }
    }

    // ---- 仕事を始める ----
    private func open(_ h: [String: Any]) {
        if job != nil || benching { return fail("ほかの仕事の最中") }
        let mode = h["mode"] as? String ?? "compress"
        guard mode == "compress" || mode == "decompress" else { return fail("mode は compress か decompress") }
        let chunk = h["chunk"] as? Int ?? (16 << 20)
        // 圧縮の塊は 64 MiB まで。解凍の chunk は「元の大きさ」の最大（解凍後の置き場の見積もりに使う）なので 256 MiB まで
        guard chunk >= 4096, chunk <= (mode == "decompress" ? 256 << 20 : 64 << 20) else { return fail("chunk は 4 KiB〜64 MiB（解凍は 256 MiB）") }
        let codec: CZCodec
        do {
            codec = try CZCodec.make(algo: h["algo"] as? String ?? "xz", level: h["level"] as? Int ?? 6, extreme: h["extreme"] as? Bool ?? false)
        } catch {
            return fail("\(error)")
        }
        // 糸の数：頼まれた数（0 なら全コア）を、空きメモリで抑える
        var threads = h["threads"] as? Int ?? 0
        if threads <= 0 { threads = ProcessInfo.processInfo.activeProcessorCount }
        if ProcessInfo.processInfo.thermalState == .critical { threads = min(threads, 2) }
        let avail = Int(os_proc_available_memory()) >> 20
        var note = codec.note
        if avail > 0 {
            let budget = avail * 6 / 10
            let per = codec.memMB(chunk)
            let fixed = { (t: Int) in (t + 2) * 2 * (chunk >> 20) } // 窓の分の入力と出力の置き場
            let want = threads
            while threads > 1 && threads * per + fixed(threads) > budget { threads -= 1 }
            if threads * per + fixed(threads) > budget { return fail("メモリが足りない（空き \(avail) MB・1 本 \(per) MB）。chunk を小さくするか、段階を下げる") }
            if threads < want { note += (note.isEmpty ? "" : "・") + "メモリのため糸を \(want) → \(threads) 本に減らした" }
        }
        let j = CZJob(mode: mode, codec: codec, threads: threads, window: threads + 2)
        job = j
        hasJobFlag = true
        j.startHeartbeat(on: q) { [weak self] head in self?.send(head) }
        service.log("圧縮：\(mode) \(codec.algo) \(codec.engine)・糸 \(threads)・塊 \(chunk >> 20) MiB")
        send(["op": "open", "ok": true, "threads": threads, "window": j.window, "engine": codec.engine, "note": note])
    }

    private func put(_ h: [String: Any], _ body: Data) {
        guard let j = job else { return fail("open の前に put が来た") }
        guard let seq = h["seq"] as? Int else { return fail("put に seq が無い") }
        j.puts += 1
        if j.puts - j.outStarted > j.window { return fail("窓（\(j.window)）を超えて送られた") }
        let rawLen = h["raw_len"] as? Int
        if j.mode == "decompress" && rawLen == nil { return fail("解凍の put には raw_len が要る", seq: seq) }
        j.ops.addOperation { [weak self, weak j] in
            guard let j, !j.isCancelled else { return }
            let t0 = DispatchTime.now()
            var out = Data()
            var failure: String?
            do {
                out = try (j.mode == "compress" ? j.codec.encode(body) : j.codec.decode(body, rawLen ?? 0))
            } catch {
                failure = "\(error)"
            }
            let ms = Int((DispatchTime.now().uptimeNanoseconds - t0.uptimeNanoseconds) / 1_000_000)
            self?.q.async {
                guard let self, self.job === j, !self.closed else { return }
                if let failure { self.fail("塊 \(seq) が失敗：\(failure)", seq: seq); return }
                j.outStarted += 1
                j.doneIn += j.mode == "compress" ? body.count : (rawLen ?? 0)
                j.doneOut += out.count
                j.chunks += 1
                j.cpuMs += ms
                self.send(["op": "out", "seq": seq, "in": body.count, "out": out.count, "ms": ms,
                           "done_in": j.doneIn, "done_out": j.doneOut], out)
                j.outDone += 1
                self.maybeFinish(j)
            }
        }
    }

    private func end() {
        guard let j = job else { return }
        j.ended = true
        maybeFinish(j)
    }

    // 受けた塊が全部 out になり、PC が end を送っていたら、done を出して仕事を閉じる（out は同じ待ち行列で先に出ている）
    private func maybeFinish(_ j: CZJob) {
        guard j.ended, j.outDone == j.puts else { return }
        let ms = Int(Date().timeIntervalSince(j.startedAt) * 1000)
        send(["op": "done", "chunks": j.chunks, "in": j.doneIn, "out": j.doneOut, "ms": ms, "cpu_ms": j.cpuMs])
        service.log("圧縮：終わり \(j.chunks) 塊・\(j.doneIn >> 20) MiB → \(j.doneOut >> 20) MiB・\(ms) ms")
        j.cancel()
        job = nil
        hasJobFlag = false
    }
}

// 仕事 1 つぶん。数える物は q の上だけで触る。ops は糸の数だけ同時に動く
fileprivate final class CZJob {
    let mode: String
    let codec: CZCodec
    let threads: Int
    let window: Int
    let ops = OperationQueue()
    let startedAt = Date()
    var puts = 0
    var outStarted = 0
    var outDone = 0
    var ended = false
    var doneIn = 0
    var doneOut = 0
    var chunks = 0
    var cpuMs = 0
    private(set) var isCancelled = false
    private var timer: DispatchSourceTimer?

    init(mode: String, codec: CZCodec, threads: Int, window: Int) {
        self.mode = mode
        self.codec = codec
        self.threads = threads
        self.window = window
        ops.maxConcurrentOperationCount = threads
        ops.qualityOfService = .userInitiated // 性能コアから先に使われる
    }

    // 1 秒ごとに、動いている塊の数と進みと熱を知らせる（1 つの塊が長くかかっても、止まっていないと分かる）
    func startHeartbeat(on q: DispatchQueue, send: @escaping ([String: Any]) -> Void) {
        let t = DispatchSource.makeTimerSource(queue: q)
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [weak self] in
            guard let self, !self.isCancelled else { return }
            send(["op": "progress", "running": self.puts - self.outStarted, "done_in": self.doneIn,
                  "thermal": thermalName(), "elapsed_ms": Int(Date().timeIntervalSince(self.startedAt) * 1000)])
        }
        t.resume()
        timer = t
    }

    func cancel() {
        isCancelled = true
        ops.cancelAllOperations()
        timer?.cancel()
        timer = nil
    }
}
