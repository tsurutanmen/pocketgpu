# MetalFX 調べ物メモ（iOS 26・受け手は A19 Pro）

調査日: 2026-10-10
対象: 空間拡大、時間拡大、フレーム補間、ノイズ除去（denoiser）
方法: ウェブ検索とウェブ取得だけで調べた。git、iPhone、USB、GPU には触れていない。

## 取り方と注意

- Apple の公式ページは、通常の URL では題名しか取れないものがあった。`.md` を付けた URL（例 https://developer.apple.com/documentation/metalfx/mtlfxframeinterpolatorbase.md ）で本文を取った。本文の文言はこの方法で取った確認済みのもの。
- WWDC の本文は、動画ページを取得して要約させた結果。引用はその要約の文言なので、正確な語句は動画で確かめること。
- Metal Feature Set Tables（対応チップの表、PDF）は、調査の担当は圧縮されていて読めなかった。後から別の手段でテキストにして読み、MetalFX の 4 行の最小 GPU 世代を表に足した（出典は各行に記載）。
- 二次情報（記事、開発者フォーラムの投稿）は、本文ではなく検索結果の要約で見たものもある。その旨を各行に書いた。

## 記号の定義

- 〇 ＝ 動きの向きと深度を渡さなくても、色だけで使える。
- △ ＝ 条件つきで使える。受け手の描画の作り替え、ジッターの扱い、テクスチャの用途の設定などが要る。
- ✕ ＝ 動きの向き、深度、または材料情報が必須で、受け手の今の作りでは使えない。
- 確認度「確認済み」＝ Apple の公式ページか WWDC の本文で直接確かめた。「推測」＝ 推論、または二次情報だけに基づく。

## 機能の表

