# DX12 FL 12_0 と OpenGL 3.3 を MoltenVK（iOS）で名乗るための調査

調査日: 2026-10-10
対象: vkd3d-proton（D3D12 → Vulkan）、Zink（OpenGL → Vulkan、Mesa）、MoltenVK（iOS、Metal 上の Vulkan）
実行したこと: ウェブ検索とウェブ取得のみ。git、iPhone、USB、GPU には触れていない。

## 読み方

- 「確認度」の「確認済み」は、一次情報（公式ドキュメントやソース）を取得して記述を読んだもの。
- 「確認済み（要約経由）」は、ウェブ取得の要約器を通して読んだもの。原文の行番号は未確認。
- 「推測」は、関連する記述から筋道で導いたか、一次情報が取れなかったもの。
- 「出典 URL」の欄は、その行の根拠を置いた URL。「記載なし」の行は、読んだ文書に記述が無かったことを根拠にしている。
- 出典ごとの取得方法は末尾の「出典一覧」にまとめた。

> **点検で追記（2026-10-10）**：調査の担当は、原文の一部を「要約器」を通してしか読めていませんでした。点検で、次のソースを自分で取得して全文を読み（実行はしていない）、核の主張を確かめ直しました。表の「確認済み（原文のソース）」の行は、その結果です。
> - vkd3d-proton の `libs/vkd3d/device.c`（master。約 497,000 文字の全体）と README
> - MoltenVK の `MoltenVK/GPUObjects/MVKDevice.mm`（main。約 265,000 文字の全体）と `Docs/MoltenVK_Runtime_UserGuide.md`
> 行番号は、取得した版のもの。版が変わると動く。結論は 7 章と 9 章を見る。

## 1. 機能ごとの表

列: 機能／何のために要るか／要る・要らない／MoltenVK（iOS）で出る・出ない・一部／真似る方法／確認度／出典 URL

