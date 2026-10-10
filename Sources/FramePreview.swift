import SwiftUI
import UIKit

// PC から来た命令で iPhone の GPU が描いた絵を、この iPhone の画面にも出す。
// 受け手が PC に返した JPEG のうち最新の 1 枚を、1/30 秒ごとに番号だけ見て、変わったときだけ読む。
// 受け手が 1 枚写しておくのは、出している間（vkr_set_preview(1)）だけ。
// 読むのと絵にする（JPEG をほどく）のは後ろの糸でして、画面の糸を止めない。画面に出す文は仮。言葉は鶴田さんが決める
final class FramePreviewModel: ObservableObject {
    @Published private(set) var image: UIImage?
    // 1 枚の JPEG の上限。フル HD で数百 KB なので、4K でも足りる大きさにする。これより大きいコマは出さない
    private static let cap = 8 << 20
    // 受け手の糸より後に回す
    private let queue = DispatchQueue(label: "frame-preview", qos: .utility)
    private var timer: DispatchSourceTimer? // 画面の糸だけで触る
    // ここから下は queue の上だけで触る
    private var buf: [UInt8] = []
    private var lastSerial: UInt64?

    deinit {
        if let t = timer {
            t.cancel()
            vkr_set_preview(0)
        }
    }

    // 画面の糸から呼ぶ。on の間だけ受け手に写してもらい、1/30 秒ごとに見る。off にしたら写すのをやめて絵を消す
    func setOn(_ on: Bool) {
        if on {
            guard timer == nil else { return }
            vkr_set_preview(1)
            queue.async { [weak self] in self?.lastSerial = nil }
            let t = DispatchSource.makeTimerSource(queue: queue)
            t.schedule(deadline: .now(), repeating: 1.0 / 30, leeway: .milliseconds(5))
            t.setEventHandler { [weak self] in self?.poll() }
            timer = t
            t.resume()
        } else {
            guard let t = timer else { return }
            timer = nil
            t.cancel()
            vkr_set_preview(0)
            image = nil
            // 8 MB の置き場も返す
            queue.async { [weak self] in
                self?.buf = []
                self?.lastSerial = nil
            }
        }
    }

    // queue の上で呼ばれる
    private func poll() {
        var serial: UInt64 = 0
        _ = vkr_latest_frame(nil, 0, &serial) // 番号だけ見る
        if serial == lastSerial { return }
        if buf.isEmpty { buf = [UInt8](repeating: 0, count: Self.cap) }
        let data: Data? = buf.withUnsafeMutableBufferPointer { b -> Data? in
            guard let p = b.baseAddress else { return nil }
            let n = Int(vkr_latest_frame(p, Int32(b.count), &serial))
            return n > 0 ? Data(bytes: p, count: n) : nil
        }
        lastSerial = serial
        // 0 バイトは、まだ無い・PC が切れて消えた・大きすぎる。そのときは絵を消す
        let img = data.flatMap { UIImage(data: $0) }?.preparingForDisplay()
        DispatchQueue.main.async { [weak self] in
            // off にしたあとに届いた絵は出さない
            guard let self, self.timer != nil else { return }
            self.image = img
        }
    }
}

// 絵 1 枚。縦横比を保って枠に収める。PC がつながっていないときと、まだ絵が無いときは文を出す
struct FrameImageView: View {
    @ObservedObject var model: FramePreviewModel
    @ObservedObject var status: DeviceStatus
    var maxHeight: CGFloat = .infinity // 「状態」の中に出すときの高さの上限
    var textColor: Color = .secondary

    var body: some View {
        if !status.connected {
            Text(L("PC を待っています", "Waiting for the PC")).font(.footnote).foregroundStyle(textColor)
        } else if let img = model.image {
            // 行の高さは決まっていないので、高さの上限は幅を縦横比で抑えて守る（縦長の絵がはみ出さない）
            let aspect = max(img.size.width, 1) / max(img.size.height, 1)
            Image(uiImage: img).resizable().scaledToFit()
                .frame(maxWidth: maxHeight * aspect)
        } else {
            Text(L("絵が届くのを待っています", "Waiting for a frame")).font(.footnote).foregroundStyle(textColor)
        }
    }
}

// 「状態」の中に出す絵。タップで全画面にする
struct FramePreviewView: View {
    @ObservedObject var model: FramePreviewModel
    @ObservedObject var status: DeviceStatus
    @State private var full = false

    var body: some View {
        FrameImageView(model: model, status: status, maxHeight: 360)
            .frame(maxWidth: .infinity, minHeight: 60)
            .contentShape(Rectangle())
            .onTapGesture { if model.image != nil { full = true } }
            .fullScreenCover(isPresented: $full) {
                FrameFullScreenView(model: model, status: status)
            }
    }
}

// 全画面。背景は黒。もう一度タップで戻る
struct FrameFullScreenView: View {
    @ObservedObject var model: FramePreviewModel
    @ObservedObject var status: DeviceStatus
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            FrameImageView(model: model, status: status, textColor: Color(white: 0.7))
        }
        .contentShape(Rectangle())
        .onTapGesture { dismiss() }
        .statusBarHidden()
    }
}
