import AVFoundation
import Combine
import CoreImage
import CoreMedia
import CoreMotion
import CoreVideo
import Metal
import Network
import SwiftUI
import UIKit
import VideoToolbox

// iPhone のカメラを、USB でつないだ Windows の PC のウェブカメラにする送り手。
// PC は work/webcam/pc/webcam_rx.c（受けてほどいて窓に出し、仮想カメラへ流す）。約束は work/webcam/DESIGN.md と pc/wcam.h と同じ。
//
// つなぎ方：オンの間だけ、USB（iPhone の中の 127.0.0.1）の 50064 番で待つ（ScreenMirror の 50062 番と同じ作り）。一度に 1 本だけ受ける。
//   PC が HELLO（解像度・コマ数・カメラ・ビットレート）を送る → こちらがカメラを開いて ACK（決まった形）を返す
//   → カメラが渡す NV12 の絵を、そのまま VideoToolbox（H.264・低遅延・B フレームなし）に渡して、できた物を Annex B にして 1 コマずつ送る。
// カメラは iOS の決まりで、アプリが前にいる間しか使えない（後ろに回ると iOS が止める）。止まったら EVENT を送り、戻ったら続きを送る。
// Vulkan の受け手（50060）には触らない。
//
// camset で足した所：
//   アプリの画面の設定（カメラ・解像度・フレームレート・画質）。PC の HELLO が 0（おまかせ）の項目はこの設定を使う。
//   PC が値を決めた項目は PC を使う。ただし画面の「PC からの指定を優先する」を切ると、画面の設定を使う。
//   つないだまま画面で設定を変えたら、その場でカメラとエンコーダを作り直し、ACK（flags に CHANGED）を送ってキーフレームから流す。
//   ただし PC が WC_HELLO_LIVE を立てていない（古い webcam_rx。途中で大きさが変わると壊れる）ときは、大きさかコマ数が変わる変更は次の接続まで待つ。
//   PC は同じつなぎのまま HELLO を送り直してよい（仮想カメラを開いたアプリが形を選んだとき）。
// iOS 15 でも動く書き方にしてある（あとで作るカメラとマイクだけの軽い版で使うため）。iOS 17 からの API は #available で分けた。
// 画面に出す文は仮。言葉は鶴田さんが決める
//
// camctl で足した所（work/camctl/PROTOCOL.md に PC から操作する案）：
//   ピント・明るさ・色・ズーム・ライト：接続中に変えたら、カメラを作り直さずに lockForConfiguration でその場で変える（liveApply）。
//   向き（左右反転・上下反転・90 度ずつの回転）・手ぶれ補正・シネマティック・切り抜きの入り切り：カメラを作り直してキーフレームから流す。
//     回転は接続（AVCaptureConnection）で回す（ハードウェアで回る。エンコーダの前で回すより CPU を使わない）。上下反転は 180 度＋左右反転。
//     左右反転は PC の HELLO の mirror と同じ決まり（PC が立てたら PC・「PC からの指定を優先する」を切ると画面）。
//     90 度と 270 度は幅と高さが入れ替わる。古い webcam_rx（WC_HELLO_LIVE が無い）は縦長を仮想カメラに書かないので、その相手には使わない。
//   切り抜きと水平を保つ：エンコーダの前で Core Image が切り出して回し、選んだ解像度に拡大する（iOS 15 でも動く。VTPixelTransferSession は iOS 16 からで、斜めに回せない）。
//   カメラ 4：背面の仮想カメラ（三つか二つのレンズ）。倍率に合わせて iOS がレンズを切り替える。PC には「背面」（0）と知らせる。
//   iPhone のレンズの F 値は変えられない（lensAperture は読むだけ）。背景ぼかしは iOS 26 のシネマティックか、コントロールセンターのポートレート
//
// camctl2 で足した所（PC から操作する。通信の形は work/camctl/PROTOCOL.md・番号の表は work/camctl2/ctl_ids.md）：
//   PC が HELLO の flags に WC_HELLO_CTRL（8）を立てたつなぎだけ、CTRL_CAPS（5）と CTRL_STATE（6）を送り、CTRL_SET（18）と CTRL_GET（19）を受ける。
//   立てない PC（古い webcam_rx）には、今までどおり何も増やさない。
//   PC の SET は、画面で変えたときと同じ道を通す（画面の糸で UserDefaults に書き、controlsChanged か settingsChanged）。
//   だから画面にもすぐ出て、次に開いたときも残る。画面と PC の両方で変えたら、あとから変えた方が残る
//
// 10/13（work/ios1013b）Wi-Fi のつなぎは、HELLO の合言葉を確かめてから古いつなぎと入れ替える（accept・acceptWifi）。
//   前は、つながった時点で古いつなぎを切っていたので、同じ Wi-Fi にいる人が、合言葉を知らなくても何度でも PC のカメラを切れた。
//   USB（127.0.0.1）の扱いは前のまま：つながったらすぐ入れ替える
//
// 10/15（work/camgpu1015）PC の GPU の負荷を iPhone に移す：
//   PC は、仮想カメラが 1920×1080 までしか出せないのに、PC アプリで 4K を選ぶと 4K を頼み、GPU（NVDEC）でほどいたあと CPU に読み戻して縮めて捨てていた。
//   HELLO の flags に WC_HELLO_FIT（16）が立っているとき、40 バイト目からの max_w・max_h（PC が使える最大の大きさ）を読み、
//   PC の指定でも画面の設定でも、その箱（縦横どちらの向きでもよい）に収まる大きさにする（resolve。fitSize）。ACK の flags に WC_ACK_FIT（64）を立てて守ったと知らせる。
//   小さい形で撮ればカメラの中（ISP）で縮むので、iPhone は何も増やさない。その大きさの形が無いとき（シネマティック・エフェクトで形が限られるとき）だけ、
//   大きめに撮ってエンコーダの前で縮める（切り抜きと同じ stage の道）。FIT の印が無い PC（古い webcam_rx）には、今までどおり何も変えない
//
// 10/15（work/cam60）PC が 60 コマ/秒を頼んだのに 30 で来ていたのを直す：
//   ACK の fps は「作る前に決めた値（plan）」で、PC の指定が通ったあとにコマ数が下がる道が、記録に何も残さず 4 つあった：
//   シネマティック（30 まで）・コントロールセンターのエフェクト（その形の上限）・熱・その大きさの形が 60 を撮れないとき（大きさの違う別の形に替わっていた）。
//   1. PC がコマ数を頼んだときは、シネマティックを止めて頼みを通す（resolve。画面の「PC からの指定を優先する」を切っているときは、前のまま画面の設定）
//   2. エフェクトの上限は、そのエフェクトに対応しない同じ大きさの形で頼みのコマ数を撮れるなら、その形に替えて避ける（plan）
//   3. その大きさの形が頼みのコマ数を撮れないときは、大きさを変えずに、撮れる最高のコマ数にする（pickFormat。前は大きさの違う別の形になった）。
//      420v に無くて 420f にだけ在るときは 420f の形を使う（出力は 420v のまま）
//   4. 下がったときは、頼まれたコマ数・下がった理由・その大きさの形の一覧を記録に出す。画面にも書く（pcNote）
//   5. カメラが実際に受けているコマ数を、走らせる前に読み直して ACK に書く（iOS が直してしまったとき用）。走り始めて 2〜5 秒の実測も記録に出す
//   6. エンコーダ：キーフレームの間隔を秒でも縛る。60 コマ/秒で作れなければ、30 コマ/秒に落として作り直す。1080p60 の画質の段を 20 Mbps に
//
// 10/15（work/camrot1015）4K60 で 60 コマ/秒に届かず 45〜50 になるのを軽くする（本物の iPhone では未確認。効き目は記録の「内訳」「内訳2」で見る）：
//   1. 回転と反転：AVCaptureConnection の videoRotationAngle・isVideoMirrored は、毎コマ iOS が絵を物理的に作り直す（Apple の資料）。
//      180 度と左右・上下の反転（縦横が変わらない物）は、接続では行わず、
//        PC が WC_HELLO_ROT180（32）を立て、回転が 180 度だけのとき：回さずに送り、ACK に WC_ACK_PC_ROT180（128）を立てる。PC が 180 度回す。iPhone は何もしない
//        そうでないとき：Metal の compute で、カメラの絵を送る大きさの入れ物へ 1 回で写す（FrameFlipper。CVMetalTextureCache でコピーなし）
//      印を知らない PC（古い webcam_rx）は WC_HELLO_ROT180 を立てないので、今までどおり iPhone が回した絵を受ける。
//      90 度・270 度と、切り抜き・水平を保つ（Core Image が回す）ときは、前のとおり接続で回す
//   2. 中央の切り抜き：水平を保たず、位置が中央なら、Core Image で毎コマ切り出さず、カメラの倍率（videoZoomFactor）に切り抜きの倍率を掛ける（camCrop）
//   3. 記録に出す物：カメラから来たコマ・エンコーダから出たコマ・エンコーダが落とした数（camfix1015）に、iOS が捨てたコマ（遅れ・入れ物不足）・
//      VTCompressionSessionEncodeFrame の呼び出しの時間・回す処理の時間・エンコーダの実際の設定（ハード・RealTime・MaxFrameDelayCount）を足した
//   4. 隠しスイッチ UserDefaults "webcamDebug"（起動引数 -webcamDebug 4 でも入る）：1 低遅延のレート制御を使わない・2 DataRateLimits を付けない・
//      4 回転と反転を前のとおり接続で行う・8 中央の切り抜きを前のとおり Core Image で行う。ふだんは 0。本物で効き目を比べるため
//
// 10/16（work/cam4k1016）4K60 で、カメラの受け口（captureOutput）の中でエンコーダを同期で呼ぶため、受け口が 16〜20 ms かかり、
// iOS が遅れたコマを捨てて 49〜51 コマ/秒になるのを直す（本物の iPhone では未確認。効き目は記録の「内訳」「内訳3」で見る）：
//   1. 受け口は、受け取ったコマの CVPixelBuffer を持って、待ちの列に入れるだけにした。糸は 2 本：
//        回す処理（Metal・Core Image）をする prepQueue（回す必要があるときだけ）→ エンコーダを呼ぶ encodeQueue。
//        回す必要がなければ、受け口から直接 encodeQueue の列へ入れる。2 本に分けたのは、回す時間（180 度で約 3.5 ms）とエンコーダの呼び出しが重ならないため
//   2. 列は糸ごとに 2 枚まで。超えたら古いコマを捨てて新しいコマを優先する（待ちで捨てたコマ）。番号（seq）とキーフレームの印は、
//      エンコーダに渡す直前に決めるので、待ちで捨てても番号は飛ばず、キーフレームも要らない（エンコーダに入っていない）
//   3. カメラを作り直す・止めるときは、列を空けて、2 本の糸が戻るのを待ってから、エンコーダを閉じる
//   4. 記録の「内訳」に、待ちで捨てたコマとエンコードの糸の1コマの時間を足し、「内訳3」で受け口・回す糸・エンコードの糸の時間と、待ちで捨てたコマを出す
//   5. 隠しスイッチ（webcamDebug）に 16 受け口の中で同期（前の作り）・32 列を 1 枚にする・64 エンコーダに入力の形（NV12・大きさ）を伝えて作る・
//      128 回さないときも Metal で自分の入れ物へ写してからエンコーダに渡す、を足した
final class CameraBridge: NSObject, ObservableObject, AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureMetadataOutputObjectsDelegate {
    static let shared = CameraBridge()
    static let port: UInt16 = 50064
    static let version: UInt32 = 1

    // メッセージの型（pc/wcam.h と同じ）
    private enum Msg {
        static let hello: UInt32 = 1, ack: UInt32 = 2, frame: UInt32 = 3, event: UInt32 = 4
        static let reqKey: UInt32 = 16, setKbps: UInt32 = 17, ping: UInt32 = 20, pong: UInt32 = 21
        // camctl2：PC から操作する。5・6 は iPhone→PC、18・19 は PC→iPhone
        static let ctrlCaps: UInt32 = 5, ctrlState: UInt32 = 6, ctrlSet: UInt32 = 18, ctrlGet: UInt32 = 19
    }
    // HELLO と ACK の flags（pc/wcam.h と同じ）
    private enum Flag {
        static let mirror: UInt32 = 1, cameraSet: UInt32 = 2, live: UInt32 = 4
        static let fit: UInt32 = 16        // camgpu1015：HELLO。PC が使える大きさの上限（max_w・max_h）がある
        static let pcRot180: UInt32 = 32   // camrot1015：HELLO。PC は ACK の PC_ROT180 を見て、受けた絵を自分で 180 度回せる（PC が立てる印。立てない PC には iPhone が回して送る）
        static let ackPcRot180: UInt32 = 128 // camrot1015：ACK。この絵は 180 度回していない。PC が回す（pcRot180 を立てた PC にだけ立つ）
        static let ackAppSettings: UInt32 = 1, ackAppPriority: UInt32 = 2, ackLive: UInt32 = 4, ackChanged: UInt32 = 8
        static let ackFit: UInt32 = 64     // camgpu1015：ACK。この iPhone は上限を守った（FIT を名乗った PC にだけ立てる）
        static let ackThermal: UInt32 = 16 // camctl：熱のためにコマ数を下げた（work/camctl/PROTOCOL.md の案。古い PC は見ない）
        static let ctrl: UInt32 = 8        // camctl2：HELLO。PC は CTRL のメッセージを受け、送れる
        static let ackCtrl: UInt32 = 32    // camctl2：ACK。この iPhone は CTRL を受ける（CTRL を名乗った PC にだけ立てる）
    }

    // アプリの画面の設定（UserDefaults の鍵）
    enum Key {
        static let camera = "webcamCamera"     // 0 背面の広角・1 前面・2 背面の超広角・3 背面の望遠・4 背面のレンズ自動切り替え（camctl）
        static let height = "webcamHeight"     // 720・1080・2160（幅は 16:9）
        static let fps = "webcamFps"           // 30・60
        static let quality = "webcamQuality"   // 1 標準・2 高画質・3 最高
        static let preferPC = "webcamPreferPC" // PC が値を決めた項目は PC を使う
        // camctl で足した。カメラの操作
        static let mirror = "webcamMirror"           // 左右反転
        static let flipV = "webcamFlipV"             // 上下反転
        static let rotation = "webcamRotation"       // 0・90・180・270 度
        static let focus = "webcamFocus"             // 0 自動・1 固定・2 手動
        static let focusPos = "webcamFocusPos"       // 手動のレンズの位置。0 が近く・1 が遠く
        static let focusTarget = "webcamFocusTarget" // 0 顔を優先・1 画面の真ん中
        static let exposure = "webcamExposure"       // 0 自動・1 固定・2 手動
        static let bias = "webcamBias"               // 露出補正（EV）
        static let iso = "webcamISO"                 // 手動の ISO。0 は今の値から
        static let shutter = "webcamShutter"         // 手動のシャッター速度の分母（100 は 1/100 秒）。0 はコマ数の 2 倍
        static let wb = "webcamWB"                   // 0 自動・1 固定・2 色温度
        static let wbTemp = "webcamWBTemp"           // 色温度（K）
        static let zoom = "webcamZoom"               // 見え方の倍率。1 は背面の広角と同じ・0.5 は超広角
        static let torch = "webcamTorch"             // ライト（接続している間だけ点ける）
        static let torchLevel = "webcamTorchLevel"   // 0.25・0.5・0.75・1
        static let stab = "webcamStab"               // 0 オフ・1 遅れの少ない補正（iOS 26）・2 標準・3 シネマティック
        static let cinematic = "webcamCinematic"     // 背景をぼかす（iOS 26 のシネマティックビデオ）
        static let aperture = "webcamAperture"       // シネマティックの F 値。0 はその形の初めの値
        static let centerStage = "webcamCenterStage" // センターフレーム。触るまでは値が無い（コントロールセンターのまま）
        static let crop = "webcamCrop"               // 切り抜く
        static let cropPos = "webcamCropPos"         // 0 中央・1 左寄り・2 右寄り・3 顔に合わせる・4 手動
        static let cropSize = "webcamCropSize"       // 枠の大きさ（映像の幅に対する割合 0.3〜1）
        static let cropX = "webcamCropX", cropY = "webcamCropY" // 手動のときの枠の中心（0〜1。左上が 0）
        static let level = "webcamLevel"             // 水平を保つ（背面のカメラだけ）
        static let screenWarm = "webcamScreenWarm"   // 画面のライトを暖かい色にする
        static let thermalFps = "webcamThermalFps"   // iPhone が熱いときはフレームレートを下げる
        static let debug = "webcamDebug"             // camrot1015：本物で効き目を比べる隠しスイッチ（ビット。上の説明の 4）。画面には出さない
    }
    // camrot1015：隠しスイッチ。1 低遅延のレート制御を使わない・2 DataRateLimits を付けない・4 回転と反転を接続で行う・8 中央の切り抜きを Core Image で行う
    // cam4k1016：16 受け口の中で同期にエンコードする（前の作り）・32 列を 1 枚にする（ふだんは 2 枚）・64 エンコーダを作るとき入力の形（NV12・大きさ）を伝える・
    //   128 回さないときも Metal で自分の入れ物へ写してからエンコーダに渡す（カメラの入れ物のまま渡すより、エンコーダの呼び出しが速いかを比べる）
    private static var debugBits: Int { UserDefaults.standard.integer(forKey: Key.debug) }
    // 初めの値（画面の @AppStorage の初めの値と同じにしておく）
    static let defaultCamera = 0, defaultHeight = 1080, defaultFps = 30, defaultQuality = 2, defaultPreferPC = true

    // 画面に見せる（画面の糸だけで触る）
    @Published private(set) var connected = false
    @Published private(set) var fps = 0.0
    @Published private(set) var detail = ""      // 例：1920×1080・30 fps・背面カメラ
    @Published private(set) var pcNote = ""      // PC が指定した項目。画面の設定より優先しているとき
    @Published private(set) var pendingNote = false // 画面の変更が次の接続まで待ちになっている
    // camctl：画面に出す今の状態（tick で 1 秒ごとに読み直す）
    @Published private(set) var torchState = ""      // ライトの状態の文
    @Published private(set) var portraitOn = false   // コントロールセンターのポートレートが入っているか
    @Published private(set) var portraitActive = false
    @Published private(set) var studioOn = false
    @Published private(set) var centerStageOn = false
    @Published private(set) var cinematicActive = false
    @Published private(set) var rotSkipped = false   // PC が古いので 90 度・270 度の回転を使っていない
    @Published private(set) var levelAngle = 0.0     // 水平を保つ：いま直している角度（度）
    @Published private(set) var thermalNote = ""     // 熱でフレームレートを下げているときの文
    @Published private(set) var pcControl = false    // camctl2：接続中の PC は CTRL を名乗った（PC のアプリからも操作できる）
    // Wi-Fi でつなぐときの合言葉（起動のたびに変わる 6 けた）。PC は webcam_rx --addr iPhoneの住所:50064 --code これ
    let wifiCode = UInt32.random(in: 100_000...999_999)
    static let wifiKey = "webcamWifi"

    private var on = false
    private var lastTick = Date()
    private var shownCount: UInt64 = 0
    private var lastShown: UInt64 = 0

    // PC の頼み（HELLO の中身）
    private struct Hello {
        var version: UInt32, width: Int, height: Int, fps: Int, kbps: Int, gop: Int, camera: UInt32, flags: UInt32, quality: Int
        var maxW = 0, maxH = 0 // camgpu1015：40 バイト目からの「PC が使える最大の大きさ」（flags に FIT があるときだけ意味がある。古い PC は 0）
    }
    // 頼みと画面の設定を混ぜて決めた形（実際に撮る大きさは pickFormat で決まる）
    private struct Resolved: Equatable {
        var camera: UInt32, w: Int, h: Int, fps: Int, quality: Int, kbps: Int, gop: Int, mirror: Bool, ackFlags: UInt32
        var pcSize: Bool, pcFps: Bool, pcCam: Bool
        // camctl。ここが変わったらカメラを作り直す。ピントや明るさなど、作り直さずに変える物は入れない
        var pcMirror = false     // 左右反転を PC の指定で決めた
        var rot = 0              // 接続で回す角度（上下反転を含めた値）
        var connMirror = false   // 接続で左右反転するか（上下反転を含めた値）
        var portrait = false     // 90 度か 270 度。幅と高さが入れ替わる
        var rotSkipped = false   // PC が古いので 90 度・270 度を使わなかった
        var stab = 0             // 手ぶれ補正（Key.stab の値）
        var cinematic = false
        var stage = false        // エンコーダの前で切り抜く（切り抜き・水平を保つ）
        var capH = 0             // stage のときに撮る高さ（切り抜いても粗くならないように大きめに撮る）
        var face = false         // 顔の位置を調べる（切り抜きを顔に合わせる）
        var fx = 0               // コントロールセンターで入っているエフェクト（対応する形とコマ数が変わる）
        var fitShrunk = false    // camgpu1015：PC の上限（FIT）に合わせて、頼まれた（か画面の設定の）大きさより小さくした
        var fitStage = false     // camgpu1015：箱に収まる形が無いので、大きく撮ってエンコーダの前で縮める（stage を使う）
        var askedFps = 0         // cam60：PC（か画面）が決めたコマ数。シネマティックなどで下げる前の値
        var cineDropped = false  // cam60：PC がコマ数を頼んだので、シネマティックを止めた
        var camCrop = false      // camrot1015：中央の切り抜きだけ（水平を保たない・顔に合わせない）。Core Image を使わず、カメラの倍率に切り抜きの倍率を掛ける
        var pcRot = false        // camrot1015：回転が 180 度だけで、PC が回せる。回さずに送り、ACK の ackPcRot180 で知らせる
    }

    // cam60：決めたコマ数から下がった理由
    private enum FpsWhy {
        case cinematic, format, effect, hot, ios, encoder
        var ja: String {
            switch self {
            case .cinematic: return "シネマティックは 30 コマ/秒まで"
            case .format: return "その大きさの形が撮れる最高のコマ数"
            case .effect: return "コントロールセンターのエフェクトの上限"
            case .hot: return "iPhone が熱い"
            case .ios: return "iOS がカメラの設定を直した"
            case .encoder: return "エンコーダがそのコマ数で作れなかった"
            }
        }
        var en: String {
            switch self {
            case .cinematic: return "Cinematic is limited to 30 fps"
            case .format: return "the camera's maximum for that size"
            case .effect: return "the limit of a Control Center effect"
            case .hot: return "the iPhone is hot"
            case .ios: return "iOS changed the camera setting"
            case .encoder: return "the encoder could not be created at that rate"
            }
        }
    }

    // 作り直すときに決める、カメラと形と送る大きさ
    private struct Plan {
        let dev: AVCaptureDevice, fmt: AVCaptureDevice.Format, frameDur: CMTime, fps: Int
        let baseFps: Int         // 熱で下げる前のコマ数
        let outW: Int, outH: Int // エンコーダと PC に渡す大きさ（回したあと）
        let why: [FpsWhy]        // cam60：r.askedFps から下がった理由（下がっていなければ空）
    }

    // エンコーダの前で切り抜く・水平を保つための値。q で作り、カメラの糸が lock の中で写して読む
    private struct StageParams {
        var on = false
        var scale: CGFloat = 1      // 枠の小ささ。1 は全体・2 は幅が半分
        var extra: CGFloat = 1      // 水平の余白のために、カメラの倍率から移した分
        var pos = 0                 // Key.cropPos と同じ
        var cx: CGFloat = 0.5, cy: CGFloat = 0.5
        var level = false
        var levelMax: CGFloat = 0   // 直す角度の上限（ラジアン）
        var refAngle: CGFloat = 0   // 水平のときの iPhone の傾き（接続で回す角度から決まる）
        var sign: CGFloat = 1       // 接続で左右反転しているときは -1
        var outW = 0, outH = 0
    }

    // ここから下は q の上だけで触る
    private let q = DispatchQueue(label: "camera-bridge", qos: .userInitiated)
    private var listener: NWListener?
    private var wifiListener: NWListener? // 設定で入れたときだけ。Wi-Fi の口で待ち、合言葉を確かめる
    private var wifiMisses = 0              // 合言葉の違いが 5 回続いたら Wi-Fi の口を閉じる（当てずっぽうを止める）
    private var pending: [NWConnection] = [] // Wi-Fi で来て、合言葉をまだ確かめていないつなぎ。確かめるまで conn には入れない（古いつなぎに触らない）
    private static let maxPending = 4        // 確かめ中のつなぎの数の上限。超えたら、いちばん古いのを閉じる
    private static let pendingSeconds = 10.0 // HELLO がこの秒数までに来ないつなぎは閉じる（PC は、つないですぐに HELLO を送る）
    private static let maxFirstHello = 256   // 確かめる前に読む HELLO の長さの上限（本物は 32〜40 バイト）
    private var conn: NWConnection?
    private var connRemote = false
    private var connID = 0
    private var listening = false
    private var failNoted = false
    private var appActive = true
    private var session: AVCaptureSession?
    private var deviceInput: AVCaptureDeviceInput?
    private var venc: VTCompressionSession?
    private var curFps = 30, curKbps = 4000, curW = 0, curH = 0
    private var lastHello: Hello?         // いまのつなぎで最後に受けた HELLO。画面の設定が変わったら、これと混ぜて作り直す
    private var curReq: Resolved?          // いま流している形の元（nil は流していない）
    private var settingsGen = 0
    private var observers: [NSObjectProtocol] = []
    private var pendingApply = false       // アプリが前にいない間（コントロールセンターなど）に設定が変わった。戻ったら当てる
    private var focusTargetApplied = -1    // いまのカメラに当てたピントの合わせ方（同じなら合わせ直さない）
    private var torchDev: AVCaptureDevice? // 点けたライトの持ち主（前面カメラのときは背面のカメラ）
    private var baseFps = 0                // 熱で下げる前のコマ数（いまのカメラ）
    // camctl2：PC から操作する（CTRL）。q の上だけで触る
    private var ctl = false                    // このつなぎの PC は HELLO で CTRL を名乗った
    private var ctlNow = CtlCaps()             // PC に送ってある CAPS（SET を範囲に収める・STATE の値を決める）
    private var ctlSent: [UInt16: Float] = [:] // PC に送った値（変わった物だけ送るため）
    private var ctlReplies = Set<UInt16>()     // 次の STATE で SET への返事（why 8）にする id
    private var ctlFlushQueued = false
    private var ctlPollGen = 0                 // 1 秒ごとの見回りの番号（つなぎ直し・作り直しで前の見回りを止める）
    private var ctlZoom: CGFloat?              // PC がカメラを決めて画面のカメラと違うときに、PC が選んだ倍率（このつなぎの間だけ）
    private var dimCopy = false                // 画面を暗くしている（ScreenDimmer から写す）
    private var lightCopy = false              // 画面のライトを出している（ScreenDimmer から写す）
    private var screenSubs: [AnyCancellable] = []