| 機能 | 何のために要るか | 要る・要らない | MoltenVK（iOS）で出る・出ない・一部 | 真似る方法 | 確認度 | 出典 URL |
|---|---|---|---|---|---|---|
| Vulkan 1.3 以上 | vkd3d-proton の最低要件（README） | 要る | 一部（MoltenVK の README は「完全準拠ではない」と明記。Vulkan 1.4 の「ほぼ完全な部分集合」と書く） | 上位版なので対象外 | 確認済み | https://github.com/HansKristian-Work/vkd3d-proton ／ https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/README.md |
| descriptor indexing（UpdateAfterBind 100 万以上、UniformBuffer 以外の全型。VkPhysicalDeviceDescriptorIndexingFeatures の大半） | vkd3d-proton の最低要件。12_0 の ResourceBindingTier 2 にも関係すると推測 | 要る | 一部。user guide は「初期版は Metal Tier 1 に限定（テクスチャ 96/128、サンプラ 16）」と書き、macOS 11 以降などの例外と argument buffer の設定に条件を付けている。iOS でどこまで出るかは文から読み切れない | argument buffer を使う設定にする（user guide の条件に従う） | 確認済み（記述の存在）。機能の充足は推測 | https://github.com/HansKristian-Work/vkd3d-proton ／ https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md |
| samplerMirrorClampToEdge（VK_KHR_sampler_mirror_clamp_to_edge） | vkd3d-proton の最低要件 | 要る | 出る（条件付き）。Mac GPU または Apple family 7 が必要と書かれている。A19 Pro は family 7 以上と推測 | 不要 | 確認済み | https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md |
| VK_EXT_robustness2（robustBufferAccess2、robustImageAccess2、nullDescriptor） | vkd3d-proton は 3 つとも必須。1 つでも無いとデバイス作成が失敗する（E_INVALIDARG。device.c 2639〜2651 行） | 要る（致命的） | 一部。拡張は掲載あり（user guide）。ただし MoltenVK は robustBufferAccess2 = false、nullDescriptor = false、robustImageAccess2 = Apple GPU のとき true（MVKDevice.mm 635〜638 行）。つまり 2 つが足りない | vkd3d-proton を直して必須から外す（パッチ）、または MoltenVK に実装する。ほかの事業が同じ穴をどう埋めたかは見つからなかった | 確認済み（原文のソース。両方の側） | https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/device.c ／ https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md |
| VK_KHR_push_descriptor | vkd3d-proton の最低要件 | 要る | 掲載あり（拡張一覧に名前があり、注記は無い） | なし | 確認済み（掲載の事実） | https://github.com/HansKristian-Work/vkd3d-proton ／ https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md |
| sparseResidencyBuffer、sparseResidencyImage2D、sparseResidencyAliased、residencyNonResidentStrict（sparseBinding を含む） | 12_0 の条件 TiledResourcesTier 2 の前提。sparse 系が 1 つでも欠けると TiledResourcesTier は NOT_SUPPORTED になる（vkd3d-proton の device.c 8938〜8962 行） | 要る（12_0 の条件）。ただし欠けてもデバイス作成は失敗しない。sparse を無効にして先に進む（device.c 2471〜2474 行） | 出ない。MoltenVK の initFeatures（MVKDevice.mm 2796〜2866 行）に sparse 系の設定が 1 つも無く、すべて false。sparseAddressSpaceSize も 0（3146 行） | 「名乗るだけ」なら環境変数 VKD3D_FEATURE_LEVEL（9 章）で TiledResourcesTier 2 を強制できる。実際に sparse を使うゲームは動かない。Metal 側の sparse heap を Vulkan の sparse に結びつける実装は見つからなかった | 確認済み（原文のソース） | https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/device.c ／ https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm |
| TypedUAVLoadAdditionalFormats | 12_0 の条件（device.c の要約） | 要る（12_0） | 不明（MoltenVK の文書に記載なし） | 不明 | 推測 | https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/libs/vkd3d/device.c |
| OutputMergerLogicOp（logicOp） | 11_1 の条件。11_1 は 12_0 の前提（device.c 9652 行）。D3D12 の OutputMergerLogicOp は Vulkan の logicOp 機能そのもの（device.c 9263 行） | 要る（11_1 以上） | 設定しだい。MoltenVK は logicOp = useMetalPrivateAPI（MVKDevice.mm 2802 行）。MVK_CONFIG_USE_METAL_PRIVATE_API が真のときだけ出る。既定値は未確認 | MoltenVK を Metal の非公開 API つきでビルドして設定を有効にする。または VKD3D_FEATURE_LEVEL で強制する（9 章） | 確認済み（原文のソース）。既定値は未確認 | https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/device.c ／ https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm |
| vertexPipelineStoresAndAtomics | 11_1 の条件（device.c 9652 行） | 要る（11_1 以上） | 出る。MoltenVK は true を返す（MVKDevice.mm 2819 行） | 不要 | 確認済み（原文のソース） | https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/device.c ／ https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm |
| ROV（VK_EXT_fragment_shader_interlock 系。fragmentShaderInterlock） | 12_1 の条件（device.c 9663 行） | 要る（12_1） | 一部。MoltenVK は fragmentShaderSampleInterlock と fragmentShaderPixelInterlock を、Metal の raster order groups に対応する機器でだけ true にする（MVKDevice.mm 738〜741 行）。A19 Pro で対応するかは未確認 | raster order groups で ROV を代用する（MoltenVK がすでに行っている） | 確認済み（原文のソース）。A19 Pro の値は未確認 | https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/device.c |
| Conservative rasterization（VK_EXT_conservative_rasterization。ConservativeRasterizationTier 1 以上） | 12_1 の条件（device.c 9663 行） | 要る（12_1） | 出ない見込み。user guide の拡張一覧に VK_EXT_conservative_rasterization が無い。動的状態の conservative rasterization mode は false（MVKDevice.mm 716 行） | 不明 | 確認済み（不在の確認） | https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md ／ https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/device.c |
| shaderBufferInt64Atomics、vulkanMemoryModel、drawIndirectCount（Vulkan 1.2 の一部） | vkd3d-proton の 12_2 では Int64ShaderOps などが条件。12_0 には要らない見込み（推測） | 12_2 のみ要る見込み（推測） | 出ない（2020〜2022 年の issue #1567 の時点。shaderBufferInt64Atomics は #1692 も参照） | 不明 | 確認済み（時点付き） | https://github.com/KhronosGroup/MoltenVK/issues/1567 |
| samplerFilterMinmax（VK_EXT_sampler_filter_minmax） | 要否は未確認 | 未確認 | 一部。user guide は「Apple GPU family 10 と macOS 26 / iOS 26 が必要」と書く。issue #1567 の時点では出ないとされていた。時期で記述が変わっている | 不明 | 確認済み（記述の存在）。A19 Pro が条件を満たすかは推測 | https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md ／ https://github.com/KhronosGroup/MoltenVK/issues/1567 |
| VK_EXT_transform_feedback（xfb_features.transformFeedback、xfb_properties.transformFeedbackQueries） | vkd3d-proton は transformFeedbackQueries が無いとデバイス作成が失敗する（"Lacking support for transform feedback"、E_INVALIDARG。device.c 2502〜2506 行）。Zink は GL 3.0 の要件（Mesa の zink ドキュメント。要約経由） | 要る（vkd3d-proton は致命的。Zink は GL 3.0） | 出ない。MoltenVK の文書にも MVKDevice.mm にも transform feedback の機能構造体の処理が無い。issue #1588 は 2026 年の取得時点でも open | 頂点ステージの出力を storage buffer に書き出す（Ryujinx の方式。geometry と tessellation を使わない描画に限られる）。compute で代替する案も issue 内で提案されている。実装は MoltenVK の側に作るか、vkd3d-proton の側で必須から外すかのどちらか | 確認済み（原文のソース。出ないことは不在の確認） | https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/device.c ／ https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://github.com/KhronosGroup/MoltenVK/issues/1588 ／ https://docs.mesa3d.org/drivers/zink.html |
| geometryShader | Zink の GL 3.2 の要件。vkd3d-proton の 12_0 の条件には出てこない | GL 3.2: 要る。12_0: 要らない | 出ない。MoltenVK の initFeatures に geometryShader の設定が無い（MVKDevice.mm 2796〜2866 行）。Metal に geometry 段が無い | compute や mesh での代替が要る。MoltenVK の外側に層を作る以外の道は見つからなかった（Apple の Game Porting Toolkit は geometry shader を Metal 3 向けに翻訳した、と WWDC23 の講演にある。要約経由） | 確認済み（原文のソース。不在の確認） | https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://docs.mesa3d.org/drivers/zink.html ／ https://developer.apple.com/videos/play/wwdc2023/10123/ |
| tessellationShader | Zink の GL 4.0 の要件。GL 3.3 には要らない | GL 4.0: 要る。GL 3.3: 要らない | 出る。MoltenVK は Apple GPU の Apple3 以降（A9 以降）と Mac で true にする（MVKDevice.mm 2838〜2842 行、2858〜2862 行）。A19 Pro は含まれる | 不要 | 確認済み（原文のソース） | https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://docs.mesa3d.org/drivers/zink.html |
| maxPerStageDescriptorSamplers が 16 以上 | Zink の GL 3.1 の要件 | 要る（GL 3.1） | 推測で出る。user guide の descriptor indexing の上限に「16 samplers」とあるため、16 は確保できそうとの推測 | 不要 | 推測 | https://docs.mesa3d.org/drivers/zink.html ／ https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md |
| depthClamp、VK_EXT_depth_clip_enable | Zink の GL 3.2 の要件 | 要る（GL 3.2） | 出る。depthClamp は true（MVKDevice.mm 2828 行）。VK_EXT_depth_clip_enable は user guide の拡張一覧に掲載あり | 不要 | 確認済み（原文のソースと user guide） | https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md |
| VK_EXT_conditional_rendering | Zink の GL 3.0 の要件（Mesa の zink ドキュメント。要約経由） | 要る（GL 3.0） | 出ない。MoltenVK の user guide にも MVKDevice.mm にも conditionalRendering の記述が無い | Zink 側で条件つき描画を使わない経路があるかは未確認 | 確認済み（MoltenVK 側の不在）。Zink の要件は要約経由 | https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md ／ https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://docs.mesa3d.org/drivers/zink.html |
| independentBlend | Zink の GL 3.0 の要件 | 要る（GL 3.0） | 出る。true（MVKDevice.mm 2800 行） | 不要 | 確認済み（原文のソース） | https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://docs.mesa3d.org/drivers/zink.html |
| dualSrcBlend | Zink の GL 3.3 の要件 | 要る（GL 3.3） | 出る。true（MVKDevice.mm 2827 行） | 不要 | 確認済み（原文のソース） | https://github.com/KhronosGroup/MoltenVK/blob/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm ／ https://docs.mesa3d.org/drivers/zink.html |
| VK_EXT_vertex_attribute_divisor（vertexAttributeInstanceRateDivisor と vertexAttributeInstanceRateZeroDivisor） | Zink の GL 3.3 の要件。vkd3d-proton は両方の機能を必須にしている（device.c 2495〜2500 行。無いとデバイス作成が失敗） | 要る（GL 3.3。vkd3d-proton は致命的） | 拡張は user guide の一覧に掲載あり（VK_EXT と VK_KHR の両方）。ただし 2 つの機能が両方 true かは未確認 | 不明 | 確認済み（掲載）。機能の中身は未確認 | https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md ／ https://github.com/HansKristian-Work/vkd3d-proton/blob/master/libs/vkd3d/device.c ／ https://docs.mesa3d.org/drivers/zink.html |

