import AVFoundation
import CryptoKit
import ImageIO
import Network
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

// この iPhone の写真と動画を PC へ送る（ポート 50066。PC は work/xfer/pc/photo_rx.exe）。設計は work/xfer/DESIGN.md、約束は pc/pgx.h と同じ。
//
// 選ぶ：PHPickerViewController（別のプロセスで動くので、写真ライブラリの許可は要らない）。
//   「元のまま」は preferredAssetRepresentationMode = .current（HEIC と HEVC をそのまま）、「互換性優先」は .compatible（iOS が JPEG と H.264 に直す）。
//   選んだ物は 1 つずつ loadFileRepresentation で受け取り、tmp/PhotoTransfer に写す。SHA-256 と撮った日時を出してから PC に見せる。
// つなぎ：USB（iPhone の中の 127.0.0.1:50066）はアプリの起動から待つ。Wi-Fi は「Wi-Fi でも送る」がオンの間だけ待つ。
//   PC が引く形：PC が LIST で一覧をもらい、GET で 1 MiB の塊を頼む。こちらは頼まれた所を読んで返すだけ（DATA）。PC が受け取り終えたら DONE が来る。
//   USB と Wi-Fi のどちらから頼まれても同じ一覧を見せるので、PC が 2 本で同時に頼めば、速い方が多く運ぶ。片方が切れても PC がもう一方で頼み直す。
//   1 本のつなぎでは頼みを順に読み、送り終えていない返事が 2 つになったら、次の頼みを読むのを待つ（メモリを使いすぎない）。
// Wi-Fi の守り：握手で X25519 の鍵を作り、ペアリングの鍵（USB の HELLO_ACK で PC に渡す 32 バイト）か、画面の 6 けたのコードで、
//   両方が本物かを HMAC-SHA256 で確かめる。そのあとはメッセージ 1 つを AES-256-GCM の箱 1 つに入れる。
//   コードの間違いが 5 回続いたら Wi-Fi の受け付けを止め、画面の切り替えをオフに戻す。USB は 127.0.0.1 だけなので、確かめずに平文で送る。
// 画面に出す文は仮。言葉は鶴田さんが決める
final class PhotoTransfer: ObservableObject {
    static let shared = PhotoTransfer()
    static let port: UInt16 = 50066
    static let wifiKey = "xferWifi"                 // Wi-Fi でも送る
    static let keepOriginalKey = "xferKeepOriginal" // 元のまま送る
    private static let pairKeyKey = "xferPairKey"
    private static let deviceIDKey = "xferDeviceID"

    enum ItemState: Equatable { case loading, preparing, waiting, sending, saved, existed, failed, loadFailed, cancelled }
    struct Row: Identifiable, Equatable {
        let id: UInt64
        var name: String
        var size: UInt64
        var state: ItemState
        var progress: Double
    }

    // 画面に見せる（main だけで触る）
    @Published private(set) var rows: [Row] = []
    @Published private(set) var usbConnected = false
    @Published private(set) var wifiConnected = false
    @Published private(set) var speedText = ""
    @Published private(set) var wifiLocked = false
    // Wi-Fi で使うコード（起動のたびに変わる 6 けた）。PC は photo_rx --wifi iPhoneのアドレス --code これ
    let wifiCode = UInt32.random(in: 100_000...999_999)

    // ここから下の fileprivate の物は lock の中で触る（線の糸・準備の糸・main のどれからも触る）
    fileprivate let lock = NSLock()
    fileprivate var items: [XferItem] = []
    fileprivate var links: [ObjectIdentifier: XferLink] = [:]
    fileprivate var bytesUSB: UInt64 = 0
    fileprivate var bytesWifi: UInt64 = 0
    fileprivate var wifiFails = 0
    fileprivate var wifiListening = false
    fileprivate var offersGen: UInt32 = 0
    fileprivate var pairKey: [UInt8] = []
    fileprivate var deviceID: [UInt8] = []

    private let q = DispatchQueue(label: "photo-transfer")
    private var usbListener: NWListener?  // q の上だけ
    private var wifiListener: NWListener? // q の上だけ
    private var started = false
    private var pending: [(XferItem, NSItemProvider)] = [] // main だけ。まだ読み込んでいない物
    private var loadingOne = false                          // main だけ
    private let dir = FileManager.default.temporaryDirectory.appendingPathComponent("PhotoTransfer", isDirectory: true)
    private var lastTick = Date()
    private var lastUSB: UInt64 = 0
    private var lastWifi: UInt64 = 0
    private var bgTask: UIBackgroundTaskIdentifier = .invalid
    private var observers: [NSObjectProtocol] = []

    private init() {
        let d = UserDefaults.standard
        d.register(defaults: [Self.wifiKey: false, Self.keepOriginalKey: true])
        if let k = d.data(forKey: Self.pairKeyKey), k.count == 32 {
            pairKey = [UInt8](k)
        } else {
            pairKey = Self.randomBytes(32)
            d.set(Data(pairKey), forKey: Self.pairKeyKey)
        }
        if let k = d.data(forKey: Self.deviceIDKey), k.count == 16 {
            deviceID = [UInt8](k)
        } else {
            deviceID = Self.randomBytes(16)
            d.set(Data(deviceID), forKey: Self.deviceIDKey)
        }
    }

    static func randomBytes(_ n: Int) -> [UInt8] { (0..<n).map { _ in UInt8.random(in: 0...255) } }