    // カメラの糸・エンコーダの糸からも触る物（lock で守る）
    private let lock = NSLock()
    private var sendConn: NWConnection? // 送り先。つなぎが無い・止めているときは nil
    private var forceKey = true
    private var inflight = 0            // 送り始めて、まだ iOS が送り終えていないコマの数
    private var seq: UInt32 = 0
    private var sent: UInt64 = 0
    private var dropped: UInt64 = 0
    private var capCalls: UInt64 = 0   // camfix1015：カメラから来たコマの数（実測の記録用）
    private var encDone: UInt64 = 0    // camfix1015：エンコーダから出てきたコマの数
    private var vtDropped: UInt64 = 0  // camfix1015：エンコーダが落としたコマの数
    private var capBusyUs: UInt64 = 0  // camfix1015：captureOutput の中にいた時間の合計（µs）
    private var lateDrops: UInt64 = 0  // camrot1015：iOS が「遅れた」として捨てたコマ（alwaysDiscardsLateVideoFrames。captureOutput(didDrop:)）
    private var bufDrops: UInt64 = 0   // camrot1015：iOS が入れ物の不足で捨てたコマ
    private var otherDrops: UInt64 = 0 // camrot1015：iOS が別の理由で捨てたコマ（途切れなど）
    private var encCallUs: UInt64 = 0  // camrot1015：VTCompressionSessionEncodeFrame を呼んで返るまでの時間の合計（µs）
    private var encCalls: UInt64 = 0   // camrot1015：その呼び出しの数
    private var flipUs: UInt64 = 0     // camrot1015：Metal で回した時間の合計（µs。GPU が終わるまで待つ時間を含む）
    private var flipCalls: UInt64 = 0  // camrot1015：Metal で回したコマの数
    private var flipFails: UInt64 = 0  // camrot1015：Metal で回せなかったコマの数（このつなぎの間。回せないコマは送らない）
    private var flipCopy: FrameFlipper? // camrot1015：180 度・反転を Metal で行うとき（なければ nil）
    private var waitDrops: UInt64 = 0  // cam4k1016：糸が間に合わず、列から古いコマを捨てた数（prepQueue と encodeQueue の列の合計）
    private var waitUs: UInt64 = 0     // cam4k1016：受け口に来てから、エンコーダに渡す直前までの時間の合計（µs。列で待った時間と回す時間を含む。エンコーダに渡したコマだけ）
    private var workUs: UInt64 = 0     // cam4k1016：エンコードの糸が 1 コマに使った時間の合計（µs。エンコーダの呼び出しなど。回す処理は含まない）
    private var workCalls: UInt64 = 0  // cam4k1016：その数
    private var prepUs: UInt64 = 0     // cam4k1016：回す糸が 1 コマに使った時間の合計（µs。Metal・Core Image。回したコマだけ）
    private var prepCalls: UInt64 = 0  // cam4k1016：その数
    private var sessionGen: UInt64 = 0  // カメラを作り直すたびに増やす。前のカメラのコマを、新しい形の後ろに送らないため
    private let maxInflight = 4         // これより溜まったら、カメラの絵を捨てて追いつく（USB が詰まったとき、古いコマを送り続けない）
    // camctl：lock で守る
    private var stage = StageParams()
    private var poolCopy: CVPixelBufferPool?       // 切り抜いた絵を入れる先（送る大きさ）
    private var devCopy: AVCaptureDevice?          // いまのカメラ（画面の糸が状態を読む）
    private var videoOutCopy: AVCaptureVideoDataOutput? // 顔の位置を、送る絵の向きに直すため
    private var faceTarget: CGPoint?               // 顔の真ん中（送る絵の上で 0〜1。左上が 0）
    private var srcW = 0, srcH = 0                 // カメラが渡す絵の大きさ（回したあと）
    private var levelShown = 0.0                   // 画面に出す、直している角度（度）
    private var cineCopy = false                   // シネマティックが入っている

    private let captureQueue = DispatchQueue(label: "camera-capture", qos: .userInteractive)
    // cam4k1016：受け口（captureQueue）は、コマを列に入れるだけ。回す処理は prepQueue、エンコーダの呼び出しは encodeQueue（どちらも直列）。
    // 回す必要がないコマは、prepQueue を通らず、受け口から encodeQueue の列へ入る。
    // 下の「カメラの糸だけで触る」物（ciContext・levelNow・faceNow・lastStagePts）と FrameFlipper は、prepQueue だけで触る（前は captureQueue）
    private let prepQueue = DispatchQueue(label: "camera-prep", qos: .userInteractive)
    private let encodeQueue = DispatchQueue(label: "camera-encode", qos: .userInteractive)
    private static let maxWait = 2                // 列の上限（枚）。超えたら古い方を捨てる。隠しスイッチ 32 で 1 枚
    private var waitLimit: Int { Self.debugBits & 32 != 0 ? 1 : Self.maxWait }
    // 受け口が渡す 1 コマ。pb は、prepQueue の前ならカメラの入れ物、後なら回した入れ物。持っている間、入れ物は戻らない
    fileprivate struct CamFrame { let pb: CVPixelBuffer; let pts: CMTime; let gen: UInt64; let enteredNs: UInt64 }
    // 古い順の列。入れる側と取り出す側が別の糸。running は「糸が列を空にしている最中」で、入れた側は running が false のときだけ糸を起こす
    fileprivate final class Mailbox {
        private let l = NSLock()
        private var items: [CamFrame] = []
        private var running = false
        // 入れる。limit を超えたら古い方を捨てる。返り値：捨てた数と、糸を起こす必要があるか
        func push(_ x: CamFrame, limit: Int) -> (dropped: Int, wake: Bool) {
            l.lock(); defer { l.unlock() }
            items.append(x)
            var d = 0
            while items.count > limit { items.removeFirst(); d += 1 }
            let w = !running
            if w { running = true }
            return (d, w)
        }
        // 古い方から 1 枚取り出す。空なら running を下ろして nil を返す（下ろすのと空の確かめが同じ lock の中なので、入れた 1 枚を取りこぼさない）
        func pop() -> CamFrame? {
            l.lock(); defer { l.unlock() }
            if items.isEmpty { running = false; return nil }
            return items.removeFirst()
        }
        func clear() { l.lock(); items.removeAll(); l.unlock() }
    }
    private let prepBox = Mailbox()    // prepQueue を待つコマ
    private let encBox = Mailbox()     // encodeQueue を待つコマ
    private let metaQueue = DispatchQueue(label: "camera-meta", qos: .userInitiated)
    // camctl：カメラの糸だけで触る（cam4k1016：いまは prepQueue だけ。隠しスイッチ 16 のときは captureQueue）
    private lazy var ciContext = CIContext(options: [.workingColorSpace: NSNull(), .cacheIntermediates: false])
    private var levelNow: CGFloat = 0              // なめらかにした傾き（ラジアン）
    private var faceNow = CGPoint(x: 0.5, y: 0.5)  // なめらかにした顔の位置
    private var lastStagePts = -1.0
    // 画面の糸だけで触る
    private var lastFx = CameraBridge.effectsMask()
    // iPhone の傾き（水平を保つ）。読むのはカメラの糸、始める・止めるのは q
    static let motion = CMMotionManager()

