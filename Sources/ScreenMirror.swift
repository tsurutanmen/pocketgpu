import Network
import SwiftUI
import UIKit

// PC の画面そのものを、この iPhone に映す。「iPhone を PC のモニターにする」の第一歩。
// PC は app/pc/screen_mirror.py。USB（iPhone の中の 127.0.0.1）の 50062 番で待ち、一度に 1 本だけ受ける。
// 届く物：[u32 長さ・リトルエンディアン][JPEG] の繰り返し。長さ 0 は何もしない。上限より長いときは形が崩れたとみなして切る。
// 受け取るのは後ろの糸で、絵にしていない最新の 1 枚だけを持つ。絵にしている間に次が届いたら、古い方は捨てる。
// 絵が画面に出てから次を絵にするので、画面が遅いときも古いコマはたまらない。
// Vulkan の受け手（50060）とは別の口で、受け手の C の部分には触らない。糸は FramePreview と同じく受け手より後に回す。
// 画面に出す文は仮。言葉は鶴田さんが決める
final class ScreenMirrorModel: ObservableObject {
    static let shared = ScreenMirrorModel()
    static let port: UInt16 = 50062
    // 1 枚の JPEG の上限。FramePreview と同じ大きさ
    private static let cap = 8 << 20

    @Published private(set) var image: UIImage?
    @Published private(set) var connected = false
    @Published private(set) var fps = 0.0

    // ここから下の 5 つは画面の糸だけで触る
    private var on = false
    private var live: Int? // いま画面に出してよいつなぎの番号。切れたら nil
    private var shown: UInt64 = 0
    private var lastShown: UInt64 = 0
    private var lastTick = Date()

    private let queue = DispatchQueue(label: "screen-mirror", qos: .utility)
    private let decodeQueue = DispatchQueue(label: "screen-mirror-decode", qos: .utility)
    // ここから下は queue の上だけで触る
    private var listening = false
    private var listener: NWListener?
    private var conn: NWConnection?
    private var connID = 0 // つなぎごとに 1 増える
    private var pending: Data? // まだ絵にしていない最新のコマ
    private var decoding = false
    private var failNoted = false // 口を開けないときの記録は、開けるまで 1 回だけ書く

    private init() {}

    // 画面の糸から呼ぶ。on の間だけ 50062 番で待つ。off にしたら口を閉じ、つなぎを切り、絵を消す
    func setOn(_ v: Bool) {
        guard v != on else { return }
        on = v
        if !v {
            live = nil
            image = nil
            connected = false
            fps = 0
        }
        queue.async { [weak self] in
            guard let self else { return }
            self.listening = v
            if v { self.listen() } else { self.close() }
        }
    }

    // 画面の糸から 1 秒ごとに呼ぶ。出したコマの数の差で毎秒のコマ数を出す
    func tick() {
        let now = Date()
        let dt = now.timeIntervalSince(lastTick)
        guard dt >= 0.5 else { return }
        let f = Double(shown &- lastShown) / dt
        lastShown = shown
        lastTick = now
        if f != fps { fps = f }
    }

    private func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }

    // ---- ここから下は queue の上で呼ぶ ----

    private func listen() {
        guard listening, listener == nil, let p = NWEndpoint.Port(rawValue: Self.port) else { return }
        let params = NWParameters.tcp
        // USB（iPhone の中の 127.0.0.1）だけで待つ。Wi-Fi には出さない
        params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: p)
        params.allowLocalEndpointReuse = true
        let l: NWListener
        do {
            l = try NWListener(using: params)
        } catch {
            noteFailure("\(error)")
            retryLater()
            return
        }
        l.stateUpdateHandler = { [weak self, weak l] st in
            guard let self, let l, l === self.listener else { return }
            switch st {
            case .ready:
                self.failNoted = false
                self.log("PC の画面を映す口：127.0.0.1:\(Self.port) で待っている")
            case .failed(let e):
                // 閉じたばかりの口がまだ残っているときなど。1 秒あとに開き直す
                self.noteFailure("\(e)")
                self.listener = nil
                l.cancel()
                self.retryLater()
            default:
                break
            }
        }
        l.newConnectionHandler = { [weak self, weak l] c in
            guard let self, let l, l === self.listener else { c.cancel(); return }
            self.accept(c)
        }
        listener = l
        l.start(queue: queue)
    }

    private func noteFailure(_ why: String) {
        if !failNoted { log("PC の画面を映す口を開けない：\(why)。1 秒ごとに開き直す") }
        failNoted = true
    }

    private func retryLater() {
        queue.asyncAfter(deadline: .now() + 1) { [weak self] in self?.listen() }
    }

    private func close() {
        if let c = conn { drop(c) }
        if let l = listener {
            listener = nil
            l.cancel()
            log("PC の画面を映す口を閉じた")
        }
        failNoted = false
    }

    // 一度に 1 本。新しいつなぎが来たら古い方を切る。PC の送り手を開き直したとき、切れ残りを待たずに済む
    private func accept(_ c: NWConnection) {
        if let old = conn { drop(old) }
        connID += 1
        let id = connID
        conn = c
        c.stateUpdateHandler = { [weak self, weak c] st in
            guard let self, let c else { return }
            switch st {
            case .failed, .cancelled: self.drop(c)
            default: break
            }
        }
        DispatchQueue.main.async { [weak self] in
            guard let self, self.on else { return }
            self.live = id
            self.connected = true
        }
        log("PC の画面：つながった")
        c.start(queue: queue)
        readFrame(c)
    }

    // いまのつなぎなら切って、画面の絵を消す。古いつなぎや、もう切ったつなぎなら何もしない
    private func drop(_ c: NWConnection) {
        guard c === conn else { return }
        conn = nil
        pending = nil
        c.cancel()
        let id = connID
        log("PC の画面：切れた")
        DispatchQueue.main.async { [weak self] in
            guard let self, self.live == id else { return }
            self.live = nil
            self.connected = false
            self.image = nil
        }
    }

    // [u32 長さ][JPEG] を 1 つ読み、次を待つ
    private func readFrame(_ c: NWConnection) {
        readExactly(c, 4) { [weak self] h in
            guard let self, c === self.conn else { return }
            guard let h else { self.drop(c); return }
            let b = [UInt8](h)
            let n = Int(b[0]) | Int(b[1]) << 8 | Int(b[2]) << 16 | Int(b[3]) << 24
            if n == 0 { self.readFrame(c); return }
            if n > Self.cap {
                self.log("PC の画面：長さが \(n) バイトで上限を超えた。形が違うので切る")
                self.drop(c)
                return
            }
            self.readExactly(c, n) { [weak self] body in
                guard let self, c === self.conn else { return }
                guard let body else { self.drop(c); return }
                self.pending = body // まだ絵にしていない古いコマは捨てる
                if !self.decoding { self.decodeNext() }
                self.readFrame(c)
            }
        }
    }

    // 切れたときは nil。足りないまま終わったときも nil
    private func readExactly(_ c: NWConnection, _ n: Int, _ done: @escaping (Data?) -> Void) {
        c.receive(minimumIncompleteLength: n, maximumLength: n) { data, _, _, err in
            if let data, data.count == n, err == nil { done(data) } else { done(nil) }
        }
    }

    // 最新のコマを後ろの糸で絵にする。画面に出し終えてから次へ進む
    private func decodeNext() {
        guard let d = pending else { decoding = false; return }
        pending = nil
        decoding = true
        let id = connID
        decodeQueue.async { [weak self] in self?.decode(d, id) }
    }

    // decodeQueue の上で呼ぶ。ほどけないコマは出さずに飛ばす
    private func decode(_ d: Data, _ id: Int) {
        let img = UIImage(data: d)?.preparingForDisplay()
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            // 絵にしている間に切れた・off にした・次のつなぎに替わったなら出さない
            if let img, self.live == id {
                self.image = img
                self.shown &+= 1
            }
            self.queue.async { [weak self] in self?.decodeNext() }
        }
    }
}