    func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }

    // ---- main から呼ぶ ----

    // アプリの起動で 1 回。USB で待ち始め、設定がオンなら Wi-Fi でも待つ。前の起動の写しは消す
    func start() {
        guard !started else { return }
        started = true
        try? FileManager.default.removeItem(at: dir)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let nc = NotificationCenter.default
        // 資料のとおり、後ろに回る前（前から外れるとき）に頼んでおく
        observers.append(nc.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in self?.enteredBackground() })
        observers.append(nc.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in self?.endBackground() })
        let wifi = UserDefaults.standard.bool(forKey: Self.wifiKey)
        q.async { [weak self] in
            self?.listenUSB()
            if wifi { self?.listenWifi() }
        }
    }

    // 「Wi-Fi でも送る」を切り替えた
    func setWifi(_ on: Bool) {
        if on { wifiLocked = false }
        q.async { [weak self] in
            guard let self else { return }
            if on {
                self.lock.lock(); self.wifiFails = 0; self.lock.unlock()
                self.listenWifi()
            } else {
                self.closeWifi()
            }
        }
    }

    // ピッカーで選んだ物を、選んだ順に並べる。読み込みは 1 つずつ
    func add(_ results: [PHPickerResult]) {
        guard !results.isEmpty else { return }
        lock.lock()
        for r in results {
            let p = r.itemProvider
            let video = p.hasItemConformingToTypeIdentifier(UTType.movie.identifier)
            let it = XferItem(id: UInt64.random(in: 1...UInt64.max), name: p.suggestedName ?? (video ? "video" : "photo"), kind: video ? 2 : 1)
            items.append(it)
            pending.append((it, p))
        }
        lock.unlock()
        refresh()
        loadNext()
    }

    // 終わっていない物をすべて取り消す
    func cancelAll() {
        pending.removeAll()
        var urls: [URL] = []
        lock.lock()
        for it in items where Self.unfinished(it.state) {
            it.state = .cancelled
            it.progress?.cancel()
            if let u = closeFile(it) { urls.append(u) }
        }
        offersGen &+= 1
        lock.unlock()
        for u in urls { try? FileManager.default.removeItem(at: u) }
        refresh()
        endBackground()
    }

    // 終わった物を一覧から消す
    func clearFinished() {
        lock.lock()
        items.removeAll { !Self.unfinished($0.state) && $0.readers == 0 }
        lock.unlock()
        refresh()
    }

    // ペアリングの鍵を作り直す。前に USB でつないだ PC は、もう一度 USB かコードでつなぐ
    func resetPairing() {
        let k = Self.randomBytes(32)
        UserDefaults.standard.set(Data(k), forKey: Self.pairKeyKey)
        lock.lock(); pairKey = k; lock.unlock()
        log("写真の送信：ペアリングの鍵を作り直した")
    }

    // 画面の時計から 0.5 秒ごと。一覧・接続・速さを写す
    func refresh() {
        let now = Date()
        lock.lock()
        let rs: [Row] = items.map { (it: XferItem) -> Row in
            var p = 0.0
            var st = it.state
            if st == .loading {
                p = it.progress?.fractionCompleted ?? 0
            } else if st == .waiting {
                p = it.size > 0 ? Double(it.servedBytes) / Double(it.size) : 0
                if it.servedBytes > 0 { st = .sending }
            }
            return Row(id: it.id, name: it.name, size: it.size, state: st, progress: min(1, max(0, p)))
        }
        let usb = links.values.contains { !$0.wifi && $0.ready }
        let wifi = links.values.contains { $0.wifi && $0.ready }
        let bu = bytesUSB, bw = bytesWifi
        lock.unlock()
        let dt = now.timeIntervalSince(lastTick)
        if dt >= 0.4 {
            let ru = Double(bu &- lastUSB) / dt / 1e6, rw = Double(bw &- lastWifi) / dt / 1e6
            lastUSB = bu
            lastWifi = bw
            lastTick = now
            var parts: [String] = []
            if ru >= 0.05 { parts.append("USB " + String(format: "%.1f", ru) + " MB/s") }
            if rw >= 0.05 { parts.append("Wi-Fi " + String(format: "%.1f", rw) + " MB/s") }
            let s = parts.joined(separator: "・")
            if s != speedText { speedText = s }
        }
        if rs != rows { rows = rs }
        if usb != usbConnected { usbConnected = usb }
        if wifi != wifiConnected { wifiConnected = wifi }
    }

    static func stateText(_ r: Row) -> String {
        switch r.state {
        case .loading: return L("読み込み中です", "Loading")
        case .preparing: return L("準備中です", "Preparing")
        case .waiting: return L("PC の受信アプリを待っています", "Waiting for the receiver app on the PC")
        case .sending: return L("送信中です", "Sending") + " \(Int(r.progress * 100))%"
        case .saved: return L("PC に保存しました", "Saved on the PC")
        case .existed: return L("PC に同じファイルがあります", "Already on the PC")
        case .failed: return L("送信できませんでした", "Could not send")
        case .loadFailed: return L("読み込めませんでした", "Could not load")
        case .cancelled: return L("取り消しました", "Canceled")
        }
    }

    private static func unfinished(_ s: ItemState) -> Bool { s == .loading || s == .preparing || s == .waiting || s == .sending }

    // ---- 読み込み（main）→ 準備（ほかの糸）----

    private func loadNext() {
        guard !loadingOne, !pending.isEmpty else { return }
        let (it, p) = pending.removeFirst()
        lock.lock(); let cancelled = it.state == .cancelled; lock.unlock()
        if cancelled { loadNext(); return }
        loadingOne = true
        // 動画は動画の、写真は写真の、いちばん先に並んでいる形（元のままなら HEIC・HEVC、互換性優先なら JPEG・H.264）
        let want: UTType = it.kind == 2 ? .movie : .image
        let tid = p.registeredTypeIdentifiers.first { UTType($0)?.conforms(to: want) == true } ?? want.identifier
        let dst = dir
        let prog = p.loadFileRepresentation(forTypeIdentifier: tid) { [weak self] url, err in
            // url はこの中でしか残らないので、ここで写す（同じボリュームなので APFS の複製で速い）
            // 写せた先（let にする。@Sendable の返しに var を持ち込まない）
            let copied: URL? = url.flatMap { from in
                let ext = from.pathExtension.isEmpty ? (UTType(tid)?.preferredFilenameExtension ?? "dat") : from.pathExtension
                let to = dst.appendingPathComponent(String(format: "%016llx", it.id)).appendingPathExtension(ext)
                return (try? FileManager.default.copyItem(at: from, to: to)) != nil ? to : nil
            }
            DispatchQueue.main.async { self?.loaded(it, copied, err) }
        }
        lock.lock(); it.progress = prog; lock.unlock()
    }

    private func loaded(_ it: XferItem, _ url: URL?, _ err: Error?) {
        loadingOne = false
        lock.lock()
        it.progress = nil
        let cancelled = it.state == .cancelled
        if !cancelled { it.state = url == nil ? .loadFailed : .preparing }
        lock.unlock()
        if let url, cancelled { try? FileManager.default.removeItem(at: url) }
        if url == nil && !cancelled { log("写真の送信：読み込めない \(it.name) \(String(describing: err))") }
        if let url, !cancelled { prepare(it, url) }
        loadNext()
    }

    // SHA-256 と撮った日時。終わったら PC に見せる
    private func prepare(_ it: XferItem, _ url: URL) {
        let video = it.kind == 2
        Task.detached(priority: .utility) {
            let hashed = PhotoTransfer.hashFile(url)
            let date: Date?
            if video {
                date = await PhotoTransfer.videoDate(url)
            } else {
                date = PhotoTransfer.photoDate(url)
            }
            PhotoTransfer.shared.ready(it, url, hashed, date)
        }
    }

    private func ready(_ it: XferItem, _ url: URL, _ hashed: (size: UInt64, sha: [UInt8])?, _ date: Date?) {
        var fd: Int32 = -1
        if hashed != nil {
            fd = url.withUnsafeFileSystemRepresentation { (p: UnsafePointer<CChar>?) -> Int32 in
                guard let p else { return -1 }
                return Darwin.open(p, O_RDONLY)
            }
        }
        lock.lock()
        guard it.state == .preparing, let hashed, fd >= 0 else {
            if it.state == .preparing { it.state = .loadFailed }
            lock.unlock()
            if fd >= 0 { Darwin.close(fd) }
            try? FileManager.default.removeItem(at: url)
            return
        }
        it.url = url
        it.fd = fd
        it.size = hashed.size
        it.sha = hashed.sha
        it.mtimeMs = date.map { Int64($0.timeIntervalSince1970 * 1000) } ?? 0
        let ext = url.pathExtension
        if !ext.isEmpty && (it.name as NSString).pathExtension.lowercased() != ext.lowercased() { it.name += "." + ext }
        it.state = .waiting
        offersGen &+= 1
        lock.unlock()
    }

    private static func hashFile(_ url: URL) -> (size: UInt64, sha: [UInt8])? {
        guard let fh = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? fh.close() }
        var h = SHA256()
        var total: UInt64 = 0
        while true {
            let chunk: Data
            do {
                chunk = try autoreleasepool { try fh.read(upToCount: 4 << 20) } ?? Data()
            } catch {
                return nil
            }
            if chunk.isEmpty { break }
            h.update(data: chunk)
            total += UInt64(chunk.count)
        }
        return (total, Array(h.finalize()))
    }

    // 写真：Exif の撮影日時（OffsetTimeOriginal があればその時差、無ければ iPhone の時差）
    private static func photoDate(_ url: URL) -> Date? {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [String: Any],
              let exif = props[kCGImagePropertyExifDictionary as String] as? [String: Any],
              let s = exif[kCGImagePropertyExifDateTimeOriginal as String] as? String else { return nil }
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy:MM:dd HH:mm:ss"
        if let off = exif[kCGImagePropertyExifOffsetTimeOriginal as String] as? String, let tz = PhotoTransfer.timeZone(off) {
            f.timeZone = tz
        } else {
            f.timeZone = TimeZone.current
        }
        return f.date(from: s)
    }

    private static func timeZone(_ s: String) -> TimeZone? {
        guard let first = s.first, first == "+" || first == "-" else { return nil }
        let parts = s.dropFirst().split(separator: ":")
        guard parts.count == 2, let h = Int(parts[0]), let m = Int(parts[1]) else { return nil }
        return TimeZone(secondsFromGMT: (first == "-" ? -1 : 1) * (h * 3600 + m * 60))
    }

    // 動画：作成日（QuickTime の creationdate）
    private static func videoDate(_ url: URL) async -> Date? {
        let asset = AVURLAsset(url: url)
        guard let item = try? await asset.load(.creationDate) else { return nil }
        return try? await item.load(.dateValue)
    }

    // ---- 後ろに回ったとき：送っている途中なら、iOS がくれる少しの時間だけ続ける ----

    private func enteredBackground() {
        guard bgTask == .invalid, hasWork() else { return }
        bgTask = UIApplication.shared.beginBackgroundTask(withName: "PhotoTransfer") { [weak self] in self?.endBackground() }
    }

    private func endBackground() {
        guard bgTask != .invalid else { return }
        UIApplication.shared.endBackgroundTask(bgTask)
        bgTask = .invalid
    }

    private func hasWork() -> Bool {
        lock.lock(); defer { lock.unlock() }
        return items.contains { Self.unfinished($0.state) }
    }

    // ---- 待つ（q の上）----

    private static func params() -> NWParameters {
        let tcp = NWProtocolTCP.Options()
        tcp.noDelay = true // GET と一覧の返事は小さいので、まとめ待ちをさせない
        tcp.enableKeepalive = true
        tcp.keepaliveIdle = 15
        let p = NWParameters(tls: nil, tcp: tcp)
        p.allowLocalEndpointReuse = true
        return p
    }

    private func listenUSB() {
        guard usbListener == nil, let p = NWEndpoint.Port(rawValue: Self.port) else { return }
        let params = Self.params()
        params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: p) // USB（iPhone の中の 127.0.0.1）だけ
        guard let l = try? NWListener(using: params) else {
            log("写真の送信：USB のポートを開けない。1 秒後に開き直す")
            q.asyncAfter(deadline: .now() + 1) { [weak self] in self?.listenUSB() }
            return
        }
        l.stateUpdateHandler = { [weak self, weak l] st in
            guard let self, let l, l === self.usbListener else { return }
            switch st {
            case .ready:
                self.log("写真の送信：127.0.0.1:\(Self.port) で待っている")
            case .failed(let e):
                self.log("写真の送信：USB のポートが閉じた \(e)。1 秒後に開き直す")
                self.usbListener = nil
                l.cancel()
                self.q.asyncAfter(deadline: .now() + 1) { [weak self] in self?.listenUSB() }
            default:
                break
            }
        }
        l.newConnectionHandler = { [weak self] c in
            guard let self else { c.cancel(); return }
            self.accept(c, wifi: false)
        }
        usbListener = l
        l.start(queue: q)
    }

    private func listenWifi() {
        guard wifiListener == nil, UserDefaults.standard.bool(forKey: Self.wifiKey), let p = NWEndpoint.Port(rawValue: Self.port) else { return }
        let params = Self.params()
        params.requiredInterfaceType = .wifi // Wi-Fi のポートだけ。鍵かコードを確かめる
        guard let l = try? NWListener(using: params, on: p) else { log("写真の送信：Wi-Fi のポートを開けない"); return }
        l.stateUpdateHandler = { [weak self, weak l] st in
            guard let self, let l, l === self.wifiListener else { return }
            switch st {
            case .ready:
                self.log("写真の送信：Wi-Fi の \(Self.port) 番でも待っている")
                self.lock.lock(); self.wifiListening = true; self.offersGen &+= 1; self.lock.unlock()
            case .failed(let e):
                self.log("写真の送信：Wi-Fi のポートが閉じた \(e)")
                self.wifiListener = nil
                l.cancel()
                self.lock.lock(); self.wifiListening = false; self.lock.unlock()
            default:
                break
            }
        }
        l.newConnectionHandler = { [weak self] c in
            guard let self else { c.cancel(); return }
            self.accept(c, wifi: true)
        }
        wifiListener = l
        l.start(queue: q)
    }

    private func closeWifi() {
        if let l = wifiListener {
            wifiListener = nil
            l.cancel()
            log("写真の送信：Wi-Fi のポートを閉じた")
        }
        lock.lock()
        wifiListening = false
        let ws = links.values.filter { $0.wifi }
        lock.unlock()
        for w in ws { w.closeFromOutside() }
    }

    private func accept(_ c: NWConnection, wifi: Bool) {
        let link = XferLink(c, wifi: wifi, owner: self)
        lock.lock()
        let full = links.count >= 8 // 握手の途中の接続をたくさん開かれても、8 本までしか受けない
        if !full { links[ObjectIdentifier(link)] = link }
        lock.unlock()
        if full { c.cancel(); return }
        link.start()
    }

    // ---- 線から呼ぶ（線の糸の上）----

    fileprivate func removed(_ link: XferLink) {
        lock.lock(); links.removeValue(forKey: ObjectIdentifier(link)); lock.unlock()
    }

    fileprivate func linkUp(_ link: XferLink) {
        lock.lock(); link.ready = true; lock.unlock()
        log("写真の送信：PC が \(link.wifi ? "Wi-Fi" : "USB") で接続した")
    }

    fileprivate func ackInfo() -> (deviceID: [UInt8], pairKey: [UInt8], wifiOn: Bool, ip: [UInt8]?) {
        lock.lock()
        let d = deviceID, k = pairKey, w = wifiListening
        lock.unlock()
        return (d, k, w, w ? Self.wifiIPv4() : nil)
    }

    private static func wifiIPv4() -> [UInt8]? {
        guard let ip = listAddresses().first(where: { $0.name == "en0" })?.ip else { return nil }
        let parts = ip.split(separator: ".").compactMap { UInt8($0) }
        return parts.count == 4 ? parts : nil
    }

    fileprivate var isLocked: Bool {
        lock.lock(); defer { lock.unlock() }
        return wifiFails >= 5
    }

    // key_id = SHA256("PGX1 key-id" || 鍵) の先頭 8 バイト（鍵そのものは見せない）
    fileprivate func keyID() -> [UInt8] {
        var d = Data("PGX1 key-id".utf8)
        d.append(contentsOf: pairKeyBytes())
        return Array(Array(SHA256.hash(data: d)).prefix(8))
    }

    fileprivate func pairKeyBytes() -> [UInt8] {
        lock.lock(); defer { lock.unlock() }
        return pairKey
    }

    // コードから作る秘密 = SHA256("PGX1 code" || 6 文字の数字)
    fileprivate func codeSecret() -> [UInt8] {
        var d = Data("PGX1 code".utf8)
        d.append(contentsOf: Array(String(format: "%06u", wifiCode).utf8))
        return Array(SHA256.hash(data: d))
    }

    fileprivate func authFailed() {
        lock.lock(); wifiFails += 1; let n = wifiFails; lock.unlock()
        log("写真の送信：Wi-Fi の鍵かコードが違う（\(n) 回目）")
        guard n >= 5 else { return }
        q.async { [weak self] in self?.closeWifi() }
        DispatchQueue.main.async { [weak self] in
            self?.wifiLocked = true
            UserDefaults.standard.set(false, forKey: PhotoTransfer.wifiKey)
        }
    }

    fileprivate func authOK() {
        lock.lock(); wifiFails = 0; lock.unlock()
    }

    // 一覧：頭 16 バイト（gen・数・読み込み中の数・Wi-Fi で受け付けているか）＋項目（id・大きさ・日時・SHA-256・種類・名前）
    fileprivate func offers() -> Data {
        lock.lock()
        let ready = items.filter { $0.state == .waiting && !$0.closePending }
        let prep = items.filter { $0.state == .loading || $0.state == .preparing }.count
        var d = Data()
        XferWire.le32(&d, offersGen)
        XferWire.le32(&d, UInt32(ready.count))
        XferWire.le32(&d, UInt32(prep))
        XferWire.le32(&d, wifiListening ? 1 : 0)
        for it in ready {
            let nm = Array(it.name.utf8.prefix(255))
            XferWire.le64(&d, it.id)
            XferWire.le64(&d, it.size)
            XferWire.le64(&d, UInt64(bitPattern: it.mtimeMs))
            d.append(contentsOf: it.sha)
            XferWire.le32(&d, it.kind)
            XferWire.le32(&d, UInt32(nm.count))
            d.append(contentsOf: nm)
        }
        lock.unlock()
        return d
    }

    // 頼まれた塊を読む。状態：0 よい・1 もう無い・2 読めない・3 範囲が変
    fileprivate func readChunk(_ id: UInt64, _ off: UInt64, _ n: Int) -> (Int32, Data) {
        lock.lock()
        guard let it = items.first(where: { $0.id == id }), it.fd >= 0, !it.closePending else { lock.unlock(); return (1, Data()) }
        guard n >= 0, n <= 8 << 20, off <= it.size, it.size - off >= UInt64(n) else { lock.unlock(); return (3, Data()) }
        it.readers += 1 // 読んでいる間は閉じない（取り消し・DONE は readers が 0 になってから閉じる）
        let fd = it.fd
        lock.unlock()
        var buf = Data(count: n)
        var got = 0
        let ok: Bool = buf.withUnsafeMutableBytes { (p: UnsafeMutableRawBufferPointer) -> Bool in
            guard let base = p.baseAddress else { return n == 0 }
            while got < n {
                let r = Darwin.pread(fd, base + got, n - got, off_t(off) + off_t(got))
                if r <= 0 { return false }
                got += r
            }
            return true
        }
        var del: URL?
        lock.lock()
        it.readers -= 1
        if it.closePending && it.readers == 0 { del = finishClose(it) }
        lock.unlock()
        if let del { try? FileManager.default.removeItem(at: del) }
        return ok ? (0, buf) : (2, Data())
    }

    fileprivate func served(_ id: UInt64, _ off: UInt64, _ n: Int, wifi: Bool) {
        lock.lock()
        if wifi { bytesWifi &+= UInt64(n) } else { bytesUSB &+= UInt64(n) }
        if let it = items.first(where: { $0.id == id }), it.served.insert(off).inserted { it.servedBytes += UInt64(n) }
        lock.unlock()
    }

    // PC が受け取り終えた。結果：0 保存・1 同じファイルがあった・2 別の名前で保存・負は失敗
    fileprivate func done(_ id: UInt64, _ result: Int32) {
        var del: URL?
        var name = ""
        lock.lock()
        if let it = items.first(where: { $0.id == id }), it.state == .waiting {
            it.state = result == 1 ? .existed : result >= 0 ? .saved : .failed
            name = it.name
            del = closeFile(it)
            offersGen &+= 1
        }
        lock.unlock()
        if let del { try? FileManager.default.removeItem(at: del) }
        if !name.isEmpty { log("写真の送信：PC が \(name) を受け取った（結果 \(result)）") }
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            if !self.hasWork() { self.endBackground() }
        }
    }

    // lock の中で呼ぶ。読んでいる線がいなければ閉じて、消すファイルを返す
    private func closeFile(_ it: XferItem) -> URL? {
        it.closePending = true
        return it.readers == 0 ? finishClose(it) : nil
    }

    private func finishClose(_ it: XferItem) -> URL? {
        if it.fd >= 0 { Darwin.close(it.fd); it.fd = -1 }
        let u = it.url
        it.url = nil
        return u
    }
}

