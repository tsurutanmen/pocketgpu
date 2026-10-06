# 新しい DXVK をこのドライバ越しに動かす計画

調べた日は 2026-10-06。読む・調べる・下書きだけをした。D:\iphone-rpc\app の中は書き換えていない。git も触っていない。

## 0. 先に結論

1. 新しい DXVK の最新は 2.x ではなく 3.1.1（2026-09-15）。2.x の最後は 2.7.1（2025-08-30）。依頼の 16 個は 3.x の必須に一致する。2.7.1 では inlineUniformBlock・depthClipEnable・maintenance6・load_store_op_none は必須でない。master（2026-10-05）の必須は 3.1.1 と同じ 53 項目だった。
2. 16 個のうち、MoltenVK 1.4.2 が持っていて ICD の絞り込みで見えなくなっているだけの物は 11 個。直す所は icd.c の数行から数十行。
3. MoltenVK 1.4.2 が持たない物は 3 個。depthClipEnable と robustBufferAccess2 と nullDescriptor。どれも ICD の中の代役で済ませる案を書いた。
4. occlusionQueryPrecise はコード上は解決済み。確かめた根拠は 3 節の 1。
5. 依頼に無かった必須で、MoltenVK 1.4.2 が持たない物が 2 個ある。shaderCullDistance は持たない。textureCompressionBC は端末しだいで、iPhone では false の公算が大きい。geometryShader と合わせた 3 個は MoltenVK に無い。偽って名乗って使われたら失敗させるか、DXVK を 1 行ずつ直して必須から外すしかない。
6. bufferDeviceAddress は思ったほど重くない。DXVK 3.1.1 は、MoltenVK の条件では shader の中でアドレスをたどらない。根拠は 4 節。必要なのは、機能と入口とメモリの印と、受け手の本物のアドレスを返す往復 1 つ。
7. 新しい命令は 1 つだけ。VR_GET_BUFFER_ADDRESS。ほかは末尾に足す運び方か、ドライバの中で済む。
8. 新しい物を名乗る入口は、環境変数 IPHONE_VK_DXVK3=1 のときだけ開く形にする。llama.cpp などの今動いている使い方を変えないため。

## 1. DXVK の必須の一覧

出典はすべて https://github.com/doitsujin/dxvk の v3.1.1 タグ。

- Vulkan の版は 1.3。src/dxvk/dxvk_instance.h:12 の DxvkVulkanApiVersion。判定は src/dxvk/dxvk_device_info.cpp:746-751。
- 機能と拡張の一覧は同じファイルの getFeatureList、825-1092 行。末尾が true の物が必須。判定は checkDeviceCompatibility の 756-765 行で、足りないと装置を使わない。
- 非 heap の経路では push 定数は 256 バイト要る。767 行。MoltenVK は 4096 なので足りる。

必須は 53 項目。機能 46 と拡張つき 7。数字は device_info.cpp の行。

| 区分 | 項目と行 |
|---|---|
| 1.0 の機能 22 | depthBiasClamp 826、depthClamp 828、dualSrcBlend 830、fillModeNonSolid 831、fragmentStoresAndAtomics 832、fullDrawIndexUint32 833、geometryShader 834、imageCubeArray 835、independentBlend 836、multiDrawIndirect 839、multiViewport 840、occlusionQueryPrecise 841、robustBufferAccess 843、sampleRateShading 844、samplerAnisotropy 845、shaderClipDistance 846、shaderCullDistance 847、shaderImageGatherExtended 849、shaderInt16 850、shaderInt64 851、shaderSampledImageArrayDynamicIndexing 853、textureCompressionBC 868 |
| 1.1 の機能 2 | shaderDrawParameters 873、storageBuffer16BitAccess 874 |
| 1.2 の機能 14 | bufferDeviceAddress 877、descriptorIndexing 878、storageBuffer8BitAccess 879、descriptorBindingSampledImageUpdateAfterBind 889、descriptorBindingUpdateUnusedWhilePending 890、descriptorBindingPartiallyBound 891、hostQueryReset 893、runtimeDescriptorArray 894、samplerMirrorClampToEdge 896、scalarBlockLayout 897、shaderInt8 899、timelineSemaphore 902、uniformBufferStandardLayout 903、vulkanMemoryModel 904 |
| 1.3 の機能 8 | inlineUniformBlock 906、computeFullSubgroups 907、dynamicRendering 908、maintenance4 909、shaderDemoteToHelperInvocation 912、shaderZeroInitializeWorkgroupMemory 913、subgroupSizeControl 914、synchronization2 915 |
| 拡張つき 7 | VK_EXT_depth_clip_enable の depthClipEnable 932、VK_EXT_robustness2 の robustBufferAccess2 988 と nullDescriptor 990、VK_KHR_load_store_op_none 1032、VK_KHR_maintenance5 1035、VK_KHR_maintenance6 1036、VK_KHR_swapchain 1062 |

descriptorIndexing まわりの 5 つは、descriptorIndexing、descriptorBindingSampledImageUpdateAfterBind、descriptorBindingUpdateUnusedWhilePending、descriptorBindingPartiallyBound、runtimeDescriptorArray。icd.c の 874-893 行はこの 5 つを 1 つも通していないので、今は全部 false で見える。

load_store_op_none は DXVK が名前を KHR で要る。依頼文は EXT と書いてあったが、3.1.1 の 1032 行は VK_KHR_load_store_op_none。MoltenVK は KHR と EXT の両方を持つ。

swapchain の必須は VK_KHR_swapchain だけ。ICD は icd.c:466 で VK_KHR_swapchain の版 70 をもう名乗っている。名前の違いが問題になる所は見つからなかった。近い話は 3 つある。

- 任意の VK_KHR_swapchain_maintenance1 と VK_EXT_swapchain_maintenance1。両方あると DXVK は KHR を使う。device_info.cpp:342-357。ICD はどちらも名乗らないので、DXVK は使わない。
- instance の VK_KHR_surface_maintenance1 と VK_EXT_surface_maintenance1。同じく片方だけ使う。dxvk_instance.cpp:162-164。どちらも任意。
- 依頼文が指す物が別にあるなら、確かでない。

2.7.1 との差。2.7.1 の getFeatureList は dxvk_device_info.cpp の 709-885 行付近。