行数: 22 行（機能の数。ヘッダーを除く）。点検（2026-10-10）で、原文のソースを読んで 13 行を書き直した

## 2. Zink の GL 3.0〜4.0 の段（Mesa の zink ドキュメントの要点）

出典: https://docs.mesa3d.org/drivers/zink.html（取得済み。要約器経由）。同じ内容が https://kusma.pages.freedesktop.org/mesa/drivers/zink.html（別ミラー）でも確認できた。

- GL 2.1（最低限）: Vulkan 1.0。logicOp、fillModeNonSolid、alphaToOne、shaderClipDistance の機能。拡張は VK_KHR_maintenance1、VK_KHR_maintenance5、VK_KHR_create_renderpass2、VK_KHR_imageless_framebuffer、VK_KHR_timeline_semaphore、VK_EXT_custom_border_color、VK_EXT_provoking_vertex、VK_EXT_line_rasterization など。
- GL 3.0: independentBlend。VK_EXT_transform_feedback、VK_EXT_conditional_rendering。
- GL 3.1: maxPerStageDescriptorSamplers が 16 以上。
- GL 3.2: depthClamp、geometryShader、shaderTessellationAndGeometryPointSize。VK_EXT_depth_clip_enable。
- GL 3.3: dualSrcBlend。VK_EXT_vertex_attribute_divisor。（検索の要約では、古い版は occlusionQueryPrecise を挙げていた。版の違いは未確認）
- GL 4.0: sampleRateShading、tessellationShader、imageCubeArray。VK_KHR_maintenance2。VK_FORMAT_R32G32B32 系の UNIFORM_TEXEL_BUFFER。
- 検証の注記: GL 2.1 の要件はすべてが検証されるわけではない。GL 3.2 と 3.3 は実行時に検証されていない項目がありうる、と書かれている。
- macOS と MoltenVK: 「Apple macOS and MoltenVK」の節があり、macOS の Zink は実験的で機能が非常に限られると明記。ビルドには Vulkan SDK 1.3.250 以降が要る。iOS への言及は無い。