    private override init() {
        super.init()
        UserDefaults.standard.register(defaults: [Key.camera: Self.defaultCamera, Key.height: Self.defaultHeight, Key.fps: Self.defaultFps,
                                                  Key.quality: Self.defaultQuality, Key.preferPC: Self.defaultPreferPC])
        // camctl の初めの値（画面の @AppStorage の初めの値と同じ）。センターフレームは触るまで値を持たない
        UserDefaults.standard.register(defaults: [Key.mirror: false, Key.flipV: false, Key.rotation: 0,
                                                  Key.focus: 0, Key.focusPos: 0.5, Key.focusTarget: 0,
                                                  Key.exposure: 0, Key.bias: 0.0, Key.iso: 0.0, Key.shutter: 0,
                                                  Key.wb: 0, Key.wbTemp: 5000.0, Key.zoom: 1.0, Key.torch: false, Key.torchLevel: 0.5,
                                                  Key.stab: 0, Key.cinematic: false, Key.aperture: 0.0,
                                                  Key.crop: false, Key.cropPos: 0, Key.cropSize: 0.7, Key.cropX: 0.5, Key.cropY: 0.5,
                                                  Key.level: false, Key.screenWarm: false, Key.thermalFps: true])
        let nc = NotificationCenter.default
        // アプリが前にいるか。iOS はカメラを後ろで使わせない。前にいない間に変わった設定は、戻ったときに当てる
        observers.append(nc.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: nil) { [weak self] _ in
            self?.q.async {
                guard let self else { return }
                self.appActive = true
                if self.pendingApply { self.pendingApply = false; self.applySettings() }
            }
        })
        observers.append(nc.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: nil) { [weak self] _ in self?.q.async { self?.appActive = false } })
        observers.append(nc.addObserver(forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: nil) { [weak self] _ in
            let s = UInt32(ProcessInfo.processInfo.thermalState.rawValue)
            self?.q.async {
                self?.sendEvent(3, s)
                self?.applyThermal()
                self?.ctlSendState(all: false, readOnly: true, why: CtlWhy.ios) // camctl2：温度の状態（102）をすぐ
            }
        })
        // camctl2：画面で操作を変えた（@AppStorage も UserDefaults に書く）。CTRL の PC に今の値を送る
        observers.append(nc.addObserver(forName: UserDefaults.didChangeNotification, object: nil, queue: nil) { [weak self] _ in
            self?.q.async { self?.ctlSchedule() }
        })
        // camctl2：画面を暗くした・画面のライトを出した（画面の糸から）
        ScreenDimmer.shared.changes.sink { [weak self] on in
            self?.q.async { self?.dimCopy = on; self?.ctlSchedule() }
        }.store(in: &screenSubs)
        ScreenDimmer.shared.lightChanges.sink { [weak self] on in
            self?.q.async { self?.lightCopy = on; self?.ctlSchedule() }
        }.store(in: &screenSubs)
    }

    private func log(_ s: String) { (s + "\n").withCString { rpc_bridge_log_line($0) } }

    // ---- 画面の糸から呼ぶ ----

    // on の間だけ 50064 番で待つ。off にしたらカメラを止め、口を閉じ、つなぎを切る
    func setOn(_ v: Bool) {
        guard v != on else { return }
        on = v
        if !v {
            connected = false
            fps = 0
            detail = ""
            pcNote = ""
            pendingNote = false
            rotSkipped = false
            thermalNote = ""
            pcControl = false
        }
        let active = UIApplication.shared.applicationState == .active
        q.async { [weak self] in
            guard let self else { return }
            self.appActive = active
            self.listening = v
            if v { self.listen(); self.listenWifi() } else { self.close() }
        }
    }

    // 1 秒ごとに呼ぶ。送ったコマの数の差で毎秒のコマ数を出す
    // camctl：画面を暗くしている間は、画面に出す値を変えない（描き直さない。熱の対策）
    func tick() {
        if ScreenDimmer.shared.dimmed { return }
        let now = Date()
        let dt = now.timeIntervalSince(lastTick)
        guard dt >= 0.5 else { return }
        lock.lock(); let s = sent; lock.unlock()
        let f = Double(s &- lastShown) / dt
        lastShown = s
        lastTick = now
        if f != fps { fps = f }
        pollState()
    }

    // 画面でカメラの設定を変えた。つないでいれば、その場で切り替える。続けて変えたときは、最後の 1 回だけ作り直す
    // camctl：作り直さなくてよい変更（切り抜きの位置など）なら、作り直さずに今のカメラへ当てる
    func settingsChanged() {
        q.async { [weak self] in
            guard let self else { return }
            self.settingsGen += 1
            let g = self.settingsGen
            self.q.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                guard let self, g == self.settingsGen else { return }
                self.applySettings()
            }
        }
    }

    // camctl：ピント・明るさ・色・ズーム・ライトなど、作り直さずに変える操作。待たずに今のカメラへ当てる
    func controlsChanged() {
        q.async { [weak self] in self?.liveApply() }
    }

    // camctl：センターフレームを画面で切り替えた。アプリとコントロールセンターの両方で切り替えられる形にして当て、
    // 対応する形とコマ数で作り直す
    func setCenterStage(_ on: Bool) {
        UserDefaults.standard.set(on, forKey: Key.centerStage)
        Self.applyCenterStage(on) // 画面の糸のまま当てる（1 秒ごとの見回りが古い値で上書きしないように）
        centerStageOn = on
        q.async { [weak self] in self?.applySettings() }
    }

    // camctl：1 秒ごと（画面の糸）。ライト・エフェクト・水平の状態を画面に出す。
    // コントロールセンターでエフェクトを入れ替えたら、対応する形で作り直す（前にいないときは戻ってから）
    private func pollState() {
        let pe = AVCaptureDevice.isPortraitEffectEnabled
        var sl = false
        if #available(iOS 16.0, *) { sl = AVCaptureDevice.isStudioLightEnabled }
        let cs = AVCaptureDevice.isCenterStageEnabled
        lock.lock(); let dev = devCopy; let deg = levelShown; let cine = cineCopy; lock.unlock()
        let pa = dev?.isPortraitEffectActive ?? false
        let ts = connected ? Self.torchText(dev) : ""
        if pe != portraitOn { portraitOn = pe }
        if pa != portraitActive { portraitActive = pa }
        if sl != studioOn { studioOn = sl }
        if cs != centerStageOn { centerStageOn = cs }
        if cine != cinematicActive { cinematicActive = cine }
        if ts != torchState { torchState = ts }
        if abs(deg - levelAngle) >= 0.1 { levelAngle = deg }
        // センターフレームをコントロールセンターで切り替えた：覚えている値も合わせる（アプリが触ったことがあるときだけ）
        if AVCaptureDevice.centerStageControlMode == .cooperative, let saved = UserDefaults.standard.object(forKey: Key.centerStage) as? Bool, saved != cs {
            UserDefaults.standard.set(cs, forKey: Key.centerStage)
        }
        let fx = Self.effectsMask()
        if fx != lastFx {
            lastFx = fx
            if connected { settingsChanged() }
        }
    }

    // ---- ここから下は q の上 ----

    private func listen() {
        guard listening, listener == nil, let p = NWEndpoint.Port(rawValue: Self.port) else { return }
        let params = NWParameters.tcp
        params.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: p) // USB（iPhone の中の 127.0.0.1）だけ。Wi-Fi には出さない
        params.allowLocalEndpointReuse = true
        let l: NWListener
        do { l = try NWListener(using: params) } catch {
            noteFailure("\(error)")
            retryLater()
            return
        }
        l.stateUpdateHandler = { [weak self, weak l] st in
            guard let self, let l, l === self.listener else { return }
            switch st {
            case .ready:
                self.failNoted = false
                self.log("ウェブカメラの口：127.0.0.1:\(Self.port) で待っている")
            case .failed(let e):
                self.noteFailure("\(e)")
                self.listener = nil
                l.cancel()
                self.retryLater()
            default: break
            }
        }
        l.newConnectionHandler = { [weak self, weak l] c in
            guard let self, let l, l === self.listener else { c.cancel(); return }
            self.accept(c)
        }
        listener = l
        l.start(queue: q)
    }

    private func noteFailure(_ why: String) {
        if !failNoted { log("ウェブカメラの口を開けない：\(why)。1 秒ごとに開き直す") }
        failNoted = true
    }

    private func retryLater() { q.asyncAfter(deadline: .now() + 1) { [weak self] in self?.listen() } }

    private func close() {
        if let c = conn { drop(c) }
        if let l = listener {
            listener = nil
            l.cancel()
            log("ウェブカメラの口を閉じた")
        }
        closeWifi()
        failNoted = false
    }

    // 画面の糸から呼ぶ。Wi-Fi でも待つかを切り替える（ウェブカメラがオンの間だけ効く）
    func setWifi(_ v: Bool) {
        q.async { [weak self] in
            guard let self else { return }
            if v && self.listening { self.wifiMisses = 0; self.listenWifi() } else if !v { self.closeWifi() }
        }
    }

    private func listenWifi() {
        guard listening, wifiListener == nil, UserDefaults.standard.bool(forKey: Self.wifiKey), let p = NWEndpoint.Port(rawValue: Self.port) else { return }
        let params = NWParameters.tcp
        params.requiredInterfaceType = .wifi // Wi-Fi の口だけ。合言葉の無い頼みは断る
        params.allowLocalEndpointReuse = true
        guard let l = try? NWListener(using: params, on: p) else { log("ウェブカメラ：Wi-Fi の口を開けない"); return }
        l.stateUpdateHandler = { [weak self, weak l] st in
            guard let self, let l, l === self.wifiListener else { return }
            if case .ready = st { self.log("ウェブカメラ：Wi-Fi の \(Self.port) 番でも待っている（合言葉つき）") }
            if case .failed = st { self.wifiListener = nil; l.cancel() }
        }
        l.newConnectionHandler = { [weak self, weak l] c in
            guard let self, let l, l === self.wifiListener else { c.cancel(); return }
            self.accept(c, remote: true)
        }
        wifiListener = l
        l.start(queue: q)
    }

    private func closeWifi() {
        if let l = wifiListener {
            wifiListener = nil
            l.cancel()
            log("ウェブカメラ：Wi-Fi の口を閉じた")
        }
        cancelPending() // 口を閉じたら、確かめ中のつなぎも通さない（入れ替わったあとのつなぎは、そのまま）
    }

    // 一度に 1 本。新しいつなぎが来たら、古い方を切る（PC の橋渡しを開き直したとき、切れ残りを待たない）。
    // USB（127.0.0.1）はすぐ入れ替える。Wi-Fi は同じ Wi-Fi の誰でもつなげるので、HELLO の合言葉を確かめてから入れ替える
    // （つながった時点で切ると、合言葉を知らない人が何度でも PC のカメラを切れた）
    private func accept(_ c: NWConnection, remote: Bool = false) {
        if remote { acceptWifi(c); return }
        if let old = conn { drop(old) }
        connID += 1
        conn = c
        connRemote = false
        c.stateUpdateHandler = { [weak self, weak c] st in
            guard let self, let c else { return }
            switch st {
            case .failed, .cancelled: self.drop(c)
            default: break
            }
        }
        log("ウェブカメラ：PC がつながった")
        c.start(queue: q)
        readMessage(c)
    }

    // ---- Wi-Fi のつなぎ：合言葉を確かめてから入れ替える（すべて q の上） ----

    // まだ conn には入れない。HELLO を 1 つ読み、合言葉が合ってから古いつなぎと入れ替える。
    // 合言葉が違う・HELLO ではない・10 秒たっても来ないつなぎは、古い方に触らずに閉じる。確かめ中は 4 本まで（超えたら古い方から閉じる）
    private func acceptWifi(_ c: NWConnection) {
        while pending.count >= Self.maxPending { pending.removeFirst().cancel() }
        pending.append(c)
        c.stateUpdateHandler = { [weak self, weak c] st in
            guard let self, let c else { return }
            switch st {
            case .failed, .cancelled:
                self.forgetPending(c)
                self.drop(c) // 入れ替わったあとに切れたとき。conn でなければ何もしない
            default: break
            }
        }
        c.start(queue: q)
        q.asyncAfter(deadline: .now() + Self.pendingSeconds) { [weak self, weak c] in
            guard let self, let c, self.forgetPending(c) else { return } // もう入れ替わった・閉じた物は何もしない
            self.log("ウェブカメラ：Wi-Fi のつなぎから HELLO が来ないので閉じた")
            c.cancel()
        }
        readFirstHello(c)
    }

    // 合言葉を確かめる前に読むのは、最初の HELLO 1 つだけ（[u32 型][u32 長さ][中身]）。ほかの型・変な長さ・途中で切れたときは閉じる
    private func readFirstHello(_ c: NWConnection) {
        readExactly(c, 8) { [weak self] h in
            guard let self, self.isPending(c) else { return }
            guard let h else { self.closePending(c); return }
            let b = [UInt8](h)
            let type = Self.u32(b, 0), len = Int(Self.u32(b, 4))
            guard type == Msg.hello, len >= 32, len <= Self.maxFirstHello else {
                self.log("ウェブカメラ：Wi-Fi の最初の頼みが HELLO ではないので閉じた")
                self.closePending(c)
                return
            }
            self.readExactly(c, len) { [weak self] body in
                guard let self, self.isPending(c) else { return }
                guard let body else { self.closePending(c); return }
                let hello = [UInt8](body)
                guard self.codeOK(hello) else { self.refuseWifi(c); return }
                self.promoteWifi(c)
                self.handle(c, Msg.hello, hello) // 合言葉は合っている。あとは USB のつなぎと同じ道
                self.readMessage(c)
            }
        }
    }

    // HELLO の 32 バイト目からの 4 バイトが、この起動の合言葉か
    private func codeOK(_ body: [UInt8]) -> Bool {
        body.count >= 36 && Self.u32(body, 32) == wifiCode
    }

    // 合言葉が合った：ここで初めて古いつなぎ（USB でも Wi-Fi でも）と入れ替える
    private func promoteWifi(_ c: NWConnection) {
        forgetPending(c)
        if let old = conn { drop(old) }
        connID += 1
        conn = c
        connRemote = true
        log("ウェブカメラ：PC がつながった")
    }

    // 合言葉が違う：古いつなぎには触らず、このつなぎだけ断って閉じる。違いが 5 回続いたら Wi-Fi の口を閉じる（前と同じ）
    private func refuseWifi(_ c: NWConnection) {
        forgetPending(c)
        wifiMisses += 1
        log("ウェブカメラ：Wi-Fi の合言葉が違う（\(wifiMisses) 回目）。いまのつなぎはそのまま")
        sendAck(status: 6, w: 0, h: 0, fps: 0, kbps: 0, camera: 0, flags: 0, to: c)
        q.asyncAfter(deadline: .now() + 0.2) { c.cancel() } // ACK が出てから切る
        if wifiMisses >= 5 { closeWifi() }
    }

    private func isPending(_ c: NWConnection) -> Bool { pending.contains { $0 === c } }

    @discardableResult
    private func forgetPending(_ c: NWConnection) -> Bool {
        guard let i = pending.firstIndex(where: { $0 === c }) else { return false }
        pending.remove(at: i)
        return true
    }

    private func closePending(_ c: NWConnection) {
        forgetPending(c)
        c.cancel()
    }

    private func cancelPending() {
        let list = pending
        pending.removeAll()
        for c in list { c.cancel() }
    }

    // いまのつなぎなら、カメラとエンコーダを止めて切る
    private func drop(_ c: NWConnection) {
        guard c === conn else { return }
        conn = nil
        lastHello = nil
        stopCapture()
        ctlReset(false)
        c.cancel()
        log("ウェブカメラ：切れた")
        DispatchQueue.main.async { [weak self] in
            self?.connected = false
            self?.detail = ""
            self?.pcNote = ""
            self?.pendingNote = false
            self?.rotSkipped = false
            self?.thermalNote = ""
            self?.pcControl = false
        }
    }

    // [u32 型][u32 長さ][中身] を 1 つ読み、次を待つ
    private func readMessage(_ c: NWConnection) {
        readExactly(c, 8) { [weak self] h in
            guard let self, c === self.conn else { return }
            guard let h else { self.drop(c); return }
            let b = [UInt8](h)
            let type = Self.u32(b, 0), len = Int(Self.u32(b, 4))
            if len > 1 << 16 { self.log("ウェブカメラ：長さが \(len) バイトで上限を超えた。形が違うので切る"); self.drop(c); return }
            if len == 0 { self.handle(c, type, []); self.readMessage(c); return }
            self.readExactly(c, len) { [weak self] body in
                guard let self, c === self.conn else { return }
                guard let body else { self.drop(c); return }
                self.handle(c, type, [UInt8](body))
                self.readMessage(c)
            }
        }
    }

    private func readExactly(_ c: NWConnection, _ n: Int, _ done: @escaping (Data?) -> Void) {
        c.receive(minimumIncompleteLength: n, maximumLength: n) { data, _, _, err in
            if let data, data.count == n, err == nil { done(data) } else { done(nil) }
        }
    }

    private static func u32(_ b: [UInt8], _ at: Int) -> UInt32 {
        UInt32(b[at]) | UInt32(b[at + 1]) << 8 | UInt32(b[at + 2]) << 16 | UInt32(b[at + 3]) << 24
    }

    private func handle(_ c: NWConnection, _ type: UInt32, _ body: [UInt8]) {
        switch type {
        case Msg.hello where body.count >= 32:
            if connRemote {
                guard codeOK(body) else {
                    wifiMisses += 1
                    log("ウェブカメラ：Wi-Fi の合言葉が違う（\(wifiMisses) 回目）")
                    sendAck(status: 6, w: 0, h: 0, fps: 0, kbps: 0, camera: 0, flags: 0)
                    q.asyncAfter(deadline: .now() + 0.2) { [weak self] in if let self, c === self.conn { self.drop(c) } }
                    if wifiMisses >= 5 { closeWifi() }
                    return
                }
                wifiMisses = 0
            }
            let f = (0..<8).map { Self.u32(body, $0 * 4) } // version, width, height, fps, kbps, gop, camera, flags
            // 36 バイト目からは画質の段（camset の PC だけが送る。古い PC は 32 か 36 バイト）
            let quality = body.count >= 40 ? Int(Self.u32(body, 36)) : 0
            // 40 バイト目からは PC が使える最大の大きさ（camgpu1015 の PC だけが送る。古い PC は 32〜40 バイトなので 0）
            let maxW = body.count >= 48 ? Int(Self.u32(body, 40)) : 0
            let maxH = body.count >= 48 ? Int(Self.u32(body, 44)) : 0
            let hi = Hello(version: f[0], width: Int(f[1]), height: Int(f[2]), fps: Int(f[3]), kbps: Int(f[4]), gop: Int(f[5]), camera: f[6], flags: f[7], quality: quality, maxW: maxW, maxH: maxH)
            lastHello = hi
            // camctl2：CTRL を名乗った PC か（頼み直しの HELLO でも読み直す）
            let wantCtl = hi.flags & Flag.ctrl != 0
            if wantCtl != ctl {
                ctlReset(wantCtl)
                if wantCtl { log("ウェブカメラ：PC が CTRL を名乗った。PC からカメラを操作できる") }
            }
            startSession(c, hi, changed: curReq != nil)
        case Msg.reqKey:
            lock.lock(); forceKey = true; lock.unlock()
        case Msg.setKbps where body.count >= 4:
            setBitrate(Int(Self.u32(body, 0)))
        case Msg.ping where body.count >= 8:
            var d = Data()
            d.append(contentsOf: body[0..<8])
            Self.appendLE64(&d, Self.nowUs())
            sendMsg(Msg.pong, d)
        case Msg.ctrlSet where ctl:
            ctlSet(body)
        case Msg.ctrlGet where ctl:
            // CAPS と今の値を送り直す（カメラを動かしていないとき＝作り直しの途中などは、このあとの CAPS で届く）
            ctlSendCaps()
            ctlSendState(all: true, readOnly: true, why: CtlWhy.all)
        default:
            break // 知らない型は読み捨てる（先の版が足した物でも壊れない）
        }
    }

    // ---- 頼みと画面の設定を混ぜる ----

    private func resolve(_ hi: Hello) -> Resolved? {
        let d = UserDefaults.standard
        let preferPC = d.bool(forKey: Key.preferPC)
        let appCam = UInt32(max(0, min(4, d.integer(forKey: Key.camera))))
        let (appW, appH) = Self.size(forHeight: d.integer(forKey: Key.height))
        let appFps = d.integer(forKey: Key.fps) > 0 ? d.integer(forKey: Key.fps) : Self.defaultFps
        let appQ = max(1, min(3, d.integer(forKey: Key.quality)))
        let appMirror = d.bool(forKey: Key.mirror)
        let pcSize = hi.width > 0 && hi.height > 0, pcFps = hi.fps > 0
        let pcCam = hi.camera != 0 || hi.flags & Flag.cameraSet != 0
        let pcRate = hi.kbps > 0 || hi.quality > 0
        // 左右反転：PC が mirror を立てたら「PC が決めた」。立っていなければおまかせで、画面の設定を使う
        let pcMirror = hi.flags & Flag.mirror != 0
        var flags: UInt32 = 0
        if !(pcSize && pcFps && pcCam && pcRate) { flags |= Flag.ackAppSettings }
        if !preferPC && (pcSize || pcFps || pcCam || pcRate || pcMirror) { flags |= Flag.ackAppPriority | Flag.ackAppSettings }
        let useSize = pcSize && preferPC, useFps = pcFps && preferPC, useCam = pcCam && preferPC, useRate = pcRate && preferPC
        let useMirror = pcMirror && preferPC
        // 無いカメラを頼まれたら：画面の設定のカメラ → 背面の広角 → 前面の順に代える（ACK の camera で、実際に使ったカメラを知らせる）
        var cam = useCam ? min(hi.camera, 4) : appCam
        for alt in [cam, appCam, 0, 1] where Self.device(alt) != nil { cam = alt; break }
        guard Self.device(cam) != nil else { return nil }
        var r = Resolved(camera: cam, w: useSize ? hi.width : appW, h: useSize ? hi.height : appH, fps: useFps ? hi.fps : appFps,
                         quality: useRate && hi.quality > 0 ? min(hi.quality, 3) : appQ, kbps: useRate ? hi.kbps : 0, gop: hi.gop,
                         mirror: useMirror || appMirror, ackFlags: flags, pcSize: useSize, pcFps: useFps, pcCam: useCam)
        r.pcMirror = useMirror
        r.askedFps = r.fps
        if !useMirror && appMirror { r.ackFlags |= Flag.ackAppSettings }
        // 形はいつも横長で選ぶ（カメラの形は横長）。縦長にするのは回転
        if r.h > r.w { (r.w, r.h) = (r.h, r.w) }
        // camgpu1015：PC が使える大きさの上限（HELLO の FIT）。PC の指定でも画面の設定でも、箱に収まる大きさにする。
        // 画面の「PC からの指定を優先する」とは別の話（上限は好みではなく、PC が受け取れない大きさ）なので、優先を切っていても守る
        let box = hi.flags & Flag.fit != 0 && hi.maxW > 0 && hi.maxH > 0
        if box {
            r.ackFlags |= Flag.ackFit
            let (fw, fh) = Self.fitSize(w: r.w, h: r.h, maxW: hi.maxW, maxH: hi.maxH)
            if fw != r.w || fh != r.h { r.w = fw; r.h = fh; r.fitShrunk = true }
        }
        // 回転と上下反転。上下反転は 180 度回して左右反転するのと同じ
        let want = d.integer(forKey: Key.rotation)
        var rot = [0, 90, 180, 270].contains(want) ? want : 0
        let flipV = d.bool(forKey: Key.flipV)
        // 古い webcam_rx（WC_HELLO_LIVE が無い）は縦長の絵を仮想カメラに書かないので、90 度と 270 度は使わない
        if rot % 180 != 0 && hi.flags & Flag.live == 0 { rot = 0; r.rotSkipped = true }
        r.portrait = rot % 180 != 0
        r.rot = flipV ? (rot + 180) % 360 : rot
        r.connMirror = r.mirror != flipV
        if rot != 0 || flipV { r.ackFlags |= Flag.ackAppSettings }
        // 縦長の 4K は送らない（Windows の H.264 のデコーダの資料は 4096×2304 まで）。1080 の高さに縮める
        if r.portrait && r.h > 1080 { r.w = (r.w * 1080 / r.h) & ~1; r.h = 1080 }
        r.stab = max(0, min(3, d.integer(forKey: Key.stab)))
        // シネマティックは 30 fps まで（WWDC25 の 319：1080p と 4K の 30 fps）。実際の上限は形の値で plan が決め直す
        r.cinematic = d.bool(forKey: Key.cinematic) && Self.cinematicDevice(cam) != nil
        // cam60：PC が 31 コマ/秒以上を頼んだ（画面の「PC からの指定を優先する」が入っていて、それが通る）ときは、頼みを通してシネマティックを止める。
        // 前は、画面で背景ぼかしを入れたままだと、PC が 60 を頼んでも黙って 30 になっていた。PC の指定を優先しない設定のときは、前のまま（画面の設定が通る）
        if r.cinematic && useFps && hi.fps > 30 {
            r.cinematic = false
            r.cineDropped = true
        }
        if r.cinematic { r.fps = min(r.fps, 30) }
        let cropOn = d.bool(forKey: Key.crop)
        let levelOn = d.bool(forKey: Key.level) && Self.levelAvailable(r.cinematic ? Self.cinematicDevice(cam) : Self.device(cam))
        // camrot1015：位置が中央の切り抜きだけ（水平を保たない）なら、Core Image で毎コマ切り出さず、カメラの倍率で済ませる（applyZoom）。
        // 切り抜く枠は中央で動かないので、カメラの中央を拡大するのと同じ絵になる
        r.camCrop = cropOn && !levelOn && d.integer(forKey: Key.cropPos) == 0 && Self.debugBits & 8 == 0
        r.stage = (cropOn && !r.camCrop) || levelOn
        r.face = cropOn && d.integer(forKey: Key.cropPos) == 3
        if r.stage {
            let s = Self.stageScale(crop: cropOn ? d.double(forKey: Key.cropSize) : 1, level: levelOn, w: r.w, h: r.h)
            r.capH = Self.captureHeight(camera: cam, outH: r.h, fps: r.fps, scale: s)
        }
        r.fx = Self.effectsMask()
        // camgpu1015：箱に収まる大きさを頼んでも、撮れる形のほうが箱より大きくなってしまう（その大きさの形が無い）ときだけ、
        // その形で撮って、エンコーダの前で箱の大きさに縮める（切り抜きと同じ stage の道。切り抜きも水平も切ってあれば、ただの縮小になる）。
        // plan は capH の高さの形をぴったりで選ぶので、ここで選んだ形と同じ形になる
        if box, !r.stage, let dev = r.cinematic ? Self.cinematicDevice(cam) : Self.device(cam),
           let picked = Self.pickFormat(dev, w: r.w, h: r.h, fps: r.fps, only: r.cinematic ? Self.cinematicFormat : nil, prefer: Self.effectScore(r.fx), full: !r.cinematic) {
            let dims = CMVideoFormatDescriptionGetDimensions(picked.0.formatDescription)
            let fw = Int(dims.width), fh = Int(dims.height)
            if !Self.fitsBox(fw, fh, maxW: hi.maxW, maxH: hi.maxH), Self.heightChoices.contains(fh), Self.size(forHeight: fh).0 == fw {
                r.stage = true
                r.fitStage = true
                r.capH = fh
            }
        }
        // camrot1015：回転が 180 度だけ（反転なし・切り抜きなし）で、PC が回せるなら、iPhone は回さずに送る。
        // 回すのが無料になる。印を立てない PC（古い webcam_rx）には立てず、前どおり iPhone が回す
        if hi.flags & Flag.pcRot180 != 0 && r.rot == 180 && !r.connMirror && !r.stage && Self.debugBits & 4 == 0 {
            r.pcRot = true
            r.ackFlags |= Flag.ackPcRot180
        }
        return r
    }

    // 画面で設定が変わった：同じつなぎのまま作り直す（できるとき）
    private func applySettings() {
        guard let c = conn, let hi = lastHello, let cur = curReq else { return } // つないでいなければ、次の HELLO のときに使う
        // アプリが前にいない（コントロールセンターを開いているなど）ときは作り直せない（iOS はカメラを止める）。前に戻ってから
        guard appActive else { pendingApply = true; return }
        guard let r = resolve(hi) else { return }
        if r == cur { liveApply(); return } // 作り直さなくてよい変更（切り抜きの位置など）
        // 形が同じで画質だけ違う：作り直さずにビットレートだけ変える（どの PC でもよい）
        var onlyRate = r
        onlyRate.quality = cur.quality
        onlyRate.kbps = cur.kbps
        if onlyRate == cur {
            let k = r.kbps > 0 ? r.kbps : Self.qualityKbps(r.quality, w: curW, h: curH, fps: curFps)
            setBitrate(k)
            curReq = r
            liveApply()
            return
        }
        // 大きさとコマ数が今と同じになるなら、古い PC でも作り直してよい（絵の大きさが変わらない）
        var sameSize = false
        if let p = plan(r) {
            sameSize = p.outW == curW && p.outH == curH && p.fps == curFps
        }
        if hi.flags & Flag.live != 0 || sameSize {
            log("ウェブカメラ：画面で設定が変わったので、カメラとエンコーダを作り直す")
            startSession(c, hi, changed: true)
        } else {
            log("ウェブカメラ：画面で設定が変わった。PC が途中の切り替えに対応しないので、次の接続から使う")
            DispatchQueue.main.async { [weak self] in self?.pendingNote = true }
            liveApply() // 待つ間も、作り直さずに変えられる物は今のカメラへ
        }
    }

    // 作り直すときのカメラ・形・コマ数・送る大きさを決める
    private func plan(_ r: Resolved) -> Plan? {
        guard let dev = r.cinematic ? Self.cinematicDevice(r.camera) : Self.device(r.camera) else { return nil }
        // 切り抜くときは大きめに撮って、送る大きさに縮める
        let (cw, ch) = r.stage ? Self.size(forHeight: r.capH) : (r.w, r.h)
        guard let picked = Self.pickFormat(dev, w: cw, h: ch, fps: r.fps, only: r.cinematic ? Self.cinematicFormat : nil, prefer: Self.effectScore(r.fx), full: !r.cinematic) else { return nil }
        var fmt = picked.0
        var dur = picked.1, fr = picked.2
        // cam60：決めたコマ数から下がった理由を覚える（記録と画面に出す）
        var why: [FpsWhy] = []
        if r.fps < r.askedFps { why.append(.cinematic) }
        if fr < r.fps { why.append(.format) }
        // エフェクトが入っているときは、そのコマ数の範囲に収める（iOS も自分で収めるが、ACK に正しいコマ数を書くため）。
        // cam60：その前に、エフェクトに対応しない同じ大きさの形で、頼みのコマ数を撮れるなら、その形に替える（エフェクトは入らなくなるが、頼みが通る）
        let cap = Self.effectFpsCap(fmt, fx: r.fx, cinematic: r.cinematic)
        if cap > 0 && fr > cap {
            let fx = r.fx
            var avoided = false
            if !r.cinematic,
               let alt = Self.pickFormat(dev, w: cw, h: ch, fps: r.fps, only: { Self.effectFpsCap($0, fx: fx, cinematic: false) == 0 }, prefer: nil),
               alt.2 == fr {
                let d0 = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription)
                let d1 = CMVideoFormatDescriptionGetDimensions(alt.0.formatDescription)
                if d0.width == d1.width && d0.height == d1.height {
                    fmt = alt.0
                    dur = alt.1
                    avoided = true
                }
            }
            if !avoided, Self.canRun(fmt, cap) {
                fr = cap
                dur = CMTime(value: 1, timescale: CMTimeScale(cap))
                why.append(.effect)
            }
        }
        let base = fr
        // iPhone が熱いときは、コマ数を下げて始める（設定で切れる）。下げたあとに冷えたら applyThermal が戻す
        let hot = Self.thermalFps(fr)
        if hot < fr, Self.canRun(fmt, hot) {
            fr = hot
            dur = CMTime(value: 1, timescale: CMTimeScale(hot))
            why.append(.hot)
        }
        let dims = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription)
        var ow = Int(dims.width), oh = Int(dims.height)
        if r.stage { ow = r.w & ~1; oh = r.h & ~1 }
        if r.portrait { (ow, oh) = (oh, ow) }
        return Plan(dev: dev, fmt: fmt, frameDur: dur, fps: fr, baseFps: base, outW: ow, outH: oh, why: why)
    }

    // その形がそのコマ数で撮れるか（範囲の外を activeVideoMinFrameDuration に入れると例外で落ちる）
    private static func canRun(_ f: AVCaptureDevice.Format, _ fps: Int) -> Bool {
        fps > 0 && f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= Double(fps) && Double(fps) <= $0.maxFrameRate }
    }

    // cam60：1 コマの長さ（秒）から、1 秒のコマ数（四捨五入）。長さが読めなければ 0
    private static func fpsOf(_ d: CMTime) -> Int {
        let s = CMTimeGetSeconds(d)
        return s.isFinite && s > 0 ? Int((1 / s).rounded()) : 0
    }

    // 熱いときのコマ数。serious は 60 を 30 に・30 を 24 に、critical は 15。設定を切っているときはそのまま
    private static func thermalFps(_ fps: Int) -> Int {
        guard UserDefaults.standard.bool(forKey: Key.thermalFps) else { return fps }
        switch ProcessInfo.processInfo.thermalState {
        case .critical: return min(fps, 15)
        case .serious: return min(fps, fps > 30 ? 30 : 24)
        default: return fps
        }
    }

    // 熱の状態が変わった（q の上）。作り直さずに、カメラのコマ数とエンコーダの見込みのコマ数だけ変える。
    // 大きさは変わらないので、どの PC でもよい。途中の ACK を受けられる PC（LIVE）には、ACK（CHANGED と THERMAL）で知らせる
    private func applyThermal() {
        guard let input = deviceInput, let r = curReq, baseFps > 0 else { return }
        let target = Self.thermalFps(baseFps)
        guard target != curFps, Self.canRun(input.device.activeFormat, target) else { return }
        let dev = input.device
        do { try dev.lockForConfiguration() } catch { return }
        let dur = CMTime(value: 1, timescale: CMTimeScale(target))
        dev.activeVideoMinFrameDuration = dur
        dev.activeVideoMaxFrameDuration = dur
        dev.unlockForConfiguration()
        let down = target < curFps
        curFps = target
        lock.lock(); fpsCopy = target; lock.unlock()
        if let e = venc { _ = VTSessionSetProperty(e, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: NSNumber(value: target)) }
        log("ウェブカメラ：熱の状態 \(ProcessInfo.processInfo.thermalState.rawValue) のため、コマ数を \(target) にした")
        liveApply() // 手動のシャッター速度を、新しい 1 コマの長さに収め直す
        if let hi = lastHello, hi.flags & Flag.live != 0 {
            sendAck(status: 0, w: curW, h: curH, fps: target, kbps: curKbps, camera: Self.ackCamera(r.camera),
                    flags: r.ackFlags | Flag.ackLive | Flag.ackChanged | (down ? Flag.ackThermal : 0) | (ctl ? Flag.ackCtrl : 0))
        }
        sendEvent(5, UInt32(target)) // 5：熱でコマ数を変えた（arg が新しいコマ数）。古い PC は記録に出すだけ
        // camctl2：シャッター速度の選択肢と送っているコマ数（108）が変わるので、CAPS と今の値を送り直す
        if ctl {
            ctlSendCaps()
            ctlSendState(all: true, readOnly: true, why: CtlWhy.all)
        }
        let w = curW, h = curH, kb = curKbps
        let name = Self.cameraName(r.camera)
        let note = down ? L("iPhone が熱いため、フレームレートを \(target) fps に下げています。", "The iPhone is hot, so the frame rate is lowered to \(target) fps.") : ""
        DispatchQueue.main.async { [weak self] in
            guard let self, self.connected else { return }
            self.detail = "\(w)×\(h)・\(target) fps・\(name)・\(String(format: "%.0f", Double(kb) / 1000)) Mbps"
            self.thermalNote = note
        }
    }

    // ---- 始める：権限 → カメラ → エンコーダ → ACK → 流す ----

    private func startSession(_ c: NWConnection, _ hi: Hello, changed: Bool) {
        func refuse(_ status: Int32, _ why: String) {
            log("ウェブカメラ：断った（\(status)・\(why)）")
            sendAck(status: status, w: 0, h: 0, fps: 0, kbps: 0, camera: hi.camera, flags: Flag.ackLive)
            // ACK が出てから切る
            q.asyncAfter(deadline: .now() + 0.2) { [weak self] in if let self, c === self.conn { self.drop(c) } }
        }
        stopCapture() // 2 回目の HELLO（形を変えて頼み直す）・画面の設定の変更のときは、前のカメラを止めてから
        let sizeOK = (hi.width == 0 && hi.height == 0) || (hi.width >= 64 && hi.height >= 64 && hi.width <= 4096 && hi.height <= 3072)
        guard hi.version == Self.version, sizeOK, hi.fps >= 0, hi.fps <= 120 else { refuse(5, "頼みが変"); return }
        guard appActive else { refuse(3, "アプリが前にいない（iOS はカメラを後ろで使わせない）"); return }
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized: break
        case .notDetermined:
            // 許可の画面を出す。答えが来たらもう一度ここへ（HELLO をそのまま使う）
            AVCaptureDevice.requestAccess(for: .video) { [weak self] ok in
                self?.q.async {
                    guard let self, c === self.conn else { return }
                    if ok { self.startSession(c, hi, changed: changed) } else { refuse(1, "カメラの許可が無い") }
                }
            }
            return
        default: refuse(1, "カメラの許可が無い"); return
        }
        guard let r = resolve(hi), Self.device(r.camera) != nil else { refuse(2, "使えるカメラが無い"); return }
        guard let p = plan(r) else { refuse(2, "そのカメラに使える形が無い"); return }
        let dev = p.dev, fmt = p.fmt
        var fr = p.fps
        var fpsWhy = p.why
        // camrot1015：回転と反転を接続でなく、PC（ackPcRot180）か Metal に任せるか。縦横が変わらない物（0 度・180 度＋左右反転）だけ。
        // 切り抜き・水平（stage）のときは、Core Image が回した絵を前提に枠を決めているので、前のとおり接続で回す
        var connRot = r.rot, connMirror = r.connMirror
        var flipper: FrameFlipper?
        var rotHow = r.rot == 0 && !r.connMirror ? "なし" : "接続"
        if r.pcRot {
            connRot = 0
            rotHow = "PC"
        } else if !r.stage && r.rot % 180 == 0 && (r.rot == 180 || r.connMirror) && Self.debugBits & 4 == 0 {
            // 先に左右を反転する向き：180 度は上下も左右も逆。回してから左右反転するので、左右は「180 度か」と「反転か」が違うときだけ反転
            flipper = FrameFlipper(w: p.outW, h: p.outH, flipX: (r.rot == 180) != r.connMirror, flipY: r.rot == 180)
            if flipper != nil {
                connRot = 0
                connMirror = false
                rotHow = "Metal"
            } else {
                log("ウェブカメラ：Metal で回す準備ができない（\(FrameFlipper.buildError)）。前のとおり接続で回す")
            }
        }
        // cam4k1016：隠しスイッチ 128。回さないときも、Metal で自分の入れ物（IOSurface の NV12）へそのまま写してからエンコーダに渡す。
        // 4K60 の記録では、Metal で写した 180 度の方がエンコーダの呼び出しが約 3 ms 短かった（0 度 19.5 ms・180 度 16.4 ms）。写す入れ物の違いが効いているかを比べる
        if flipper == nil && !r.stage && connRot == 0 && !connMirror && Self.debugBits & 128 != 0 {
            flipper = FrameFlipper(w: p.outW, h: p.outH, flipX: false, flipY: false)
            if flipper != nil {
                rotHow += "＋Metal の写し"
            } else {
                log("ウェブカメラ：Metal で写す準備ができない（\(FrameFlipper.buildError)）。カメラの入れ物のまま渡す")
            }
        }
        let s = AVCaptureSession()
        s.beginConfiguration()
        s.sessionPreset = .inputPriority // 形は device.activeFormat で決める（プリセットだと 1080p は 30 コマまでのことがある）
        let out = AVCaptureVideoDataOutput()
        var rotated = r.rot == 0
        var cineOn = false
        do {
            let input = try AVCaptureDeviceInput(device: dev)
            guard s.canAddInput(input) else { throw NSError(domain: "CameraBridge", code: 1) }
            s.addInput(input)
            deviceInput = input
            try dev.lockForConfiguration()
            dev.activeFormat = fmt
            // frameDur は pickFormat が fmt の videoSupportedFrameRateRanges の中から選んだ物（範囲の外を入れると例外で落ちる）
            dev.activeVideoMinFrameDuration = p.frameDur
            dev.activeVideoMaxFrameDuration = p.frameDur
            // HDR の映像にしない（形式が変わり、色が合わなくなる）
            if fmt.isVideoHDRSupported { dev.automaticallyAdjustsVideoHDREnabled = false; dev.isVideoHDREnabled = false }
            dev.unlockForConfiguration()
            out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange]
            out.alwaysDiscardsLateVideoFrames = true // 遅れたコマは溜めずに捨てる
            out.setSampleBufferDelegate(self, queue: captureQueue)
            guard s.canAddOutput(out) else { throw NSError(domain: "CameraBridge", code: 2) }
            s.addOutput(out)
            if let cn = out.connection(with: .video) {
                // 向き：初めは iPhone の持ち方に関わらず、横長（センサーの向き）で固定する。画面の「回転」で 90 度ずつ回す。
                // 回すのは接続で（ハードウェアで回る。エンコーダの前で回すより CPU を使わない）。回してから左右反転する
                rotated = Self.orient(cn, angle: connRot) // camrot1015：PC か Metal に任せるときは 0 度（回すのは接続ではない）
                if cn.isVideoMirroringSupported {
                    cn.automaticallyAdjustsVideoMirroring = false
                    cn.isVideoMirrored = connMirror // ふつうは反転しない。鏡のように見せるのは使う側のアプリ
                }
                // 手ぶれ補正。オフ以外は写る範囲が狭くなる。標準とシネマティックは遅れが増える
                if cn.isVideoStabilizationSupported { cn.preferredVideoStabilizationMode = Self.stabMode(r.stab) }
            }
            // シネマティックを先に入れてから、顔の出力を足す。シネマティックの間は、顔の出力の metadataObjectTypes を
            // requiredMetadataObjectTypesForCinematicVideoCapture と同じにしないと例外で落ちる（前は [.face] にしていて、両方を入れると落ちた）
            cineOn = enableCinematic(input, r)
            if r.face { addFaceOutput(s, cinematic: cineOn) }
        } catch {
            s.commitConfiguration()
            deviceInput = nil
            refuse(2, "カメラを開けない：\(error)")
            return
        }
        s.commitConfiguration()
        // cam60：カメラが実際に受けているコマ数を読み直す。iOS が設定を直してしまうことがあるので、ACK には作る前に決めた値ではなく、読み直した値を書く。
        // 違っていたら、決めたコマ数をもう一度当ててみて、それでも違えば iOS の値に合わせる
        var base = p.baseFps
        let planned = fr
        if Self.canRun(fmt, planned), Self.fpsOf(dev.activeVideoMinFrameDuration) != planned {
            let seen = Self.fpsOf(dev.activeVideoMinFrameDuration)
            do {
                try dev.lockForConfiguration()
                dev.activeVideoMinFrameDuration = p.frameDur
                dev.activeVideoMaxFrameDuration = p.frameDur
                dev.unlockForConfiguration()
            } catch {
                log("ウェブカメラ：コマ数を当て直せない（\(error)）")
            }
            let again = Self.fpsOf(dev.activeVideoMinFrameDuration)
            log("ウェブカメラ：カメラのコマ数が決めた値（\(planned)）と違った（読み直し \(seen)・当て直したあと \(again)）")
            if again > 0 && again != planned {
                fr = again
                fpsWhy.append(.ios)
            }
        }
        if fr < planned && planned == base { base = fr } // 熱で下げたのではないので、戻す先も下げる
        // 送る大きさ。回せなかったときは、センサーの向きのまま（幅と高さも元のまま）
        var aw = p.outW, ah = p.outH
        if !rotated && r.portrait { (aw, ah) = (ah, aw) }
        var kb = r.kbps > 0 ? r.kbps : Self.qualityKbps(r.quality, w: aw, h: ah, fps: fr)
        var g = r.gop > 0 ? r.gop : fr * 2
        var encMade = makeEncoder(w: aw, h: ah, fps: fr, kbps: kb, gop: g)
        // cam60：31 コマ/秒以上で作れないときは、低遅延の方式を使わずに作ってみる。それでも作れなければ 30 コマ/秒に落とす
        if encMade == nil && fr > 30 {
            encMade = makeEncoder(w: aw, h: ah, fps: fr, kbps: kb, gop: g, lowLatency: false)
        }
        if encMade == nil && fr > 30 && Self.canRun(fmt, 30) {
            let d30 = CMTime(value: 1, timescale: 30)
            do {
                try dev.lockForConfiguration()
                dev.activeVideoMinFrameDuration = d30
                dev.activeVideoMaxFrameDuration = d30
                dev.unlockForConfiguration()
                fr = 30
                fpsWhy.append(.encoder)
                base = min(base, fr)
                kb = r.kbps > 0 ? r.kbps : Self.qualityKbps(r.quality, w: aw, h: ah, fps: fr)
                g = r.gop > 0 ? r.gop : fr * 2
                encMade = makeEncoder(w: aw, h: ah, fps: fr, kbps: kb, gop: g)
            } catch {
                log("ウェブカメラ：30 コマ/秒に落とせない（\(error)）")
            }
        }
        guard let enc = encMade else {
            deviceInput = nil
            captureQueue.async { for i in s.inputs { s.removeInput(i) }; for o in s.outputs { s.removeOutput(o) } }
            refuse(4, "エンコーダを作れない")
            return
        }
        // 切り抜く・水平を保つときは、送る大きさの入れ物を用意する
        let pool = r.stage ? Self.makePool(w: aw, h: ah) : nil
        if r.stage && pool == nil { log("ウェブカメラ：切り抜きの入れ物を作れない。切り抜かずに送る") }
        session = s
        venc = enc
        curFps = fr; curKbps = kb; curW = aw; curH = ah
        curReq = r
        baseFps = base
        focusTargetApplied = -1
        lock.lock()
        sendConn = c; encCopy = enc; fpsCopy = fr; wCopy = aw; hCopy = ah; forceKey = true; inflight = 0; if !changed { seq = 0; sent = 0; dropped = 0 }
        poolCopy = pool; devCopy = dev; videoOutCopy = out; cineCopy = cineOn; faceTarget = nil
        flipCopy = flipper; flipFails = 0
        lock.unlock()
        observeSession(s)
        // ピント・明るさ・色・ズーム・ライト・切り抜きの値を、走らせる前に当てる
        liveApply(initial: true)
        // camctl2：CTRL の PC には ACK に CTRL を立てる。熱でコマ数を下げて始めたときは THERMAL も立てる
        let hot = fr < base
        var ackFlags = r.ackFlags | Flag.ackLive | (changed ? Flag.ackChanged : 0)
        if ctl { ackFlags |= Flag.ackCtrl | (hot ? Flag.ackThermal : 0) }
        sendAck(status: 0, w: aw, h: ah, fps: fr, kbps: kb, camera: Self.ackCamera(r.camera), flags: ackFlags)
        // 温度の状態は変わったときに EVENT 3 で送っている。つないだ直後にも今の状態を 1 回送る（古い PC は記録に出すだけ）
        if !changed { sendEvent(3, UInt32(ProcessInfo.processInfo.thermalState.rawValue)) }
        // camctl2：CTRL の PC には、熱で下げて始めたことも EVENT 5 で知らせ、操作の一覧（CAPS）と今の値（STATE）を送る
        if ctl {
            if hot { sendEvent(5, UInt32(fr)) }
            ctlStart()
        }
        let ctlOn = ctl
        // camgpu1015：PC が使える大きさの上限を、記録に出す（縮めたか・形が無くてエンコーダの前で縮めているか）
        var fitText = "なし"
        if hi.flags & Flag.fit != 0 {
            fitText = "\(hi.maxW)×\(hi.maxH)"
            if r.fitShrunk { fitText += "に収めた" }
            if r.fitStage { fitText += "（その大きさの形が無いので、大きく撮ってエンコーダの前で縮める）" }
        }
        log("ウェブカメラ：\(changed ? "切り替えた" : "始める") \(aw)×\(ah)・\(fr) コマ/秒・\(kb) kbps・カメラ \(r.camera)（\(dev.localizedName)）・回転 \(r.rot)・反転 \(r.connMirror ? 1 : 0)（回し方 \(rotHow)）・切り抜き \(pool != nil ? 1 : 0)（カメラの倍率で \(r.camCrop ? 1 : 0)）・隠しスイッチ \(Self.debugBits)・手ぶれ補正 \(r.stab)・シネマティック \(cineOn ? 1 : 0)・ACK flags \(ackFlags)・PC の上限 \(fitText)")
        // cam60：頼まれたコマ数から下がったときは、理由と、その大きさの形の一覧を記録に出す（本物で何が撮れるかが、ここで分かる）
        let askedFps = r.askedFps
        let whyJa = fpsWhy.map { $0.ja }.joined(separator: "、")
        let whyEn = fpsWhy.map { $0.en }.joined(separator: ", ")
        if fr < askedFps {
            let dims = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription)
            log("ウェブカメラ：頼まれたコマ数 \(askedFps) を \(fr) に下げた（\(whyJa.isEmpty ? "理由が分からない" : whyJa)）。\(dims.width)×\(dims.height) の形：\(Self.formatList(dev, w: Int(dims.width), h: Int(dims.height)))・エフェクト \(r.fx)・熱 \(ProcessInfo.processInfo.thermalState.rawValue)・シネマティック \(cineOn ? 1 : 0)")
        }
        if r.cineDropped { log("ウェブカメラ：PC が \(hi.fps) コマ/秒を頼んだので、背景ぼかし（シネマティック）を止めた") }
        // startRunning は返ってくるまでに時間がかかるので、カメラの糸で（前のカメラの stopRunning のあとに並ぶ）。
        // ライトは走り始めてからもう一度当てる。走らせる前（上の liveApply(initial:)）に点けても、走り始めで消えて点かないことがあった
        captureQueue.async { [weak self] in
            s.startRunning()
            self?.q.async {
                guard let self, self.session === s, let input = self.deviceInput else { return }
                self.applyTorch(input.device)
            }
        }
        // cam60：走り始めて 2 秒後から 3 秒の間に送ったコマ数を数えて、実測のコマ数を記録に出す（ACK の値と実際が合っているかを、本物で確かめるため）
        let ackFps = fr
        q.asyncAfter(deadline: .now() + 2.0) { [weak self] in
            guard let self, self.session === s else { return }
            self.lock.lock(); let n0 = self.sent; let c0 = self.capCalls; let e0 = self.encDone; let v0 = self.vtDropped; let b0 = self.capBusyUs
            let wd0 = self.waitDrops; let wu0 = self.workUs; let wc0 = self.workCalls; self.lock.unlock()
            let t0 = Date()
            self.q.asyncAfter(deadline: .now() + 3.0) { [weak self] in
                guard let self, self.session === s else { return }
                self.lock.lock(); let n1 = self.sent; let drop = self.dropped; let c1 = self.capCalls; let e1 = self.encDone; let v1 = self.vtDropped; let b1 = self.capBusyUs
                let wd1 = self.waitDrops; let wu1 = self.workUs; let wc1 = self.workCalls; self.lock.unlock()
                let secs = Date().timeIntervalSince(t0)
                let rate = (secs > 0 && n1 >= n0) ? Double(n1 - n0) / secs : 0
                let capRate = (secs > 0 && c1 >= c0) ? Double(c1 - c0) / secs : 0
                let encRate = (secs > 0 && e1 >= e0) ? Double(e1 - e0) / secs : 0
                let vtDrop = v1 >= v0 ? v1 - v0 : 0
                let busyMs = (c1 > c0 && b1 >= b0) ? Double(b1 - b0) / Double(c1 - c0) / 1000 : 0
                let workMs = (wc1 > wc0 && wu1 >= wu0) ? Double(wu1 - wu0) / Double(wc1 - wc0) / 1000 : 0
                self.log("ウェブカメラ：内訳 カメラから \(String(format: "%.1f", capRate)) コマ/秒・エンコーダから出た \(String(format: "%.1f", encRate)) コマ/秒・エンコーダが落とした \(vtDrop)・カメラの受け口の1コマの時間 \(String(format: "%.2f", busyMs)) ms・エンコードの糸の1コマの時間 \(String(format: "%.2f", workMs)) ms・待ちで捨てたコマ \(Self.delta(wd1, wd0))")
                let camFps = Self.fpsOf(dev.activeVideoMinFrameDuration)
                self.log("ウェブカメラ：実測 \(String(format: "%.1f", rate)) コマ/秒（ACK \(ackFps)・頼み \(askedFps)・カメラの設定 \(camFps)・捨てたコマ \(drop)・熱 \(ProcessInfo.processInfo.thermalState.rawValue)）")
            }
        }
        // camrot1015：iOS が捨てたコマ・エンコーダの呼び出しの時間・回す処理の時間。始めた直後と、落ち着いた 22 秒後の 2 回（始めた直後はエンコーダの立ち上がりで遅い）
        logBreakdown2(s, after: 2.0, label: "始めて 2 秒後・回し方 \(rotHow)")
        logBreakdown2(s, after: 22.0, label: "22 秒後・回し方 \(rotHow)")
        let name = Self.cameraName(r.camera)
        var noteText = Self.pcNoteText(r, w: aw, h: ah, fps: fr)
        if fr < askedFps {
            let extra = L("\(askedFps) fps を \(fr) fps に下げています（\(whyJa)）。", "Lowered from \(askedFps) fps to \(fr) fps (\(whyEn)).")
            noteText = noteText.isEmpty ? extra : noteText + " " + extra
        }
        if r.cineDropped {
            let extra = L("コマ数を優先して、背景ぼかしを止めています。", "Background blur is off to keep the frame rate.")
            noteText = noteText.isEmpty ? extra : noteText + " " + extra
        }
        let note = noteText
        let skipped = r.rotSkipped
        let hotNote = fr < base ? L("iPhone が熱いため、フレームレートを \(fr) fps に下げています。", "The iPhone is hot, so the frame rate is lowered to \(fr) fps.") : ""
        DispatchQueue.main.async { [weak self] in
            self?.connected = true
            self?.detail = "\(aw)×\(ah)・\(fr) fps・\(name)・\(String(format: "%.0f", Double(kb) / 1000)) Mbps"
            self?.pcNote = note
            self?.pendingNote = false
            self?.rotSkipped = skipped
            self?.thermalNote = hotNote
            self?.pcControl = ctlOn
        }
    }

    // camrot1015：数えている物の写し（lock の中で読む。q の上から呼ぶ）
    private struct CamCounters {
        var late: UInt64, buf: UInt64, other: UInt64, encUs: UInt64, encCalls: UInt64, flipUs: UInt64, flipCalls: UInt64, flipFails: UInt64
        // cam4k1016
        var capUs: UInt64, capCalls: UInt64, workUs: UInt64, workCalls: UInt64, waitUs: UInt64, waitDrops: UInt64, prepUs: UInt64, prepCalls: UInt64
    }
    private func counters() -> CamCounters {
        lock.lock(); defer { lock.unlock() }
        return CamCounters(late: lateDrops, buf: bufDrops, other: otherDrops, encUs: encCallUs, encCalls: encCalls,
                           flipUs: flipUs, flipCalls: flipCalls, flipFails: flipFails,
                           capUs: capBusyUs, capCalls: capCalls, workUs: workUs, workCalls: workCalls, waitUs: waitUs, waitDrops: waitDrops,
                           prepUs: prepUs, prepCalls: prepCalls)
    }
    private static func delta(_ a: UInt64, _ b: UInt64) -> UInt64 { a >= b ? a - b : 0 }

    // camrot1015：delay 秒後から 3 秒の間の、iOS が捨てたコマ・エンコーダの呼び出しの時間・回す処理の時間を記録に出す（q の上）
    private func logBreakdown2(_ s: AVCaptureSession, after delay: Double, label: String) {
        q.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self, self.session === s else { return }
            let k0 = self.counters()
            let t0 = Date()
            self.q.asyncAfter(deadline: .now() + 3.0) { [weak self] in
                guard let self, self.session === s else { return }
                let k1 = self.counters()
                let secs = Date().timeIntervalSince(t0)
                guard secs > 0 else { return }
                let late = Self.delta(k1.late, k0.late), buf = Self.delta(k1.buf, k0.buf), other = Self.delta(k1.other, k0.other)
                let ec = Self.delta(k1.encCalls, k0.encCalls), eu = Self.delta(k1.encUs, k0.encUs)
                let fc = Self.delta(k1.flipCalls, k0.flipCalls), fu = Self.delta(k1.flipUs, k0.flipUs), ff = Self.delta(k1.flipFails, k0.flipFails)
                let encMs = ec > 0 ? Double(eu) / Double(ec) / 1000 : 0
                let flipMs = fc > 0 ? Double(fu) / Double(fc) / 1000 : 0
                self.log("ウェブカメラ：内訳2（\(label)）\(String(format: "%.1f", secs)) 秒間・iOS が捨てたコマ 遅れ \(late)・入れ物不足 \(buf)・その他 \(other)・エンコーダの呼び出し 1 コマ \(String(format: "%.2f", encMs)) ms（\(ec) コマ）・Metal で回す 1 コマ \(String(format: "%.2f", flipMs)) ms（\(fc) コマ・失敗 \(ff)）")
                // cam4k1016：受け口・回す糸・エンコードの糸の 1 コマの時間、受け口に来てからエンコーダに渡す直前までの時間、列から捨てたコマ。1 行を 200 字に収める
                let cc = Self.delta(k1.capCalls, k0.capCalls), wc = Self.delta(k1.workCalls, k0.workCalls), pc = Self.delta(k1.prepCalls, k0.prepCalls)
                let capMs = cc > 0 ? Double(Self.delta(k1.capUs, k0.capUs)) / Double(cc) / 1000 : 0
                let prepMs = pc > 0 ? Double(Self.delta(k1.prepUs, k0.prepUs)) / Double(pc) / 1000 : 0
                let workMs = wc > 0 ? Double(Self.delta(k1.workUs, k0.workUs)) / Double(wc) / 1000 : 0
                let waitMs = wc > 0 ? Double(Self.delta(k1.waitUs, k0.waitUs)) / Double(wc) / 1000 : 0
                self.log("ウェブカメラ：内訳3（\(label)）受け口 1 コマ \(String(format: "%.2f", capMs)) ms（\(cc) コマ）・回す糸 \(String(format: "%.2f", prepMs)) ms（\(pc) コマ）・エンコードの糸 \(String(format: "%.2f", workMs)) ms（\(wc) コマ）・受け口から渡す直前まで \(String(format: "%.2f", waitMs)) ms・待ちで捨てたコマ \(Self.delta(k1.waitDrops, k0.waitDrops))")
            }
        }
    }

    // ---- camctl：向き・顔・シネマティック ----

    // 接続で回す。iOS 17 からは videoRotationAngle（0・90・180・270）。それより前は videoOrientation を、回していない向きから 90 度ずつ進める。
    // 回せたら true（0 度はいつも true。前と同じく、出力の既定のセンサーの向きのまま）
    private static func orient(_ cn: AVCaptureConnection, angle: Int) -> Bool {
        if #available(iOS 17.0, *) {
            let a = CGFloat(angle)
            guard cn.isVideoRotationAngleSupported(a) else { return angle == 0 }
            cn.videoRotationAngle = a
            return true
        }
        if angle == 0 { return true }
        guard cn.isVideoOrientationSupported else { return false }
        let order: [AVCaptureVideoOrientation] = [.landscapeRight, .portrait, .landscapeLeft, .portraitUpsideDown]
        let base = order.firstIndex(of: cn.videoOrientation) ?? 0
        cn.videoOrientation = order[(base + angle / 90) % 4]
        return true
    }

    // 顔の位置を調べる出力（切り抜きを顔に合わせるときだけ）。顔の検出はカメラの中で行われ、重くない。
    // シネマティック（iOS 26）を入れたあとに呼ぶ。シネマティックの間は、metadataObjectTypes を requiredMetadataObjectTypesForCinematicVideoCapture
    // にしないと NSInvalidArgumentException で落ちる（Apple の資料）。そのときは届いた物のうち .face だけを使う（metadataOutput が型で選ぶ）
    private func addFaceOutput(_ s: AVCaptureSession, cinematic: Bool) {
        let mo = AVCaptureMetadataOutput()
        guard s.canAddOutput(mo) else { log("ウェブカメラ：顔の位置を調べる出力を足せない"); return }
        s.addOutput(mo)
        #if compiler(>=6.2)
        if #available(iOS 26.0, *), cinematic {
            let need = mo.requiredMetadataObjectTypesForCinematicVideoCapture
            mo.metadataObjectTypes = need
            mo.setMetadataObjectsDelegate(self, queue: metaQueue)
            if !need.contains(.face) { log("ウェブカメラ：シネマティックの間は、このカメラは顔の位置を出さない") }
            return
        }
        #endif
        if mo.availableMetadataObjectTypes.contains(.face) {
            mo.metadataObjectTypes = [.face]
            mo.setMetadataObjectsDelegate(self, queue: metaQueue)
        } else {
            log("ウェブカメラ：このカメラは顔の位置を出さない")
        }
    }

    // 顔が見つかった（metaQueue）。いちばん大きい顔の真ん中を、送る絵の上の位置（回転と反転のあと）に直して覚える
    func metadataOutput(_ output: AVCaptureMetadataOutput, didOutput metadataObjects: [AVMetadataObject], from connection: AVCaptureConnection) {
        lock.lock(); let outCopy = videoOutCopy; lock.unlock()
        guard let vo = outCopy, let vc = vo.connection(with: .video) else { return }
        var best: CGRect?
        for o in metadataObjects where o.type == .face {
            guard let b = vo.transformedMetadataObject(for: o, connection: vc)?.bounds else { continue }
            if let cur = best, cur.width * cur.height >= b.width * b.height { continue }
            best = b
        }
        guard let b = best else { return }
        var pt = CGPoint(x: b.midX, y: b.midY)
        lock.lock()
        // 0〜1 で来るはず。画素の数で来たときは、絵の大きさで割る
        if (pt.x > 1.5 || pt.y > 1.5) && srcW > 0 && srcH > 0 { pt.x /= CGFloat(srcW); pt.y /= CGFloat(srcH) }
        faceTarget = CGPoint(x: min(max(pt.x, 0), 1), y: min(max(pt.y, 0), 1))
        lock.unlock()
    }

    // シネマティック（iOS 26）を入れる。入れられたら true。beginConfiguration の中で呼ぶ（入れると作り直しに時間がかかる）
    private func enableCinematic(_ input: AVCaptureDeviceInput, _ r: Resolved) -> Bool {
        guard r.cinematic else { return false }
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            guard input.isCinematicVideoCaptureSupported else { log("ウェブカメラ：この形ではシネマティックを使えない"); return false }
            input.isCinematicVideoCaptureEnabled = true
            setAperture(input)
            return true
        }
        #endif
        return false
    }

    // シネマティックの F 値（ぼかしの強さ）。形の範囲の中に収める。範囲の外や、変えられない形で入れると例外で落ちる
    private func setAperture(_ input: AVCaptureDeviceInput) {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            guard input.isCinematicVideoCaptureEnabled else { return }
            let f = input.device.activeFormat
            guard f.minSimulatedAperture > 0, f.maxSimulatedAperture >= f.minSimulatedAperture else { return }
            var a = Float(UserDefaults.standard.double(forKey: Key.aperture))
            if a <= 0 { a = f.defaultSimulatedAperture }
            a = min(max(a, f.minSimulatedAperture), f.maxSimulatedAperture)
            if abs(input.simulatedAperture - a) > 0.01 { input.simulatedAperture = a }
        }
        #endif
    }

    // ACK の camera。古い PC は 0〜3 しか知らないので、レンズ自動切り替え（4）は背面（0）と知らせる
    private static func ackCamera(_ cam: UInt32) -> UInt32 { cam == 4 ? 0 : cam }

    // ---- カメラと形 ----

    // カメラ（0 背面の広角・1 前面・2 背面の超広角・3 背面の望遠・4 背面のレンズ自動切り替え）。無い機種では入れない
    // 4 は iOS の仮想カメラ（三つのレンズ → 超広角と広角 → 広角と望遠の順に探す）。倍率に合わせて iOS がレンズを切り替える。
    // 仮想カメラは手動のピント・手動の露出・色温度の固定ができない（Apple の資料）。その操作は画面に出さない
    private static let deviceTable: [UInt32: AVCaptureDevice] = {
        func find(_ type: AVCaptureDevice.DeviceType, _ pos: AVCaptureDevice.Position) -> AVCaptureDevice? {
            AVCaptureDevice.DiscoverySession(deviceTypes: [type], mediaType: .video, position: pos).devices.first
        }
        var t: [UInt32: AVCaptureDevice] = [:]
        t[0] = find(.builtInWideAngleCamera, .back)
        t[1] = find(.builtInWideAngleCamera, .front)
        t[2] = find(.builtInUltraWideCamera, .back)
        t[3] = find(.builtInTelephotoCamera, .back)
        t[4] = find(.builtInTripleCamera, .back) ?? find(.builtInDualWideCamera, .back) ?? find(.builtInDualCamera, .back)
        return t
    }()
    static func device(_ id: UInt32) -> AVCaptureDevice? { deviceTable[id] }
    static func cameraName(_ id: UInt32) -> String {
        switch id {
        case 1: return L("前面カメラ", "Front camera")
        case 2: return L("超広角", "Ultra wide")
        case 3: return L("望遠", "Telephoto")
        case 4: return L("背面・レンズ自動切り替え", "Back, auto lens switching")
        default: return L("背面カメラ", "Back camera")
        }
    }

    // シネマティック（iOS 26）に使うカメラ。背面はデュアル広角・前面は TrueDepth（WWDC25 の 319）。望遠では使えない
    private static let cineTable: [UInt32: AVCaptureDevice] = {
        var t: [UInt32: AVCaptureDevice] = [:]
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            func find(_ type: AVCaptureDevice.DeviceType, _ pos: AVCaptureDevice.Position) -> AVCaptureDevice? {
                guard let d = AVCaptureDevice.DiscoverySession(deviceTypes: [type], mediaType: .video, position: pos).devices.first else { return nil }
                return d.formats.contains { $0.isCinematicVideoCaptureSupported && CameraBridge.is420v($0) } ? d : nil
            }
            let back = find(.builtInDualWideCamera, .back)
            t[0] = back; t[2] = back; t[4] = back
            t[1] = find(.builtInTrueDepthCamera, .front)
        }
        #endif
        return t
    }()
    static func cinematicDevice(_ id: UInt32) -> AVCaptureDevice? { cineTable[id] }
    private static func is420v(_ f: AVCaptureDevice.Format) -> Bool {
        CMFormatDescriptionGetMediaSubType(f.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
    }
    // シネマティックに使える形だけを選ぶ
    private static func cinematicFormat(_ f: AVCaptureDevice.Format) -> Bool {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) { return f.isCinematicVideoCaptureSupported }
        #endif
        return false
    }

    // コントロールセンターで入っているエフェクト（1 ポートレート・2 スタジオ照明・4 センターフレーム・8 背景の置き換え）
    static func effectsMask() -> Int {
        var m = 0
        if AVCaptureDevice.isPortraitEffectEnabled { m |= 1 }
        if #available(iOS 16.0, *), AVCaptureDevice.isStudioLightEnabled { m |= 2 }
        if AVCaptureDevice.isCenterStageEnabled { m |= 4 }
        if #available(iOS 18.0, *), AVCaptureDevice.isBackgroundReplacementEnabled { m |= 8 }
        return m
    }
    // 入っているエフェクトに対応する形を先に選ぶための点数
    private static func effectScore(_ fx: Int) -> (AVCaptureDevice.Format) -> Int {
        { f in
            var n = 0
            if fx & 1 != 0 && f.isPortraitEffectSupported { n += 1 }
            if #available(iOS 16.0, *), fx & 2 != 0 && f.isStudioLightSupported { n += 1 }
            if fx & 4 != 0 && f.isCenterStageSupported { n += 1 }
            if #available(iOS 18.0, *), fx & 8 != 0 && f.isBackgroundReplacementSupported { n += 1 }
            return n
        }
    }
    // エフェクトが入っているときの、コマ数の上限（0 は上限なし）
    private static func effectFpsCap(_ f: AVCaptureDevice.Format, fx: Int, cinematic: Bool) -> Int {
        var cap = 0
        func take(_ r: AVFrameRateRange?) {
            guard let r else { return }
            let m = Int(r.maxFrameRate.rounded(.down))
            if m > 0 { cap = cap == 0 ? m : min(cap, m) }
        }
        if fx & 1 != 0 && f.isPortraitEffectSupported { take(f.videoFrameRateRangeForPortraitEffect) }
        if #available(iOS 16.0, *), fx & 2 != 0 && f.isStudioLightSupported { take(f.videoFrameRateRangeForStudioLight) }
        if fx & 4 != 0 && f.isCenterStageSupported { take(f.videoFrameRateRangeForCenterStage) }
        if #available(iOS 18.0, *), fx & 8 != 0 && f.isBackgroundReplacementSupported { take(f.videoFrameRateRangeForBackgroundReplacement) }
        #if compiler(>=6.2)
        if #available(iOS 26.0, *), cinematic { take(f.videoFrameRateRangeForCinematicVideo) }
        #endif
        return cap
    }

    // センターフレーム：アプリとコントロールセンターの両方で切り替えられる形にしてから入れる（user の形のまま入れると例外で落ちる）
    static func applyCenterStage(_ on: Bool) {
        if AVCaptureDevice.centerStageControlMode != .cooperative { AVCaptureDevice.centerStageControlMode = .cooperative }
        if AVCaptureDevice.isCenterStageEnabled != on { AVCaptureDevice.isCenterStageEnabled = on }
    }

    // 水平を保てるか：背面のカメラで、傾きを読めるとき（前面はセンサーの向きが機種で違うので、本物で確かめるまで出さない）
    static func levelAvailable(_ dev: AVCaptureDevice?) -> Bool {
        dev?.position == .back && motion.isDeviceMotionAvailable
    }
    // 水平を保つときに直す角度の上限（度）。この角度まで直せるだけの余白を取る
    static let levelMaxDeg: CGFloat = 6
    // 水平の余白に要る拡大：枠を θ 回して、同じ縦横比の元の絵に収めるには cos θ ＋ 長辺/短辺 × sin θ 倍
    static func levelScale(w: Int, h: Int) -> CGFloat {
        let a = levelMaxDeg * .pi / 180
        let r = CGFloat(max(w, h)) / CGFloat(max(1, min(w, h)))
        return cos(a) + r * sin(a)
    }
    // 切り抜きと水平を合わせた、エンコーダの前で拡大する倍率
    static func stageScale(crop size: Double, level: Bool, w: Int, h: Int) -> CGFloat {
        let c = 1 / CGFloat(min(max(size, 0.3), 1))
        return max(c, level ? levelScale(w: w, h: h) : 1)
    }
    // 切り抜くときに撮る高さ：拡大が 1.25 倍までに収まる、いちばん小さい高さ（撮れなければ送る高さのまま）
    static func captureHeight(camera: UInt32, outH: Int, fps: Int, scale: CGFloat) -> Int {
        let need = Double(outH) * Double(scale) / 1.25
        let ok = heightChoices.filter { $0 >= outH && supports(camera: camera, height: $0, fps: fps) }
        return ok.first(where: { Double($0) >= need }) ?? ok.last ?? outH
    }

    // 手ぶれ補正の値（Key.stab）と、その形で選べる物
    static func stabMode(_ v: Int) -> AVCaptureVideoStabilizationMode {
        switch v {
        case 1:
            #if compiler(>=6.2)
            if #available(iOS 26.0, *) { return .lowLatency }
            #endif
            return .off
        case 2: return .standard
        case 3: return .cinematic
        default: return .off
        }
    }
    static func stabChoices(_ f: AVCaptureDevice.Format) -> [Int] {
        var s = [0]
        #if compiler(>=6.2)
        if #available(iOS 26.0, *), f.isVideoStabilizationModeSupported(.lowLatency) { s.append(1) }
        #endif
        if f.isVideoStabilizationModeSupported(.standard) { s.append(2) }
        if f.isVideoStabilizationModeSupported(.cinematic) { s.append(3) }
        return s
    }

    // 見え方の倍率（背面の広角を 1 とする）とカメラの倍率（videoZoomFactor）の比。iOS 18 からは iOS が教える値
    static func zoomMultiplier(_ dev: AVCaptureDevice) -> CGFloat {
        // 仮想カメラは、最初のレンズが超広角なら、切り替わる倍率の最初（広角に替わる所）を 1 にする（iOS の版によらず同じ見え方）
        if dev.isVirtualDevice, dev.constituentDevices.first?.deviceType == .builtInUltraWideCamera,
           let f = dev.virtualDeviceSwitchOverVideoZoomFactors.first?.doubleValue, f > 1 {
            return CGFloat(1 / f)
        }
        if #available(iOS 18.0, *) {
            let m = dev.displayVideoZoomFactorMultiplier
            if m > 0 && m != 1 { return m }
        }
        // iOS 17 まで、または iOS が 1 と言うとき：超広角は 0.5
        if dev.deviceType == .builtInUltraWideCamera { return 0.5 }
        // 望遠は画角の比から（0.5 刻みに丸める）
        if dev.deviceType == .builtInTelephotoCamera, let wide = device(0) {
            let a = tan(Double(wide.activeFormat.videoFieldOfView) * .pi / 360), b = tan(Double(dev.activeFormat.videoFieldOfView) * .pi / 360)
            if b > 0 { return CGFloat(max(1, (a / b * 2).rounded() / 2)) }
        }
        return 1
    }
    // カメラを替えたときの倍率：そのカメラをそのまま使う見え方（超広角は 0.5・レンズ自動切り替えは広角の 1）
    static func baseZoom(_ cam: UInt32) -> CGFloat {
        guard cam != 4, let d = device(cam) else { return 1 }
        return zoomMultiplier(d)
    }
    // 倍率のボタン：各レンズの 1 枚目と、その 2 倍（48MP のセンサーの真ん中を使う倍率）。見え方の倍率で返す
    static func zoomStops(_ dev: AVCaptureDevice, m: CGFloat) -> [CGFloat] {
        var base: [CGFloat] = [m]
        for f in dev.virtualDeviceSwitchOverVideoZoomFactors { base.append(CGFloat(f.doubleValue) * m) }
        var s = Set<CGFloat>()
        for b in base {
            s.insert(roundZoom(b))
            if b >= 0.99 { s.insert(roundZoom(b * 2)) }
        }
        return s.sorted()
    }
    // 0.1 刻みに丸め、整数に近ければ整数にする
    static func roundZoom(_ z: CGFloat) -> CGFloat {
        let n = z.rounded()
        if n >= 1 && abs(z - n) < 0.15 { return n }
        return (z * 10).rounded() / 10
    }
    // レンズが替わる倍率のうち、z 以下でいちばん大きい物（水平の余白で倍率を下げるとき、ここより下げるとレンズが替わる）
    private static func lensFloor(_ dev: AVCaptureDevice, for z: CGFloat) -> CGFloat {
        var f: CGFloat = 1
        for n in dev.virtualDeviceSwitchOverVideoZoomFactors {
            let v = CGFloat(n.doubleValue)
            if v <= z + 0.001 { f = max(f, v) }
        }
        return f
    }

    // ライトの持ち主：そのカメラにライトがあればそのカメラ。前面カメラのときは背面のカメラ（使えるかは isTorchAvailable で決まる）
    static func torchDevice(for cur: AVCaptureDevice) -> AVCaptureDevice? {
        if cur.hasTorch { return cur }
        if cur.position == .front, let b = device(0), b.hasTorch { return b }
        return nil
    }
    // ライトの状態の文（画面の糸）。熱いときは iOS が弱めたり消したりする
    static func torchText(_ cur: AVCaptureDevice?) -> String {
        guard UserDefaults.standard.bool(forKey: Key.torch), let cur, let t = torchDevice(for: cur) else { return "" }
        if !t.isTorchAvailable {
            return L("今は使えません。iPhone が熱いときや、前面カメラを使っているときは、使えないことがあります。",
                     "Not available now. It may be unavailable when the iPhone is hot or the front camera is in use.")
        }
        guard t.isTorchActive else { return L("点いていません。", "The light is off.") }
        let now = Double(t.torchLevel)
        let want = min(max(UserDefaults.standard.double(forKey: Key.torchLevel), 0.05), 1)
        let pct = Int((now * 100).rounded())
        if now + 0.05 < want {
            return L("熱を下げるため、iOS が明るさを \(pct)% に下げています。", "iOS has lowered the brightness to \(pct)% to reduce heat.")
        }
        return L("点いています。明るさは \(pct)% です。", "The light is on at \(pct)%.")
    }
    static func size(forHeight h: Int) -> (Int, Int) {
        switch h {
        case 720: return (1280, 720)
        case 2160: return (3840, 2160)
        default: return (1920, 1080)
        }
    }

    // camgpu1015：PC が使える箱（maxW × maxH）に収まるか。縦長でも横長でもよいので、長い辺と短い辺で比べる
    static func fitsBox(_ w: Int, _ h: Int, maxW: Int, maxH: Int) -> Bool {
        max(w, h) <= max(maxW, maxH) && min(w, h) <= min(maxW, maxH)
    }
    // 箱に収まる、いちばん大きい標準の大きさ（16:9 の 720・1080・2160）。頼まれた高さより大きくはしない。
    // 標準の大きさが一つも入らない小さい箱のときだけ、縦横比を保って縮める（偶数にそろえる）
    static func fitSize(w: Int, h: Int, maxW: Int, maxH: Int) -> (Int, Int) {
        if fitsBox(w, h, maxW: maxW, maxH: maxH) { return (w, h) }
        for hh in heightChoices.reversed() {
            let (sw, sh) = size(forHeight: hh)
            if sh <= h && fitsBox(sw, sh, maxW: maxW, maxH: maxH) { return (sw, sh) }
        }
        let s = min(Double(max(maxW, maxH)) / Double(max(w, h)), Double(min(maxW, maxH)) / Double(min(w, h)))
        return (max(64, Int(Double(w) * s) & ~1), max(64, Int(Double(h) * s) & ~1))
    }

    // 画面に出す選び肢。cam60：4K の 60 fps も出す（前は 30 まで。Windows の H.264 のデコーダの資料は level 5.1 までだが、GPU（NVDEC）でも CPU でも、4K60 は PC で実際にほどけた。PC の負荷は大きい）
    static let heightChoices = [720, 1080, 2160]
    static let fpsChoices = [30, 60]
    static func availableCameras() -> [UInt32] { [UInt32(0), 4, 1, 2, 3].filter { device($0) != nil } }
    static func heights(for cam: UInt32) -> [Int] { heightChoices.filter { !rates(for: cam, height: $0).isEmpty } }
    static func rates(for cam: UInt32, height: Int) -> [Int] {
        fpsChoices.filter { f in supports(camera: cam, height: height, fps: f) }
    }
    // そのカメラが、その大きさ（16:9）とコマ数を NV12（420v）で撮れるか
    static func supports(camera: UInt32, height: Int, fps: Int) -> Bool {
        guard let dev = device(camera) else { return false }
        let (w, h) = size(forHeight: height)
        let want = Double(fps)
        return dev.formats.contains { f in
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return CMFormatDescriptionGetMediaSubType(f.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
                && Int(d.width) == w && Int(d.height) == h
                && f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= want && want <= $0.maxFrameRate }
        }
    }

    // 撮る形を選ぶ。返すのは形・1 コマの長さ・実際のコマ数。
    //   1. 大きさがぴったりで、そのコマ数を撮れる形（420v）。同じ大きさが何種類かあれば、いちばん広く写る物（videoFieldOfView が大きい）
    //   2. cam60：420v に無ければ、420f（フルレンジ）の同じ大きさの形。出力は 420v に指定してあるので、iOS が変換する（full が true のときだけ）
    //   3. cam60：同じ大きさの形はあるが、そのコマ数は撮れない：大きさを変えずに、頼みにいちばん近いコマ数（たいていは撮れる最高）で。
    //      前は、ここで大きさの違う別の形（そのコマ数を撮れて、面積がいちばん近い形）に替わっていた。PC の頼みの大きさは守り、コマ数を下げて ACK に正しく書く
    //   4. 同じ大きさの形がまったく無い：そのコマ数を撮れる中で、面積がいちばん近い形
    //   5. それも無い：面積がいちばん近い形を、頼みにいちばん近いコマ数で
    // activeVideoMinFrameDuration に範囲の外の値を入れると例外で落ちるので、1 コマの長さは必ず範囲の中の値にする
    // camctl：only は使える形を絞る（シネマティック）。prefer は同じ大きさの中で先に選ぶ形の点数（エフェクトに対応する形）
    private static func pickFormat(_ dev: AVCaptureDevice, w: Int, h: Int, fps: Int,
                                   only: ((AVCaptureDevice.Format) -> Bool)? = nil, prefer: ((AVCaptureDevice.Format) -> Int)? = nil,
                                   full: Bool = false) -> (AVCaptureDevice.Format, CMTime, Int)? {
        let nv = dev.formats.filter { CMFormatDescriptionGetMediaSubType($0.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && (only?($0) ?? true) }
        let nf = full ? dev.formats.filter { CMFormatDescriptionGetMediaSubType($0.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange && (only?($0) ?? true) } : []
        let score = prefer ?? { _ in 0 }
        let want = Double(fps)
        func area(_ f: AVCaptureDevice.Format) -> Int {
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return Int(d.width) * Int(d.height)
        }
        func exact(_ f: AVCaptureDevice.Format) -> Bool {
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            return Int(d.width) == w && Int(d.height) == h
        }
        func canDo(_ f: AVCaptureDevice.Format) -> Bool { fps > 0 && f.videoSupportedFrameRateRanges.contains { $0.minFrameRate <= want && want <= $0.maxFrameRate } }
        func widest(_ list: [AVCaptureDevice.Format]) -> AVCaptureDevice.Format? {
            list.max { a, b in
                let sa = score(a), sb = score(b)
                return sa != sb ? sa < sb : a.videoFieldOfView < b.videoFieldOfView
            }
        }
        func nearest(_ list: [AVCaptureDevice.Format]) -> AVCaptureDevice.Format? { list.min { abs(area($0) - w * h) < abs(area($1) - w * h) } }
        // 頼みにいちばん近いコマ数で撮れる形。頼みが範囲の外なら、範囲の端（上の外なら最高・下の外なら最低）。同じ近さなら、点数・広さの順
        func closest(_ list: [AVCaptureDevice.Format]) -> (AVCaptureDevice.Format, CMTime, Int)? {
            var best: (f: AVCaptureDevice.Format, dur: CMTime, fps: Int, diff: Double)?
            for f in list {
                for rg in f.videoSupportedFrameRateRanges {
                    let rate = min(max(want, rg.minFrameRate), rg.maxFrameRate)
                    let diff = abs(rate - want)
                    var better = best == nil
                    if let b = best {
                        if diff < b.diff - 0.001 {
                            better = true
                        } else if abs(diff - b.diff) <= 0.001 {
                            let sa = score(f), sb = score(b.f)
                            better = sa != sb ? sa > sb : f.videoFieldOfView > b.f.videoFieldOfView
                        }
                    }
                    guard better else { continue }
                    let n = max(1, Int((rate + 0.05).rounded(.down)))
                    let dur: CMTime
                    if rate >= rg.maxFrameRate { dur = rg.minFrameDuration }
                    else if rate <= rg.minFrameRate { dur = rg.maxFrameDuration }
                    else { dur = CMTime(value: 1, timescale: CMTimeScale(n)) }
                    best = (f, dur, n, diff)
                }
            }
            guard let b = best else { return nil }
            return (b.f, b.dur, b.fps)
        }
        if let f = widest(nv.filter { exact($0) && canDo($0) }) ?? widest(nf.filter { exact($0) && canDo($0) }) {
            return (f, CMTime(value: 1, timescale: CMTimeScale(fps)), fps)
        }
        let sameSize = nv.filter(exact) + nf.filter(exact)
        if !sameSize.isEmpty { return closest(sameSize) }
        if let f = nearest(nv.filter(canDo)) {
            return (f, CMTime(value: 1, timescale: CMTimeScale(fps)), fps)
        }
        guard let f = nearest(nv) else { return nil }
        return closest([f])
    }

    // cam60：その大きさの形の一覧（記録用）。例：「420v 1-30 / 420v 1-60 / 420f 1-30」。コマ数が下がったときに、本物で何が撮れるかを知るため
    private static func formatList(_ dev: AVCaptureDevice, w: Int, h: Int) -> String {
        func code(_ t: FourCharCode) -> String {
            let b: [UInt8] = [UInt8((t >> 24) & 255), UInt8((t >> 16) & 255), UInt8((t >> 8) & 255), UInt8(t & 255)]
            return String(bytes: b, encoding: .ascii) ?? "?"
        }
        var items: [String] = []
        for f in dev.formats {
            let d = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
            guard Int(d.width) == w && Int(d.height) == h else { continue }
            let rs = f.videoSupportedFrameRateRanges.map { String(format: "%.0f-%.0f", $0.minFrameRate, $0.maxFrameRate) }.joined(separator: ",")
            items.append("\(code(CMFormatDescriptionGetMediaSubType(f.formatDescription))) \(rs)")
        }
        return items.isEmpty ? "なし" : items.joined(separator: " / ")
    }

    // 画質の段とビットレート（kbps）。表は [段][720p・1080p・4K][30・60 fps]
    //   最高は USB（毎秒 50 MB 以上）の太さを生かす値。1080p60 で 36 Mbps（H.264 の level 4.2 の上限は High で 62.5 Mbps）、
    //   4K30 で 50 Mbps（level 5.1 の上限は 300 Mbps）。PC のほどく側（Windows の H.264 デコーダ）は level 5.1 までと資料にある。
    //   cam60：高画質の段（q=2。初めの値）の 1080p60 を 18 から 20 Mbps に（目安は 1080p60 で 20 Mbps 前後・4K60 で 40〜50 Mbps 前後。4K60 は 45 Mbps のまま）。
    //   PC の work/camset/pc/fake_iphone.c の quality_kbps は 1080p60 の高画質だけ 18 のまま（偽の iPhone なので、本物とずれても動作に関係しない）
    static func qualityKbps(_ q: Int, w: Int, h: Int, fps: Int) -> Int {
        let table: [[[Int]]] = [[[4000, 6000], [8000, 12000], [20000, 30000]],
                                [[6000, 9000], [12000, 20000], [30000, 45000]],
                                [[12000, 18000], [24000, 36000], [50000, 60000]]]
        let tier = max(1, min(3, q)) - 1
        let size = w * h >= 3840 * 2160 ? 2 : w * h >= 1920 * 1080 ? 1 : 0
        return table[tier][size][fps >= 50 ? 1 : 0]
    }

    // PC が指定した項目を画面に出す文。画面の設定を優先しているときもそう書く
    private static func pcNoteText(_ r: Resolved, w: Int, h: Int, fps: Int) -> String {
        if r.ackFlags & Flag.ackAppPriority != 0 { return L("PC からの指定ではなく、この画面の設定を使っています。", "Using the settings on this screen, not the ones chosen on the PC.") }
        var items: [String] = []
        if r.pcSize { items.append("\(r.w)×\(r.h)") }
        if r.pcFps { items.append("\(r.fps) fps") }
        if r.pcCam { items.append(cameraName(r.camera)) }
        if items.isEmpty { return "" }
        let s = items.joined(separator: L("・", ", "))
        return L("PC が \(s) を指定しています。この画面の設定より優先されます。", "The PC asked for \(s). This is used instead of the settings on this screen.")
    }

    // ---- エンコーダ（VideoToolbox・server.c の vid_open と同じ設定） ----

    // cam60：lowLatency を false にすると、低遅延の方式を使わずに作る（60 コマ/秒で低遅延の方式が使えないときの、作り直し用）
    private func makeEncoder(w: Int, h: Int, fps: Int, kbps: Int, gop: Int, lowLatency lowLatencyWanted: Bool = true) -> VTCompressionSession? {
        var s: VTCompressionSession?
        let lowLatency = lowLatencyWanted && Self.debugBits & 1 == 0 // camrot1015：隠しスイッチ。低遅延のレート制御を使わない
        let spec: CFDictionary? = lowLatency ? ([kVTVideoEncoderSpecification_EnableLowLatencyRateControl as String: true] as CFDictionary) : nil
        var llUsed = lowLatency // camrot1015：実際に低遅延で作れたか（記録用）
        // cam4k1016：隠しスイッチ 64。エンコーダに入力の形（NV12・大きさ・IOSurface）を伝えて作る。ふだんは伝えない（nil）。伝えると、エンコーダが入力を受ける入れ物を
        // その形に合わせて用意するので、形の違いによる中の変換やコピーが無くなる見込み（本物では未確認）。カメラの絵も Metal・Core Image の写し先も同じ形
        let srcAttrs: CFDictionary? = Self.debugBits & 64 != 0 ? ([
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey as String: w,
            kCVPixelBufferHeightKey as String: h,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
        ] as [String: Any] as CFDictionary) : nil
        var st = VTCompressionSessionCreate(allocator: nil, width: Int32(w), height: Int32(h), codecType: kCMVideoCodecType_H264, encoderSpecification: spec,
                                            imageBufferAttributes: srcAttrs, compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        if st != noErr || s == nil {
            // 低遅延のモードを持つエンコーダが無いとき（古い iPhone の軽いアプリ PocketCam のため）は、ふつうのエンコーダで作り直す。
            // 低遅延で作れる機種ではここを通らないので、動きは前と同じ
            log("ウェブカメラ：低遅延のエンコーダを作れない（\(st)）。ふつうのエンコーダで作る")
            llUsed = false
            s = nil
            st = VTCompressionSessionCreate(allocator: nil, width: Int32(w), height: Int32(h), codecType: kCMVideoCodecType_H264, encoderSpecification: nil,
                                            imageBufferAttributes: srcAttrs, compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &s)
        }
        guard st == noErr, let s else { log("ウェブカメラ：エンコーダを作れない（\(st)）"); return nil }
        func set(_ k: CFString, _ v: CFTypeRef) { _ = VTSessionSetProperty(s, key: k, value: v) }
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse) // B フレームなし
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue)
        if VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_ConstrainedHigh_AutoLevel) != noErr {
            set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel)
        }
        set(kVTCompressionPropertyKey_AverageBitRate, NSNumber(value: kbps * 1000))
        setPeak(s, kbps)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, NSNumber(value: fps))
        // キーフレームの間隔：コマ数（gop）と秒の両方で縛る。60 コマ/秒でも gop が 2 秒分（fps × 2）なら同じ 2 秒。
        // カメラが遅れたコマを捨てると、コマ数だけでは間隔が秒で伸びるので、秒でも縛る（PC が途中から受け始めても、2 秒以内にほどける）
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, NSNumber(value: gop))
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, NSNumber(value: Double(gop) / Double(max(fps, 1))))
        let ps = VTCompressionSessionPrepareToEncodeFrames(s)
        // 31 コマ/秒以上は、準備できなければ失敗にして、呼ぶ側が別の方式か 30 コマ/秒で作り直す（30 以下は前のまま、結果を見ない）
        if ps != noErr && fps > 30 {
            log("ウェブカメラ：\(w)×\(h)・\(fps) コマ/秒のエンコーダを準備できない（\(ps)・低遅延 \(lowLatency ? 1 : 0)）")
            VTCompressionSessionInvalidate(s)
            return nil
        }
        // camrot1015：エンコーダが実際に持っている設定を読み直して記録に出す（ハードで動いているか・出力を詰まらせる上限が付いていないか）
        log("ウェブカメラ：エンコーダ \(w)×\(h)・\(fps) コマ/秒・\(kbps) kbps・準備 \(ps)・低遅延 \(llUsed ? 1 : 0)・ハード \(Self.propText(s, Self.hwKey))・RealTime \(Self.propText(s, kVTCompressionPropertyKey_RealTime))・MaxFrameDelayCount \(Self.propText(s, kVTCompressionPropertyKey_MaxFrameDelayCount))・速さ優先 \(Self.propText(s, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality))・並べ替え \(Self.propText(s, kVTCompressionPropertyKey_AllowFrameReordering))・想定コマ数 \(Self.propText(s, kVTCompressionPropertyKey_ExpectedFrameRate))・入力の形 \(srcAttrs != nil ? 1 : 0)・隠しスイッチ \(Self.debugBits)")
        return s
    }

    // camrot1015：エンコーダの設定を読んで文字にする（読めなければ「不明」とエラー番号）
    private static let hwKey: CFString = "UsingHardwareAcceleratedVideoEncoder" as CFString // kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder の値（SDK の版で名前が無いことがあるので文字で持つ）
    private static func propText(_ s: VTCompressionSession, _ key: CFString) -> String {
        var v: CFTypeRef?
        let st = VTSessionCopyProperty(s, key: key, allocator: kCFAllocatorDefault, valueOut: &v)
        guard st == noErr, let v else { return "不明(\(st))" }
        return "\(v)"
    }

    // 1 秒あたりの上限を平均の 1.5 倍に（動きの多い場面で USB が詰まらないように）。エンコーダが受けなければ、そのまま（返り値は見ない）
    private func setPeak(_ s: VTCompressionSession, _ kbps: Int) {
        if Self.debugBits & 2 != 0 { return } // camrot1015：隠しスイッチ。DataRateLimits を付けない（付けると 4K60 で詰まるかを本物で比べる）
        let bytesPerSec = kbps * 1000 / 8 * 3 / 2
        _ = VTSessionSetProperty(s, key: kVTCompressionPropertyKey_DataRateLimits, value: [NSNumber(value: bytesPerSec), NSNumber(value: 1)] as CFArray)
    }

    private func setBitrate(_ kbps: Int) {
        guard kbps >= 200, kbps <= 100_000, let s = venc else { return }
        _ = VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: kbps * 1000))
        setPeak(s, kbps)
        curKbps = kbps
        log("ウェブカメラ：ビットレートを \(kbps) kbps に変えた")
        let w = curW, h = curH, f = curFps
        let name = Self.cameraName(curReq?.camera ?? 0)
        DispatchQueue.main.async { [weak self] in
            if self?.connected == true { self?.detail = "\(w)×\(h)・\(f) fps・\(name)・\(String(format: "%.0f", Double(kbps) / 1000)) Mbps" }
        }
    }

    // ---- camctl：作り直さずに、今のカメラへ当てる（q の上） ----

    // ピント・明るさ・色・ズーム・ライト・切り抜き・水平を、lockForConfiguration でその場で変える。
    // 作り直したあと（initial：ズームを ramp せずに一度で）と、画面で操作を変えたときに呼ぶ
    private func liveApply(initial: Bool = false) {
        guard let input = deviceInput, let r = curReq else { return }
        let dev = input.device
        let d = UserDefaults.standard
        var st = stageParams(r)
        lock.lock()
        st.on = st.on && poolCopy != nil // 入れ物を作れなかったときは切り抜かない（倍率も移さない）
        stage = st
        lock.unlock()
        if st.on && st.level { startMotion() } else { stopMotion() }
        let cine = isCinematicOn(input)
        do {
            try dev.lockForConfiguration()
            // シネマティックの間はピントの形を変えられない（変えると例外で落ちる）
            if !cine { applyFocus(dev, d) }
            Self.applyExposure(dev, d, fps: curFps)
            Self.applyWhiteBalance(dev, d)
            applyZoom(dev, d, r, extra: st.on ? st.extra : 1, initial: initial)
            dev.unlockForConfiguration()
        } catch {
            log("ウェブカメラ：カメラの設定を変えられない：\(error)")
        }
        if cine { setAperture(input) }
        applyTorch(dev)
        // センターフレームは、画面で触ったことがあるときだけアプリが決める（触っていなければコントロールセンターのまま）
        if let cs = d.object(forKey: Key.centerStage) as? Bool, dev.formats.contains(where: { $0.isCenterStageSupported }) {
            Self.applyCenterStage(cs)
        }
    }

    private func isCinematicOn(_ input: AVCaptureDeviceInput) -> Bool {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) { return input.isCinematicVideoCaptureEnabled }
        #endif
        return false
    }

    // ピント：0 自動（連続）・1 固定（今の位置）・2 手動（レンズの位置）。合わせる所は 0 顔を優先・1 画面の真ん中
    private func applyFocus(_ dev: AVCaptureDevice, _ d: UserDefaults) {
        let mode = d.integer(forKey: Key.focus)
        let target = d.integer(forKey: Key.focusTarget) == 1 ? 1 : 0
        let canAF = dev.isFocusModeSupported(.continuousAutoFocus)
        if #available(iOS 15.4, *), canAF {
            if target == 1 {
                // 顔ではなく真ん中に合わせる：顔に合わせる働きを切る（先に automaticallyAdjusts を切らないと例外で落ちる）
                dev.automaticallyAdjustsFaceDrivenAutoFocusEnabled = false
                dev.isFaceDrivenAutoFocusEnabled = false
            } else if !dev.automaticallyAdjustsFaceDrivenAutoFocusEnabled {
                dev.automaticallyAdjustsFaceDrivenAutoFocusEnabled = true // iOS の初めの形（顔を優先）に戻す
            }
        }
        if mode == 2 && dev.isLockingFocusWithCustomLensPositionSupported {
            let p = Float(min(max(d.double(forKey: Key.focusPos), 0), 1))
            if dev.focusMode != .locked || abs(dev.lensPosition - p) > 0.002 { dev.setFocusModeLocked(lensPosition: p, completionHandler: nil) }
        } else if mode == 1 && dev.isFocusModeSupported(.locked) {
            if dev.focusMode != .locked { dev.focusMode = .locked }
        } else if canAF {
            // 合わせる所を変えたときと、自動に戻したときだけ合わせ直す（ズームを変えるたびに合わせ直さない）
            if dev.focusMode != .continuousAutoFocus || focusTargetApplied != target {
                if dev.isFocusPointOfInterestSupported { dev.focusPointOfInterest = CGPoint(x: 0.5, y: 0.5) }
                dev.focusMode = .continuousAutoFocus
            }
        }
        focusTargetApplied = target
    }

    // 明るさ：0 自動・1 固定（今の明るさ）・2 手動（ISO とシャッター速度）。露出補正は自動と固定で効く
    private static func applyExposure(_ dev: AVCaptureDevice, _ d: UserDefaults, fps: Int) {
        let mode = d.integer(forKey: Key.exposure)
        let f = dev.activeFormat
        if mode == 2 && dev.isExposureModeSupported(.custom) {
            // シャッター速度は 1 コマの長さより短くする（長いとコマ数が落ちる）。範囲の外は例外で落ちるので収める
            var den = d.double(forKey: Key.shutter)
            if den <= 0 { den = Double(max(fps, 30) * 2) }
            let frame = CMTime(value: 1, timescale: CMTimeScale(max(1, fps)))
            let hiDur = CMTimeMinimum(f.maxExposureDuration, frame)
            var dur = CMTimeMakeWithSeconds(1 / den, preferredTimescale: 1_000_000)
            dur = CMTimeMaximum(f.minExposureDuration, CMTimeMinimum(dur, hiDur))
            var iso = Float(d.double(forKey: Key.iso))
            if iso <= 0 { iso = dev.iso } // 初めは今の値から
            iso = min(max(iso, f.minISO), f.maxISO)
            dev.setExposureModeCustom(duration: dur, iso: iso, completionHandler: nil)
            return
        }
        if mode == 1 && dev.isExposureModeSupported(.locked) {
            if dev.exposureMode != .locked { dev.exposureMode = .locked }
        } else if dev.isExposureModeSupported(.continuousAutoExposure) {
            if dev.exposureMode != .continuousAutoExposure { dev.exposureMode = .continuousAutoExposure }
        }
        let b = min(max(Float(d.double(forKey: Key.bias)), dev.minExposureTargetBias), dev.maxExposureTargetBias)
        if abs(dev.exposureTargetBias - b) > 0.01 { dev.setExposureTargetBias(b, completionHandler: nil) }
    }

    // 色：0 自動・1 固定（今の色）・2 色温度。色温度は iOS 8 からの変換で RGB の倍率にして、1〜上限に収めてから固定する
    private static func applyWhiteBalance(_ dev: AVCaptureDevice, _ d: UserDefaults) {
        let mode = d.integer(forKey: Key.wb)
        if mode == 2 && dev.isLockingWhiteBalanceWithCustomDeviceGainsSupported {
            let t = Float(min(max(d.double(forKey: Key.wbTemp), 2000), 10000))
            var g = dev.deviceWhiteBalanceGains(for: AVCaptureDevice.WhiteBalanceTemperatureAndTintValues(temperature: t, tint: 0))
            let mx = dev.maxWhiteBalanceGain
            g.redGain = min(max(g.redGain, 1), mx)
            g.greenGain = min(max(g.greenGain, 1), mx)
            g.blueGain = min(max(g.blueGain, 1), mx)
            dev.setWhiteBalanceModeLocked(with: g, completionHandler: nil)
        } else if mode == 1 && dev.isWhiteBalanceModeSupported(.locked) {
            if dev.whiteBalanceMode != .locked { dev.whiteBalanceMode = .locked }
        } else if dev.isWhiteBalanceModeSupported(.continuousAutoWhiteBalance) {
            if dev.whiteBalanceMode != .continuousAutoWhiteBalance { dev.whiteBalanceMode = .continuousAutoWhiteBalance }
        }
    }

    // ズーム：見え方の倍率をカメラの倍率に直す。仮想カメラなら、倍率に合わせて iOS がレンズを切り替える。
    // extra は水平の余白のぶん。カメラの倍率を下げて、エンコーダの前で拡大する（レンズが替わる倍率より下には下げない）
    private func applyZoom(_ dev: AVCaptureDevice, _ d: UserDefaults, _ r: Resolved, extra: CGFloat, initial: Bool) {
        let m = Self.zoomMultiplier(dev)
        // PC がカメラを決めて、画面のカメラと違うときは、そのレンズの 1 枚目の見え方（画面の倍率は別のレンズの物なので使わない）
        let stored = ctlZoomStored(r)
        var want = stored ? CGFloat(d.double(forKey: Key.zoom)) / m : 1
        // camctl2：そのときは、PC が CTRL で選んだ倍率を使う（このつなぎの間だけ。画面の倍率は別のレンズの物なので書かない）
        if !stored, let z = ctlZoom { want = z / m }
        if want <= 0 { want = 1 / m }
        // camrot1015：中央の切り抜きは、カメラの倍率に切り抜きの倍率を掛けて済ませる（Core Image で毎コマ切り出さない）。
        // カメラが出せる倍率の上限（下の hi）で止まるので、そのときは切り抜きが浅くなる
        if r.camCrop { want *= 1 / CGFloat(min(max(d.double(forKey: Key.cropSize), 0.3), 1)) }
        if extra > 1.001 { want = max(Self.lensFloor(dev, for: want), want / extra) }
        let lo = max(1, dev.minAvailableVideoZoomFactor)
        let hi = max(lo, min(dev.maxAvailableVideoZoomFactor, dev.activeFormat.videoMaxZoomFactor))
        let z = min(max(want, lo), hi)
        if abs(dev.videoZoomFactor - z) < 0.001 { return }
        if initial { dev.videoZoomFactor = z } else { dev.ramp(toVideoZoomFactor: z, withRate: 8) }
    }

    // ライト。前面カメラのときは背面のライトを点ける（iOS が許すときだけ）。熱いときは、今出せるいちばん明るい段にする
    private func applyTorch(_ cur: AVCaptureDevice) {
        let d = UserDefaults.standard
        guard let t = Self.torchDevice(for: cur) else { return }
        do { try t.lockForConfiguration() } catch { return }
        defer { t.unlockForConfiguration() }
        if d.bool(forKey: Key.torch) && t.isTorchAvailable && t.isTorchModeSupported(.on) {
            let lv = Float(min(max(d.double(forKey: Key.torchLevel), 0.05), 1))
            do {
                try t.setTorchModeOn(level: lv)
            } catch {
                try? t.setTorchModeOn(level: AVCaptureDevice.maxAvailableTorchLevel)
            }
            torchDev = t
        } else if t.torchMode != .off && t.isTorchModeSupported(.off) {
            t.torchMode = .off
        }
    }

    private func torchOff() {
        guard let t = torchDev else { return }
        torchDev = nil
        do { try t.lockForConfiguration() } catch { return }
        if t.torchMode != .off && t.isTorchModeSupported(.off) { t.torchMode = .off }
        t.unlockForConfiguration()
    }

    // 切り抜きと水平の値を作る
    private func stageParams(_ r: Resolved) -> StageParams {
        var p = StageParams()
        guard r.stage, let input = deviceInput else { return p }
        let d = UserDefaults.standard
        let cropOn = d.bool(forKey: Key.crop) && !r.camCrop // camrot1015：camCrop の切り抜きはカメラの倍率で済んでいる。ここでは切らない
        let c: CGFloat = cropOn ? 1 / CGFloat(min(max(d.double(forKey: Key.cropSize), 0.3), 1)) : 1
        p.on = true
        p.outW = curW; p.outH = curH
        p.level = d.bool(forKey: Key.level) && Self.levelAvailable(input.device)
        p.scale = max(c, p.level ? Self.levelScale(w: curW, h: curH) : 1)
        p.extra = p.scale / c
        p.pos = cropOn ? max(0, min(4, d.integer(forKey: Key.cropPos))) : 0
        p.cx = CGFloat(min(max(d.double(forKey: Key.cropX), 0), 1))
        p.cy = CGFloat(min(max(d.double(forKey: Key.cropY), 0), 1))
        p.levelMax = Self.levelMaxDeg * .pi / 180
        // 水平のときの傾き。背面のカメラで回す角度 0 は、上を左にした横向き（傾き -90 度）。回す角度ぶん進む
        p.refAngle = CGFloat(r.rot) * .pi / 180 - .pi / 2
        p.sign = r.connMirror ? -1 : 1
        return p
    }

    private func startMotion() {
        let m = Self.motion
        guard m.isDeviceMotionAvailable, !m.isDeviceMotionActive else { return }
        m.deviceMotionUpdateInterval = 1.0 / 60
        m.startDeviceMotionUpdates() // 読むのはカメラの糸から deviceMotion を見るだけ（受け取りの関数は使わない）
    }

    private func stopMotion() {
        if Self.motion.isDeviceMotionActive { Self.motion.stopDeviceMotionUpdates() }
    }

    // 切り抜いた絵の入れ物（送る大きさの NV12。エンコーダと Core Image の両方が使える IOSurface）
    private static func makePool(w: Int, h: Int) -> CVPixelBufferPool? {
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey as String: w,
            kCVPixelBufferHeightKey as String: h,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
            kCVPixelBufferMetalCompatibilityKey as String: true,
        ]
        var pool: CVPixelBufferPool?
        guard CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool) == kCVReturnSuccess else { return nil }
        return pool
    }

    // カメラとエンコーダを止める（つなぎ自体は切らない）
    private func stopCapture() {
        lock.lock()
        sendConn = nil; encCopy = nil; sessionGen &+= 1
        stage = StageParams(); poolCopy = nil; devCopy = nil; videoOutCopy = nil; faceTarget = nil; cineCopy = false; levelShown = 0; flipCopy = nil
        lock.unlock()
        // cam4k1016：列を空けて（持っているカメラの入れ物を手放す）、回す糸とエンコードの糸がいま扱っている 1 コマを終えるのを待つ。
        // これから閉じるエンコーダを、その糸が呼んでいる最中にしないため。上で sendConn を nil にして世代を進めてあるので、
        // 待つ間に新しく列に入る物は無く（入っても世代が違い、prepareFrame・submitFrame が捨てる）、糸は列を空けたら戻る。
        // 糸の中から q を待つ所は無いので、q の上から sync で待っても詰まらない。回す糸が先（そこから encBox へ入るため）
        // 作り直しで手放した分は「待ちで捨てたコマ」に数えない（記録の窓をゆがめない）
        prepBox.clear()
        prepQueue.sync {}
        encBox.clear()
        encodeQueue.sync {}
        curReq = nil
        baseFps = 0
        stopMotion()
        torchOff()
        for o in sessionObservers { NotificationCenter.default.removeObserver(o) }
        sessionObservers.removeAll()
        if let s = session {
            session = nil
            deviceInput = nil
            captureQueue.async { [weak self] in // 走っている最中のコマの扱いが終わってから止める
                s.stopRunning()
                for i in s.inputs { s.removeInput(i) }
                for o in s.outputs { s.removeOutput(o) }
                // cam4k1016：これらは回す糸が触る物なので、その糸で戻す（stopRunning が戻ったあとなので、新しいコマはもう来ない）
                self?.prepQueue.async {
                    self?.levelNow = 0
                    self?.faceNow = CGPoint(x: 0.5, y: 0.5)
                    self?.lastStagePts = -1
                }
            }
        }
        if let e = venc {
            venc = nil
            VTCompressionSessionCompleteFrames(e, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(e)
        }
    }

    // カメラが止まった・戻った（アプリが後ろに回った・電話・他のアプリが使った）を PC に知らせる
    private var sessionObservers: [NSObjectProtocol] = []
    private func observeSession(_ s: AVCaptureSession) {
        let nc = NotificationCenter.default
        sessionObservers.append(nc.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: s, queue: nil) { [weak self] n in
            let why = (n.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int) ?? 0
            self?.q.async { self?.log("ウェブカメラ：カメラが止まった（理由 \(why)）"); self?.sendEvent(1, UInt32(why)) }
        })
        sessionObservers.append(nc.addObserver(forName: AVCaptureSession.interruptionEndedNotification, object: s, queue: nil) { [weak self] _ in
            self?.q.async {
                self?.log("ウェブカメラ：カメラが戻った")
                self?.lock.lock(); self?.forceKey = true; self?.lock.unlock() // 戻ったあとの最初のコマはキーフレームにする
                self?.sendEvent(2, 0)
            }
        })
        sessionObservers.append(nc.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: s, queue: nil) { [weak self, weak s] n in
            let e = n.userInfo?[AVCaptureSessionErrorKey] as? NSError
            self?.q.async {
                self?.log("ウェブカメラ：カメラの実行中の失敗 \(String(describing: e))")
                // メディアサービスが落ちたときなどは、もう一度走らせれば戻る
                if e?.code == AVError.mediaServicesWereReset.rawValue, let s { self?.captureQueue.async { s.startRunning() } }
            }
        })
    }

    // ---- カメラの糸：1 コマ来るたびに、列へ入れて、回す糸・エンコードの糸へ渡す ----
    // cam4k1016：ここ（受け口）が 16.7 ms を超えると iOS が遅れたコマを捨てる（alwaysDiscardsLateVideoFrames）ので、軽い記帳と列への出し入れだけにする。
    // 回す処理は prepQueue の prepareFrame、エンコーダの呼び出し（4K60 で 16〜19 ms かかる）は encodeQueue の submitFrame。
    // コマの入れ物（CVPixelBuffer）を持つ間、カメラの入れ物は戻らない。持つのは、列の 2 枚＋回す糸の 1 枚＋エンコーダの中の分まで

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        let enterNs = DispatchTime.now().uptimeNanoseconds
        defer {
            let us = (DispatchTime.now().uptimeNanoseconds &- enterNs) / 1000
            lock.lock(); capBusyUs &+= us; lock.unlock()
        }
        lock.lock()
        capCalls &+= 1
        let live = sendConn != nil
        let gen = sessionGen
        let needPrep = stage.on || flipCopy != nil // 切り抜く・水平を保つ・Metal で回すコマは、prepQueue を通す
        lock.unlock()
        guard live, let src = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let f = CamFrame(pb: src, pts: CMSampleBufferGetPresentationTimeStamp(sampleBuffer), gen: gen, enteredNs: enterNs)
        // 隠しスイッチ 16：前の作り（受け口の中で回してエンコーダを呼ぶ）。本物で効き目を比べるため
        if Self.debugBits & 16 != 0 {
            if let g = prepareFrame(f) { submitFrame(g) }
            return
        }
        if needPrep {
            let r = prepBox.push(f, limit: waitLimit)
            noteWaitDrops(r.dropped)
            if r.wake { prepQueue.async { [weak self] in self?.drainPrep() } }
        } else {
            enqueueEncode(f)
        }
    }

    // 列から捨てたコマを数える
    private func noteWaitDrops(_ n: Int) {
        guard n > 0 else { return }
        lock.lock(); waitDrops &+= UInt64(n); lock.unlock()
    }

    // encodeQueue の列に入れ、糸が寝ていれば起こす
    private func enqueueEncode(_ f: CamFrame) {
        let r = encBox.push(f, limit: waitLimit)
        noteWaitDrops(r.dropped)
        if r.wake { encodeQueue.async { [weak self] in self?.drainEncode() } }
    }

    // prepQueue：列が空になるまで、古い順に 1 枚ずつ回して、エンコードの糸の列へ渡す
    private func drainPrep() {
        while let f = prepBox.pop() {
            if let g = prepareFrame(f) { enqueueEncode(g) }
        }
    }

    // encodeQueue：列が空になるまで、古い順に 1 枚ずつエンコーダに渡す
    private func drainEncode() {
        while let f = encBox.pop() { submitFrame(f) }
    }

    // 回す（prepQueue。隠しスイッチ 16 のときは captureQueue）。回さないコマ（設定が変わって、回す必要がなくなった）はそのまま返す。
    // 作り直す前のカメラのコマ（列に残っていた物）と、回せなかったコマは nil（捨てる）
    private func prepareFrame(_ f: CamFrame) -> CamFrame? {
        lock.lock()
        let live = sendConn != nil
        let same = f.gen == sessionGen
        let stg = stage
        let pool = poolCopy
        let flip = flipCopy
        lock.unlock()
        guard live, same else { return nil }
        guard stg.on || flip != nil else { return f }
        let t0 = DispatchTime.now().uptimeNanoseconds
        defer {
            let us = (DispatchTime.now().uptimeNanoseconds &- t0) / 1000
            lock.lock(); prepUs &+= us; prepCalls &+= 1; lock.unlock()
        }
        // camctl：切り抜く・水平を保つときは、エンコーダの前で切り出して送る大きさにする
        if stg.on {
            guard let pool, let cut = renderStage(f.pb, stg, pool, pts: f.pts) else { lock.lock(); forceKey = true; lock.unlock(); return nil }
            return CamFrame(pb: cut, pts: f.pts, gen: f.gen, enteredNs: f.enteredNs)
        }
        guard let flipper = flip else { return f }
        // camrot1015：180 度・反転を Metal で 1 回写して行う（接続で回すより軽い見込み）。回せないコマは送らない（回していない絵を送ると、向きが 1 コマだけ違って見える）
        let t1 = DispatchTime.now().uptimeNanoseconds
        let flipped = flipper.flip(f.pb)
        let fus = (DispatchTime.now().uptimeNanoseconds &- t1) / 1000
        lock.lock()
        flipUs &+= fus
        flipCalls &+= 1
        if flipped == nil { flipFails &+= 1; forceKey = true }
        let fails = flipFails
        lock.unlock()
        guard let out = flipped else {
            if fails == 1 || fails % 600 == 0 { log("ウェブカメラ：Metal で回せないコマがある（\(fails) 回目）：\(flipper.lastError)") }
            return nil
        }
        return CamFrame(pb: out, pts: f.pts, gen: f.gen, enteredNs: f.enteredNs)
    }

    // エンコーダに渡す（encodeQueue。隠しスイッチ 16 のときは captureQueue）。
    // 番号（seq）・キーフレームの印・USB の詰まり（inflight）は、ここ（エンコーダに渡す直前）で決める。列で捨てたコマには番号を使わない
    private func submitFrame(_ f: CamFrame) {
        let workT0 = DispatchTime.now().uptimeNanoseconds
        lock.lock()
        let live = sendConn != nil
        let same = f.gen == sessionGen // 作り直す前のカメラのコマは捨てる
        let busy = inflight >= maxInflight
        let go = live && same && !busy
        if busy && live && same { dropped += 1; forceKey = true } // 捨てたあとの次のコマは、参照が切れているので、キーフレームにする
        let key = forceKey && go
        if key { forceKey = false }
        let n = seq
        if go { seq &+= 1 }
        lock.unlock()
        guard go, let enc = encoderForCallback() else { return }
        let gen = f.gen
        let pts = f.pts
        let capUs = UInt64(max(0, CMTimeGetSeconds(pts)) * 1_000_000) // ホストの時計（単調）。PING の PONG と同じ時計
        let props = key ? ([kVTEncodeFrameOptionKey_ForceKeyFrame as String: true] as CFDictionary) : nil
        var info = VTEncodeInfoFlags()
        let encT0 = DispatchTime.now().uptimeNanoseconds
        let st = VTCompressionSessionEncodeFrame(enc, imageBuffer: f.pb, presentationTimeStamp: pts, duration: CMTime(value: 1, timescale: CMTimeScale(curFpsForCallback())), frameProperties: props, infoFlagsOut: &info) { [weak self] status, flags, sb in
            self?.encoded(status, flags, sb, capUs, n, gen)
        }
        let doneNs = DispatchTime.now().uptimeNanoseconds
        let encUs = (doneNs &- encT0) / 1000
        lock.lock()
        encCallUs &+= encUs; encCalls &+= 1 // camrot1015：呼び出しが返るまでの時間。エンコーダが詰まると、ここが伸びる
        workUs &+= (doneNs &- workT0) / 1000
        workCalls &+= 1
        waitUs &+= (workT0 &- f.enteredNs) / 1000 // 受け口に来てからエンコーダに渡す直前まで（列で待った時間と回した時間を含む）
        if st != noErr { forceKey = true }
        lock.unlock()
    }

    // camrot1015：iOS が捨てたコマの数と理由（alwaysDiscardsLateVideoFrames が true なので、captureOutput が遅いと「遅れ」で捨てられる）
    func captureOutput(_ output: AVCaptureOutput, didDrop sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        var why = ""
        if let v = CMGetAttachment(sampleBuffer, key: kCMSampleBufferAttachmentKey_DroppedFrameReason, attachmentModeOut: nil) { why = "\(v)" }
        lock.lock()
        if why.contains("OutOfBuffers") { bufDrops &+= 1 } else if why.contains("FrameWasLate") { lateDrops &+= 1 } else { otherDrops &+= 1 }
        lock.unlock()
    }

    // ---- camctl：カメラの糸。切り抜き・拡大・水平の回転を Core Image で 1 回で ----
    // 枠は「送る大きさと同じ縦横比で元の絵に入るいちばん大きい枠」を scale で縮めた物。水平を保つときは、iPhone の傾きと同じだけ枠を回し、
    // 回した枠の中を真っすぐにして送る。傾きはなめらかにして（0.5 秒でなじむ・0.3 度より小さい揺れは見ない）、細かく揺れないようにする。
    // 重さの見込み：GPU で 1080p を 1 枚描くのに数ミリ秒。CPU は命令を作るだけ。遅れはその描く時間だけ増える（本物では測っていない）
    private func renderStage(_ src: CVPixelBuffer, _ st: StageParams, _ pool: CVPixelBufferPool, pts: CMTime) -> CVPixelBuffer? {
        let sw = CGFloat(CVPixelBufferGetWidth(src)), sh = CGFloat(CVPixelBufferGetHeight(src))
        let ow = CGFloat(st.outW), oh = CGFloat(st.outH)
        guard sw > 0, sh > 0, ow > 0, oh > 0, st.scale > 0 else { return nil }
        let t = CMTimeGetSeconds(pts)
        let dt = lastStagePts < 0 ? 0 : min(max(t - lastStagePts, 0), 0.1)
        lastStagePts = t
        // 傾き：重さの向き（gravity）から iPhone の傾きを出し、水平のときの傾きとの差を直す
        var target: CGFloat = 0
        if st.level, let g = Self.motion.deviceMotion?.gravity, hypot(g.x, g.y) > 0.5 { // 平らに置いているときは直さない
            var e = CGFloat(atan2(g.x, -g.y)) - st.refAngle
            while e > .pi { e -= 2 * .pi }
            while e < -.pi { e += 2 * .pi }
            // 大きく傾いている（別の向きに置いた）ときは直さない。直すのは上限まで
            if abs(e) < .pi / 4 { target = min(max(e, -st.levelMax), st.levelMax) * st.sign }
        }
        levelNow = Self.smooth(levelNow, target, dt: dt, tau: 0.5, dead: 0.3 * .pi / 180)
        // 顔に合わせるときは、顔の真ん中をなめらかに追う（0.4 秒でなじむ・枠の 2% より小さい動きは見ない）
        var center = CGPoint(x: st.cx, y: st.cy)
        if st.pos == 3 {
            lock.lock(); let f = faceTarget; lock.unlock()
            if let f {
                faceNow.x = Self.smooth(faceNow.x, f.x, dt: dt, tau: 0.4, dead: 0.02)
                faceNow.y = Self.smooth(faceNow.y, f.y, dt: dt, tau: 0.4, dead: 0.02)
            }
            center = faceNow
        }
        var bw = sw, bh = sw * oh / ow
        if bh > sh { bh = sh; bw = sh * ow / oh }
        let rw = bw / st.scale, rh = bh / st.scale
        let a = levelNow
        // 回した枠が元の絵からはみ出さないように、真ん中の位置を収める
        let hx = (rw * abs(cos(a)) + rh * abs(sin(a))) / 2, hy = (rw * abs(sin(a)) + rh * abs(cos(a))) / 2
        var cx: CGFloat, cy: CGFloat
        switch st.pos {
        case 1: cx = hx; cy = sh / 2
        case 2: cx = sw - hx; cy = sh / 2
        case 3, 4: cx = center.x * sw; cy = center.y * sh
        default: cx = sw / 2; cy = sh / 2
        }
        cx = hx * 2 >= sw ? sw / 2 : min(max(cx, hx), sw - hx)
        cy = hy * 2 >= sh ? sh / 2 : min(max(cy, hy), sh - hy)
        // Core Image は左下が原点。枠の真ん中を原点へ → 傾きの分だけ戻す → 送る大きさへ → 送る絵の真ん中へ
        let s = ow / rw
        let tr = CGAffineTransform(translationX: -cx, y: -(sh - cy))
            .concatenating(CGAffineTransform(rotationAngle: -a))
            .concatenating(CGAffineTransform(scaleX: s, y: s))
            .concatenating(CGAffineTransform(translationX: ow / 2, y: oh / 2))
        var made: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &made) == kCVReturnSuccess, let out = made else { return nil }
        CVBufferPropagateAttachments(src, out) // 色の決まり（BT.709 など）を写す。Core Image はこれを見て YCbCr に戻す
        let img = CIImage(cvPixelBuffer: src).clampedToExtent().transformed(by: tr)
        ciContext.render(img, to: out, bounds: CGRect(x: 0, y: 0, width: ow, height: oh), colorSpace: nil)
        lock.lock(); srcW = Int(sw); srcH = Int(sh); levelShown = Double(a * 180 / .pi); lock.unlock()
        return out
    }

    // なめらかにする：dead より小さい差は動かさない。それより大きい差は、tau 秒でなじむように少しずつ寄せる
    private static func smooth(_ cur: CGFloat, _ target: CGFloat, dt: Double, tau: Double, dead: CGFloat) -> CGFloat {
        let diff = target - cur
        if abs(diff) <= dead { return cur }
        if dt <= 0 { return target } // 初めのコマ
        let k = CGFloat(1 - exp(-dt / tau))
        return cur + (diff - (diff > 0 ? dead : -dead)) * k
    }

    // エンコーダと設定の値を、q の外（カメラの糸）から読む。q を待たない（待つと遅れる）ので、壊れても困らない値だけを lock の中の写しから読む
    private var encCopy: VTCompressionSession?
    private var fpsCopy = 30, wCopy = 0, hCopy = 0
    private func encoderForCallback() -> VTCompressionSession? { lock.lock(); defer { lock.unlock() }; return encCopy }
    private func curFpsForCallback() -> Int { lock.lock(); defer { lock.unlock() }; return fpsCopy }

    // ---- エンコーダの糸：できた 1 コマを Annex B にして送る ----

    private func encoded(_ status: OSStatus, _ info: VTEncodeInfoFlags, _ sb: CMSampleBuffer?, _ capUs: UInt64, _ seq: UInt32, _ gen: UInt64) {
        guard status == noErr, let sb, !info.contains(.frameDropped), let bb = CMSampleBufferGetDataBuffer(sb) else {
            lock.lock(); forceKey = true; if info.contains(.frameDropped) { vtDropped &+= 1 }; lock.unlock() // 落とされた。次をキーフレームにする
            return
        }
        var key = true
        if let arr = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]], let a = arr.first, let notSync = a[kCMSampleAttachmentKey_NotSync] as? Bool, notSync {
            key = false
        }
        let sc: [UInt8] = [0, 0, 0, 1]
        var out = Data()
        out.reserveCapacity(40 + CMBlockBufferGetDataLength(bb) + 128)
        out.append(contentsOf: [UInt8](repeating: 0, count: 40)) // 頭（メッセージ 8 バイト＋コマ 32 バイト）は最後に埋める
        var nalLen = 4
        if let fd = CMSampleBufferGetFormatDescription(sb) {
            var count = 0
            var nl: Int32 = 4
            if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil, parameterSetCountOut: &count, nalUnitHeaderLengthOut: &nl) == noErr {
                nalLen = Int(nl)
                if key {
                    for i in 0..<count { // SPS と PPS
                        var p: UnsafePointer<UInt8>?
                        var sz = 0
                        if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, parameterSetIndex: i, parameterSetPointerOut: &p, parameterSetSizeOut: &sz, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr, let p {
                            out.append(contentsOf: sc)
                            out.append(p, count: sz)
                        }
                    }
                }
            }
        }
        let total = CMBlockBufferGetDataLength(bb)
        var raw = [UInt8](repeating: 0, count: total)
        guard total > 0, nalLen >= 1, nalLen <= 4, CMBlockBufferCopyDataBytes(bb, atOffset: 0, dataLength: total, destination: &raw) == kCMBlockBufferNoErr else {
            lock.lock(); forceKey = true; lock.unlock()
            return
        }
        var at = 0
        while at + nalLen <= total { // AVCC（長さ＋NAL）を Annex B（00 00 00 01 ＋ NAL）に直す
            var l = 0
            for k in 0..<nalLen { l = (l << 8) | Int(raw[at + k]) }
            at += nalLen
            if l > total - at { break }
            out.append(contentsOf: sc)
            out.append(contentsOf: raw[at..<(at + l)])
            at += l
        }
        // 頭：メッセージ（型・長さ）＋コマの頭（撮った時刻・送る時刻・番号・印・幅・高さ）
        let nowUs = Self.nowUs()
        // カメラを作り直したあとに、前のカメラのコマを送らない（新しい形の ACK の後ろに、古い大きさのコマが並ばないように）
        lock.lock(); let conn = sendConn; let cw = wCopy, ch = hCopy; let same = gen == sessionGen
        encDone &+= 1
        if conn != nil && same { inflight += 1 }
        lock.unlock()
        guard let conn, same else { return }
        Self.putLE32(&out, 0, Msg.frame)
        Self.putLE32(&out, 4, UInt32(out.count - 8))
        Self.putLE64(&out, 8, capUs)
        Self.putLE64(&out, 16, nowUs)
        Self.putLE32(&out, 24, seq)
        Self.putLE32(&out, 28, key ? 1 : 0)
        Self.putLE32(&out, 32, UInt32(cw))
        Self.putLE32(&out, 36, UInt32(ch))
        conn.send(content: out, completion: .contentProcessed { [weak self] err in
            guard let self else { return }
            self.lock.lock()
            self.inflight = max(0, self.inflight - 1)
            if err == nil { self.sent &+= 1 } else { self.forceKey = true }
            self.lock.unlock()
        })
    }

    // ---- camctl2：PC から操作する（CTRL。q の上） ----
    // CAPS：使える操作と範囲。画面の CameraControlsView に出る物と同じ物を、いま使っているカメラと形で出す。ACK のあと（作り直すたびに）と GET で送る。
    //   {u32 数}{項目 24 バイト × 数}{u32 倍率のボタンの数}{f32 × 数}{u32 選択肢の表の数}{u16 id・u16 数・f32 × 数 を表の数だけ}
    // STATE：{u16 id・u16 why・f32 値} × n。why は 0 全部・1 画面で変えた・2 iOS が変えた・8 SET への返事。
    //   画面の変更は UserDefaults の変更で気づき、0.1 秒まとめて送る。読むだけの値（100 から）は 1 秒ごとに見て、変わった物を送る

    // CAPS の 1 項目
    private struct CtlItem {
        var id: UInt16, kind: UInt16, min: Float, max: Float, step: Float, flags: UInt32
        var choices: [Float]
    }
    // PC に送ってある CAPS
    private struct CtlCaps {
        var items: [CtlItem] = []
        var stops: [Float] = []
        var cam = CameraCaps()
    }
    private enum CtlKind { static let toggle: UInt16 = 0, choice: UInt16 = 1, number: UInt16 = 2, readOnly: UInt16 = 3 }
    private enum CtlFlag { static let rebuild: UInt32 = 1, unavailable: UInt32 = 2, log: UInt32 = 4 }
    private enum CtlWhy { static let all: UInt16 = 0, app: UInt16 = 1, ios: UInt16 = 2, reply: UInt16 = 8 }

    // つなぎが変わった・CTRL を名乗るかが変わった：送った物の覚えを捨てる
    private func ctlReset(_ on: Bool) {
        ctl = on
        ctlNow = CtlCaps()
        ctlSent = [:]
        ctlReplies = []
        ctlZoom = nil
        ctlPollGen += 1
    }

    // ACK のあと：CAPS と全部の値を送り、1 秒ごとの見回りを始める
    private func ctlStart() {
        guard ctl else { return }
        ctlSendCaps()
        ctlSendState(all: true, readOnly: true, why: CtlWhy.all)
        ctlPollGen += 1
        ctlPollLater(ctlPollGen)
    }

    // 1 秒ごと：ライトの今の明るさ・温度・エフェクトなど、iOS が変える値を見て、変わった物を送る
    private func ctlPollLater(_ g: Int) {
        q.asyncAfter(deadline: .now() + 1) { [weak self] in
            guard let self, g == self.ctlPollGen, self.ctl, self.conn != nil else { return }
            self.ctlSendState(all: false, readOnly: true, why: CtlWhy.ios)
            self.ctlPollLater(g)
        }
    }

    // 画面の変更・SET のあと。0.1 秒まとめてから送る（同じ id は 0.1 秒に 1 回まで）
    private func ctlSchedule() {
        guard ctl, !ctlFlushQueued else { return }
        ctlFlushQueued = true
        q.asyncAfter(deadline: .now() + 0.1) { [weak self] in
            guard let self else { return }
            self.ctlFlushQueued = false
            self.ctlSendState(all: false, readOnly: false, why: CtlWhy.app)
        }
    }

    // 倍率を画面の値（Key.zoom）で決めるか。PC がカメラを決めて画面のカメラと違うときは、画面の倍率は別のレンズの物なので使わない
    private func ctlZoomStored(_ r: Resolved) -> Bool {
        !r.pcCam || r.camera == UInt32(max(0, UserDefaults.standard.integer(forKey: Key.camera)))
    }

    // いまの倍率（見え方。applyZoom と同じ決まり）
    private func ctlZoomValue(_ dev: AVCaptureDevice, _ r: Resolved) -> Float {
        let m = Self.zoomMultiplier(dev)
        if ctlZoomStored(r) {
            let z = UserDefaults.standard.double(forKey: Key.zoom)
            return Float(z > 0 ? z : 1)
        }
        return Float(ctlZoom ?? m)
    }

    // いまのカメラと形で、PC に出す操作を決める（画面の CameraControlsView に出る物と同じ）
    private func ctlBuild() -> CtlCaps {
        var n = CtlCaps()
        guard let input = deviceInput, let r = curReq else { return n }
        let dev = input.device
        let c = Self.caps(dev: dev, format: dev.activeFormat, camera: r.camera, height: r.h, fps: curFps)
        n.cam = c
        var items: [CtlItem] = []
        func add(_ id: UInt16, _ kind: UInt16, _ lo: Float, _ hi: Float, _ step: Float, _ flags: UInt32 = 0, _ choices: [Float] = []) {
            items.append(CtlItem(id: id, kind: kind, min: lo, max: hi, step: step, flags: flags, choices: choices))
        }
        func choice(_ id: UInt16, _ values: [Float], _ flags: UInt32 = 0) {
            add(id, CtlKind.choice, values.min() ?? 0, values.max() ?? 0, 1, flags, values)
        }
        func toggle(_ id: UInt16, _ flags: UInt32 = 0) {
            add(id, CtlKind.toggle, 0, 1, 1, flags)
        }
        // ピント。背景をぼかしている間は変えられない（画面も「ピントは自動」と出す）
        let fl: UInt32 = isCinematicOn(input) ? CtlFlag.unavailable : 0
        if c.focusAuto || c.focusManual {
            var fm: [Float] = [0]
            if c.focusLock { fm.append(1) }
            if c.focusManual { fm.append(2) }
            choice(1, fm, fl)
            if c.focusManual { add(2, CtlKind.number, 0, 1, 0.01, fl) }
            if c.focusAuto { choice(3, [0, 1], fl) }
        }
        // 明るさ
        var em: [Float] = [0]
        if c.expLock { em.append(1) }
        if c.expManual { em.append(2) }
        choice(4, em)
        if c.biasMax > c.biasMin { add(5, CtlKind.number, c.biasMin, c.biasMax, 0.1) }
        if c.expManual {
            add(6, CtlKind.number, max(c.isoMin, 1), max(c.isoMax, c.isoMin + 1), 1, CtlFlag.log)
            if !c.shutters.isEmpty { choice(7, c.shutters.map { Float($0) }) }
        }
        // 色
        var wm: [Float] = [0]
        if c.wbLock { wm.append(1) }
        if c.wbTemp { wm.append(2) }
        choice(8, wm)
        if c.wbTemp { add(9, CtlKind.number, 2500, 8000, 100) }
        // ズーム（見え方の倍率）と倍率のボタン
        if c.zoomMax > c.zoomMin + 0.01 {
            add(10, CtlKind.number, Float(c.zoomMin), Float(c.zoomMax), 0.01, CtlFlag.log)
            n.stops = c.zoomStops.map { Float($0) }
        }
        // ライト。前面カメラで iOS が背面のライトを許さないときは「今は使えない」
        if c.torch {
            let ok = Self.torchDevice(for: dev)?.isTorchAvailable ?? false
            let tf: UInt32 = c.front && !ok ? CtlFlag.unavailable : 0
            toggle(11, tf)
            add(12, CtlKind.number, 0.05, 1, 0.05, tf)
            add(100, CtlKind.readOnly, 0, 1, 1)
            add(101, CtlKind.readOnly, 0, 1, 0.01)
        }
        // 手ぶれ補正・背景ぼかし・センターフレーム（作り直す）
        choice(13, c.stabModes.map { Float($0) }, CtlFlag.rebuild)
        if c.cinematic {
            toggle(14, CtlFlag.rebuild)
            if c.apertureMax > c.apertureMin { add(15, CtlKind.number, c.apertureMin, c.apertureMax, 0.1) }
            add(106, CtlKind.readOnly, 0, 1, 1)
        }
        if c.centerStage { toggle(16, CtlFlag.rebuild) }
        // 向き・切り抜き・水平（作り直す。切り抜く位置の横と縦は作り直さない）
        toggle(17, CtlFlag.rebuild)
        toggle(18, CtlFlag.rebuild)
        add(19, CtlKind.choice, 0, 270, 90, CtlFlag.rebuild, [0, 90, 180, 270])
        toggle(20, CtlFlag.rebuild)
        choice(21, [0, 1, 2, 3, 4], CtlFlag.rebuild)
        add(22, CtlKind.number, 0.3, 1, 0.01, CtlFlag.rebuild)
        add(23, CtlKind.number, 0, 1, 0.01)
        add(24, CtlKind.number, 0, 1, 0.01)
        if c.level {
            toggle(25, CtlFlag.rebuild)
            add(107, CtlKind.readOnly, Float(-Self.levelMaxDeg), Float(Self.levelMaxDeg), 0.1)
        }
        // 画面のライト・暖かい色・熱いときにコマ数を下げる・画面を暗くする
        toggle(26)
        toggle(27)
        toggle(28)
        toggle(29)
        // 読むだけ
        add(102, CtlKind.readOnly, 0, 3, 1)
        if c.portrait {
            add(103, CtlKind.readOnly, 0, 1, 1)
            add(104, CtlKind.readOnly, 0, 1, 1)
        }
        if c.studio { add(105, CtlKind.readOnly, 0, 1, 1) }
        add(108, CtlKind.readOnly, 1, 120, 1)
        items.sort { $0.id < $1.id }
        n.items = items
        return n
    }

    // いまの値。画面に出ている値と同じにする（選べない値が入っていたら、画面と同じく初めの選択肢）。readOnly が false なら 100 からは入れない
    private func ctlValues(readOnly: Bool) -> [UInt16: Float] {
        var v: [UInt16: Float] = [:]
        guard let input = deviceInput, let r = curReq else { return v }
        let dev = input.device
        let d = UserDefaults.standard
        let c = ctlNow.cam
        let torch = Self.torchDevice(for: dev)
        lock.lock(); let deg = levelShown; let cine = cineCopy; lock.unlock()
        func b(_ k: String) -> Float { d.bool(forKey: k) ? 1 : 0 }
        func i(_ k: String) -> Float { Float(d.integer(forKey: k)) }
        func f(_ k: String) -> Float { Float(d.double(forKey: k)) }
        for it in ctlNow.items {
            if it.id >= 100 && !readOnly { continue }
            var x: Float
            switch it.id {
            case 1: x = i(Key.focus)
            case 2: x = f(Key.focusPos)
            case 3: x = d.integer(forKey: Key.focusTarget) == 1 ? 1 : 0
            case 4: x = i(Key.exposure)
            case 5: x = f(Key.bias)
            case 6: x = f(Key.iso) // 0 は「今の値から」（画面も「今の値」と出す）
            case 7:
                let s = d.integer(forKey: Key.shutter)
                let fb = c.shutters.first(where: { $0 >= max(curFps, 30) * 2 }) ?? c.shutters.first ?? 0
                x = Float(c.shutters.contains(s) ? s : fb)
            case 8: x = i(Key.wb)
            case 9: x = f(Key.wbTemp)
            case 10: x = ctlZoomValue(dev, r)
            case 11: x = b(Key.torch)
            case 12: x = f(Key.torchLevel)
            case 13: x = i(Key.stab)
            case 14: x = b(Key.cinematic)
            case 15:
                let a = f(Key.aperture)
                x = a > 0 ? a : c.apertureDefault
            case 16: x = AVCaptureDevice.isCenterStageEnabled ? 1 : 0
            case 17: x = b(Key.mirror)
            case 18: x = b(Key.flipV)
            case 19: x = i(Key.rotation)
            case 20: x = b(Key.crop)
            case 21: x = i(Key.cropPos)
            case 22: x = f(Key.cropSize)
            case 23: x = f(Key.cropX)
            case 24: x = f(Key.cropY)
            case 25: x = b(Key.level)
            case 26: x = lightCopy ? 1 : 0
            case 27: x = b(Key.screenWarm)
            case 28: x = b(Key.thermalFps)
            case 29: x = dimCopy ? 1 : 0
            case 100: x = torch?.isTorchActive == true ? 1 : 0
            case 101: x = torch?.isTorchActive == true ? (torch?.torchLevel ?? 0) : 0
            case 102: x = Float(ProcessInfo.processInfo.thermalState.rawValue)
            case 103: x = AVCaptureDevice.isPortraitEffectEnabled ? 1 : 0
            case 104: x = dev.isPortraitEffectActive ? 1 : 0
            case 105:
                x = 0
                if #available(iOS 16.0, *) { x = AVCaptureDevice.isStudioLightEnabled ? 1 : 0 }
            case 106: x = cine ? 1 : 0
            case 107: x = Float((deg * 10).rounded() / 10)
            case 108: x = Float(curFps)
            default: continue
            }
            if !x.isFinite { x = 0 }
            // 範囲の外の値（古い設定など）は、範囲に収めて見せる。ISO の 0 は「今の値から」なのでそのまま
            if it.kind == CtlKind.number && it.id != 6 { x = min(max(x, it.min), it.max) }
            if it.kind == CtlKind.choice && !it.choices.isEmpty && !it.choices.contains(x) { x = it.choices[0] }
            v[it.id] = x
        }
        return v
    }

    private func ctlSendCaps() {
        guard ctl, conn != nil, curReq != nil else { return }
        ctlNow = ctlBuild()
        let vals = ctlValues(readOnly: true)
        var d = Data()
        Self.appendLE32(&d, UInt32(ctlNow.items.count))
        for it in ctlNow.items {
            Self.appendLE16(&d, it.id)
            Self.appendLE16(&d, it.kind)
            Self.appendF32(&d, it.min)
            Self.appendF32(&d, it.max)
            Self.appendF32(&d, it.step)
            Self.appendF32(&d, vals[it.id] ?? 0)
            Self.appendLE32(&d, it.flags)
        }
        Self.appendLE32(&d, UInt32(ctlNow.stops.count))
        for s in ctlNow.stops { Self.appendF32(&d, s) }
        // camctl2 で足した：選ぶ項目の選択肢（PROTOCOL.md の案の後ろ。案だけ読む PC は倍率のボタンまでで止める）
        let lists = ctlNow.items.filter { $0.kind == CtlKind.choice }
        Self.appendLE32(&d, UInt32(lists.count))
        for it in lists {
            Self.appendLE16(&d, it.id)
            Self.appendLE16(&d, UInt16(it.choices.count))
            for x in it.choices { Self.appendF32(&d, x) }
        }
        sendMsg(Msg.ctrlCaps, d)
    }

    // STATE を送る。all は全部（CAPS のあと・GET）。それ以外は、前に送った値と違う物と、SET への返事
    private func ctlSendState(all: Bool, readOnly: Bool, why: UInt16) {
        guard ctl, conn != nil, curReq != nil, !ctlNow.items.isEmpty else { return }
        let vals = ctlValues(readOnly: readOnly)
        var d = Data()
        for id in vals.keys.sorted() {
            guard let x = vals[id] else { continue }
            let reply = ctlReplies.contains(id)
            let changed = ctlSent[id].map { abs($0 - x) > 0.0001 } ?? true
            guard all || reply || changed else { continue }
            var w = why
            if all { w = CtlWhy.all } else if reply { w = CtlWhy.reply } else if id >= 100 { w = CtlWhy.ios }
            Self.appendLE16(&d, id)
            Self.appendLE16(&d, w)
            Self.appendF32(&d, x)
            ctlSent[id] = x
        }
        ctlReplies.subtract(vals.keys)
        if !d.isEmpty { sendMsg(Msg.ctrlState, d) }
    }

    // PC が値を変えた。範囲に収めてから、画面で変えたときと同じ道で当てる。
    // 使えない操作・範囲の外の値にも、STATE（why 8）で今の値を返す。CAPS に無い id は読み捨てる
    private func ctlSet(_ body: [UInt8]) {
        guard let r = curReq else { return } // 作り直しの途中など。このあとの CAPS と STATE で今の値が届く
        var apply: [(UInt16, Double)] = []
        var zoomLive = false
        var at = 0
        while at + 8 <= body.count {
            let id = Self.u16(body, at)
            let raw = Self.f32(body, at + 4)
            at += 8
            guard let it = ctlNow.items.first(where: { $0.id == id }) else { continue }
            guard it.kind != CtlKind.readOnly, it.flags & CtlFlag.unavailable == 0, raw.isFinite else { ctlReplies.insert(id); continue }
            var v = Double(raw)
            if it.kind == CtlKind.toggle {
                v = v != 0 ? 1 : 0
            } else if it.kind == CtlKind.choice {
                // いちばん近い選択肢
                var best = Double(it.choices.first ?? it.min)
                for x in it.choices where abs(Double(x) - v) < abs(best - v) { best = Double(x) }
                v = best
            } else {
                let st = Double(it.step)
                if st > 0 { v = (v / st).rounded() * st }
                v = min(max(v, Double(it.min)), Double(it.max))
            }
            if id == 10 && !ctlZoomStored(r) {
                // PC が決めたカメラの倍率は、このつなぎの間だけ（画面の倍率は別のレンズの物なので書かない）
                ctlZoom = CGFloat(v)
                zoomLive = true
                ctlReplies.insert(id)
                continue
            }
            apply.append((id, v))
        }
        if zoomLive { liveApply() }
        guard !apply.isEmpty else { ctlSchedule(); return }
        let todo = apply // 糸をまたぐ閉包には let で渡す
        let ids = todo.map { $0.0 }
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.ctlApplyOnMain(todo)
            // 書いたあとの値を返す（書く前に返すと古い値になる）
            self.q.async {
                self.ctlReplies.formUnion(ids)
                self.ctlSchedule()
            }
        }
    }

    // 画面の糸。画面の操作（CameraControlsView）と同じ鍵に書き、同じ関数を呼ぶ。だから画面にもすぐ出て、次に開いたときも残る
    private func ctlApplyOnMain(_ items: [(UInt16, Double)]) {
        let d = UserDefaults.standard
        var controls = false, rebuild = false, thermal = false
        for (id, v) in items {
            let on = v != 0
            switch id {
            case 1: d.set(Int(v), forKey: Key.focus); controls = true
            case 2: d.set(v, forKey: Key.focusPos); controls = true
            case 3: d.set(Int(v), forKey: Key.focusTarget); controls = true
            case 4: d.set(Int(v), forKey: Key.exposure); controls = true
            case 5: d.set(v, forKey: Key.bias); controls = true
            case 6: d.set(v, forKey: Key.iso); controls = true
            case 7: d.set(Int(v), forKey: Key.shutter); controls = true
            case 8: d.set(Int(v), forKey: Key.wb); controls = true
            case 9: d.set(v, forKey: Key.wbTemp); controls = true
            case 10: d.set(v, forKey: Key.zoom); controls = true
            case 11: d.set(on, forKey: Key.torch); controls = true
            case 12: d.set(v, forKey: Key.torchLevel); controls = true
            case 13: d.set(Int(v), forKey: Key.stab); rebuild = true
            case 14: d.set(on, forKey: Key.cinematic); rebuild = true
            case 15: d.set(v, forKey: Key.aperture); controls = true
            case 16: setCenterStage(on)
            case 17: d.set(on, forKey: Key.mirror); rebuild = true
            case 18: d.set(on, forKey: Key.flipV); rebuild = true
            case 19: d.set(Int(v), forKey: Key.rotation); rebuild = true
            case 20: d.set(on, forKey: Key.crop); rebuild = true
            case 21: d.set(Int(v), forKey: Key.cropPos); rebuild = true
            case 22: d.set(v, forKey: Key.cropSize); rebuild = true
            case 23: d.set(v, forKey: Key.cropX); controls = true
            case 24: d.set(v, forKey: Key.cropY); controls = true
            case 25: d.set(on, forKey: Key.level); rebuild = true
            case 26:
                // 画面のライトは、アプリが前にいるときだけ出す（消すのはいつでも）
                if !on || UIApplication.shared.applicationState == .active { ScreenDimmer.shared.setLight(on) }
            case 27: d.set(on, forKey: Key.screenWarm)
            case 28: d.set(on, forKey: Key.thermalFps); thermal = true
            case 29: ctlDim(on)
            default: break
            }
        }
        if controls { controlsChanged() }
        if rebuild { settingsChanged() }
        if thermal { thermalSettingChanged() }
    }

    // 画面の糸。画面を暗くする・戻す。画面のライトを出しているときは、閉じてから暗くする。
    // ライトの明るさは setLight(false) がその場で元に戻すので、続けて暗くしてよい。前は 0.6 秒後に暗くしていて、
    // 閉じる動きの終わりにライトの画面（onDisappear）が明るさを戻すのと競り合い、明るさが最大のまま残ることがあった
    private func ctlDim(_ on: Bool) {
        let dm = ScreenDimmer.shared
        guard on else { dm.setDim(false); return }
        guard UIApplication.shared.applicationState == .active else { return }
        if dm.light { dm.setLight(false) }
        dm.setDim(true)
    }

    private static func u16(_ b: [UInt8], _ at: Int) -> UInt16 { UInt16(b[at]) | UInt16(b[at + 1]) << 8 }
    private static func f32(_ b: [UInt8], _ at: Int) -> Float { Float(bitPattern: u32(b, at)) }
    private static func appendLE16(_ d: inout Data, _ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    private static func appendF32(_ d: inout Data, _ v: Float) { appendLE32(&d, v.bitPattern) }

    // ---- 送る ----

    private func sendMsg(_ type: UInt32, _ body: Data, to target: NWConnection? = nil) {
        guard let c = target ?? conn else { return } // target は、まだ conn に入れていない Wi-Fi のつなぎ（合言葉が違うと断るとき）だけ
        var d = Data()
        Self.appendLE32(&d, type)
        Self.appendLE32(&d, UInt32(body.count))
        d.append(body)
        c.send(content: d, completion: .contentProcessed { _ in })
    }

    private func sendAck(status: Int32, w: Int, h: Int, fps: Int, kbps: Int, camera: UInt32, flags: UInt32, to target: NWConnection? = nil) {
        var d = Data()
        Self.appendLE32(&d, Self.version)
        Self.appendLE32(&d, UInt32(bitPattern: status))
        Self.appendLE32(&d, UInt32(w)); Self.appendLE32(&d, UInt32(h)); Self.appendLE32(&d, UInt32(fps)); Self.appendLE32(&d, UInt32(kbps))
        Self.appendLE32(&d, camera)
        Self.appendLE32(&d, flags)
        sendMsg(Msg.ack, d, to: target)
    }

    private func sendEvent(_ code: UInt32, _ arg: UInt32) {
        var d = Data()
        Self.appendLE32(&d, code)
        Self.appendLE32(&d, arg)
        sendMsg(Msg.event, d)
    }

    // iPhone の単調な時計（µs）。カメラの絵の時刻（ホストの時計）と同じ物
    private static func nowUs() -> UInt64 { UInt64(max(0, CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock()))) * 1_000_000) }

    private static func appendLE32(_ d: inout Data, _ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    private static func appendLE64(_ d: inout Data, _ v: UInt64) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    private static func putLE32(_ d: inout Data, _ at: Int, _ v: UInt32) { for i in 0..<4 { d[at + i] = UInt8((v >> (8 * UInt32(i))) & 0xff) } }
    private static func putLE64(_ d: inout Data, _ at: Int, _ v: UInt64) { for i in 0..<8 { d[at + i] = UInt8((v >> (8 * UInt64(i))) & 0xff) } }
}