// 「状態」に出す 1 行。つながっているか・毎秒のコマ数・絵の大きさ
struct ScreenMirrorStatusRow: View {
    @ObservedObject var model: ScreenMirrorModel

    var body: some View {
        LabeledContent(L("PC の画面", "PC screen"), value: text)
    }

    private var text: String {
        guard model.connected else { return L("待っている", "Waiting") }
        let f = String(format: "%.1f", model.fps)
        var s = L("毎秒 \(f) コマ", "\(f) fps")
        if let img = model.image {
            s += L("・", ", ") + "\(Int(img.size.width))×\(Int(img.size.height))"
        }
        return s
    }
}

// 映した絵 1 枚。縦横比を保って枠に収める。PC がつながっていないときと、まだ絵が無いときは文を出す。FrameImageView と同じ作り
struct ScreenMirrorImageView: View {
    @ObservedObject var model: ScreenMirrorModel
    var maxHeight: CGFloat = .infinity // 「状態」の中に出すときの高さの上限
    var textColor: Color = .secondary

    var body: some View {
        if !model.connected {
            Text(L("PC を待っています", "Waiting for the PC")).font(.footnote).foregroundStyle(textColor)
        } else if let img = model.image {
            // 行の高さは決まっていないので、高さの上限は幅を縦横比で抑えて守る
            let aspect = max(img.size.width, 1) / max(img.size.height, 1)
            Image(uiImage: img).resizable().scaledToFit()
                .frame(maxWidth: maxHeight * aspect)
        } else {
            Text(L("絵が届くのを待っています", "Waiting for a frame")).font(.footnote).foregroundStyle(textColor)
        }
    }
}

// 「状態」の中に出す絵。タップで全画面にする。FramePreviewView と同じ作り
struct ScreenMirrorView: View {
    @ObservedObject var model: ScreenMirrorModel
    @State private var full = false

    var body: some View {
        ScreenMirrorImageView(model: model, maxHeight: 360)
            .frame(maxWidth: .infinity, minHeight: 60)
            .contentShape(Rectangle())
            .onTapGesture { if model.image != nil { full = true } }
            .fullScreenCover(isPresented: $full) {
                ScreenMirrorFullScreenView(model: model)
            }
    }
}

// 全画面。背景は黒。もう一度タップで戻る。FrameFullScreenView と同じ作りで、横向きで固定の設定も同じ鍵を使う
struct ScreenMirrorFullScreenView: View {
    @ObservedObject var model: ScreenMirrorModel
    @AppStorage("fullLandscape") private var landscape = true
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        GeometryReader { g in
            ZStack {
                Color.black
                if landscape {
                    ScreenMirrorImageView(model: model, textColor: Color(white: 0.7))
                        .frame(width: g.size.height, height: g.size.width)
                        .rotationEffect(.degrees(90))
                } else {
                    ScreenMirrorImageView(model: model, textColor: Color(white: 0.7))
                }
            }
            .frame(width: g.size.width, height: g.size.height)
        }
        .ignoresSafeArea()
        .contentShape(Rectangle())
        .onTapGesture { dismiss() }
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
    }
}