## 3. vkd3d-proton の feature level 判定（device.c の要約）

出典: https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/libs/vkd3d/device.c（要約器経由。約 497,000 文字のうち 0〜400,000 文字目を読み、残りは未読）

- 関数 d3d12_device_caps_init_feature_level が caps->max_feature_level を決める。初期値は 11_0。
- 11_1: OutputMergerLogicOp、vertexPipelineStoresAndAtomics、maxPerStageDescriptorStorageBuffers と maxPerStageDescriptorStorageImages が D3D12_UAV_SLOT_COUNT 以上。
- 12_0: 11_1 を満たし、TiledResourcesTier 2 以上、ResourceBindingTier 2 以上、TypedUAVLoadAdditionalFormats。
- 12_1: 12_0 を満たし、ROVsSupported、ConservativeRasterizationTier 1 以上。
- 12_2: 12_1 を満たし、Shader Model 6.5 以上、Int64ShaderOps、WaveOps、ResourceBindingTier 3、TiledResourcesTier 3 など多数。
- TiledResourcesTier の判定（d3d12_device_determine_tiled_resources_tier）は sparseBinding、sparseResidencyAliased、sparseResidencyBuffer、sparseResidencyImage2D を参照する。
- vkd3d_init_device_caps の要約では、sparseResidencyBuffer、sparseResidencyImage2D、residencyNonResidentStrict が無いと sparse を無効化する。robustBufferAccess2、robustImageAccess2、nullDescriptor、samplerMirrorClampToEdge、shaderDrawParameters を必須チェックに使う。
- 要約は xfb_properties.transformFeedbackQueries と vertex divisor の機能も必須チェックに使うと読めた。README の必須リストには無いので、原文で確かめる必要がある。
- geometryShader と tessellationShader は、読んだ範囲の caps 条件には出てこない（multiviewGeometryShader のみ）。
- 12_2 の条件に VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation があり、GS エミュレーションを前提にしている名残と読める（推測）。