// 「状態」に出す 1 行。接続中か・毎秒のコマ数・形。iOS 15 でも使えるように LabeledContent は使わない
struct CameraBridgeStatusRow: View {
    @ObservedObject var model: CameraBridge

    var body: some View {
        HStack {
            Text(L("ウェブカメラ", "Webcam"))
            Spacer()
            Text(text).foregroundColor(.secondary).multilineTextAlignment(.trailing)
        }
        if !model.pcNote.isEmpty {
            Text(model.pcNote).font(.footnote)
        }
        if model.pendingNote {
            Text(L("変更した設定は、PC を接続し直すと反映されます。", "The new settings apply when the PC reconnects.")).font(.footnote)
        }
        // camctl2：PC のアプリからも操作できるつなぎ
        if model.connected && model.pcControl {
            Text(L("PC のアプリからもカメラを操作できます。両方で変えたときは、あとから変えた方が使われます。",
                   "The camera can also be controlled from the PC app. If you change a setting in both places, the later change is used.")).font(.footnote)
        }
        // camctl：カメラを使っている最中の温度（信号の 3 色。危険は点が点滅する）
        HStack {
            Text(L("温度", "Temperature"))
            Spacer()
            ThermalBadge()
        }
        if !model.thermalNote.isEmpty {
            Text(model.thermalNote).font(.footnote)
        }
    }