- 2.7.1 で必須でなく 3.1.1 で必須になった物。inlineUniformBlock、depthClipEnable、maintenance6、load_store_op_none、shaderInt16、shaderInt8、fragmentStoresAndAtomics、samplerAnisotropy、storageBuffer16BitAccess、storageBuffer8BitAccess、scalarBlockLayout、computeFullSubgroups、subgroupSizeControl。
- 2.7.1 で必須で 3.1.1 で外れた物。拡張 VK_KHR_pipeline_library。2.7.1 の 878 行。MoltenVK は持たないので、2.7.1 を狙うなら ICD が名前だけ名乗る必要がある。
- ほかの必須は同じ。BDA、descriptorIndexing の 5 つ、robustness2 の 2 つ、maintenance5 は 2.7.1 でも必須。

## 2. ICD と受け手の今

### 2.1 ICD（icd.c）

名乗るか。

- 拡張は受け手の一覧を allowed_ext の許可表で絞って見せる。384-410 行。許可表にあるのは 28 個と、caps つきの 6 個。ほかに ICD の中だけで VK_KHR_swapchain と VK_KHR_copy_commands2 を足す。466-468 行。
- 機能。1.0 は受け手の値をそのまま見せるが、sparse 系と pipelineStatisticsQuery と inheritedQueries は false にする。occlusionQueryPrecise は VR_CAP_QUERY があれば通す。415-421 行。
- 1.1 は multiview 系と protectedMemory と samplerYcbcrConversion だけ false。870-873 行。
- 1.2 は memset で全部 0 にして、中継できる 15 個と、caps があるときの hostQueryReset だけ戻す。874-893 行。bufferDeviceAddress と samplerMirrorClampToEdge と descriptorIndexing 系は戻さないので false。
- 1.3 も同じ作り。894-911 行。inlineUniformBlock は 898 行の注釈どおり戻さない。
- 版は 1.3。受け手が VR_CAP_VK13 を名乗ったときだけ。475-477 行。

受け手に送るか。

- 機能と性質の問い合わせは、FEAT_ST と PROP_ST に載っている sType だけ受け手に尋ねる。813-860 行と 952-973 行。載っていない構造体は触らない。つまり DXVK が鎖につなぐ VkPhysicalDeviceRobustness2FeaturesEXT や VkPhysicalDeviceDepthClipEnableFeaturesEXT は、今は 0 のまま返る。
- vkCreateDevice は拡張名を allowed_ext に通った物だけ、機能の鎖を FEAT_ST にある物だけ送る。1131-1166 行。ほかは捨てる。これは今回の代役に都合がいい。名乗りだけの機能が受け手の装置作りを失敗させない。
- 1.3 に向けて足した機能の構造体は、全部 1 にした写しを mask_ext_features に通して「中継できる機能」の印を作り、重ねてから送る。1150-1162 行。

pNext の扱い。

| 入口 | 今の扱い |
|---|---|
| vkAllocateMemory 1222 | pNext を見ない。VkMemoryAllocateFlagsInfo は落ちる |
| vkCreateBuffer 1311 | pNext を見ない。usage はそのまま送る。SHADER_DEVICE_ADDRESS の印は届く |
| vkCreateImage 1383 | pNext を見ない |
| vkCreateSampler 1811 | pNext を見ない |
| vkCreateGraphicsPipelines 1703 | pNext は VkPipelineRenderingCreateInfo だけ、末尾に付ける。1789-1801 行。rasterization 状態の pNext と stage の pNext は見ない |
| vkCreateComputePipelines 2317 | stage の pNext は required subgroup size だけ |
| vkCreateDescriptorSetLayout 2262 | pNext を見ない。binding flags は落ちる。layout の flags は送る |
| vkCreateDescriptorPool 2370 | pNext を見ない。pool の flags は送る |
| vkUpdateDescriptorSets 2495 | pNext を見ない。inline uniform block は飛ばす。copy は飛ばす |
| vkQueueSubmit 3607 | pNext は timeline だけ |
| vkCreateRenderPass2 1577 | depth stencil resolve と MEMORY_BARRIER_2 だけ |

入口の表。

- まだ無い入口は 3966-3971 行の NOT_YET。vkGetBufferDeviceAddress、vkGetBufferOpaqueCaptureAddress、vkGetDeviceMemoryOpaqueCaptureAddress、vkCmdDrawIndirectCount、vkCmdDrawIndexedIndirectCount など。呼ばれると名前を出して FEATURE_NOT_PRESENT を返す。
- lookup_device 3984-4222 に無い物。DXVK 3.1.1 が名前で引く入口のうち、vkCmdBindDescriptorSets2、vkCmdPushConstants2、vkCmdBindIndexBuffer2、vkGetRenderingAreaGranularity、vkGetImageSubresourceLayout2、vkGetDeviceImageSubresourceLayout。すべて KHR 名でも引かれる。出典は src/vulkan/vulkan_loader.h の 487-501 行。
- 同期は shadow 方式。GPU が読む範囲は descriptor・vertex・index・copy から推し量って submit の前に送る。書く範囲は同じ見立てで、待ちのあとに引き取る。icd.c の頭の注釈と 3611-3622 行。アドレスで直接たどる読み書きは見えない。

### 2.2 受け手（server.c）

- caps の決め方。detect_caps の 210-291 行。VR_CAP_DELTA・CMD10・QUERY は 1.0 の入口だけで済むので常に付く。237 行。
- VR_HELLO は装置の性質と拡張の一覧をそのまま返す。1371-1410 行。
- VR_CREATE_DEVICE は届いた機能の鎖を中身ごと vkCreateDevice に渡す。1470-1488 行。拡張名は届いた物に VK_KHR_portability_subset を足す。MoltenVK のとき。
- VR_CREATE_BUFFER は flags・size・usage だけ。1505-1523 行。VR_ALLOC_MEMORY は size と種類だけ。1531-1560 行。
- VR_CREATE_DSL は binding の 4 つの値だけ読む。1721-1737 行。
- VR_UPDATE_DESCRIPTOR_SETS は、書き換える set を使っている出し済みの仕事まで待ってから書く。1884 行。
- VR_CREATE_GRAPHICS_PIPELINE は末尾の [sType][中身] の並びを読む。2535-2554 行。いまは VkPipelineRenderingCreateInfo だけ読む。
- 知らない命令が来ると 2822 行で 0 を返して接続を切る。だから新しい命令は caps の印で名乗ったときだけ使う決まりは守る。
- vertex buffer と index buffer は null をそのまま MoltenVK に渡す。930-947 行と 1097-1108 行。