## 4. 他の事業は MoltenVK の穴をどう埋めているか

- Ryujinx（エミュレータ）: 頂点ステージの最後で、transform feedback の出力を storage buffer に書く。GS と tessellation を使わない描画でしか成り立たない。出典: https://github.com/KhronosGroup/MoltenVK/issues/1588 （issue のコメントの引用。二次情報）
- LunarG の KosmicKrisp（Mesa 系の Vulkan-on-Metal）: macOS の M1 以降向け。iOS は対象外と XDC 2025 の資料にある。Vulkan 1.3 の CTS 合格を LunarG が主張。SPIRV-Cross ではなく Mesa の NIR を使う。出典: https://www.lunarg.com/lunarg-achieves-vulkan-1-3-conformance-with-kosmickrisp-on-apple-silicon/ 、https://www.lunarg.com/wp-content/uploads/2025/10/XDC-2025-KosmicKrisp-Overview.pdf（いずれも検索の要約のみ）。iPhone では使えないので、iOS に関しては手がかりにならない。
- Asahi の Honeykrisp（Apple GPU の Linux 向け Vulkan）: Vulkan 1.3 conformant と記事にある。対象は M1 と記事で明記。border colour をシェーダで補正、compute でコピー、YCbCr をソフトウェアで実装、と書かれている。transform feedback は「今後の課題」。出典: https://asahilinux.org/2024/06/vk13-on-the-m1-in-1-month/ （取得済み。要約経由）。Mesa の meson_options.txt の注記は「後の版で geometry shader、tessellation、transform feedback を追加」と述べるが、検索結果の要約のみで未取得（推測。出典: https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/meson_options.txt ）。
- Apple の Game Porting Toolkit（D3DMetal）: 実装の内部は公開されていない。WWDC23 の講演で、サンプルゲームの geometry shader を Metal 3 に翻訳した、と話している（検索の要約。出典: https://developer.apple.com/videos/play/wwdc2023/10123/ ）。transform feedback の扱いは不明。D3D12 の feature level の報告が足りず落ちるという開発者フォーラムの報告がある（二次情報。https://developer.apple.com/forums/thread/774169 ）。GPTK 4 の D3DMetal 4 の話は二次情報で未確認。
- DXVK: 一部の処理で geometry shader を使わないようにしている（VK_EXT_shader_viewport_index_layer があれば）と、検索の要約にある。これは MoltenVK 向けの対応とされる。一次の release notes は未取得。
- Metal に geometry 段が無いこと: Unity フォーラムと Blender の変更（https://projects.blender.org/blender/blender/pulls/105114 ）で、Apple デバイスは geometry shader を持たないとされる（二次情報）。Blender は UV の辺の描画を compute 的に代替している。Apple 開発者フォーラムの回答では、layered rendering が geometry の用途の一部を代替する（https://developer.apple.com/forums/thread/3566 、二次情報）。
- VMware Fusion の 2017 年のプレビュー: Metal 上で DirectX 10（geometry shader を含む）をエミュレートしたと、フォーラムにある（二次情報。https://moltengl.com/forums/topic/vmware-tech-preview-2017-supports-dx10-via-metal-so-geometry-shaders-also ）。

## 5. 確かめた事実（一次情報から）