    private var text: String {
        guard model.connected else { return L("PC の接続待ち", "Waiting for the PC") }
        return String(format: "%.1f fps", model.fps) + "・" + model.detail
    }
}

// 「ウェブカメラとして使う」の下に出す、カメラの設定。PC が「おまかせ」にした項目はここの設定を使う。
// その iPhone に無いカメラ、その組み合わせで撮れない解像度とフレームレートは出さない。変えたら、接続中ならその場で切り替える
// iOS 15 でも使えるように、onChange（iOS 17 の形）を使わず、選んだときに Binding の set で知らせる
struct CameraSettingsView: View {
    @ObservedObject var model: CameraBridge
    @AppStorage(CameraBridge.Key.camera) private var camera = CameraBridge.defaultCamera
    @AppStorage(CameraBridge.Key.height) private var height = CameraBridge.defaultHeight
    @AppStorage(CameraBridge.Key.fps) private var fps = CameraBridge.defaultFps
    @AppStorage(CameraBridge.Key.quality) private var quality = CameraBridge.defaultQuality
    @AppStorage(CameraBridge.Key.preferPC) private var preferPC = CameraBridge.defaultPreferPC

    var body: some View {
        let cams = CameraBridge.availableCameras()
        if cams.isEmpty {
            Text(L("この iPhone ではカメラが見つかりません。", "No camera was found on this iPhone.")).font(.footnote)
        } else {
            Picker(L("カメラ", "Camera"), selection: cameraBinding) {
                ForEach(cams, id: \.self) { c in Text(CameraBridge.cameraName(c)).tag(Int(c)) }
            }
            .pickerStyle(.menu)
            Picker(L("解像度", "Resolution"), selection: bind($height)) {
                ForEach(CameraBridge.heights(for: UInt32(clamping: camera)), id: \.self) { h in Text(Self.heightName(h)).tag(h) }
            }
            .pickerStyle(.menu)
            Picker(L("フレームレート", "Frame rate"), selection: bind($fps)) {
                ForEach(CameraBridge.rates(for: UInt32(clamping: camera), height: height), id: \.self) { f in Text("\(f) fps").tag(f) }
            }
            .pickerStyle(.menu)
            Picker(L("画質", "Quality"), selection: bind($quality)) {
                Text(L("標準", "Standard")).tag(1)
                Text(L("高画質", "High")).tag(2)
                Text(L("最高", "Maximum")).tag(3)
            }
            .pickerStyle(.menu)
            Text(Self.qualityNote(quality, height: height, fps: fps)).font(.footnote)
            Toggle(L("PC からの指定を優先する", "Use the settings chosen on the PC first"), isOn: bind($preferPC))
            if height >= 2160 {
                Text(fps > 30
                     ? L("4K の 60 fps は、PC の負荷が大きくなります。カメラを使うアプリには、PC で 1920×1080 に縮めて渡します。", "4K at 60 fps puts a heavy load on the PC. Apps on the PC get it scaled to 1920×1080.")
                     : L("カメラを使うアプリには、PC で 1920×1080 に縮めて渡します。", "Apps on the PC get it scaled to 1920×1080.")).font(.footnote)
            }
        }
    }