## 3. 足りない物ひとつずつ

見方。手間は小・中・大。効き目は DXVK 3.1.1 が装置を作れるか、描けるかへの効き。

### 1. occlusionQueryPrecise

結論。解決済み。確認だけ。

- ICD。icd.c:420 は VR_CAP_QUERY があれば受け手の値を通す。
- 受け手。server.c:237 が VR_CAP_QUERY を常に付ける。VR_CMD_BEGIN_QUERY の flags はそのまま vkCmdBeginQuery に渡る。1120-1124 行。
- MoltenVK。Apple 3 以上の GPU で true。MVKDevice.mm:2801-2803。A19 Pro は Apple 10。
- 測った記録。D:\iphone-rpc\vkbuild\d3d11\dxvk_icd.log の DXVK 1.10.3 の装置情報に occlusionQueryPrecise が 1 と出ている。caps 0x3ff。ただし代役の RTX 3060 越し。一方 D:\iphone-rpc\vkbuild\delta_runs\vulkaninfo_v13.txt は query を足す前の DLL で、false のまま。
- 確かでない。いまの DLL を iPhone 越しで vulkaninfo にかけた記録は無い。
- 手間 なし。効き目 必須の 1 つが通る。

### 2. bufferDeviceAddress

詳しくは 4 節。要点だけここに書く。

- DXVK。必須。877 行。すべての buffer に SHADER_DEVICE_ADDRESS の印を付ける。dxvk_buffer.cpp:30-31。buffer をメモリにつないだ直後に vkGetBufferDeviceAddress を呼ぶ。dxvk_memory.cpp:1108-1109 と 1433-1434。メモリは VkMemoryAllocateFlagsInfo の DEVICE_ADDRESS を付けて取る。dxvk_memory.cpp:1385-1389。
- MoltenVK。持つ。iOS 16 以上で機能が true。MVKFoundation.cpp:139 と MVKDevice.mm:214。拡張は Tier 2 の argument buffers が要る。MVKDevice.mm:3562-3566。address は MTLBuffer の gpuAddress に offset を足した値。MVKBuffer.mm:172-173。
- ドライバで足す物。
  - mask_ext_features の 1.2 の枝に bufferDeviceAddress を戻す。874-893 行。
  - vkGetBufferDeviceAddress と KHR 名と EXT 名の入口を作る。3970 行の NOT_YET から外す。受け手に往復して本物のアドレスをもらい、struct buf に覚える。
  - alloc_memory で VkMemoryAllocateFlagsInfo を読み、flags を VR_ALLOC_MEMORY の末尾に付ける。
  - vkGetBufferOpaqueCaptureAddress と vkGetDeviceMemoryOpaqueCaptureAddress は 0 を返す。
- 受け手で足す物。新しい命令 VR_GET_BUFFER_ADDRESS。VR_ALLOC_MEMORY の末尾の flags を読んで VkMemoryAllocateFlagsInfo をつなぐ。detect_caps で VR_CAP_BDA を名乗る。
- 手間 中。同期の安全装置まで入れると大。
- 効き目 必須の 1 つが通る。

### 3. descriptorIndexing まわり 5 つ

- DXVK。1.2 の 5 つは必須。なぜ要るか。legacy の経路で、全 sampler を入れた 1 つの descriptor set を持つ。dxvk_sampler.cpp:303-362。種類は VK_DESCRIPTOR_TYPE_SAMPLER で 2048 個の配列。dxvk_sampler.h:375。binding flags に UPDATE_AFTER_BIND と UPDATE_UNUSED_WHILE_PENDING と PARTIALLY_BOUND を付ける。layout に UPDATE_AFTER_BIND_POOL、pool に UPDATE_AFTER_BIND を付ける。binding flags を使うのは、探した範囲ではこの 1 か所だけだった。ほかの set は普通の作りで、描画のたびに取り直して書く。dxvk_context.cpp:7087-7298。
- MoltenVK。5 つとも持つ。descriptorIndexing は arrayOfTextures と arrayOfSamplers で決まる。Apple 3 以上の GPU で true。MVKDevice.mm:2544-2557 と 2834。A19 Pro は Apple 10。残り 4 つは定数 true。MVKDevice.mm:197、202-205。
- ドライバで足す物。
  - 1.2 の枝に 5 つを戻す。874-893 行。
  - create_dsl 2262-2280 行で VkDescriptorSetLayoutBindingFlagsCreateInfo を読み、binding ごとの flags を VR_CREATE_DSL の末尾に付ける。
- 受け手で足す物。server.c:1721-1737 で末尾の flags を読み、VkDescriptorSetLayoutBindingFlagsCreateInfo を pNext につなぐ。caps は VR_CAP_DESC_BINDING を新設。
- 順番の注意。まず末尾の運びを省いて 5 つの印だけで試す手もある。MoltenVK 1.4.2 は set の中身を GPU の buffer に直接書く作りで、書き換えは実行時に読まれる。1.4.2 の MVKDescriptorSet.mm:1085-1120 付近。確かでない。省いて動くかは実機で見ないと分からない。
- 性能の注意。sampler 置き場を書くたびに、server.c:1884 が、その set を使っている出し済みの仕事を全部待つ。ほぼ全 submit が置き場を使うので、sampler を作るたびに GPU を待つことになる。サンプラーの作成は少ないので最初は許す。後で、binding flags に UPDATE_UNUSED_WHILE_PENDING がある set は待たない形にする。
- 確かでない。置き場は 2048 個。MoltenVK が Tier 2 で答える maxDescriptorSetUpdateAfterBindSamplers は MTLDevice の maxArgumentBufferSamplerCount。MVKDevice.mm:863、870。A19 Pro の値は確認していない。2048 に届かないと置き場の作成が通らない恐れがある。
- 手間 機能の印だけなら小。binding flags の運びまで入れて中。
- 効き目 必須の 5 つが通る。

### 4. samplerMirrorClampToEdge