- vkd3d-proton の README: 必須は Vulkan 1.3、descriptor indexing 100 万以上、VkPhysicalDeviceDescriptorIndexingFeatures の大半、samplerMirrorClampToEdge、shaderDrawParameters、VK_EXT_robustness2、VK_KHR_push_descriptor。推奨は VK_EXT_image_view_min_lod など。README の対応プラットフォームは Windows と Linux だけで、MoltenVK と macOS への言及は無い。feature level 12_0 への言及も無い。
- MoltenVK の README: 「Vulkan 1.4 のグラフィックスとコンピュートのほぼ完全な部分集合」「完全準拠ではない」。
- MoltenVK の user guide: 実行条件は macOS 12.0、iOS 15、tvOS 15 以上。geometry shader、tessellation、transform feedback、sparse binding、sparse residency、dualSrcBlend、independentBlend の記述は無い。「Known MoltenVK Limitations」という節はあるが、読んだ範囲に該当項目は無い。
- MoltenVK の issue #1588（VK_EXT_transform_feedback の要望）: 2022-05 に起票、2026 年の取得時点でも open。担当者もマイルストーンも PR も無い。
- Zink の GL 段の要件は上の 2 節のとおり。Mesa の zink ドキュメントは macOS 向けに実験的と明記。

## 6. 推測（一次情報が無い、または要約経由）

- MoltenVK に sparse（sparseBinding と sparseResidency 系）が無いこと。文書に記述が無いことからの推測。Apple 側は sparse textures の機能があるが、対応する家族は PDF から読めなかった。
- MoltenVK に transform feedback が無いこと。文書に記述が無く、issue が open のまま。
- MoltenVK に geometry shader が無いこと。文書には記述が無いが、二次情報の複数が「無い」と書いている。
- vkd3d-proton が VK_EXT_transform_feedback を必須にしているか。要約器の読み取りでそう読めたが、README の必須リストには無い。
- 12_0 の TiledResourcesTier 2 に sparse が要ることは、要約器の読み取りによる。原文の行は未確認。

## 7. 結論（点検で書き直し。原文のソースによる）

- **いちばん手前の壁は、feature level の 12_0 ではなく、デバイスの作成そのもの**（確認済み・原文）。vkd3d-proton は、次のどれかが欠けるとデバイスの作成が失敗する（E_INVALIDARG）。MoltenVK の上流のままでは、1 と 2 は欠ける。
  1. transform feedback（transformFeedbackQueries）。device.c 2502〜2506 行。MoltenVK には無い。
  2. VK_EXT_robustness2 の robustBufferAccess2（false）と nullDescriptor（false）。device.c 2639〜2651 行。MoltenVK は MVKDevice.mm 635〜638 行で false にしている。
  3. （要確認）vertexAttributeInstanceRateDivisor と ZeroDivisor。device.c 2495〜2500 行。MoltenVK が両方 true かは未確認。
- **この壁を越えない限り、12_0 の話には入れない。** iPhone で今 vkd3d-proton が一部でも動いているなら、そちらの build は、これらの検査をパッチしてあるはず。その場合、上の「欠ける」は、パッチの内容しだいで変わる。（私は、そちらの build の中身を見ていない。）
- **その先の 12_0 の条件**（device.c 9652〜9661 行、確認済み）：11_1 を満たし、TiledResourcesTier 2 以上、ResourceBindingTier 2 以上、TypedUAVLoadAdditionalFormats。
  - 11_1 の条件：OutputMergerLogicOp（= Vulkan の logicOp）、vertexPipelineStoresAndAtomics、ストレージの上限。MoltenVK の logicOp は useMetalPrivateAPI しだい（既定値は未確認）。vertexPipelineStoresAndAtomics は出る。
  - TiledResourcesTier 2：sparse 系（sparseBinding、sparseResidencyAliased、sparseResidencyBuffer、sparseResidencyImage2D）が全部要る。MoltenVK は 1 つも出さない。sparse が無くてもデバイス作成は失敗せず、TiledResourcesTier が NOT_SUPPORTED になり、feature level が 11_x で止まる。
  - 「名乗るだけ」なら、環境変数 VKD3D_FEATURE_LEVEL（9 章）で強制できる。
- **Zink の OpenGL 3.x の壁**（MoltenVK 側は確認済み・原文。Zink の要件は要約経由）：
  - GL 3.0：transform feedback と VK_EXT_conditional_rendering が要る。MoltenVK はどちらも出さない。independentBlend は出る。
  - GL 3.2：geometryShader が要る。MoltenVK は出さない。depthClamp と VK_EXT_depth_clip_enable は出る。
  - GL 3.3：dualSrcBlend は出る。VK_EXT_vertex_attribute_divisor は拡張が掲載されている。
  - つまり、GL 3.0 の壁が 2 つ（transform feedback、conditional rendering）、GL 3.2 の壁が 1 つ（geometryShader）。