// 送る物 1 つ。中身は PhotoTransfer.lock の中で触る
fileprivate final class XferItem: @unchecked Sendable {
    let id: UInt64
    let kind: UInt32 // 1 写真・2 動画
    var name: String
    var size: UInt64 = 0
    var mtimeMs: Int64 = 0
    var sha: [UInt8] = []
    var url: URL?
    var fd: Int32 = -1
    var state: PhotoTransfer.ItemState = .loading
    var progress: Progress?
    var served = Set<UInt64>() // 送った塊の位置（PC が重ねて頼んだ分を二重に数えない）
    var servedBytes: UInt64 = 0
    var readers = 0
    var closePending = false

    init(id: UInt64, name: String, kind: UInt32) {
        self.id = id
        self.name = name
        self.kind = kind
    }
}

// 約束の数（pc/pgx.h と同じ）
fileprivate enum XferWire {
    static let magic: UInt32 = 0x3158_4750 // "PGX1"
    static let version: UInt32 = 1
    static let hello: UInt32 = 1, ack: UInt32 = 2, auth: UInt32 = 3, authResult: UInt32 = 4
    static let list: UInt32 = 10, get: UInt32 = 11, done: UInt32 = 12, offers: UInt32 = 30, data: UInt32 = 31
    static let helloLen = 128, ackLen = 168, resultLen = 36
    static let maxIn = 1 << 16 // PC から来るメッセージの上限（GET・DONE・LIST は小さい）
    static let dirPhone: UInt32 = 1, dirPC: UInt32 = 2

    static func u32(_ b: [UInt8], _ at: Int) -> UInt32 {
        UInt32(b[at]) | UInt32(b[at + 1]) << 8 | UInt32(b[at + 2]) << 16 | UInt32(b[at + 3]) << 24
    }
    static func u64(_ b: [UInt8], _ at: Int) -> UInt64 { UInt64(u32(b, at)) | UInt64(u32(b, at + 4)) << 32 }
    static func le32(_ d: inout Data, _ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    static func le64(_ d: inout Data, _ v: UInt64) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    static func put32(_ a: inout [UInt8], _ at: Int, _ v: UInt32) { for i in 0..<4 { a[at + i] = UInt8((v >> (8 * UInt32(i))) & 0xff) } }
    // 箱の nonce：向き（u32）＋数（u64）
    static func nonce(_ dir: UInt32, _ ctr: UInt64) -> [UInt8] {
        var d = Data()
        le32(&d, dir)
        le64(&d, ctr)
        return [UInt8](d)
    }
    static func same(_ a: [UInt8], _ b: [UInt8]) -> Bool {
        guard a.count == b.count else { return false }
        var x: UInt8 = 0
        for i in 0..<a.count { x |= a[i] ^ b[i] }
        return x == 0
    }
}

// つなぎ 1 本（USB か Wi-Fi）。状態に触るのは q の上だけ（NWConnection の返しも q に来る）
fileprivate final class XferLink {
    let c: NWConnection
    let wifi: Bool
    unowned let owner: PhotoTransfer
    var ready = false // 握手が済んだ。owner.lock の中で触る
    private let q: DispatchQueue
    private var key: SymmetricKey?
    private var txCtr: UInt64 = 0
    private var rxCtr: UInt64 = 0
    private var authed = false
    private var hello: [UInt8] = []
    private var ack: [UInt8] = []
    private var eph: Curve25519.KeyAgreement.PrivateKey?
    private var authMode: UInt32 = 0
    private var reading = false
    private var sending = 0
    private var closed = false

    init(_ c: NWConnection, wifi: Bool, owner: PhotoTransfer) {
        self.c = c
        self.wifi = wifi
        self.owner = owner
        q = DispatchQueue(label: wifi ? "photo-transfer-wifi" : "photo-transfer-usb", qos: .userInitiated)
    }

    func start() {
        c.stateUpdateHandler = { [weak self] st in
            switch st {
            case .failed, .cancelled: self?.close()
            default: break
            }
        }
        c.start(queue: q)
        // 握手が 10 秒で終わらなければ切る
        q.asyncAfter(deadline: .now() + 10) { [weak self] in
            if let self, !self.authed { self.close() }
        }
        pump()
    }

    func closeFromOutside() { q.async { [weak self] in self?.close() } }

    private func close() {
        guard !closed else { return }
        closed = true
        c.cancel()
        owner.removed(self)
        if authed { owner.log("写真の送信：\(wifi ? "Wi-Fi" : "USB") の接続が切れた") }
    }

    private func closeSoon() {
        q.asyncAfter(deadline: .now() + 0.2) { [weak self] in self?.close() }
    }

    // 頼みを 1 つ読んで答え、次を読む。送り終えていない返事が 2 つあるときは待つ
    private func pump() {
        guard !closed, !reading, sending < 2 else { return }
        reading = true
        readMessage { [weak self] type, body in
            guard let self else { return }
            self.reading = false
            guard !self.closed else { return }
            self.handle(type, body)
            self.pump()
        }
    }

    private func readExactly(_ n: Int, _ done: @escaping ([UInt8]?) -> Void) {
        if n == 0 { done([]); return }
        c.receive(minimumIncompleteLength: n, maximumLength: n) { data, _, _, err in
            if let data, data.count == n, err == nil { done([UInt8](data)) } else { done(nil) }
        }
    }

    // 平文：[u32 型][u32 長さ][中身]。箱：[u32 n][暗号文 n][タグ 16]（中は平文と同じ形）
    private func readMessage(_ done: @escaping (UInt32, [UInt8]) -> Void) {
        if let key {
            readExactly(4) { [weak self] h in
                guard let self else { return }
                guard let h else { self.close(); return }
                let n = Int(XferWire.u32(h, 0))
                guard n >= 8, n <= XferWire.maxIn + 8 else { self.close(); return }
                self.readExactly(n + 16) { [weak self] box in
                    guard let self else { return }
                    guard let box, let plain = self.unseal(box, n: n, aad: h, key: key), Int(XferWire.u32(plain, 4)) == n - 8 else {
                        self.owner.log("写真の送信：Wi-Fi の箱が壊れている。切る")
                        self.close()
                        return
                    }
                    done(XferWire.u32(plain, 0), Array(plain[8...]))
                }
            }
        } else {
            readExactly(8) { [weak self] h in
                guard let self else { return }
                guard let h else { self.close(); return }
                let len = Int(XferWire.u32(h, 4))
                guard len <= XferWire.maxIn else { self.close(); return }
                self.readExactly(len) { [weak self] body in
                    guard let self else { return }
                    guard let body else { self.close(); return }
                    done(XferWire.u32(h, 0), body)
                }
            }
        }
    }

    private func unseal(_ box: [UInt8], n: Int, aad: [UInt8], key: SymmetricKey) -> [UInt8]? {
        let nonce = XferWire.nonce(XferWire.dirPC, rxCtr)
        rxCtr &+= 1
        guard let nn = try? AES.GCM.Nonce(data: nonce),
              let sb = try? AES.GCM.SealedBox(nonce: nn, ciphertext: Data(box[0..<n]), tag: Data(box[n..<(n + 16)])),
              let plain = try? AES.GCM.open(sb, using: key, authenticating: Data(aad)) else { return nil }
        return [UInt8](plain)
    }

    private func send(_ type: UInt32, _ body: Data) {
        var inner = Data(capacity: body.count + 8)
        XferWire.le32(&inner, type)
        XferWire.le32(&inner, UInt32(body.count))
        inner.append(body)
        var out: Data
        if let key {
            var aad = Data()
            XferWire.le32(&aad, UInt32(inner.count))
            let nonce = XferWire.nonce(XferWire.dirPhone, txCtr)
            txCtr &+= 1
            guard let nn = try? AES.GCM.Nonce(data: nonce), let sb = try? AES.GCM.seal(inner, using: key, nonce: nn, authenticating: aad) else {
                close()
                return
            }
            out = aad
            out.append(sb.ciphertext)
            out.append(sb.tag)
        } else {
            out = inner
        }
        sending += 1
        c.send(content: out, completion: .contentProcessed { [weak self] err in
            guard let self else { return }
            self.sending -= 1
            if err != nil { self.close(); return }
            self.pump()
        })
    }

    private func handle(_ type: UInt32, _ b: [UInt8]) {
        if !authed {
            switch type {
            case XferWire.hello: onHello(b)
            case XferWire.auth where wifi: onAuth(b)
            default: close()
            }
            return
        }
        switch type {
        case XferWire.list:
            send(XferWire.offers, owner.offers())
        case XferWire.get where b.count >= 24:
            let id = XferWire.u64(b, 0), off = XferWire.u64(b, 8), n = Int(XferWire.u32(b, 16)), req = XferWire.u32(b, 20)
            let (st, data) = owner.readChunk(id, off, n)
            var d = Data(capacity: 32 + data.count)
            XferWire.le64(&d, id)
            XferWire.le64(&d, off)
            XferWire.le32(&d, UInt32(data.count))
            XferWire.le32(&d, req)
            XferWire.le32(&d, UInt32(bitPattern: st))
            XferWire.le32(&d, 0)
            d.append(data)
            send(XferWire.data, d)
            if st == 0 { owner.served(id, off, data.count, wifi: wifi) }
        case XferWire.done where b.count >= 16:
            owner.done(XferWire.u64(b, 0), Int32(bitPattern: XferWire.u32(b, 8)))
        default:
            break // 知らない型は読み捨てる
        }
    }

    // HELLO（128 バイト）：0 magic・4 version・8 auth・12 flags・16 pc_id・32 pc_nonce・48 pc_pub・80 key_id・88 pc_name
    // ACK（168 バイト）：0 magic・4 version・8 status・12 flags・16 device_id・32 phone_nonce・48 phone_pub・80 pair_key・112 wifi_ip・116 wifi_port・120 device_name
    private func onHello(_ b: [UInt8]) {
        var a = [UInt8](repeating: 0, count: XferWire.ackLen)
        XferWire.put32(&a, 0, XferWire.magic)
        XferWire.put32(&a, 4, XferWire.version)
        guard b.count >= XferWire.helloLen, XferWire.u32(b, 0) == XferWire.magic, XferWire.u32(b, 4) == XferWire.version else {
            XferWire.put32(&a, 8, 2) // 約束の版が違う
            send(XferWire.ack, Data(a))
            closeSoon()
            return
        }
        hello = b
        let auth = XferWire.u32(b, 8), flags = XferWire.u32(b, 12)
        let info = owner.ackInfo()
        a.replaceSubrange(16..<32, with: info.deviceID)
        let name = Array("iPhone".utf8)
        a.replaceSubrange(120..<(120 + name.count), with: name)
        var f: UInt32 = 0
        if info.wifiOn, let ip = info.ip {
            a.replaceSubrange(112..<116, with: ip)
            XferWire.put32(&a, 116, UInt32(PhotoTransfer.port))
            f |= 4
        }
        if !wifi {
            // USB：確かめない。頼まれたらペアリングの鍵を渡す（次から Wi-Fi だけでもコードなしでつなげる）
            if flags & 1 != 0 {
                a.replaceSubrange(80..<112, with: info.pairKey)
                f |= 2
            }
            XferWire.put32(&a, 12, f)
            send(XferWire.ack, Data(a))
            authed = true
            owner.linkUp(self)
            return
        }
        var st: UInt32 = 0
        if owner.isLocked { st = 3 }
        else if auth == 1 { if Array(b[80..<88]) != owner.keyID() { st = 4 } }
        else if auth != 2 { st = 5 }
        if st != 0 {
            XferWire.put32(&a, 8, st)
            send(XferWire.ack, Data(a))
            closeSoon()
            return
        }
        let e = Curve25519.KeyAgreement.PrivateKey()
        eph = e
        authMode = auth
        a.replaceSubrange(32..<48, with: PhotoTransfer.randomBytes(16))
        a.replaceSubrange(48..<80, with: [UInt8](e.publicKey.rawRepresentation))
        XferWire.put32(&a, 12, f | 1)
        ack = a
        send(XferWire.ack, Data(a))
    }

    // K = HMAC(秘密, "PGX1 session" || X25519 の共有の値 || SHA256(HELLO || ACK))。PC の印 = HMAC(K, "PGX1 pc")・こちらの印 = HMAC(K, "PGX1 phone")
    private func onAuth(_ b: [UInt8]) {
        guard let e = eph, b.count >= 32, hello.count >= XferWire.helloLen else { close(); return }
        eph = nil
        let secret = authMode == 1 ? owner.pairKeyBytes() : owner.codeSecret()
        guard let peer = try? Curve25519.KeyAgreement.PublicKey(rawRepresentation: Array(hello[48..<80])),
              let shared = try? e.sharedSecretFromKeyAgreement(with: peer) else { close(); return }
        let dh: [UInt8] = shared.withUnsafeBytes { Array($0) }
        guard dh.contains(where: { $0 != 0 }) else { close(); return }
        var t = Data(hello)
        t.append(contentsOf: ack)
        var m = Data("PGX1 session".utf8)
        m.append(contentsOf: dh)
        m.append(contentsOf: Array(SHA256.hash(data: t)))
        let k = SymmetricKey(data: Data(HMAC<SHA256>.authenticationCode(for: m, using: SymmetricKey(data: secret))))
        let want = Array(HMAC<SHA256>.authenticationCode(for: Data("PGX1 pc".utf8), using: k))
        var res = [UInt8](repeating: 0, count: XferWire.resultLen)
        guard XferWire.same(want, Array(b[0..<32])) else {
            owner.authFailed()
            XferWire.put32(&res, 0, 6) // 違う
            send(XferWire.authResult, Data(res))
            closeSoon()
            return
        }
        owner.authOK()
        let mine = Array(HMAC<SHA256>.authenticationCode(for: Data("PGX1 phone".utf8), using: k))
        res.replaceSubrange(4..<36, with: mine)
        send(XferWire.authResult, Data(res)) // ここまでは平文。次から箱
        key = k
        authed = true
        owner.linkUp(self)
    }
}

// ---- 画面 ----

// 写真と動画を選ぶ。PHPicker は別のプロセスで動くので、写真ライブラリの許可は要らない
struct XferPicker: UIViewControllerRepresentable {
    let keepOriginal: Bool
    let done: ([PHPickerResult]) -> Void

    func makeUIViewController(context: Context) -> PHPickerViewController {
        var cfg = PHPickerConfiguration()
        cfg.selectionLimit = 0 // いくつでも
        cfg.filter = .any(of: [.images, .videos])
        cfg.preferredAssetRepresentationMode = keepOriginal ? .current : .compatible
        cfg.selection = .ordered
        let vc = PHPickerViewController(configuration: cfg)
        vc.delegate = context.coordinator
        return vc
    }

    func updateUIViewController(_ vc: PHPickerViewController, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(done: done) }

    final class Coordinator: NSObject, PHPickerViewControllerDelegate {
        let done: ([PHPickerResult]) -> Void
        init(done: @escaping ([PHPickerResult]) -> Void) { self.done = done }
        func picker(_ picker: PHPickerViewController, didFinishPicking results: [PHPickerResult]) { done(results) }
    }
}

// 「PC に送る」のタブ
struct PhotoTransferView: View {
    @ObservedObject private var model = PhotoTransfer.shared
    @AppStorage(PhotoTransfer.wifiKey) private var wifi = false
    @AppStorage(PhotoTransfer.keepOriginalKey) private var keepOriginal = true
    @State private var picking = false
    @State private var askReset = false
    private let tick = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Button {
                        picking = true
                    } label: {
                        Label(L("写真と動画を選ぶ", "Choose photos and videos"), systemImage: "photo.on.rectangle")
                    }
                    Picker(L("形式", "Format"), selection: $keepOriginal) {
                        Text(L("元のまま", "Original")).tag(true)
                        Text(L("互換性優先", "Most compatible")).tag(false)
                    }
                    .pickerStyle(.menu)
                    Text(keepOriginal
                         ? L("撮ったときの形式のまま送ります。iPhone で撮った写真は HEIC、動画は HEVC です。画質は変わりません。", "Sends files in the format they were taken in. iPhone photos are HEIC and videos are HEVC. The quality stays the same.")
                         : L("写真は JPEG、動画は H.264 に変換してから送ります。どの PC でも開けますが、変換に時間がかかり、画質が少し下がります。", "Photos are converted to JPEG and videos to H.264 before sending. Any PC can open them, but converting takes time and lowers the quality a little."))
                        .font(.footnote)
                }
                Section(L("接続", "Connection")) {
                    LabeledContent("USB", value: model.usbConnected ? L("接続中", "Connected") : L("未接続", "Not connected"))
                    LabeledContent("Wi-Fi", value: !wifi ? L("オフ", "Off") : model.wifiConnected ? L("接続中", "Connected") : L("接続待ち", "Waiting"))
                    if !model.speedText.isEmpty {
                        LabeledContent(L("速度", "Speed"), value: model.speedText)
                    }
                    Toggle(L("Wi-Fi でも送る", "Send over Wi-Fi too"), isOn: $wifi)
                    if wifi {
                        LabeledContent(L("コード", "Code"), value: codeText)
                        Text(L("PC で実行：", "On the PC: ") + "photo_rx --wifi \(wifiIP) --code \(codeText)").font(.footnote.monospaced()).textSelection(.enabled)
                    }
                    if model.wifiLocked {
                        Text(L("コードの間違いが続いたため、Wi-Fi での受け付けを止めました。もう一度オンにしてください。", "Wi-Fi was turned off after too many wrong codes. Turn it on again.")).font(.footnote)
                    }
                    Text(L("USB ケーブルで一度接続した PC は、次から Wi-Fi だけでもコードなしで接続できます。Wi-Fi の通信は暗号化されます。", "A PC that has connected once with a USB cable can later connect over Wi-Fi alone without the code. Wi-Fi traffic is encrypted.")).font(.footnote)
                }
                Section(L("送る項目", "Items")) {
                    if model.rows.isEmpty {
                        Text(L("まだ何も選んでいません。", "Nothing has been chosen yet.")).foregroundColor(.secondary)
                    } else {
                        ForEach(model.rows) { r in XferRowView(row: r) }
                        Button(L("完了した項目を一覧から消す", "Clear finished items")) { model.clearFinished() }
                        Button(L("すべて取り消す", "Cancel all"), role: .destructive) { model.cancelAll() }
                    }
                }
                Section {
                    Text(L("PC で photo_rx.exe を起動すると受け取ります。保存先は PC のピクチャの PocketGPU フォルダです。", "Run photo_rx.exe on the PC to receive. Files are saved in the PocketGPU folder in Pictures on the PC.")).font(.footnote)
                    Text(L("送信中はこの画面を開いたままにしてください。ほかのアプリに切り替えると、少しあとで止まります。", "Keep this screen open while sending. If you switch to another app, sending stops after a short time.")).font(.footnote)
                    Button(L("ペアリングをリセット", "Reset pairing")) { askReset = true }
                }
                Section { BetaNote() }
            }
            .navigationTitle(L("PC に送る", "Send to PC"))
            .sheet(isPresented: $picking) {
                XferPicker(keepOriginal: keepOriginal) { results in
                    picking = false
                    model.add(results)
                }
                .ignoresSafeArea()
            }
            .confirmationDialog(L("ペアリングをリセットしますか", "Reset pairing?"), isPresented: $askReset, titleVisibility: .visible) {
                Button(L("リセット", "Reset"), role: .destructive) { model.resetPairing() }
            } message: {
                Text(L("これまでに接続した PC は、もう一度 USB かコードで接続する必要があります。", "PCs that connected before need to connect again with USB or the code."))
            }
            .onReceive(tick) { _ in model.refresh() }
            .onChange(of: wifi) { _, on in model.setWifi(on) }
        }
    }

    private var codeText: String { String(format: "%06u", model.wifiCode) }
    private var wifiIP: String { listAddresses().first(where: { $0.name == "en0" })?.ip ?? "?" }
}

// 一覧の 1 行：名前・大きさ・状態・進み具合
private struct XferRowView: View {
    let row: PhotoTransfer.Row

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(row.name).lineLimit(1).truncationMode(.middle)
                Spacer()
                if row.size > 0 {
                    Text(ByteCountFormatter.string(fromByteCount: Int64(clamping: row.size), countStyle: .file)).font(.footnote).foregroundColor(.secondary)
                }
            }
            Text(PhotoTransfer.stateText(row)).font(.footnote).foregroundColor(.secondary)
            if row.state == .loading || row.state == .sending {
                ProgressView(value: row.progress)
            }
        }
    }
}