- DXVK。必須。896 行。拡張 VK_KHR_sampler_mirror_clamp_to_edge は ICD がもう中継している。icd.c:393。足りないのは 1.2 の機能の印だけ。
- MoltenVK。持つ。Apple 7 以上で true。MVKDevice.mm:176、2584-2589。拡張の表は MVKExtensions.def:98。
- ドライバで足す物。874-893 行に f->samplerMirrorClampToEdge = k.samplerMirrorClampToEdge を 1 行。create_device は 1.2 の鎖を中身ごと送るので、ほかは要らない。create_sampler は構造体を flags 以降そのまま送るので addressMode の値は届く。1817 行。
- 受け手 変更なし。
- 手間 小。効き目 必須の 1 つが通る。

### 5. inlineUniformBlock

- DXVK。必須。906 行。ただし descriptor の種類 VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK を使うのは descriptor buffer の経路だけ。dxvk_pipemanager.cpp:424-430。legacy の経路では普通の uniform buffer を使う。つまり印だけで足りる。
- MoltenVK。持つ。MVKDevice.mm:230。
- ドライバで足す物。894-911 行の 1.3 の枝に f->inlineUniformBlock = k.inlineUniformBlock を 1 行。898 行の注釈は直す。VkWriteDescriptorSetInlineUniformBlock の書き込みは今回は飛ばしたままでよい。DXVK 3.1.1 は使わない。
- 手間 小。効き目 必須の 1 つが通る。
- 注意。ほかのアプリが inline uniform block を使うと、書き込みが飛ぶ。名乗りは IPHONE_VK_DXVK3 のときだけにする。

### 6. depthClipEnable

- DXVK。必須。932 行。使い方が重要。3.1.1 は pipeline の rasterization 状態で、いつも depthClampEnable を true にして、VkPipelineRasterizationDepthClipStateCreateInfoEXT の depthClipEnable に D3D の設定を入れる。dxvk_graphics.cpp:585 と 591-592。2.7.1 は拡張が無いときだけ depthClampEnable を depthClipEnable の逆にしていた。2.7.1 の dxvk_graphics.cpp:569-576。そこに、これは正確ではない、という DXVK 自身のコメントがある。
- いまの ICD は rasterization 状態の pNext を見ないので、depthClampEnable の true だけが届く。つまり常に「クリップしない」になり、近い面をまたぐ図形が間違って描かれる。名乗りだけでは描画が壊れる。
- MoltenVK。1.4.2 の拡張表に無い。main に 2026-08-24 の commit「feat: implement VK_EXT_depth_clip_enable」で入った。リリースはまだ出ていない。確かでない。後のリリースに入るかは知らない。ios.yml は 1.4.2 の配布物を使う。
- ドライバで足す物。ドライバの中だけで済む。DXVK 2.7.1 が自分でやっていた代役と同じ。
  1. hello() の 466-468 行に、ICD の中だけの拡張として VK_EXT_depth_clip_enable の版 1 を足す。
  2. get_features2 の query_chain のあとで、鎖の中の VkPhysicalDeviceDepthClipEnableFeaturesEXT の depthClipEnable を ICD が true にする。
  3. create_graphics_pipelines の 1743 行。rs の pNext から VkPipelineRasterizationDepthClipStateCreateInfoEXT を探し、あれば depthClampEnable として受け手に送る値を !depthClipEnable に置き換える。
  4. この構造体は FEAT_ST に載せないので、create_device で受け手に送られない。
- 受け手 変更なし。
- 注意。この代役は DXVK 2.7.1 が拡張なしの装置でやっていた物と同じで、完全には正確でない。DepthClipEnable が false の図形は depthClampEnable を true で送るので、深度が範囲に切り詰められる。D3D11 も同じ動きのはず。DepthClipEnable が true の図形は depthClampEnable を false で送るので、普通のクリップになる。大きなずれは出ない見込みだが、確かでない。
- 手間 小。効き目 必須の 1 つが通り、描画の正しさにも効く。

### 7. robustness2 の robustBufferAccess2

- DXVK。必須。988 行。実際に使われる場所は機能の判定だけ。DXVK 3.1.1 の legacy の経路で、robustness2 の値を実行中に読む所は見つからなかった。
- MoltenVK。持たない。false。MVKDevice.mm:636。拡張は持つ。MVKExtensions.def:171。一方で通常の robustBufferAccess は true で、Apple の GPU は完全にサポートする、という注釈がある。MVKDevice.mm:2761。
- ドライバで足す物。偽って名乗る。
  1. 拡張 VK_EXT_robustness2 の版 1 を ICD の中だけで名乗る。
  2. get_features2 の query_chain のあとで、VkPhysicalDeviceRobustness2FeaturesEXT の robustBufferAccess2 を true にする。robustImageAccess2 は false のまま。DXVK は任意。989 行。
  3. VkPhysicalDeviceRobustness2PropertiesEXT は PROP_ST に足して受け手の値を通す。MoltenVK は 1 と 1。MVKDevice.mm:1286-1287。
  4. この構造体は FEAT_ST に載せない。受け手の MoltenVK は未対応の機能が要求されると装置作りを VK_ERROR_FEATURE_NOT_PRESENT で失敗させる。MVKDevice.mm:5432-5441。載せなければ送られない。
- 受け手 変更なし。
- 確かでない。DXVK が robustBufferAccess2 を前提に境界の外の読みを省いていると、MoltenVK の robustBufferAccess の範囲外で結果が変わる恐れがある。実機で見る。
- 手間 小。効き目 必須の 1 つが通る。見た目の正しさは実機しだい。

### 8. robustness2 の nullDescriptor

- DXVK。必須。990 行。実際に使う。legacy の経路は、つながっていないスロットを VK_NULL_HANDLE で書く。dxvk_context.cpp:7136、7160-7167、7183-7185、7201、7217、7233、7251。vertex buffer の空きは VK_NULL_HANDLE で bind。7869 行。index buffer の空きも VK_NULL_HANDLE で bind。7824 行。
- MoltenVK。機能は false。MVKDevice.mm:638。ただし中身は部分的に null に耐える。
  - descriptor の書き込みは null の buffer と texture を setNullBuffer と setNullTexture で受ける。MVKDescriptorSet.mm:1013-1014、1033-1048、1059-1062。
  - index buffer の null は仮の buffer で代用する。MVKCmdDraw.mm の MVKCmdBindIndexBuffer の 84-105 行。
  - vertex buffer の null は null 検査が無く、落ちる恐れが高い。MVKCmdDraw.mm の MVKCmdBindVertexBuffers::setContent 44-47 行で、検査なしに mvkBuffer->getMTLBuffer() を呼ぶ。