    // camctl：カメラを替えたら、倍率はそのカメラをそのまま使う見え方に戻す（超広角は 0.5 倍・望遠はその倍率・レンズ自動切り替えは 1 倍）
    private var cameraBinding: Binding<Int> {
        Binding(get: { camera }, set: { v in
            camera = v
            normalize()
            UserDefaults.standard.set(Double(CameraBridge.baseZoom(UInt32(clamping: camera))), forKey: CameraBridge.Key.zoom)
            model.settingsChanged()
        })
    }

    // 選んだら：組み合わせを撮れる物に直してから、接続中なら切り替える
    private func bind<T>(_ b: Binding<T>) -> Binding<T> {
        Binding(get: { b.wrappedValue }, set: { v in
            b.wrappedValue = v
            normalize()
            model.settingsChanged()
        })
    }

    private func normalize() {
        let cams = CameraBridge.availableCameras()
        if !cams.contains(UInt32(max(0, camera))), let first = cams.first { camera = Int(first) }
        let hs = CameraBridge.heights(for: UInt32(clamping: camera))
        if !hs.contains(height) { height = hs.last(where: { $0 <= height }) ?? hs.first ?? 1080 }
        let rs = CameraBridge.rates(for: UInt32(clamping: camera), height: height)
        if !rs.contains(fps) { fps = rs.contains(30) ? 30 : (rs.first ?? 30) }
    }

