# iPhone の GPU で試す Vulkan のゲームの候補（2026-10-06・助手の調べ）

おすすめの順
1. vkQuake 1.36.0（GPL-2.0）＋シェアウェア版の Quake データ（pak0.pak・無料）。必須はスワップチェーンだけ。Windows 用 zip 5.5MB。`timedemo demo1` が「N frames X seconds Y fps」を出す。自動では終わらないので、`-condebug` の qconsole.log を待って止める。下書き：build-vkquake-windows.yml・run_vkquake_timedemo.ps1（本家は v145・windows-2025-vs2026。v143 で通るかは未確認）
2. Sascha Willems の Vulkan サンプル集（MIT）。`-b`（benchmark）`-bfs` `-br` で測って自動で終わり CSV に書く。geometry shader・tessellation・ray tracing のサンプルは外す
3. UZDoom / GZDoom（GPL-3.0）＋Freedoom（無料・24MB）。必須は samplerAnisotropy・fragmentStoresAndAtomics・multiDrawIndirect・independentBlend。macOS 版は MoltenVK を同梱。`+timedemo demo1` で測って終わる
4. Godot 4.7.2（MIT）＋godot-demo-projects の squash_the_creeps など。`--quit-after N` `--print-fps` `--write-movie` `--rendering-method mobile`。公式バイナリをそのまま使える（86MB）
5. SuperTuxKart 1.5（Vulkan は実験的）。iOS 版は MoltenVK で配布されている唯一の実績。`--render-driver=vulkan` `--benchmark`。723MB と大きい

補欠：Khronos Vulkan-Samples（Apache-2.0・`--benchmark` `--stop-after-frame` `--screenshot` が全部ある・1.1 以上必須）、Quake3e（データ有料）
除外：Luanti・Unvanquished・Xonotic（OpenGL）、RBDOOM-3-BFG・VulkanMod（データ有料）、Quake II RTX（重い）

iPhone 側の制約：geometry shader 無し・ray tracing 無し・descriptor indexing と buffer device address は argument buffers の段で条件つき・tessellation は確かでない
ローダー：VK_DRIVER_FILES（VK_ICD_FILENAMES は古い別名）。管理者権限のアプリでは無視される