- ドライバと受け手で足す物。
  1. 機能の名乗りは 7 と同じ。nullDescriptor を ICD が true にする。
  2. 受け手で、null の vertex buffer を代役の buffer に差し替える。server.c:930-941 と 1097-1108。0 で埋めた小さな buffer を装置ごとに 1 つ作り、bs[i] が 0 のときだけ差し替える。大きさは余裕を見て数 MiB。確かでない。範囲外の頂点の読みが 0 になるかは MoltenVK に頼る。
  3. index buffer の null は MoltenVK が受ける。そのまま。
  4. descriptor の null はそのまま通る。ICD の update_dsets は buffer が 0 でも落ちない作り。2532 行。受け手の 1924 行と 1900-1915 行も 0 を許す。
  5. 受け手に代役を入れたら、VR_CAP_NULL_DESC を名乗る。ICD は受け手がこの印を持つときだけ nullDescriptor を名乗る。古い受け手で null の vertex buffer が落ちるのを避けるため。
- 確かでない。null の texture と buffer を shader が読んだとき、Metal が 0 を返すか。MoltenVK は buffer の大きさを 0 にして範囲検査で 0 を返す作りに見えるが、texture は読んでいない。実機で見る。
- 手間 中。効き目 必須の 1 つが通る。これが外れると D3D11 のつながっていないスロットで落ちる。

### 9. maintenance5

- DXVK。必須。1035 行。拡張 VK_KHR_maintenance5 の名前と機能 maintenance5 が要る。使う入口は vkCmdBindIndexBuffer2KHR。dxvk_cmdlist.h:636-641 と dxvk_context.cpp:7808、7824。使う入口がもう 1 つ。vkGetDeviceImageSubresourceLayoutKHR。dxvk_device.cpp:104-105。これは D3D11 の直接 map のときだけ。ICD は linear 画像を隠している。icd.c:1018。つながらないはず。DXVK の設定 d3d11.disableDirectImageMapping で避けることもできる。d3d11_options.cpp:31。
- vkGetRenderingAreaGranularityKHR と vkGetImageSubresourceLayout2KHR は DXVK が入口を引くが、呼ぶ場所は見つけられなかった。
- MoltenVK。持つ。機能 MVKDevice.mm:265。拡張 MVKExtensions.def:83。
- ドライバで足す物。
  1. 拡張 VK_KHR_maintenance5 を中継する。ALLOWED_EXT に足す。IPHONE_VK_DXVK3 のときだけ通す別の表にする。
  2. FEAT_ST に VkPhysicalDeviceMaintenance5FeaturesKHR を足し、new_feature_st にも足す。送るときは「中継できる機能」として扱う。PROP_ST に VkPhysicalDeviceMaintenance5PropertiesKHR を足す。
  3. 入口をドライバの中で作る。新しい命令は要らない。
     - vkCmdBindIndexBuffer2 と KHR 名。buffer が 0 なら何も記録しない。0 でなければ cmd_bind_ib に渡す。大きさは受け手へ送らない。
     - vkGetRenderingAreaGranularity と KHR 名は 1 と 1 を返す。render_area_granularity と同じ。
     - vkGetImageSubresourceLayout2 と KHR 名は get_subres_layout に包む。
     - vkGetDeviceImageSubresourceLayout と KHR 名は 0 を返す。linear 画像を隠している限り呼ばれない。
- 受け手 変更なし。
- 手間 中。効き目 必須の 1 つが通る。

### 10. maintenance6

- DXVK。必須。1036 行。使う入口が重要。descriptor set の bind はすべて vkCmdBindDescriptorSets2KHR。dxvk_cmdlist.h:624-625。push 定数は vkCmdPushConstants2KHR。dxvk_cmdlist.h:1029-1030。この 2 つが無いと何も描けない。
- MoltenVK。持つ。MVKDevice.mm:266。拡張 MVKExtensions.def:84。
- ドライバで足す物。ドライバの中だけ。
  1. 拡張 VK_KHR_maintenance6 を中継し、FEAT_ST と PROP_ST に VkPhysicalDeviceMaintenance6FeaturesKHR と Properties を足す。
  2. vkCmdBindDescriptorSets2 と KHR 名。stageFlags に COMPUTE があれば compute で、ALL_GRAPHICS のどれかがあれば graphics で、cmd_bind_sets を呼ぶ。両方あれば 2 回。cmd_bind_sets は 2747-2769 行で、storage buffer の範囲の記録もそこでする。
  3. vkCmdPushConstants2 と KHR 名。cmd_push に包む。2771 行。
- 受け手 変更なし。
- 手間 小から中。効き目 必須の 1 つが通る。描画の前提。

### 11. VK_KHR_load_store_op_none

- DXVK。必須。1032 行。名前は KHR。
- MoltenVK。持つ。KHR は MVKExtensions.def:78。EXT は 159。
- ドライバで足す物。ALLOWED_EXT に VK_KHR_load_store_op_none を足す。中継するので、受け手の装置でも有効になる。loadOp と storeOp の値は render pass と動的 rendering のどちらでも数値のまま送っている。icd.c:1594-1595 と 3239-3240。受け手は読んだ値をそのまま使う。LOAD_OP_NONE の値は 1000400000。
- 受け手 変更なし。
- 手間 小。効き目 必須の 1 つが通る。

### 12. swapchain の拡張の名前

- 必須は VK_KHR_swapchain だけ。1062 行。ICD は名乗っている。icd.c:466。受け手へは送らない。ドライバの swapchain は ICD の中で作る。
- 任意のもの。VK_KHR_swapchain_maintenance1、VK_EXT_swapchain_maintenance1、VK_KHR_swapchain_mutable_format、present_id、present_wait、incremental_present、full_screen_exclusive。ICD はどれも名乗らないので DXVK は使わない。dxvk_presenter.cpp:44-45、594-610、758。
- 結論。足す物は無い。名乗らないことを守る。
- 確かでない。依頼文の「名前の違い」の意味。分かれば追加で見る。

### 13. geometryShader