    private static func heightName(_ h: Int) -> String { h >= 2160 ? "4K" : "\(h)p" }

    private static func qualityNote(_ q: Int, height: Int, fps: Int) -> String {
        let (w, h) = CameraBridge.size(forHeight: height)
        let mbps = Double(CameraBridge.qualityKbps(q, w: w, h: h, fps: fps)) / 1000
        let s = String(format: "%.0f", mbps)
        if q >= 3 { return L("約 \(s) Mbps です。USB で接続しているときに向いています。Wi-Fi では映像が止まることがあります。", "About \(s) Mbps. Best over USB. Over Wi-Fi the video may stall.") }
        return L("約 \(s) Mbps です。", "About \(s) Mbps.")
    }
}

// ---- camctl：画面 ----

// そのカメラでできること。画面に出す操作を決める（できない操作は出さない）
struct CameraCaps {
    var front = false
    var focusAuto = false, focusLock = false, focusManual = false
    var expLock = false, expManual = false
    var biasMin: Float = 0, biasMax: Float = 0
    var isoMin: Float = 0, isoMax: Float = 0
    var shutters: [Int] = []
    var wbLock = false, wbTemp = false
    var zoomMin: CGFloat = 1, zoomMax: CGFloat = 1, zoomOptical: CGFloat = 1
    var zoomStops: [CGFloat] = []
    var torch = false
    var stabModes: [Int] = [0]
    var cinematic = false
    var apertureMin: Float = 0, apertureMax: Float = 0, apertureDefault: Float = 0
    var portrait = false, studio = false, centerStage = false
    var level = false
    var lensAuto = false // 背面のレンズ自動切り替え（仮想カメラ）がある
}

extension CameraBridge {
    // 画面の糸から呼ぶ。画面の設定（カメラ・解像度・フレームレート）で撮る形を選び、そのカメラと形でできることを調べる
    static func caps(camera cam: UInt32, height: Int, fps: Int) -> CameraCaps {
        guard let dev = device(cam) else {
            var c = CameraCaps()
            c.lensAuto = device(4) != nil
            return c
        }
        let (w, h) = size(forHeight: height)
        let f = pickFormat(dev, w: w, h: h, fps: fps)?.0 ?? dev.activeFormat
        return caps(dev: dev, format: f, camera: cam, height: height, fps: fps)
    }

    // camctl2：カメラと形を決めて調べる。接続中の CTRL は、いま使っているカメラ（背景ぼかしのときはそのカメラ）と形で調べる
    static func caps(dev: AVCaptureDevice, format f: AVCaptureDevice.Format, camera cam: UInt32, height: Int, fps: Int) -> CameraCaps {
        var c = CameraCaps()
        c.lensAuto = device(4) != nil
        c.front = dev.position == .front
        c.focusAuto = dev.isFocusModeSupported(.continuousAutoFocus)
        c.focusLock = c.focusAuto && dev.isFocusModeSupported(.locked)
        c.focusManual = dev.isLockingFocusWithCustomLensPositionSupported
        c.expLock = dev.isExposureModeSupported(.locked)
        c.expManual = dev.isExposureModeSupported(.custom)
        var bmin = dev.minExposureTargetBias, bmax = dev.maxExposureTargetBias
        if #available(iOS 18.0, *), let r = f.systemRecommendedExposureBiasRange {
            bmin = max(bmin, r.lowerBound)
            bmax = min(bmax, r.upperBound)
        }
        c.biasMin = max(bmin, -3)
        c.biasMax = min(bmax, 3)
        c.isoMin = f.minISO
        c.isoMax = f.maxISO
        // シャッター速度は 1 コマの長さより短い物だけ（長いとコマ数が落ちる）
        let minS = CMTimeGetSeconds(f.minExposureDuration), maxS = CMTimeGetSeconds(f.maxExposureDuration)
        c.shutters = [30, 50, 60, 100, 120, 250, 500, 1000, 2000, 4000].filter { s in
            let t = 1 / Double(s)
            return s >= fps && t >= minS && t <= maxS
        }
        c.wbLock = dev.isWhiteBalanceModeSupported(.locked)
        c.wbTemp = dev.isLockingWhiteBalanceWithCustomDeviceGainsSupported
        let m = zoomMultiplier(dev)
        c.zoomMin = max(1, dev.minAvailableVideoZoomFactor) * m
        var hi = min(f.videoMaxZoomFactor, 10 / m) // 見え方で 10 倍まで。iOS 18 からは iOS の勧める範囲
        if #available(iOS 18.0, *), let r = f.systemRecommendedVideoZoomRange { hi = min(f.videoMaxZoomFactor, r.upperBound) }
        c.zoomMax = max(c.zoomMin, hi * m)
        c.zoomStops = zoomStops(dev, m: m).filter { $0 >= c.zoomMin - 0.01 && $0 <= c.zoomMax + 0.01 }
        c.zoomOptical = max(c.zoomStops.max() ?? c.zoomMin, f.videoZoomFactorUpscaleThreshold * m)
        c.torch = torchDevice(for: dev) != nil
        c.stabModes = stabChoices(f)
        #if compiler(>=6.2)
        if #available(iOS 26.0, *), let cd = cinematicDevice(cam) {
            let cf = cd.formats.filter { $0.isCinematicVideoCaptureSupported && is420v($0) }
            let want = Int32(min(height, 2160))
            if let p = cf.first(where: { CMVideoFormatDescriptionGetDimensions($0.formatDescription).height == want }) ?? cf.first {
                c.cinematic = true
                if p.minSimulatedAperture > 0 && p.maxSimulatedAperture > p.minSimulatedAperture {
                    c.apertureMin = p.minSimulatedAperture
                    c.apertureMax = p.maxSimulatedAperture
                    c.apertureDefault = p.defaultSimulatedAperture
                }
            }
        }
        #endif
        c.portrait = dev.formats.contains { $0.isPortraitEffectSupported }
        if #available(iOS 16.0, *) { c.studio = dev.formats.contains { $0.isStudioLightSupported } }
        c.centerStage = dev.formats.contains { $0.isCenterStageSupported }
        c.level = levelAvailable(dev)
        return c
    }

    // 倍率の文字（0.5×・1×・2.5× など）
    static func zoomLabel(_ z: CGFloat) -> String {
        let r = (Double(z) * 10).rounded() / 10
        return (abs(r - r.rounded()) < 0.001 ? String(format: "%.0f", r) : String(format: "%.1f", r)) + "×"
    }

    // 熱いときにフレームレートを下げる設定を変えた。今の温度で当て直す
    func thermalSettingChanged() {
        q.async { [weak self] in self?.applyThermal() }
    }
}

// カメラの操作（camctl）。「映像」の下に並べる、いくつかの Section。Form の中に直接置く。
// 変えたら、接続中ならすぐ効く（ピント・明るさ・色・ズーム・ライト）か、0.3 秒まとめてカメラを作り直す（向き・手ぶれ補正・背景ぼかし・切り抜きの入り切り）。
// 値は UserDefaults に残り、次に開いたときも同じ。iOS 15 でも使えるように、onChange（iOS 17 の形）と LabeledContent（iOS 16）は使わない
struct CameraControlsView: View {
    @ObservedObject var model: CameraBridge
    @AppStorage(CameraBridge.Key.camera) private var camera = CameraBridge.defaultCamera
    @AppStorage(CameraBridge.Key.height) private var height = CameraBridge.defaultHeight
    @AppStorage(CameraBridge.Key.fps) private var fps = CameraBridge.defaultFps
    @AppStorage(CameraBridge.Key.mirror) private var mirror = false
    @AppStorage(CameraBridge.Key.flipV) private var flipV = false
    @AppStorage(CameraBridge.Key.rotation) private var rotation = 0
    @AppStorage(CameraBridge.Key.focus) private var focus = 0
    @AppStorage(CameraBridge.Key.focusPos) private var focusPos = 0.5
    @AppStorage(CameraBridge.Key.focusTarget) private var focusTarget = 0
    @AppStorage(CameraBridge.Key.exposure) private var exposure = 0
    @AppStorage(CameraBridge.Key.bias) private var bias = 0.0
    @AppStorage(CameraBridge.Key.iso) private var iso = 0.0
    @AppStorage(CameraBridge.Key.shutter) private var shutter = 0
    @AppStorage(CameraBridge.Key.wb) private var wb = 0
    @AppStorage(CameraBridge.Key.wbTemp) private var wbTemp = 5000.0
    @AppStorage(CameraBridge.Key.zoom) private var zoom = 1.0
    @AppStorage(CameraBridge.Key.torch) private var torch = false
    @AppStorage(CameraBridge.Key.torchLevel) private var torchLevel = 0.5
    @AppStorage(CameraBridge.Key.stab) private var stab = 0
    @AppStorage(CameraBridge.Key.cinematic) private var cinematic = false
    @AppStorage(CameraBridge.Key.aperture) private var aperture = 0.0
    @AppStorage(CameraBridge.Key.crop) private var crop = false
    @AppStorage(CameraBridge.Key.cropPos) private var cropPos = 0
    @AppStorage(CameraBridge.Key.cropSize) private var cropSize = 0.7
    @AppStorage(CameraBridge.Key.cropX) private var cropX = 0.5
    @AppStorage(CameraBridge.Key.cropY) private var cropY = 0.5
    @AppStorage(CameraBridge.Key.level) private var level = false
    @AppStorage(CameraBridge.Key.screenWarm) private var screenWarm = false
    @AppStorage(CameraBridge.Key.thermalFps) private var thermalFps = true

