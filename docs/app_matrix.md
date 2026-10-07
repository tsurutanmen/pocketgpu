# 試したアプリの表

ドライバ越しに動かした Windows のアプリと結果。改造はしていない（DXUT のサンプルだけ、横の DXVK を読むよう 1 か所直して組んだ）。
- 代役：PC の RTX 3060 を受け手にしたもの
- iPhone のふり：代役の受け手に `VKR_LIKE_IOS=1`（BC・geometry shader・cull distance・logicOp・shaderFloat64・variableMultisampleRate・空の descriptor を持たないふり、キューの種類は 1 つ、テッセレーションの設定が無い pipeline を作らない。10/7 から DXVK が記録する機能 52 項目が iPhone 17 Pro と同じ）
- iPhone：iPhone 17 Pro（A19 Pro）・アプリ 0.7.23・USB
- 絵の比べ方：ふつうの DirectX か RTX 直との差。PSNR 40dB 以上なら見た目では区別できない

最終更新 2026-10-07

数（10/7 夜）：代役か iPhone のふりで動いた物 29 本（Vulkan 9・DirectX 11 20）。iPhone の実機で動いた物 17 本（Vulkan 9・DirectX 11 8）。目標は 50 本

## Vulkan のアプリ
| アプリ | 代役 | iPhone | 数字（iPhone） |
|---|---|---|---|
| vulkaninfo | ✅ | ✅ | Vulkan 1.3 |
| vkcube | ✅ | ✅ | 500×500 353.7 コマ/秒・1280×720 244.8 |
| 三角形 tri・tri13 | ✅ | ✅ | Apple の丸めで ±1 |
| 足し算 vecadd | ✅ | ⚠️ 失敗が 1 回あった。0.7.23 の通しの試験では 1 回目から正しい | 代役 3 種類では出ない。次の通しで記録を取る |
| llama.cpp（Qwen3 0.6B〜8B） | ✅ | ✅ | 0.6B 67 t/s・8B 12.3 t/s |
| whisper.cpp | ✅ | ✅ | 2 分の音声 43.8 秒 |
| stable-diffusion.cpp | ✅ | ✅ | 1 枚 66.5 秒 |
| vkQuake 1.36.0＋LibreQuake（本物のゲーム） | ✅ RTX 直 584.9・ふり 159.6 | ✅ 1280×720 timedemo demo1 | 149.6 コマ/秒（10/7） |

## DirectX 11（DXVK-macOS 1.10.3 と DXVK 3.1.1）
| アプリ | 代役 | iPhone のふり | iPhone | 数字（iPhone） |
|---|---|---|---|---|
| d3d11tri（自作） | ✅ 同じ | ✅ | ✅ | 画素数は同じ |
| d3d11cube（自作） | ✅ 同じ | ✅ | ✅ | DXVK-macOS 260・DXVK 3 246 コマ/秒 |
| d3d11cube bc1（自作） | ✅ 同じ | ✅ ドライバでほどく | ✅ GPU が読む | 234・226 コマ/秒 |
| EmptyProject11 | ✅ 同じ | ✅ 同じ | ✅ 同じ | 305 |
| SimpleSample11 | ✅ 同じ | ✅ 同じ | ✅ 65.6dB | 292 |
| BasicHLSL11 | ✅ 同じ | ✅ 同じ | ✅ 60.2dB | 312 |
| CascadedShadowMaps11 | ✅ 58dB | ✅ 56.5dB | ✅ 47.6dB | 99 |
| VarianceShadows11 | ✅ 58dB | ✅ 56.5dB | 未 | |
| HDRToneMappingCS11 | ✅ 同じ | ✅ 同じ | ✅ 56.5dB | 156 |
| FluidCS11 | ✅ 同じ | ✅ geometry shader を真似て同じ | ❌ 粒が消える（0.7.23） | 真似る作りはドライバだけなので、次に iPhone で試す |
| SimpleBezier11 | ✅ 同じ | ✅ 直したドライバで同じ | ❌ 曲面が消える | テッセレーションの点の数の送り忘れ。直し済みで受け手の組み直し待ち |
| OIT11 | ✅ 同じ | ✅ 同じ | ✅ 64.2dB | 11.4 |
| NBodyGravityCS11 | ✅ 粒の置き方は毎回違う | ✅ | 未 | |
| Tutorial01〜07 | ✅ | ✅ 同じ（04・07 は時刻で回るぶん） | 未 | |
| BasicCompute11（窓の無い計算） | ✅ CPU の答えと同じ | ✅ DXVK 2 種類とも同じ | 未 | |
| DynamicShaderLinkage11 | ふつうの DirectX でも動かない | | | |
| MultithreadedRendering11 | 素材が足りない | | | |

## 組んであるがまだ試していない物
- WebDriverAgent（iPhone のアプリを Windows から操作する⑥用）
- マイクラのサーバー Pumpkin（アプリ 0.7.23 に入っている）

## 次に試す候補（取ってくる許可待ち）
- GZDoom 4.14.2 ＋ Freedoom（Vulkan で描く。必須の機能 4 つは iPhone も持っている）
- OpenGL を zink で（docs/opengl_zink_plan.md・出るのは OpenGL 2.1 まで）
- Unigine Heaven と Valley（DirectX 11・docs/dx11_games_candidates.md）
- Godot 4.7 の見本（Mobile の描き方・Vulkan を名前で選ぶ）