4 節の後ろの 5 節で書く。

### 依頼に無かった必須で足りない物

#### shaderCullDistance

- DXVK。必須。847 行。
- MoltenVK。1.4.2 の MVKDevice.mm に設定する所が無い。つまり false。
- 扱い。13 と同じ。ICD で true と偽って名乗り、cull distance を使う shader は pipeline 作成で失敗させるか、DXVK の 847 行を false に直す。D3D11 の SV_CullDistance を使うゲームは多くない。

#### textureCompressionBC

- DXVK。必須。868 行。
- MoltenVK。MTLDevice の supportsBCTextureCompression を見て決める。MVKDevice.mm:118-119 と 2786。
- 確かでない。iPhone の GPU で true かは確認していない。Apple の資料では BC は Mac の GPU が持つ印象だが、確かめていない。実機で VR_GET_FEATURES2 の textureCompressionBC を見るのが早い。
- false だった場合。偽って名乗ると BC の画像を作る vkCreateImage が受け手で失敗する。DXVK を 1 行直して必須から外すと、D3D11 が BC を使えない装置として扱う。BC を ICD で展開する案は大。今回は範囲外。

## 4. bufferDeviceAddress を深く

聞かれた 3 つに答える。

### 4.1 DXVK 3.1.1 は shader の中でアドレスをたどるか

MoltenVK を受け手にした構成では、たどらない。理由は 3 つ。

1. constant buffer を push 定数のアドレスに変える経路は、LowerInBoundsCbvToBda の印があるときだけ。dxvk_shader_ir.cpp:933-953。この印は RADV と AMD の driver id のときだけ付く。dxvk_device.cpp:798-802。受け手は driverID が MOLTENVK で、ICD は受け手の値を通すので付かない。
2. UAV の counter をアドレスで持つ経路は minStorageBufferOffsetAlignment が 4 以下のときだけ。dxvk_shader_ir.cpp:1236。値は dxvk_device.cpp:765-766 で装置の limits から取る。MoltenVK は 16。MVKDevice.mm:2927。ICD はこの値を通す。
3. 組み込みの shader は 64 ビットの整数計算を使うだけで、アドレスの読み書きは使わない。dxvk_meta_blit.cpp:362 だけがヒットしたが、整数の shift。descriptor heap は MoltenVK で使われない。使うのは Mesa の RADV・NVK・llvmpipe・Intel・AMD の proprietary、そして Nvidia 595.84 以上のとき。dxvk_device_info.cpp:497-511。

だから最小の足し方は、DXVK が要る入口と印を満たすだけ。アドレスの値は shader に使われない。

代役の RTX 3060 も同じ。driverID は NVIDIA で、minStorageBufferOffsetAlignment は 16。D:\iphone-rpc\vkbuild\delta_runs\vulkaninfo_v13.txt の 2593 行。descriptor heap は Nvidia 595.84 未満で切られる。手元の driver は 595.71。だから代役でも shader はアドレスをたどらず、MoltenVK との違いは出ない。たどる経路を試したくなったら、DXVK を直すか設定で LowerInBoundsCbvToBda を強制する必要がある。確かでない。強制する設定は見つけていない。

### 4.2 受け手の本物のアドレスをドライバに返して使う形で足りるか

足りる。理由。

- shader が動く場所は iPhone の GPU。shader が持つポインタは iPhone の GPU のアドレス。MoltenVK の vkGetBufferDeviceAddress は MTLBuffer の gpuAddress と offset の和を返す。MVKBuffer.mm:172-173。それがそのまま shader のポインタになる。
- PC 側はアドレスをたどらない。アプリがアドレスを push 定数や buffer の中身に書いて shader に渡す道は、バイトとして運ばれるだけで、値は受け手のアドレスのまま保たれる。VR_CMD_PUSH_CONSTANTS と、MEM_WRITE で送る中身。
- アドレスの足し算は buffer の中で成り立つ。DXVK は buffer の先頭のアドレスに offset を足して使う。
- 往復。buffer をメモリにつないだあとに聞く。命令は順番に処理されるので、先に送った VR_BIND_BUFFER_MEMORY が先に済む。DXVK は buffer 1 つにつき 1 回聞くが、DXVK は大きな buffer を作って分けて使うので回数は多くない。
- 受け手が再起動するとアドレスは変わる。接続ごとに作り直すので問題ない。

本物でなく、ICD が作った見せかけのアドレスを返す案もある。DXVK 3.1.1 がたどらない前提で成り立つ。往復が要らず、受け手の変更も要らない。ただし、あとでアドレスを使う shader が来たとき、GPU がずれたアドレスを読み、装置を失う。第一段階の最小実装としてはあり。本物を返す形に移るときは、受け手が VR_CAP_BDA を名乗るかどうかで切り替える。

### 4.3 shader の中のアドレスはどうなるか、同期はどうなるか

- shader の中のアドレスは iPhone の GPU のアドレス。PC の影のメモリとは関係ない。
- 問題は同期。ICD は、GPU が読む範囲と書く範囲を descriptor・vertex・index・copy から推し量って、submit の前と待ちのあとに影を送り・引き取る。アドレスでたどる読み書きは、この推し量りに映らない。たどる shader が来たら、PC が書いた中身が届かないか、GPU が書いた中身が戻らない。
- DXVK 3.1.1 を MoltenVK で使う限り、たどる shader は来ない。だから第一段階は同期に手を入れない。
- 安全装置の案。vkCreateShaderModule で SPIR-V の OpCapability を走査し、PhysicalStorageBufferAddresses が出たら、その装置を「アドレスで読み書きされる」印で保つ。印がある装置では、submit のたびに CPU から見える全メモリの書いたページを送り、出した仕事の後に全部を GPU が書いたかもしれない範囲として引き取る。ページの送りは書き込み監視で変わった所だけ。引き取りは VR_CAP_DELTA で変わった所だけ。受け手が毎回メモリ全体を比べる CPU の費用はかかる。手間は大。効果は、アドレスを使う別のアプリを守ること。DXVK のためには要らない。
- ほかのアプリへの影響。bufferDeviceAddress を名乗ると、llama.cpp などが BDA の経路を選ぶ恐れがある。選ぶと同期が合わない。確かでない。だから名乗りは IPHONE_VK_DXVK3 のときだけにする。

### 4.4 BDA の下書き