| 機能 | 必要な入力（必須・任意） | 対応チップと OS | 遅れの目安 | 受け手で使えるか | 理由 | 確認度 | 出典 URL |
|---|---|---|---|---|---|---|---|
| 空間拡大（MTLFXSpatialScaler） | 必須: colorTexture（低解像度の色）、inputContentWidth と inputContentHeight、outputTexture。深度と動きは不要。任意: 特になし | iOS 16.0 以降（公式の availability）。GPU の世代は Apple3 以降（A9 以降）で、A19 Pro（Apple10）は含まれる（Apple の Metal Feature Set Tables（2026年5月21日付）https://developer.apple.com/metal/capabilities/）。実行時に supportsDevice で確認する | 公式の数字は見つからなかった。概要の図に「about half the time」とあるが、説明用の図で計測値ではない | 〇 | 色だけで動く。ただし、低解像度で描いた絵を受け手が作る必要がある | 確認済み（入力・OS・GPU の世代）。遅れは未確認 | https://developer.apple.com/documentation/metalfx.md ／ https://developer.apple.com/documentation/metalfx/mtlfxspatialscalerbase.md ／ https://developer.apple.com/documentation/metalfx/mtlfxspatialscaler.md |
| 時間拡大（MTLFXTemporalScaler） | 必須（概要の文言から）: colorTexture、depthTexture、motionTexture、outputTexture、inputContentWidth と inputContentHeight。任意: reactiveMaskTexture（反応マスク）、exposureTexture（isAutoExposureEnabled で MetalFX に露出を計算させられる）、reset（履歴を捨てる）。jitterOffsetX と jitterOffsetY はプロパティがあるが、必須かどうかは書かれていない | iOS 16.0 以降（公式の availability）。GPU の世代は Apple7 以降（A14 以降）で、A19 Pro（Apple10）は含まれる（Apple の Metal Feature Set Tables（2026年5月21日付）https://developer.apple.com/metal/capabilities/）。supportsDevice で実行時に確認する | 公式の数字は見つからなかった。検索の要約で、2023 年のフォーラムの投稿に「M1 Pro で約 10 ms 追加」とあったが、原文は未取得で、macOS の話 | ✕ | Apple の概要に「深度と動きの情報を渡せるなら temporal、無ければ spatial」とある。受け手は両方を持たない | 確認済み（条件の読み取りと GPU の世代）。必須性の厳密な扱いは推測 | https://developer.apple.com/documentation/metalfx.md ／ https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase.md ／ https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerdescriptor.md ／ 逸話の出どころ（未取得）: https://developer.apple.com/forums/thread/728545 |
| フレーム補間（MTLFXFrameInterpolator） | 必須（WWDC25 #211 の本文の要約より）: 現在の色（colorTexture）、前のフレームの色（prevColorTexture）、motionTexture（動きの向き）、depthTexture（深度）、outputTexture。任意: uiTexture（UI を別に渡す場合）。isUITextureComposited で UI が色に含まれているかを指定する。カメラ情報（viewToClipMatrix、worldToViewMatrix、nearPlane、farPlane、fieldOfView）と deltaTime は、正しくないと崩れると本文にある | iOS 26.0.0 以降（公式の availability）。GPU の世代は Apple5 以降（A12 以降）で、A19 Pro（Apple10）は含まれる（Apple の Metal Feature Set Tables（2026年5月21日付）https://developer.apple.com/metal/capabilities/。機種表の列は「機能・Metal の版・最小の GPU 世代」で、Metal 3 & 4 と Apple5 は矛盾しない）。実行時に supportsDevice（Metal 4 用は supportsMetal4FX）で確認する | 公式の数字は見つからなかった。WWDC25 では補間の前に「minimum 30 frames per second」を勧めている（要約）。二次情報の記事は入力遅延が増えると書く | ✕ | 動きの向きと深度が必須と本文にある。受け手は両方を持たない | 確認済み（API・OS・GPU の世代）。必須入力は WWDC の要約。遅れは未確認 | https://developer.apple.com/documentation/metalfx/mtlfxframeinterpolatorbase.md ／ https://developer.apple.com/documentation/metalfx/mtlfxframeinterpolatordescriptor.md ／ https://developer.apple.com/documentation/metalfx/mtlfxframeinterpolator.md ／ https://developer.apple.com/videos/play/wwdc2025/211/ |
| 時間拡大＋ノイズ除去（MTLFXTemporalDenoisedScaler。Metal 4 版は MTL4FXTemporalDenoisedScaler） | 必須（WWDC25 #211 の要約より）: 時間拡大の入力（色、深度、動き）に加えて、diffuseAlbedoTexture、normalTexture、roughnessTexture、specularAlbedoTexture。任意: specularHitDistanceTexture、denoiseStrengthMaskTexture、transparencyOverlayTexture | iOS 26.0.0 以降（base protocol の公式 availability）。GPU の世代は Apple9 以降（A17 Pro 以降）で、A19 Pro（Apple10）は含まれる（Apple の Metal Feature Set Tables（2026年5月21日付）https://developer.apple.com/metal/capabilities/） | 公式の数字は見つからなかった | ✕ | レイトレーシング向けの材料情報（albedo、法線、粗さ）が要る。受け手の Vulkan の命令列からは取れないと思われる | 確認済み（入力・OS・GPU の世代）。受け手で取れないは推測 | https://developer.apple.com/documentation/metalfx/mtlfxtemporaldenoisedscalerbase.md ／ https://developer.apple.com/documentation/metalfx/mtl4fxtemporaldenoisedscaler.md ／ https://developer.apple.com/videos/play/wwdc2025/211/ |
| MetalFX Denoising（WWDC26 #359。ニューラルの denoise と upscale を一緒に行う） | 必須（WWDC26 #359 の要約より）: 1 サンプル毎ピクセルのノイズのある色、diffuse albedo、depth、ジッターを除いた動きの向き（dejittered motion vectors）。任意: 透明度のオーバーレイ、denoiser の強さのマスク、reactive mask | OS と API 名は未確認。動画の要約には版の記載が無い | 数字は見つからなかった | ✕ | 動きの向き、深度、材料情報が要る。受け手は持たない | 確認済み（入力の要約）。API 名と OS は未確認 | https://developer.apple.com/videos/play/wwdc2026/359/ |