- **いちばん手前で効くもの**（推測。実装の手間は見積もっていない）：
  1. vkd3d-proton 側：transform feedback と robustness2 の致命的な検査を、MoltenVK で足りる範囲に直す（パッチ）。検査を外した場合に、実際にどのゲームが壊れるかは未確認。
  2. MoltenVK 側：transform feedback を実装する。頂点ステージの出力を storage buffer に書き出す方式が、ほかの事業（Ryujinx）で使われている。geometry と tessellation を使う描画では成り立たない。
  3. 12_0 を名乗るだけでよいゲームには、VKD3D_FEATURE_LEVEL で足りる可能性がある。
- **次に確かめること**（私は実行していない）：対象の iPhone 上で vkGetPhysicalDeviceFeatures2 と拡張一覧を出し、この章に出た機能の有無を確かめる。MoltenVK のバージョンと、MVK_CONFIG_USE_METAL_PRIVATE_API の値も記録する。

## 8. 読み切れなかったところ

1. Mesa の features 一覧（https://docs.mesa3d.org/features.html は 404。https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/docs/features.txt は Anubis で取得失敗）。OpenGL 3.0〜3.3 の各機能の zink 列の状態は未確認。
2. Mesa の zink.rst の GitLab 生ファイル（Anubis で取得失敗）。ミラーと docs.mesa3d.org で内容は揃っていたが、一次の GitLab 版は未確認。
3. vkd3d-proton の device.c の残り（約 97,000 文字）。feature level の最終的な設定箇所の全体と、geometryShader・xfb・sparseBinding の原文での使われ方は未確認。
4. vkd3d-proton の README の要件節の残りと、原文での xfb の扱い。
5. MoltenVK の user guide の拡張一覧の全体。independentBlend、dualSrcBlend、VK_EXT_vertex_attribute_divisor、VK_EXT_conditional_rendering、depthClamp、VK_EXT_depth_clip_enable、logicOp、vertexPipelineStoresAndAtomics、fragmentShaderInterlock、conservative rasterization、VK_EXT_custom_border_color などの扱い。
6. Apple の Metal Feature Set Tables（PDF の○が抽出できなかった）。sparse textures と sparse heap の家族ごとの対応。sparse buffer の扱い。Apple の sparse の文書（取得時はタイトルのみ）。
7. MoltenVK の tessellation の実装状況。検索の要約では対応とあったが一次の記述は読めなかった。
8. MoltenVK の geometry shader の現状（2025 年以降の変化）。二次情報は 2018〜2023 年のものだけ。
9. D3DMetal の feature level の報告値（12_0、12_1、12_2 のどれを名乗るか）。Apple の一次資料は見つからなかった。
10. DXVK の release notes（geometry shader を避ける条件の一次情報）。
11. Honeykrisp の geometry shader と transform feedback の実装方法（emulation か hardware か）。Mesa の meson_options の注記は未取得。
12. KosmicKrisp の geometry shader と transform feedback の扱い。
13. Vulkan の Conservative rasterization と fragment shader interlock の Metal 側の対応（Metal の raster order group と conservative rasterization の対応関係を一次で確認できなかった）。

## 9. VKD3D_FEATURE_LEVEL という環境変数（点検で見つけた。確認済み・原文）

- vkd3d-proton の device.c の d3d12_device_caps_override（9944〜10012 行。環境変数を読むのは 9965 行）に、環境変数 `VKD3D_FEATURE_LEVEL` を読む処理がある。値は 11_0、11_1、12_0、12_1、12_2 のどれか。
- 設定すると、**下の Vulkan の機能を検査せずに**、D3D12 の caps を上書きして max_feature_level をその値にする。
  - 11_1 以上：OutputMergerLogicOp を TRUE
  - 12_0 以上：TiledResourcesTier を 2 以上、ResourceBindingTier を 2 以上、TypedUAVLoadAdditionalFormats を TRUE、shader model を 6.0 以上
  - 12_1 以上：ROVsSupported を TRUE、ConservativeRasterizationTier を 1 以上
  - 12_2 以上：レイトレーシング・可変レートシェーディング・メッシュシェーダなども
- README にはこの変数の説明が無い（README の全文を grep して確認）。
- **これは「何と名乗るか」を変えるだけ**で、機能を足さない。sparse を実際に使うゲームは、名乗っても動かない。デバイス作成の致命的な検査（7 章の 1〜3）は、これでは越えられない（別の場所にある）。
- 使いどころ（推測）：feature level を確認して落ちるだけで、実際には sparse を使わないゲームには効く可能性がある。どれほどのゲームが当てはまるかは、調べていない。