proto.h。

```c
#define VR_CAP_BDA 2048u          // VR_GET_BUFFER_ADDRESS と VR_ALLOC_MEMORY の末尾の flags
...
    VR_READ_JPEG_PIPE,
    VR_GET_BUFFER_ADDRESS,        // [同期] device・buffer → u64 のアドレス（VR_CAP_BDA）
    VR_OP_COUNT
```

icd.c。OP_NAME の表にも名前を足す。

```c
static VkDeviceAddress VKAPI_CALL get_buf_addr(VkDevice dv, const VkBufferDeviceAddressInfo *i) {
    struct buf *bf = (struct buf *) i->buffer;   // struct buf に uint64_t addr を足す
    if (bf->addr) return bf->addr;
    if (g_hw.caps & VR_CAP_BDA) {
        vr_buf b = { 0 }, o = { 0 };
        vr_u64(&b, ID(dv));
        vr_u64(&b, ID(bf));
        if (rpc_call(VR_GET_BUFFER_ADDRESS, &b, &o) == VK_SUCCESS && o.n >= 8) memcpy(&bf->addr, o.p, 8);
        free(b.p); free(o.p);
    } else {
        static uint64_t next = 0x100000000ull;   // 見せかけ。shader の中では使えない
        bf->addr = next;
        next += (bf->size + 0xffffull) & ~0xffffull;
    }
    return bf->addr;
}
```

alloc_memory の末尾。

```c
uint32_t af = 0;
for (const VkBaseInStructure *s = ai->pNext; s; s = s->pNext)
    if (s->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO) af = ((const VkMemoryAllocateFlagsInfo *) s)->flags;
if (g_hw.caps & VR_CAP_BDA) vr_u32(&b, af);   // VR_ALLOC_MEMORY の末尾
```

server.c。

```c
// VKFUNCS_OPT に vkGetBufferDeviceAddress を足す。detect_caps で:
VkPhysicalDeviceBufferDeviceAddressFeatures fbda = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
LINK(v12, fbda)     // 1.2 の機能の構造体と一緒に使うと規則に反する。fhqr は単独の構造体なので同じ鎖に置ける
...
if (v12 && fbda.bufferDeviceAddress) { LOAD_OPT(vkGetBufferDeviceAddress, 1, "KHR") if (vkGetBufferDeviceAddress) caps |= VR_CAP_BDA; }

case VR_GET_BUFFER_ADDRESS: {
    VkDevice d = DEV(vr_ru64(r));
    VkBuffer b = OBJ(VkBuffer, vr_ru64(r));
    if (!b || !vkGetBufferDeviceAddress || r->bad) return reply(s, VK_ERROR_INITIALIZATION_FAILED);
    VkBufferDeviceAddressInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b };
    vr_u64(&g_out, (uint64_t) vkGetBufferDeviceAddress(d, &bi));
    return reply(s, 0);
}

// ALLOC_MEMORY の 1536 行の後ろ
VkMemoryAllocateFlagsInfo fi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
if (!r->bad && r->at + 4 <= r->n) {
    fi.flags = vr_ru32(r) & VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (fi.flags) mai.pNext = &fi;
}
```

注意。この下書きはコンパイルしていない。VkPhysicalDeviceVulkan12Features と個別の機能の構造体を同じ鎖に入れてはいけない。VUID の規則。detect_caps の鎖は個別の構造体を並べる作りなので、fbda を足すだけにする。

## 5. geometry shader

- DXVK。必須。834 行。2.7.1 も必須。717 行。DXVK-macOS 1.10.3 は必須でなかった。D3D11 が、装置が持つときだけ geometryShader を頼む形。Gcenx/DXVK-macOS v1.10.3-20230507 の d3d11_device.cpp:1951 と dxvk_adapter.cpp:121-122。だから古い版は動いた。
- DXVK 側で避ける設定は無い。dxvk_options.cpp の 5-29 行と d3d11_options.cpp の 16-36 行を見た。d3d11.maxFeatureLevel は 10_0 までしか下がらず、10_0 も GS を含む。
- MoltenVK。持たない。1.4.2 の MVKDevice.mm に geometryShader を true にする所が無い。Metal に GS が無いから。
- 失敗の時の挙動。DXVK は vkCreateGraphicsPipelines の失敗を記録して null の pipeline を返し、描画を飛ばす。dxvk_graphics.cpp:1500-1505 と dxvk_context.cpp:8409。落ちない。

選べる道。

| 道 | 内容 | 手間 | 効き目 |
|---|---|---|---|
| A | ICD が geometryShader を true と偽り、GS 段を含む pipeline の作成を VK_ERROR_FEATURE_NOT_PRESENT で返す。create_graphics_pipelines の stage の走査で止める。1717 行の前 | 小 | 装置は作れる。GS を使う D3D11 の描画だけが消える |
| B | DXVK を自前ビルドして device_info.cpp の 834 を false にする。CI の dxvk-macos.yml と同じ形で、パッチを当てる | 小 | A と同じ。ICD の嘘が要らない。同じ直しを shaderCullDistance の 847 と textureCompressionBC の 868 にもできる |
| C | GS を compute で真似る | 大 | GS を使うゲームが動く |

C の先例。

- Asahi Linux の Mesa の AGX ドライバが、Apple の GPU に GS が無いので compute で真似ている。https://alyssarosenzweig.ca/blog/aaa-gaming-on-m1.html。流れは、頂点処理の結果を buffer に出し、GS を compute にして図形を buffer に積み、数えた図形の数で間接描画をする。
- MoltenVK 自身は GS を真似ない。tessellation は compute を挟んで真似る実装がある。確かでない。この点は今回は読んでいない。

C をやる場合の見立て。DXVK が出す GS の SPIR-V を compute に書き換える部分と、ICD かの受け手で draw の前に追加の dispatch を差す部分が要る。dxvk の dxbc-spirv が出す SPIR-V を書き換えるのは大きな仕事。D3D11 の標準的なサンプルやゲームの多くは GS を使わないので、先に A か B で第一光を出して、GS が要る実物が出てから考える。

## 6. おすすめの順番

小さくて効く物から。各段は前の段のあとでよい。確認は PC の代役の受け手で先にやり、あとで iPhone に移す。RTX 3060 は必須を全部持つので、ICD の変換の間違いと MoltenVK の足りなさを分けて見られる。