## 大事な問い への答え

1. 動きの向きと深度が無くても使えるのは、空間拡大（〇）だけ。
2. 時間拡大は、Apple の概要が深度と動きを前提にしている。素直に読めば、動きの向きと深度が無いと使えない。
3. フレーム補間も、WWDC25 の本文で動きの向きと深度が必須とされている。受け手では使えない。
4. 推測（受け手側で埋める余地）:
   - 深度: ゲームが描いた深度バッファが Vulkan の render pass の中に残っていれば、受け手が読める可能性がある。MoltenVK 経由で MetalFX に渡せるかは未確認。
   - 動きの向き: 前のフレームの位置が分からないと作れない。頂点シェーダへの入力（前フレームの行列など）を追えば理論上は作れるが、Vulkan の命令列だけでは確実ではない。
   - ジッター: 受け手は投影行列を変えられないので、ジッター無しになる可能性が高い。ジッター無しでの時間拡大が成り立つかは、Apple の文書では確かめられなかった。
   - 動きの向きを 0 で代用すると、動くものがぼける可能性が高い。

## 結論（4K・毎秒 120 コマに向けて）

- いま使えるのは、空間拡大（MTLFXSpatialScaler）だけ。色だけで動き、iOS 16 以降の公式の availability がある。
- 時間拡大、フレーム補間、denoiser は、受け手が動きの向きと深度を用意できるまで使えない（✕）。
- 4K で毎秒 120 コマが空間拡大で間に合うかは、公式の数字が無いので判断できない。実機で測るしかない（確認済み: 公式の数字は見つからなかった）。
- 推測: フレーム補間で 120 を作るなら、元の描画が 60 fps 前後必要になる。WWDC25 の「30 fps 以上」の勧めからの推測。

## 読み切れなかったところ

- Metal Feature Set Tables の MetalFX の行は、後から読んで表に足した。ただし「最小の GPU 世代」が分かっただけで、A19 Pro で各機能が実際に動くか（supportsDevice の値）は実機でしか分からない。WWDC26 #359 の機能は、機種表の行に対応するものが見つかっていない。
- 遅れの公式の数字。どの機能についても見つからなかった。
- motionVectorScaleX と motionVectorScaleY の単位と符号。depth の形式の細部。ジッターの数列。該当ページは取得できなかった（404 または題名のみ）。
- フレーム補間が、時間拡大のスケーラー無しで単独で動くか。descriptor の scaler は任意として宣言されているが、動作条件の記述は見つからなかった。
- WWDC26 #359 の対応 OS と API 名。
- 検索の要約で見た、フォーラムの「M1 Pro で約 10 ms」の原文。未取得。
- MoltenVK が深度と動きのテクスチャを MetalFX に渡せるか。調べていない。
- Metal 4 版（MTL4FX*）を受け手が使うべきか。MoltenVK が Metal 4 の命令バッファを使うかは不明。
- supportsDevice の実際の値（A19 Pro）。実機でしか分からない。

## 二次情報（参考。公式ではない）

- 9to5Mac（https://9to5mac.com/?p=1006734 ）: フレーム補間は 1 枚の追加を生成する、と説明している。ノイズ除去はレイトレーシング向け。対応機種の記述は無し。
- PC Games Hardware（https://www.pcgameshardware.de/Grafikkarten-Grafikkarte-97980/News/Metal-4-mit-MetalFX-Frame-Interpolation-und-Denoising-1474693/ ）: 1 枚の追加を生成し、複数枚の生成は無い、と書いている。
- Wccftech（https://wccftech.com/apple-metal-4-api-adds-interpolation-to-boost-gaming-performance/amp/ ）: 入力遅延が増えると書いている。基礎のフレームレートが高いことが必要という論調。
- 注意: 検索の要約にあった「Apple5 / Apple9 / Apple10」の記述は、機能ごとの最小 GPU 世代（補間は Apple5、ノイズ除去つき拡大は Apple9）が混ざったものだった。機種表（Apple 公式）で確かめた値を表に採った。