## 出典一覧（取得方法つき）

| URL | 種類 | 取得方法 |
|---|---|---|
| https://github.com/HansKristian-Work/vkd3d-proton | 一次（README） | 直接取得（要約器経由） |
| https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/libs/vkd3d/device.c | 一次（ソース） | 直接取得（要約器経由。0〜400,000 文字を読んだ。残りは未読） |
| https://docs.mesa3d.org/drivers/zink.html | 一次（Mesa docs） | 直接取得（要約器経由） |
| https://kusma.pages.freedesktop.org/mesa/drivers/zink.html | 一次（Mesa docs の別ミラー） | 直接取得（要約器経由） |
| https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/README.md | 一次（MoltenVK） | 直接取得（要約器経由） |
| https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md | 一次（MoltenVK） | 直接取得（要約器経由） |
| https://github.com/KhronosGroup/MoltenVK/issues/1588 | 一次（MoltenVK の issue） | 直接取得（要約器経由） |
| https://github.com/KhronosGroup/MoltenVK/issues/1567 | 一次（MoltenVK の issue） | 検索結果の要約のみ |
| https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf | 一次（Apple） | 検索結果の要約のみ（○は抽出できず） |
| https://developer.apple.com/documentation/metal/creating-sparse-heaps-and-sparse-textures | 一次（Apple） | 取得したがタイトルのみ |
| https://asahilinux.org/2024/06/vk13-on-the-m1-in-1-month/ | 二次（Asahi の開発記事） | 直接取得（要約器経由） |
| https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/meson_options.txt | 一次（Mesa） | 検索結果の要約のみ |
| https://www.lunarg.com/lunarg-achieves-vulkan-1-3-conformance-with-kosmickrisp-on-apple-silicon/ | 二次（LunarG の発表） | 検索結果の要約のみ |
| https://www.lunarg.com/wp-content/uploads/2025/10/XDC-2025-KosmicKrisp-Overview.pdf | 二次（LunarG のスライド） | 検索結果の要約のみ |
| https://developer.apple.com/videos/play/wwdc2023/10123/ | 一次（Apple の講演） | 検索結果の要約のみ |
| https://developer.apple.com/forums/thread/774169 | 二次（開発者フォーラム） | 検索結果の要約のみ |
| https://developer.apple.com/forums/thread/3566 | 二次（開発者フォーラム） | 検索結果の要約のみ |
| https://developer.apple.com/jp/videos/play/tech-talks/605/?time=96 | 一次（Apple の講演） | 検索結果の要約のみ |
| https://projects.blender.org/blender/blender/pulls/105114 | 二次（Blender の変更） | 検索結果の要約のみ |
| https://gitlab.winehq.org/wine/vkd3d/-/merge_requests/1182 | 二次（Wine の vkd3d の変更） | 検索結果の要約のみ |
| https://moltengl.com/forums/topic/vmware-tech-preview-2017-supports-dx10-via-metal-so-geometry-shaders-also | 二次（フォーラム） | 検索結果の要約のみ |
| https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_fragment_shader_interlock.html | 一次（Khronos の仕様） | 検索結果のみ（中身は未取得） |
| https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_conservative_rasterization.html | 一次（Khronos の仕様） | 検索結果のみ（中身は未取得） |
| https://docs.mesa3d.org/features.html | 一次（Mesa） | 404 |
| https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/docs/features.txt | 一次（Mesa） | 取得失敗（Anubis） |
| https://gitlab.freedesktop.org/mesa/mesa/-/raw/main/docs/drivers/zink.rst | 一次（Mesa） | 取得失敗（Anubis） |
| https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/libs/vkd3d/device.c | 一次（ソース・全文） | 点検で全文を取得して読んだ（grep と該当行の通読。実行はしていない） |
| https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm | 一次（ソース・全文） | 点検で全文を取得して読んだ |
| https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/Docs/MoltenVK_Runtime_UserGuide.md | 一次（MoltenVK の文書・全文） | 点検で全文を取得して読んだ |
| https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/master/README.md | 一次（README・全文） | 点検で全文を取得して読んだ |