0. 入口のスイッチと記録。小。
   - 環境変数 IPHONE_VK_DXVK3=1 のときだけ、以降の名乗りを開く。既定は今まで通り。
   - 名乗った機能と拡張を IPHONE_VK_LOG で出す。
   - 確認は vulkaninfo で、必須の 53 項目のうち、足りない物が見える。
1. 印だけで済む物。小。数十行。
   - samplerMirrorClampToEdge、descriptorIndexing の 5 つ、inlineUniformBlock を mask_ext_features に足す。
   - VK_KHR_load_store_op_none、VK_KHR_maintenance5、VK_KHR_maintenance6 を中継。FEAT_ST と PROP_ST に maintenance5 と 6 の構造体。
   - 確認は DXVK 3.1.1 のログの「必須の機能が足りない」の行が、depth clip・robustness2・BDA・GS・cull・BC だけに減ること。
2. ICD の中で済む入口。小から中。
   - vkCmdBindDescriptorSets2、vkCmdPushConstants2、vkCmdBindIndexBuffer2、vkGetRenderingAreaGranularity、vkGetImageSubresourceLayout2、vkGetDeviceImageSubresourceLayout。
   - ここまでで描画の前提が揃う。
3. depth clip の変換。小。
   - 拡張と機能の名乗りと、pipeline の depthClampEnable の置き換え。描画の正しさに効くので早めに。
4. robustness2 の名乗りと null の扱い。中。
   - 偽って名乗る。受け手の null vertex buffer の差し替えと VR_CAP_NULL_DESC。実機で null の読みを確かめる。
5. descriptor binding flags の運び。中。
   - VR_CAP_DESC_BINDING。まず省いて動くかを見て、要るなら足す。
6. BDA。中。
   - 最小は見せかけのアドレスで先に進める。続けて本物のアドレスと VR_CAP_BDA。
   - 安全装置の同期は、DXVK では要らないので後回し。
7. GS・cull・BC。小から大。
   - まず A か B で必須を外す。実物のアプリで要る物が出てから C を考える。

第一光の目標。1 から 4 と 6 の最小と 7 の A か B で、DXVK 3.1.1 が装置を作り、三角形を 1 枚描く。

## 7. 確かでない点

- 現行の icd.c をビルドした DLL を、iPhone 越しに vulkaninfo で測った記録は無い。この調査は読むだけ。
- textureCompressionBC の iPhone での値。
- null の texture と buffer を shader が読んだときに 0 が返るか。
- robustBufferAccess2 を偽って名乗った場合の見た目。
- 2048 個の sampler 置き場が iPhone の MoltenVK の上限に収まるか。
- binding flags の運びを省いても update after bind が動くか。
- 依頼文にある swapchain の拡張の名前の違いが指す物。
- MoltenVK で VK_EXT_depth_clip_enable を含むリリースの時期。
- DXVK 3.1.1 を mingw でビルドして Windows の d3d11.dll にする部分と、DXGI の部分は調べていない。

## 8. 出典

読み方。GitHub のファイルは標準出力に流して読み、保存していない。差分を見るために /tmp に一時の一覧を 2 つ作り、すぐ消した。

DXVK。
- リリース https://github.com/doitsujin/dxvk/releases。v3.1.1 は 2026-09-15、v3.1 は 2026-08-28、v3.0 は 2026-06-25、v2.7.1 は 2025-08-30。
- v3.0 のリリースノートに、Vulkan 1.4 の機能と拡張の多くを必須にした、と書いてある。
- v3.1.1 の src/dxvk/dxvk_device_info.cpp、dxvk_device_info.h、dxvk_instance.h:12、dxvk_instance.cpp:162-164、dxvk_graphics.cpp:585 と 591-592 と 1500-1505、dxvk_context.cpp:7087-7298 と 7543-7600 と 7824 と 7869、dxvk_sampler.cpp:303-362、dxvk_sampler.h:375、dxvk_buffer.cpp:30-31、dxvk_memory.cpp:1108 と 1385-1389 と 1433 と 2334-2341、dxvk_shader_ir.cpp:933-953 と 1236、dxvk_device.cpp:104-105 と 765-766 と 798-802 と 725-735、dxvk_pipemanager.cpp:424-430、dxvk_cmdlist.h:624-641 と 1029-1030、src/vulkan/vulkan_loader.h:191-539、dxvk_options.cpp、d3d11_options.cpp。
- v2.7.1 の src/dxvk/dxvk_device_info.cpp:709-885、dxvk_graphics.cpp:571-575。
- master は 2026-10-05 の 5b94142f68。v3.1.1 より 63 commit 進んでいる。必須の 53 項目は同じ。
- Gcenx/DXVK-macOS v1.10.3-20230507 の src/dxvk/dxvk_adapter.cpp:121-122。

MoltenVK。
- v1.4.2 は 2026-07-24。最新のタグ。
- docs https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md の 232-433 行。
- v1.4.2 の MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm。155-271 行の機能の表、634-640 行の robustness2、1286-1287 行、2461-2560 行と 2584-2591 行の Metal の機能、2690-2705 行、2758-2840 行、2926-2927 行、3524-3566 行、5295-5345 行。
- MVKFoundation.cpp:139。MVKBuffer.mm:172-173。MVKDescriptorSet.mm:1000-1120。MVKCmdDraw.mm:29-110。Layers/MVKExtensions.def。Utility/MVKEnvironment.h:297-300。
- main の MVKExtensions.def:139 と commit 2026-08-24 の「feat: implement VK_EXT_depth_clip_enable」。
- iPhone のアプリは v1.4.2 の配布物を使う。D:\iphone-rpc\app\.github\workflows\ios.yml:71-75。

このプロジェクト。
- D:\iphone-rpc\app\vk\icd\icd.c、D:\iphone-rpc\app\vk\remote\server.c、proto.h、wire.h。
- D:\iphone-rpc\vkbuild\d3d11\dxvk_icd.log、D:\iphone-rpc\vkbuild\delta_runs\vulkaninfo_v13.txt、D:\iphone-rpc\vkbuild\apps_runs\iphone_v0710\vulkaninfo_summary.txt。

GS の先例。
- https://alyssarosenzweig.ca/blog/aaa-gaming-on-m1.html