    var body: some View {
        let c = CameraBridge.caps(camera: UInt32(clamping: camera), height: height, fps: fps)
        orientationSection
        zoomSection(c)
        focusSection(c)
        exposureSection(c)
        colorSection(c)
        lightSection(c)
        stabSection(c)
        effectsSection(c)
        heatSection
    }

    // ---- 向き ----

    private var orientationSection: some View {
        Section(L("向き", "Orientation")) {
            Toggle(L("左右反転", "Flip horizontally"), isOn: rebuild($mirror))
            Toggle(L("上下反転", "Flip vertically"), isOn: rebuild($flipV))
            Picker(L("回転", "Rotation"), selection: rebuild($rotation)) {
                Text("0°").tag(0)
                Text("90°").tag(90)
                Text("180°").tag(180)
                Text("270°").tag(270)
            }
            .pickerStyle(.segmented)
            if rotation == 90 || rotation == 270 {
                Text(L("90 度と 270 度では縦長の映像になります。4K は 1080p で送ります。", "At 90° and 270° the video is portrait. 4K is sent as 1080p.")).font(.footnote)
            }
            if model.rotSkipped {
                Text(L("PC のソフトが古いため、90 度と 270 度の回転は使っていません。PC のソフトを新しくしてください。",
                       "The PC software is old, so 90° and 270° rotation is not used. Please update the PC software.")).font(.footnote)
            }
        }
    }

    // ---- ズームと切り抜き・水平 ----

    private func zoomSection(_ c: CameraCaps) -> some View {
        Section(L("ズームと切り抜き", "Zoom and crop")) {
            if c.zoomMax > c.zoomMin + 0.01 {
                HStack(spacing: 8) {
                    ForEach(c.zoomStops, id: \.self) { z in
                        Button(CameraBridge.zoomLabel(z)) {
                            zoom = Double(z)
                            model.controlsChanged()
                        }
                        .buttonStyle(.bordered)
                        .tint(abs(zoom - Double(z)) < 0.05 ? Color.blue : Color.gray)
                    }
                }
                HStack {
                    Text(L("デジタルズーム", "Digital zoom"))
                    Spacer()
                    Text(CameraBridge.zoomLabel(CGFloat(zoom))).monospacedDigit().foregroundColor(.secondary)
                }
                Slider(value: zoomSlider(c), in: 0...1)
                Text(L("\(CameraBridge.zoomLabel(c.zoomOptical)) より大きくすると、拡大した分だけ画質が落ちます。",
                       "Beyond \(CameraBridge.zoomLabel(c.zoomOptical)), the picture gets softer as you zoom in.")).font(.footnote)
            }
            if camera == 4 {
                Text(L("倍率に合わせて、iOS が超広角・広角・望遠のレンズを切り替えます。手動のピントと色温度は使えません。",
                       "iOS switches between the ultra wide, wide, and telephoto lenses to match the zoom. Manual focus and color temperature are not available.")).font(.footnote)
            } else if c.lensAuto && !c.front {
                Text(L("0.5 倍や望遠も倍率のボタンで選ぶには、カメラで「背面・レンズ自動切り替え」を選びます。",
                       "To pick 0.5× and telephoto with the zoom buttons, choose Back, auto lens switching as the camera.")).font(.footnote)
            }
            Toggle(L("切り抜く", "Crop"), isOn: rebuild($crop))
            if crop {
                Picker(L("位置", "Position"), selection: rebuild($cropPos)) {
                    Text(L("中央", "Center")).tag(0)
                    Text(L("左寄り", "Left")).tag(1)
                    Text(L("右寄り", "Right")).tag(2)
                    Text(L("顔に合わせる", "Follow the face")).tag(3)
                    Text(L("手動", "Manual")).tag(4)
                }
                .pickerStyle(.menu)
                HStack {
                    Text(L("大きさ", "Size"))
                    Slider(value: rebuild($cropSize), in: 0.3...1)
                    Text("\(Int((cropSize * 100).rounded()))%").monospacedDigit().foregroundColor(.secondary)
                }
                if cropPos == 4 {
                    HStack {
                        Text(L("横の位置", "Horizontal"))
                        Slider(value: live($cropX), in: 0...1)
                    }
                    HStack {
                        Text(L("縦の位置", "Vertical"))
                        Slider(value: live($cropY), in: 0...1)
                    }
                }
                if cropPos == 3 {
                    Text(L("顔が見つかるまでは、真ん中を切り抜きます。", "Until a face is found, the center is cropped.")).font(.footnote)
                }
            }
            Text(L("ズームは映像の真ん中を拡大します。切り抜きは好きな位置を切り出して、選んだ解像度に拡大して送ります。",
                   "Zoom enlarges the center of the picture. Crop cuts out any part you choose and scales it to the selected resolution.")).font(.footnote)
            if c.level {
                Toggle(L("水平を保つ", "Keep level"), isOn: rebuild($level))
                if level {
                    Text(L("iPhone が少し傾いていても、映像の水平を保ちます。6 度まで直せます。傾きを直す余白のため、映像を少し拡大します。",
                           "Keeps the picture level even if the iPhone is slightly tilted, up to 6°. The picture is zoomed in a little to leave room for the correction.")).font(.footnote)
                    if model.connected {
                        Text(L("いま \(String(format: "%.1f", model.levelAngle)) 度直しています。", "Correcting \(String(format: "%.1f", model.levelAngle))° now.")).font(.footnote)
                    }
                }
            }
        }
    }

    // ---- ピント ----

    private func focusSection(_ c: CameraCaps) -> some View {
        Section(L("ピント", "Focus")) {
            if cinematic && c.cinematic {
                Text(L("背景をぼかしている間は、ピントは自動で合わせます。", "While the background is blurred, focus is automatic.")).font(.footnote)
            } else if c.focusAuto || c.focusManual {
                let modes: [Int] = [0] + (c.focusLock ? [1] : []) + (c.focusManual ? [2] : [])
                let mode = modes.contains(focus) ? focus : 0
                Picker(L("ピント", "Focus"), selection: pick($focus, modes)) {
                    Text(L("自動", "Auto")).tag(0)
                    if c.focusLock { Text(L("固定", "Lock")).tag(1) }
                    if c.focusManual { Text(L("手動", "Manual")).tag(2) }
                }
                .pickerStyle(.segmented)
                if mode == 2 {
                    HStack {
                        Text(L("近く", "Near")).font(.footnote)
                        Slider(value: live($focusPos), in: 0...1)
                        Text(L("遠く", "Far")).font(.footnote)
                    }
                } else if mode == 1 {
                    Text(L("今のピントのまま固定します。", "Keeps the current focus.")).font(.footnote)
                } else if c.focusAuto {
                    Picker(L("合わせる所", "Focus on"), selection: live($focusTarget)) {
                        Text(L("顔を優先", "Faces first")).tag(0)
                        Text(L("画面の真ん中", "Center of the picture")).tag(1)
                    }
                    .pickerStyle(.menu)
                }
            } else {
                Text(L("このカメラはピントを変えられません。", "This camera has a fixed focus.")).font(.footnote)
            }
        }
    }

    // ---- 明るさ ----

    private func exposureSection(_ c: CameraCaps) -> some View {
        Section(L("明るさ", "Brightness")) {
            let modes: [Int] = [0] + (c.expLock ? [1] : []) + (c.expManual ? [2] : [])
            let mode = modes.contains(exposure) ? exposure : 0
            Picker(L("明るさ", "Brightness"), selection: pick($exposure, modes)) {
                Text(L("自動", "Auto")).tag(0)
                if c.expLock { Text(L("固定", "Lock")).tag(1) }
                if c.expManual { Text(L("手動", "Manual")).tag(2) }
            }
            .pickerStyle(.segmented)
            if mode == 2 {
                HStack {
                    Text("ISO")
                    Spacer()
                    Text(iso > 0 ? "\(Int(iso))" : L("今の値", "Current")).monospacedDigit().foregroundColor(.secondary)
                }
                Slider(value: isoSlider(c), in: 0...1)
                if !c.shutters.isEmpty {
                    Picker(L("シャッター速度", "Shutter speed"), selection: shutterBinding(c)) {
                        ForEach(c.shutters, id: \.self) { s in Text("1/\(s)").tag(s) }
                    }
                    .pickerStyle(.menu)
                }
                Text(L("照明のちらつきを抑えるには、東日本では 1/50 か 1/100、西日本では 1/60 か 1/120 にします。",
                       "To reduce flicker from lights, use 1/50 or 1/100 where the power is 50 Hz, and 1/60 or 1/120 where it is 60 Hz.")).font(.footnote)
            } else if c.biasMax > c.biasMin {
                HStack {
                    Text(L("露出補正", "Exposure compensation"))
                    Spacer()
                    Text(String(format: "%+.1f", bias)).monospacedDigit().foregroundColor(.secondary)
                }
                Slider(value: live($bias), in: Double(c.biasMin)...Double(c.biasMax), step: 0.1)
                if mode == 1 {
                    Text(L("今の明るさのまま固定します。", "Keeps the current brightness.")).font(.footnote)
                }
            }
        }
    }

    // ---- 色 ----

    private func colorSection(_ c: CameraCaps) -> some View {
        Section(L("色", "Color")) {
            let modes: [Int] = [0] + (c.wbLock ? [1] : []) + (c.wbTemp ? [2] : [])
            let mode = modes.contains(wb) ? wb : 0
            Picker(L("ホワイトバランス", "White balance"), selection: pick($wb, modes)) {
                Text(L("自動", "Auto")).tag(0)
                if c.wbLock { Text(L("固定", "Lock")).tag(1) }
                if c.wbTemp { Text(L("色温度", "Temperature")).tag(2) }
            }
            .pickerStyle(.segmented)
            if mode == 2 {
                HStack {
                    Text(L("色温度", "Color temperature"))
                    Spacer()
                    Text("\(Int(wbTemp)) K").monospacedDigit().foregroundColor(.secondary)
                }
                Slider(value: live($wbTemp), in: 2500...8000, step: 100)
                Text(L("値を小さくすると青みが、大きくすると黄みが強くなります。", "Lower values look bluer and higher values look warmer.")).font(.footnote)
            } else if mode == 1 {
                Text(L("今の色のまま固定します。", "Keeps the current color.")).font(.footnote)
            }
        }
    }

    // ---- ライト ----

    private func lightSection(_ c: CameraCaps) -> some View {
        Section(L("ライト", "Light")) {
            if c.torch {
                Toggle(L("ライトをつける", "Turn on the light"), isOn: live($torch))
                if torch {
                    Picker(L("ライトの明るさ", "Light brightness"), selection: torchStep) {
                        Text("25%").tag(25)
                        Text("50%").tag(50)
                        Text("75%").tag(75)
                        Text("100%").tag(100)
                    }
                    .pickerStyle(.segmented)
                    if !model.torchState.isEmpty {
                        Text(model.torchState).font(.footnote)
                    }
                }
                Text(c.front
                     ? L("前面カメラのときは背面のライトが使用されます。iOS が許さないときは点きません。そのときは画面のライトを使ってください。",
                         "With the front camera, the back light is used. If iOS does not allow it, use the screen light instead.")
                     : L("PC と接続している間だけ点きます。", "The light is on only while the PC is connected.")).font(.footnote)
            }
            // camctl2：画面のライトは ScreenDimmer が持つ（PC からも点ける）。出すのはアプリのいちばん外（ScreenDimHost）
            Button(L("画面のライトをつける", "Use the screen as a light")) { ScreenDimmer.shared.setLight(true) }
            Toggle(L("画面のライトを暖かい色にする", "Warm screen light"), isOn: $screenWarm)
            Text(L("画面を白くして明るさを最大にし、前から顔を照らします。画面をタップすると元の明るさに戻ります。",
                   "Turns the screen white at full brightness to light your face. Tap the screen to restore the brightness.")).font(.footnote)
        }
    }

    // ---- 手ぶれ補正 ----

    private func stabSection(_ c: CameraCaps) -> some View {
        Section(L("手ぶれ補正", "Stabilization")) {
            Picker(L("手ぶれ補正", "Stabilization"), selection: pick($stab, c.stabModes, remake: true)) {
                Text(L("オフ", "Off")).tag(0)
                if c.stabModes.contains(1) { Text(L("遅れの少ない補正", "Low latency")).tag(1) }
                if c.stabModes.contains(2) { Text(L("標準", "Standard")).tag(2) }
                if c.stabModes.contains(3) { Text(L("シネマティック", "Cinematic")).tag(3) }
            }
            .pickerStyle(.menu)
            Text(Self.stabNote(c.stabModes.contains(stab) ? stab : 0)).font(.footnote)
        }
    }

    private static func stabNote(_ v: Int) -> String {
        switch v {
        case 1: return L("映像の遅れを増やさずに揺れを抑えます。写る範囲は少し狭くなります。", "Reduces shake without adding delay. The picture is slightly narrower.")
        case 2: return L("揺れを抑えます。写る範囲が少し狭くなり、映像が少し遅れることがあります。", "Reduces shake. The picture is slightly narrower and may be slightly delayed.")
        case 3: return L("揺れを強く抑えます。写る範囲が狭くなり、映像の遅れがはっきり増えます。", "Strongly reduces shake. The picture is narrower and the delay clearly increases.")
        default: return L("揺れを抑えません。映像の遅れがいちばん少なくなります。", "No stabilization. The delay is the shortest.")
        }
    }

    // ---- 背景ぼかしとエフェクト ----

    private func effectsSection(_ c: CameraCaps) -> some View {
        Section(L("背景ぼかしとエフェクト", "Background blur and effects")) {
            Text(L("iPhone のレンズの F 値は変えられないため、背景のぼかしは iPhone の処理で作ります。",
                   "The aperture of the iPhone lens cannot be changed, so the background blur is made by processing on the iPhone.")).font(.footnote)
            if c.cinematic {
                Toggle(L("背景をぼかす", "Blur the background"), isOn: rebuild($cinematic))
                if cinematic {
                    if c.apertureMax > c.apertureMin {
                        HStack {
                            Text(L("ぼかしの強さ", "Blur strength"))
                            Spacer()
                            Text(String(format: "f/%.1f", apertureValue(c))).monospacedDigit().foregroundColor(.secondary)
                        }
                        Slider(value: apertureBinding(c), in: Double(c.apertureMin)...Double(c.apertureMax), step: 0.1)
                    }
                    Text(L("F 値を小さくすると、背景が強くぼけます。iPhone のシネマティックモードと同じ処理です。30 fps までになり、映像が少し遅れることがあります。ピントは自動です。",
                           "A smaller f-number blurs the background more. This is the same processing as Cinematic mode on the iPhone. The video is limited to 30 fps and may be slightly delayed. Focus is automatic.")).font(.footnote)
                    if model.connected && !model.cinematicActive {
                        Text(L("今の設定では使えません。解像度かフレームレートを変えてください。", "Not available with the current settings. Change the resolution or frame rate.")).font(.footnote)
                    }
                }
            }
            if c.portrait || c.studio {
                if c.portrait { stateRow(L("ポートレート", "Portrait"), on: model.portraitOn, active: model.portraitActive) }
                if c.studio { stateRow(L("スタジオ照明", "Studio Light"), on: model.studioOn, active: false) }
                Button(L("ビデオエフェクトを開く", "Open Video Effects")) { AVCaptureDevice.showSystemUserInterface(.videoEffects) }
                Text(L("ポートレートとスタジオ照明は、コントロールセンターのビデオエフェクトで入れます。入れると 30 fps までになることがあります。",
                       "Turn on Portrait and Studio Light in Video Effects in Control Center. They may limit the video to 30 fps.")).font(.footnote)
            }
            if c.centerStage {
                Toggle(L("センターフレーム", "Center Stage"), isOn: Binding(get: { model.centerStageOn }, set: { model.setCenterStage($0) }))
                Text(L("人が動いても画面の中に収まるように、写す範囲を自動で動かします。", "Automatically moves the framing to keep people in the picture.")).font(.footnote)
            }
            if !c.cinematic && !c.portrait && !c.centerStage {
                Text(L("このカメラでは背景ぼかしを使えません。", "Background blur is not available with this camera.")).font(.footnote)
            }
        }
    }

    private func stateRow(_ name: String, on: Bool, active: Bool) -> some View {
        HStack {
            Text(name)
            Spacer()
            Text(on ? (active ? L("使用中", "In use") : L("オン", "On")) : L("オフ", "Off")).foregroundColor(.secondary)
        }
    }

    // ---- 熱の対策 ----

    private var heatSection: some View {
        Section(L("熱の対策", "Heat")) {
            Toggle(L("iPhone が熱いときはフレームレートを下げる", "Lower the frame rate when the iPhone is hot"),
                   isOn: Binding(get: { thermalFps }, set: { v in thermalFps = v; model.thermalSettingChanged() }))
            Text(L("温度が高いときは 60 fps を 30 fps に、30 fps を 24 fps にします。危険のときは 15 fps にします。温度が下がると元に戻ります。",
                   "When hot, 60 fps becomes 30 fps and 30 fps becomes 24 fps. When critical, 15 fps. It returns when the iPhone cools down.")).font(.footnote)
        }
    }

    // ---- 値のつなぎ ----

    // 今のカメラにすぐ効く操作（作り直さない）
    private func live<T>(_ b: Binding<T>) -> Binding<T> {
        Binding(get: { b.wrappedValue }, set: { v in b.wrappedValue = v; model.controlsChanged() })
    }
    // 作り直すかもしれない操作。0.3 秒まとめてから、要るときだけ作り直す
    private func rebuild<T>(_ b: Binding<T>) -> Binding<T> {
        Binding(get: { b.wrappedValue }, set: { v in b.wrappedValue = v; model.settingsChanged() })
    }
    // 選べない値が入っていたら（カメラを替えたときなど）、初めの値として見せる
    private func pick(_ b: Binding<Int>, _ allowed: [Int], remake: Bool = false) -> Binding<Int> {
        Binding(get: { allowed.contains(b.wrappedValue) ? b.wrappedValue : (allowed.first ?? 0) },
                set: { v in
                    b.wrappedValue = v
                    if remake { model.settingsChanged() } else { model.controlsChanged() }
                })
    }
    // ズームのスライダーは倍率の対数（1 倍から 2 倍と、5 倍から 10 倍が同じ長さ）
    private func zoomSlider(_ c: CameraCaps) -> Binding<Double> {
        let lo = Double(c.zoomMin), hi = Double(max(c.zoomMax, c.zoomMin + 0.01))
        return Binding(get: { min(max(log(max(zoom, lo) / lo) / log(hi / lo), 0), 1) },
                       set: { t in
                           zoom = (lo * pow(hi / lo, t) * 100).rounded() / 100
                           model.controlsChanged()
                       })
    }
    // ISO のスライダーも対数
    private func isoSlider(_ c: CameraCaps) -> Binding<Double> {
        let lo = Double(max(c.isoMin, 1)), hi = Double(max(c.isoMax, c.isoMin + 1))
        return Binding(get: {
                           let v = iso > 0 ? iso : (lo * hi).squareRoot()
                           return min(max(log(v / lo) / log(hi / lo), 0), 1)
                       },
                       set: { t in
                           iso = (lo * pow(hi / lo, t)).rounded()
                           model.controlsChanged()
                       })
    }
    private func shutterBinding(_ c: CameraCaps) -> Binding<Int> {
        let fallback = c.shutters.first(where: { $0 >= max(fps, 30) * 2 }) ?? c.shutters.first ?? 0
        return Binding(get: { c.shutters.contains(shutter) ? shutter : fallback },
                       set: { v in shutter = v; model.controlsChanged() })
    }
    private var torchStep: Binding<Int> {
        Binding(get: { min(max(Int((torchLevel * 4).rounded()), 1), 4) * 25 },
                set: { v in torchLevel = Double(v) / 100; model.controlsChanged() })
    }
    private func apertureValue(_ c: CameraCaps) -> Double {
        aperture > 0 ? min(max(aperture, Double(c.apertureMin)), Double(c.apertureMax)) : Double(c.apertureDefault)
    }
    private func apertureBinding(_ c: CameraCaps) -> Binding<Double> {
        Binding(get: { apertureValue(c) }, set: { v in aperture = (v * 10).rounded() / 10; model.controlsChanged() })
    }
}

// 画面のライト：画面を白くして明るさを最大にし、前から顔を照らす。タップか、アプリが前からいなくなると元の明るさに戻す。
// 出している間は、自動で画面を暗くする働き（ScreenDimmer）を止める。
// camctl2：出すのは ScreenDimmer.light（カメラタブのボタンと PC の両方）。暖かい色は出している間に PC から変えても変わるように、設定を直接読む
// 明るさを最大にする・元に戻す・自動で暗くするのを止めるのは ScreenDimmer.setLight がする（閉じると isPresented の Binding から setLight(false) が呼ばれる）。
// 前はこの画面の onAppear と onDisappear で明るさを変えていて、閉じる動きの終わりの onDisappear が、そのあとに暗くした明るさを上書きしていた
struct ScreenLightView: View {
    @AppStorage(CameraBridge.Key.screenWarm) private var warm = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        ZStack {
            (warm ? Color(red: 1.0, green: 0.87, blue: 0.70) : Color.white).ignoresSafeArea()
            Text(L("タップすると元の明るさに戻ります。", "Tap to restore the brightness.")).font(.footnote).foregroundColor(Color(white: 0.6))
        }
        .contentShape(Rectangle())
        .onTapGesture { dismiss() }
        .statusBar(hidden: true)
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.willResignActiveNotification)) { _ in dismiss() }
    }
}

// camrot1015：NV12 の絵を、縦横を変えずに左右・上下に反転して、別の入れ物へ写す。180 度の回転は上下と左右の両方、左右反転は X だけ、上下反転は Y だけ。
// AVCaptureConnection の videoRotationAngle・isVideoMirrored は、毎コマ iOS が絵を物理的に作り直す（Apple の資料）。その代わりに、Metal の compute で
// Y の面（R8）と CbCr の面（RG8）を 1 回ずつ写す。カメラの絵は CVMetalTextureCache で、写し先は IOSurface から直接 Metal の物にするので、CPU のコピーは無い。
// 4K の NV12 は 1 コマ約 12 MB。読みと書きで約 24 MB の GPU メモリの往復（60 コマ/秒で約 1.5 GB/秒）。
// encodeQueue（直列。隠しスイッチ 16 のときは captureQueue）だけから呼ぶ。GPU が終わるまで待ってから返す（そのあとエンコーダが読む）
final class FrameFlipper {
    static var buildError = ""
    private static let kernelSource = """
    #include <metal_stdlib>
    using namespace metal;
    kernel void flipPlane(texture2d<float, access::read> src [[texture(0)]],
                          texture2d<float, access::write> dst [[texture(1)]],
                          constant uint2 &fl [[buffer(0)]],
                          uint2 gid [[thread_position_in_grid]]) {
        uint w = dst.get_width();
        uint h = dst.get_height();
        if (gid.x >= w || gid.y >= h) { return; }
        uint sx = fl.x != 0 ? (w - 1 - gid.x) : gid.x;
        uint sy = fl.y != 0 ? (h - 1 - gid.y) : gid.y;
        dst.write(src.read(uint2(sx, sy)), gid);
    }
    """
    // GPU・命令の列・計算の用意は、つなぎ直しても使い回す（初めての 1 回だけ Metal の言葉を組む）
    private static let gpu: (device: MTLDevice, queue: MTLCommandQueue, pso: MTLComputePipelineState)? = {
        guard let dev = MTLCreateSystemDefaultDevice() else { FrameFlipper.buildError = "Metal の GPU が無い"; return nil }
        guard let q = dev.makeCommandQueue() else { FrameFlipper.buildError = "命令の列を作れない"; return nil }
        do {
            let lib = try dev.makeLibrary(source: FrameFlipper.kernelSource, options: nil)
            guard let fn = lib.makeFunction(name: "flipPlane") else { FrameFlipper.buildError = "flipPlane が無い"; return nil }
            let pso = try dev.makeComputePipelineState(function: fn)
            return (dev, q, pso)
        } catch {
            FrameFlipper.buildError = "Metal の言葉を組めない：\(error)"
            return nil
        }
    }()

    let w: Int, h: Int
    let flipX: Bool, flipY: Bool
    private(set) var lastError = ""
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let pso: MTLComputePipelineState
    private let texCache: CVMetalTextureCache
    private let pool: CVPixelBufferPool

    init?(w: Int, h: Int, flipX: Bool, flipY: Bool) {
        guard w >= 2, h >= 2, w % 2 == 0, h % 2 == 0 else { FrameFlipper.buildError = "大きさが変（\(w)×\(h)）"; return nil }
        guard let g = FrameFlipper.gpu else { return nil }
        var cache: CVMetalTextureCache?
        guard CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, g.device, nil, &cache) == kCVReturnSuccess, let cache else {
            FrameFlipper.buildError = "テクスチャの入れ物を作れない"
            return nil
        }
        // 写し先の入れ物（エンコーダに渡す物。IOSurface で、Metal からも書ける）
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey as String: w,
            kCVPixelBufferHeightKey as String: h,
            kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
            kCVPixelBufferMetalCompatibilityKey as String: true,
        ]
        var pl: CVPixelBufferPool?
        guard CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pl) == kCVReturnSuccess, let pl else {
            FrameFlipper.buildError = "絵の入れ物を作れない"
            return nil
        }
        self.w = w; self.h = h; self.flipX = flipX; self.flipY = flipY
        self.device = g.device; self.queue = g.queue; self.pso = g.pso
        self.texCache = cache; self.pool = pl
    }

    // src を反転して、新しい入れ物に写して返す。できなければ nil（lastError に理由）
    func flip(_ src: CVPixelBuffer) -> CVPixelBuffer? {
        guard CVPixelBufferGetWidth(src) == w, CVPixelBufferGetHeight(src) == h, CVPixelBufferGetPlaneCount(src) == 2 else {
            lastError = "カメラの絵が想定と違う（\(CVPixelBufferGetWidth(src))×\(CVPixelBufferGetHeight(src))・面 \(CVPixelBufferGetPlaneCount(src))・形式 \(CVPixelBufferGetPixelFormatType(src))。想定 \(w)×\(h)）"
            return nil
        }
        var made: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &made) == kCVReturnSuccess, let out = made else { lastError = "写し先の入れ物が取れない"; return nil }
        CVBufferPropagateAttachments(src, out) // 色の決まり（BT.709 など）を写す。エンコーダが映像の印に書く
        guard let surface = CVPixelBufferGetIOSurface(out)?.takeUnretainedValue() else { lastError = "写し先が IOSurface ではない"; return nil }
        guard let cb = queue.makeCommandBuffer(), let ce = cb.makeComputeCommandEncoder() else { lastError = "命令を作れない"; return nil }
        ce.setComputePipelineState(pso)
        var fl = SIMD2<UInt32>(flipX ? 1 : 0, flipY ? 1 : 0)
        ce.setBytes(&fl, length: MemoryLayout<SIMD2<UInt32>>.size, index: 0)
        let tw = pso.threadExecutionWidth
        let th = max(1, pso.maxTotalThreadsPerThreadgroup / tw)
        var holds: [CVMetalTexture] = [] // GPU が終わるまで、カメラの絵のテクスチャを手放さない
        for plane in 0..<2 {
            let fmt: MTLPixelFormat = plane == 0 ? .r8Unorm : .rg8Unorm
            let pw = CVPixelBufferGetWidthOfPlane(src, plane), ph = CVPixelBufferGetHeightOfPlane(src, plane)
            var cvTex: CVMetalTexture?
            guard CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, texCache, src, nil, fmt, pw, ph, plane, &cvTex) == kCVReturnSuccess,
                  let ct = cvTex, let srcTex = CVMetalTextureGetTexture(ct) else {
                ce.endEncoding()
                lastError = "カメラの絵をテクスチャにできない（面 \(plane)）"
                return nil
            }
            holds.append(ct)
            let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: fmt, width: pw, height: ph, mipmapped: false)
            desc.usage = .shaderWrite
            guard let dstTex = device.makeTexture(descriptor: desc, iosurface: surface, plane: plane) else {
                ce.endEncoding()
                lastError = "写し先をテクスチャにできない（面 \(plane)）"
                return nil
            }
            ce.setTexture(srcTex, index: 0)
            ce.setTexture(dstTex, index: 1)
            ce.dispatchThreadgroups(MTLSize(width: (pw + tw - 1) / tw, height: (ph + th - 1) / th, depth: 1),
                                    threadsPerThreadgroup: MTLSize(width: tw, height: th, depth: 1))
        }
        ce.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        withExtendedLifetime(holds) {}
        CVMetalTextureCacheFlush(texCache, 0)
        guard cb.status == .completed else { lastError = "GPU の処理が終わらなかった（状態 \(cb.status.rawValue)・\(String(describing: cb.error))）"; return nil }
        return out
    }
}
