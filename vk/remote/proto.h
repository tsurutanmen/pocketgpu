// ドライバと受け手の取り決め。番号を変えたら VR_PROTO_VERSION を上げる
#pragma once
#include <stdint.h>

#define VR_PROTO_VERSION 3

// 版 3 からの待ち方：ドライバは、持ち帰る物が無い待ち（fence など）には往復せずに「終わった」と答える。
// その代わり受け手は、出した仕事が使っているかもしれない物を変える命令（メモリの書き換え・記録し直し・片づけなど）の前に、
// 出した仕事が全部終わるのを自分の中で待つ（drain）。受け手の中で待つぶんには往復が要らない
#define VR_DEFAULT_PORT 50060

// 受け手ができること。あいさつの返事の最後に付ける。古い受け手は付けない。
// 印の無い命令は、古い受け手には送らない（知らない記録の命令が来ると、受け手はその command buffer を捨てる）
#define VR_CAP_DELTA 1u   // VR_MEM_READ_DELTA：前回送った中身との違いだけを詰めて送る
#define VR_CAP_VK13 2u               // 本物の GPU が Vulkan 1.3 を持ち、受け手が 1.3 の入口を本体の名前で呼べる。ドライバは 1.3 と名乗ってよい
#define VR_CAP_DYNAMIC_RENDERING 4u  // VR_CMD_BEGIN_RENDERING・VR_CMD_END_RENDERING と、graphics pipeline の末尾の「描く先の形式」
#define VR_CAP_SYNC2 8u              // VR_CMD_BARRIER_SYNC2（vkCmdPipelineBarrier2）
#define VR_CAP_RENDER_PASS2 16u      // VR_CREATE_RENDER_PASS2
#define VR_CAP_CMD10 32u             // 1.0 の残りの記録命令：VR_CMD_SET_DYNAMIC の 1.0 の値・CLEAR_ATTACHMENTS・CLEAR_DEPTH_STENCIL_IMAGE・RESOLVE_IMAGE
#define VR_CAP_EXT_DYNAMIC_STATE 64u // VR_CMD_SET_DYNAMIC の extended dynamic state（1.3 で本体入り）の値と VR_CMD_BIND_VERTEX_BUFFERS2
#define VR_CAP_QUERY 128u            // query pool（タイムスタンプ・occlusion）。VR_CREATE_QUERY_POOL など
#define VR_CAP_JPEG 256u             // VR_READ_JPEG：窓に出すコマを受け手が JPEG に縮めて返す
#define VR_CAP_JPEG_PIPE 1024u       // VR_READ_JPEG_PIPE：今のコマを後ろで縮め始めて、ひとつ前のコマの JPEG を返す
#define VR_CAP_ASYNC_CREATE 16384u  // VR_CREATE_BUFFER_NR：返事を待たずに buffer を作る。必要なメモリの量はドライバが前に聞いた物から計算する
#define VR_CAP_LINK 131072u         // VR_GET_LINK：インターネット共有の口（USB の有線ネットワーク）の住所と合言葉を、USB の口で教える
#define VR_LINK_MAGIC 0x41524b56u   // インターネット共有の口につないだら、最初にこの 4 バイト（"VKRA"）と合言葉 16 バイトを送る
#define VR_CAP_DIVISOR 65536u       // 頂点の step rate（VK_EXT_vertex_attribute_divisor）：pipeline の末尾の VkPipelineVertexInputDivisorStateCreateInfoEXT を読む
#define VR_CAP_STATUS 32768u        // VR_GET_STATUS：iPhone の熱の状態を返す
#define VR_CAP_PACK 8192u           // VR_PACKED：いくつもの命令をまとめて LZ4 で縮めた物を受けて、ほどいて順に処理する
#define VR_CAP_BDA 4096u            // buffer device address：VR_GET_BUFFER_ADDRESS と、VR_ALLOC_MEMORY の末尾の flags（VkMemoryAllocateFlagsInfo）
#define VR_CAP_NULL_DESC 2048u      // 空（VK_NULL_HANDLE）の descriptor と頂点バッファを受ける。GPU が持たなければ受け手が代わりの物を使う
#define VR_CAP_D3D 512u              // DirectX（DXVK）の土台：VR_CREATE_BUFFER_VIEW・texel buffer の descriptor・VR_GET_IMAGE_SUBRESOURCE_LAYOUT
#define VR_CAP_DESC_FLAGS 524288u   // VR_CREATE_DSL の末尾の binding ごとの flags と、VR_ALLOC_DESCRIPTOR_SETS の末尾の set ごとの数（variable descriptor count）を読む
#define VR_CAP_PROGRESS 1048576u    // VR_GET_PROGRESS：GPU を待たずに、終わった submit の番号を返す（ドライバが fence を本当の進み具合で答える）
#define VR_CAP_UPSCALE 2097152u     // VR_READ_H264 の印 VR_H264_UP15 を受ける：縮める前に MetalFX で 1.5 倍に広げる（iPhone だけ）
#define VR_CAP_H264 262144u          // VR_READ_H264：窓に出すコマを受け手が H.264 に縮めて、1 コマずつ返す（iPhone は VideoToolbox、PC の代役は Media Foundation）
// レイトレーシングの土台（VK_KHR_acceleration_structure を GPU の上で作るだけ・VK_KHR_ray_query）。VR_CAP_BDA も要る。
// VR_CREATE_ACCEL・VR_DESTROY_ACCEL・VR_GET_ACCEL_SIZES・VR_CMD_BUILD_ACCEL・VR_CMD_COPY_ACCEL・VR_CMD_WRITE_ACCEL_PROPS と、
// descriptor の種類 VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR（1 つにつき acceleration structure の番号 u64）。
// ray tracing pipeline（SBT）はまだ入らない。番号はほかの係（1<<22・1<<23 往復を減らす・1<<25 平面・1<<26 録画）と重ならないよう 1<<28
#define VR_CAP_RT 268435456u
// VR_READ_H264 の印
#define VR_H264_KEY 1u               // このコマをキーフレーム（IDR）にする。ドライバがほどき直すとき
#define VR_H264_PIPE 2u
#define VR_H264_UP15 4u              // 縮める前に、絵を 1.5 倍（720p なら 1080p）に広げる（VR_CAP_UPSCALE）。返事の頭の幅と高さは広げた後の物              // 先送り：今のコマを後ろで縮め始めて、ひとつ前に頼んだコマを返す（最初は空）
// VR_READ_H264 の返事の頭（32 バイト）：流れの番号（u64）・札（u64）・幅・高さ・印（u32。1 ならキーフレーム）・空き（u32）。
// そのあとに 1 コマぶんの NAL の並び（Annex B。00 00 00 01 で区切る。キーフレームには SPS と PPS が付く）
#define VR_H264_HEAD 32u
// 録画（OBS の録画を iPhone で・道 A）：窓に出すコマと PC の音を、受け手が高画質で符号化して mp4 に書く。
// PC の音はこの口では送らない。スピーカーの係の口 50063（ipaudio → AudioBridge）で届いた物を、受け手が録画に入れる（vkr_record_pcm）。
// iPhone は AVAssetWriter（HEVC か H.264・AAC）、PC の代役は Media Foundation の Sink Writer。
// 番号は、ほかの係と重ならないように離して取った（まとめるときに決め直してよい）
#define VR_CAP_RECORD 67108864u     // VR_RECORD_CTL・VR_RECORD_FRAME・VR_RECORD_FETCH
// VR_RECORD_CTL の動作
#define VR_REC_STOP 0u              // 止めて書き終える（終わるまで待ってから答える）
#define VR_REC_START 1u             // 始める。中身を書くのは最初の VR_RECORD_FRAME から（その大きさで）
#define VR_REC_STATUS 2u            // 様子を聞くだけ
#define VR_REC_DELETE 3u            // 最後に書いたファイルを消す（PC に持ち帰ったあと）
// VR_RECORD_CTL の行き先の印
#define VR_REC_KEEP 1u              // 受け手に残す（iPhone は「ファイル」アプリから見える所）
#define VR_REC_PHOTOS 2u            // 書き終えたら「写真」に入れる（iPhone だけ）
// 映像の形式
#define VR_REC_H264 0u
#define VR_REC_HEVC 1u
// 動画のコマの平面（R8 など）を、画質を落とさずに縮めて送る（VR_MEM_WRITE_CODED。中身の形は remote/plane_codec.h）。
// 番号は、ほかの係（1<<22・1<<23 往復を減らす係、1<<26 録画、1<<28 レイトレ）と重ならないように取った
#define VR_CAP_PLANE_CODE 33554432u // 1u << 25
#define VR_CAP_NEW_MASK (VR_CAP_VK13 | VR_CAP_DYNAMIC_RENDERING | VR_CAP_SYNC2 | VR_CAP_RENDER_PASS2 | VR_CAP_CMD10 | VR_CAP_EXT_DYNAMIC_STATE | VR_CAP_QUERY)
// 1.3 の受け手は、VR_GET_FORMAT_PROPS の返事の後ろに VkFormatProperties3 の 3 つの値（u64）を付ける。古いドライバは読まない

// 返事のある命令（往復が 1 回かかる）には [同期] と書く。ほかは送りっぱなし
enum vr_op {
    VR_HELLO = 1,             // [同期] 版 → 装置の性質・メモリ・キューの種類
    VR_GET_FEATURES2,         // [同期] 欲しい構造体の (sType, 大きさ) の並び → 中身
    VR_GET_PROPERTIES2,       // [同期] 同上
    VR_GET_FORMAT_PROPS,      // [同期] format → VkFormatProperties
    VR_CREATE_DEVICE,         // [同期] id・キュー・機能・拡張
    VR_DESTROY_DEVICE,
    VR_GET_QUEUE,             // device・family・index・id
    VR_CREATE_BUFFER,         // [同期] device・id・VkBufferCreateInfo の要点 → VkMemoryRequirements
    VR_DESTROY_BUFFER,
    VR_ALLOC_MEMORY,          // [同期] device・id・大きさ・種類
    VR_FREE_MEMORY,
    VR_BIND_BUFFER_MEMORY,
    VR_MEM_WRITE,             // memory・offset・中身（PC で書き換えたところを送る）
    VR_MEM_READ,              // [同期] memory・offset・大きさ → 中身（GPU が書いたところを引き取る）
    VR_CREATE_SHADER_MODULE,
    VR_DESTROY_SHADER_MODULE,
    VR_CREATE_DSL,            // descriptor set layout
    VR_DESTROY_DSL,
    VR_CREATE_PIPELINE_LAYOUT,
    VR_DESTROY_PIPELINE_LAYOUT,
    VR_CREATE_COMPUTE_PIPELINE, // [同期] 作れたかを返す（iPhone では SPIR-V を Metal に直すときに失敗することがある）
    VR_DESTROY_PIPELINE,
    VR_CREATE_DESCRIPTOR_POOL,
    VR_DESTROY_DESCRIPTOR_POOL,
    VR_RESET_DESCRIPTOR_POOL,
    VR_ALLOC_DESCRIPTOR_SETS,
    VR_FREE_DESCRIPTOR_SETS,
    VR_UPDATE_DESCRIPTOR_SETS,
    VR_CREATE_COMMAND_POOL,
    VR_DESTROY_COMMAND_POOL,
    VR_RESET_COMMAND_POOL,
    VR_ALLOC_COMMAND_BUFFERS,
    VR_FREE_COMMAND_BUFFERS,
    VR_RECORD_COMMAND_BUFFER, // command buffer・begin の flags・記録した命令の並び（PC でためて一度に送る）
    VR_RESET_COMMAND_BUFFER,
    VR_CREATE_FENCE,
    VR_DESTROY_FENCE,
    VR_RESET_FENCES,
    VR_WAIT_FOR_FENCES,       // [同期]
    VR_GET_FENCE_STATUS,      // [同期]
    VR_CREATE_SEMAPHORE,      // device・id・種類（binary か timeline）・はじめの値
    VR_DESTROY_SEMAPHORE,
    VR_QUEUE_SUBMIT,          // 待つ（semaphore・stage・値）・command buffer・知らせる（semaphore・値）・fence
    VR_QUEUE_WAIT_IDLE,       // [同期]
    VR_DEVICE_WAIT_IDLE,      // [同期]
    VR_PING,                  // [同期] 往復の時間を測る
    VR_WAIT_SEMAPHORES,       // [同期] device・flags・(semaphore・値)の並び・待つ時間
    VR_GET_SEMAPHORE_COUNTER, // [同期] device・semaphore → いまの値
    VR_SIGNAL_SEMAPHORE,      // device・semaphore・値
    // ---- 描画（後から足した。版は 3 のまま。古い受け手は描画を知らないだけ） ----
    VR_CREATE_IMAGE,          // [同期] device・id・VkImageCreateInfo の要点 → VkMemoryRequirements
    VR_DESTROY_IMAGE,
    VR_BIND_IMAGE_MEMORY,     // device・image・memory・offset
    VR_CREATE_IMAGE_VIEW,
    VR_DESTROY_IMAGE_VIEW,
    VR_CREATE_RENDER_PASS,
    VR_DESTROY_RENDER_PASS,
    VR_CREATE_FRAMEBUFFER,
    VR_DESTROY_FRAMEBUFFER,
    VR_CREATE_GRAPHICS_PIPELINE, // [同期] 作れたかを返す
    VR_GET_IMAGE_FORMAT_PROPS,   // [同期] → 結果・VkImageFormatProperties
    VR_CREATE_SAMPLER,           // device・id・VkSamplerCreateInfo の pNext より後ろ
    VR_DESTROY_SAMPLER,
    VR_MEM_READ_DELTA,           // [同期] memory・offset・大きさ → 形（0 そのまま・1 前回との違い）・中身。受け手が VR_CAP_DELTA を名乗ったときだけ使う
    // ---- Vulkan 1.3 に向けて足した（版は 3 のまま。受け手が caps で名乗ったときだけ使う） ----
    VR_CREATE_RENDER_PASS2,      // device・id・VkRenderPassCreateInfo2 の要点（VR_CAP_RENDER_PASS2）
    VR_CREATE_QUERY_POOL,        // device・id・種類・数・統計の印（VR_CAP_QUERY）
    VR_DESTROY_QUERY_POOL,
    VR_GET_QUERY_POOL_RESULTS,   // [同期] device・pool・最初・数・大きさ・間隔・flags → 結果・中身。受け手は出した仕事を全部待ってから読む
    VR_RESET_QUERY_POOL,         // device・pool・最初・数（CPU からの reset）
    VR_READ_JPEG,                // [同期] memory・offset・幅・高さ・行の長さ・BGRA か・質（0〜100）→ JPEG のバイト（VR_CAP_JPEG）
    VR_CREATE_BUFFER_VIEW,       // device・id・buffer・形式・offset・大きさ（VR_CAP_D3D）
    VR_DESTROY_BUFFER_VIEW,
    VR_GET_IMAGE_SUBRESOURCE_LAYOUT, // [同期] device・image・aspect・mip・layer → VkSubresourceLayout（VR_CAP_D3D）
    VR_READ_JPEG_PIPE,           // [同期] VR_READ_JPEG と同じ。返すのはひとつ前のコマの JPEG（最初は空）。縮めるのを次のコマの仕事と重ねる（VR_CAP_JPEG_PIPE）
    VR_GET_BUFFER_ADDRESS,       // [同期] device・buffer → GPU の中のアドレス（u64）（VR_CAP_BDA）
    VR_PACKED,                   // 元の長さ（u32）・LZ4 で縮めた「命令の並び」。中の命令の返事は、ふつうに順に返す（VR_CAP_PACK）
    VR_CREATE_BUFFER_NR,         // VR_CREATE_BUFFER と同じ中身で、返事をしない（VR_CAP_ASYNC_CREATE）
    VR_CREATE_IMAGE_NR,          // VR_CREATE_IMAGE と同じ中身で、返事をしない（VR_CAP_ASYNC_CREATE）
    VR_GET_STATUS,               // [同期] → 熱の状態（u32・0 ふつう〜3 とても熱い）・後ろに回っているか（u32）（VR_CAP_STATUS）
    VR_GET_LINK,                 // [同期] → 使えるか（u32）・合言葉（16 バイト）・住所（u32・IPv4 をネットワークの並びで）・ポート（u32）。USB の口からだけ答える（VR_CAP_LINK）
    VR_READ_H264,                // [同期] memory・offset・幅・高さ・行の長さ・BGRA か・印（VR_H264_*）・kbps・コマ/秒・キーフレームの間隔（コマ）・
                                 // 流れの番号（u64）・札（u64）→ 頭（VR_H264_HEAD）と 1 コマぶんの H.264。先送りの最初は空（VR_CAP_H264）
    VR_GET_PROGRESS,             // [同期] → 終わった VR_QUEUE_SUBMIT の番号（u64。その番号までの submit は全部終わっている）。GPU は待たない（VR_CAP_PROGRESS）
    // ---- 録画（VR_CAP_RECORD）。ほかの係が足す命令と番号が重ならないよう 96 から ----
    VR_RECORD_CTL = 96,          // [同期] 動作（VR_REC_*）・映像の形式（VR_REC_H264/HEVC）・kbps（0 なら受け手が決める）・コマ/秒の目安・幅・高さ（0 なら最初のコマの大きさ）・
                                 // 音（0 なら音なし・それ以外なら口 50063 で届く PC の音を入れる）・音の数・音の kbps・行き先の印（VR_REC_KEEP など）・名前（u32 の長さ＋UTF-8。空なら受け手が決める）
                                 // → 状態（u32：0 止まっている・1 録画中・2 失敗した）・コマ（u64）・落としたコマ（u64）・音の標本（u64）・
                                 //   長さ（u64・µs）・大きさ（u64・バイト）・幅（u32）・高さ（u32）・ファイルの場所（u32 の長さ＋UTF-8）
    VR_RECORD_FRAME,             // memory・offset・幅・高さ・行の長さ・BGRA か・時刻（u64・µs。PC の時計で、録画を始めた時から）。返事なし。
                                 // 受け手は、その memory を使う仕事が終わるのを待ってから写す（VR_READ_H264 と同じ待ち方）
    VR_RECORD_FETCH,             // [同期] 位置（u64）・最大のバイト（u32）→ 全体の大きさ（u64）・中身。最後に書き終えたファイルを PC に持ち帰る
    // ---- 縮めた平面の書き込み（VR_CAP_PLANE_CODE）。ほかの係が足す命令と番号が重ならないよう 104 から（録画は 96〜、レイトレは 112〜） ----
    VR_MEM_WRITE_CODED = 104,    // memory・offset・番号（u64。VR_MEM_WRITE の末尾と同じ意味で、いつも付ける）・縮めた中身（plane_codec.h。
                                 // ほどくと offset から「幅 × 行の数」バイトの隙間の無い並び）。返事なし。受け手は VR_MEM_WRITE と同じく待ってから書く
    // ---- レイトレーシング（VR_CAP_RT）。ほかの係が足す命令と番号が重ならないよう 112 から（録画は 96〜、平面は 104〜、往復を減らす係は 120） ----
    VR_CREATE_ACCEL = 112,       // [同期] device・id・buffer・offset（u64）・大きさ（u64）・種類（u32）・create の flags（u32）→ 結果・GPU の中のアドレス（u64）
    VR_DESTROY_ACCEL,            // device・id
    VR_GET_ACCEL_SIZES,          // [同期] device・build の種類（u32）・build の形（種類・flags・mode の u32 ×3 と vr_accel_geoms。アドレスは 0）・
                                 // primitive の最大数の並び（u32）→ accelerationStructureSize・updateScratchSize・buildScratchSize（u64 ×3）
    VR_OP_COUNT
};

// command buffer の中に積む命令
enum vr_cmd {
    VR_CMD_BIND_PIPELINE = 1,     // bind point・pipeline
    VR_CMD_BIND_DESCRIPTOR_SETS,  // bind point・layout・first・sets[]・dynamic offsets[]
    VR_CMD_PUSH_CONSTANTS,        // layout・stages・offset・中身
    VR_CMD_DISPATCH,              // x・y・z
    VR_CMD_DISPATCH_BASE,         // base xyz・count xyz
    VR_CMD_COPY_BUFFER,           // src・dst・regions[]
    VR_CMD_FILL_BUFFER,           // dst・offset・size・data
    VR_CMD_UPDATE_BUFFER,         // dst・offset・中身
    VR_CMD_PIPELINE_BARRIER,      // src/dst stage・dependency・memory barriers[]・buffer barriers[]
    VR_CMD_DISPATCH_INDIRECT,     // buffer・offset
    VR_CMD_RESET_QUERY_POOL,      // pool・最初・数（VR_CAP_QUERY）
    // ---- 描画 ----
    VR_CMD_BEGIN_RENDER_PASS,     // render pass・framebuffer・範囲・消す色の並び・contents
    VR_CMD_END_RENDER_PASS,
    VR_CMD_NEXT_SUBPASS,          // contents
    VR_CMD_DRAW,                  // 頂点の数・インスタンスの数・最初の頂点・最初のインスタンス
    VR_CMD_DRAW_INDEXED,          // 番号の数・インスタンスの数・最初の番号・頂点のずらし・最初のインスタンス
    VR_CMD_BIND_VERTEX_BUFFERS,   // 最初・(buffer・offset) の並び
    VR_CMD_BIND_INDEX_BUFFER,     // buffer・offset・型
    VR_CMD_SET_VIEWPORT,          // 最初・VkViewport の並び
    VR_CMD_SET_SCISSOR,           // 最初・VkRect2D の並び
    VR_CMD_COPY_IMAGE_TO_BUFFER,  // image・layout・buffer・VkBufferImageCopy の並び
    VR_CMD_COPY_BUFFER_TO_IMAGE,  // buffer・image・layout・VkBufferImageCopy の並び
    VR_CMD_PIPELINE_BARRIER2,     // VR_CMD_PIPELINE_BARRIER ＋ 画像の barrier の並び
    VR_CMD_CLEAR_COLOR_IMAGE,     // image・layout・色・範囲の並び
    VR_CMD_DRAW_INDIRECT,         // buffer・offset・数・間隔
    VR_CMD_DRAW_INDEXED_INDIRECT, // buffer・offset・数・間隔
    VR_CMD_COPY_IMAGE,            // src・layout・dst・layout・VkImageCopy の並び
    VR_CMD_BLIT_IMAGE,            // src・layout・dst・layout・VkImageBlit の並び・filter
    // ---- Vulkan 1.3 に向けて足した（受け手が caps で名乗ったときだけ積む） ----
    VR_CMD_BEGIN_RENDERING,       // flags・範囲・layer の数・view の印・色の並び・depth・stencil（VR_CAP_DYNAMIC_RENDERING）
    VR_CMD_END_RENDERING,
    VR_CMD_BARRIER_SYNC2,         // dependency flags・memory / buffer / image barrier の並び。stage と access は 64 ビット（VR_CAP_SYNC2）
    VR_CMD_SET_DYNAMIC,           // VkDynamicState の番号・値の数・値（u32 の並び。小数はそのままのビット）（VR_CAP_CMD10・VR_CAP_EXT_DYNAMIC_STATE）
    VR_CMD_BIND_VERTEX_BUFFERS2,  // 最初・(buffer・offset・大きさ・間隔) の並び（VR_CAP_EXT_DYNAMIC_STATE）
    VR_CMD_CLEAR_ATTACHMENTS,     // VkClearAttachment の並び・VkClearRect の並び（VR_CAP_CMD10）
    VR_CMD_CLEAR_DEPTH_STENCIL_IMAGE, // image・layout・値・範囲の並び（VR_CAP_CMD10）
    VR_CMD_RESOLVE_IMAGE,         // src・layout・dst・layout・VkImageResolve の並び（VR_CAP_CMD10）
    VR_CMD_BEGIN_QUERY,           // pool・番号・flags（VR_CAP_QUERY）
    VR_CMD_END_QUERY,             // pool・番号
    VR_CMD_WRITE_TIMESTAMP,       // stage（1.0 の形）・pool・番号
    VR_CMD_COPY_QUERY_RESULTS,    // pool・最初・数・buffer・offset・間隔・flags
    // ---- レイトレーシング（VR_CAP_RT）。ほかの係が足す記録の命令と番号が重ならないよう 64 から ----
    VR_CMD_BUILD_ACCEL = 64,      // 数（u32）・数だけ [種類・flags・mode（u32）・元 id・先 id（u64）・scratch のアドレス（u64）・vr_accel_geoms・
                                  //                    geometry ごとの範囲（primitiveCount・primitiveOffset・firstVertex・transformOffset の u32 ×4）]
    VR_CMD_COPY_ACCEL,            // 元 id・先 id（u64）・mode（u32）
    VR_CMD_WRITE_ACCEL_PROPS,     // 数（u32）・id の並び（u64）・query の種類（u32）・pool（u64）・最初の query（u32）
};

// vr_accel_geoms：geometry の数（u32）・数だけ [種類（u32）・flags（u32）・種類ごとの中身]。アドレスは受け手の GPU の本物（VR_GET_BUFFER_ADDRESS で渡した物）
//   三角形：頂点の形式（u32）・頂点のアドレス（u64）・頂点の間隔（u64）・最大の頂点番号（u32）・番号の型（u32）・番号のアドレス（u64）・行列のアドレス（u64）
//   AABB：アドレス（u64）・間隔（u64）
//   instance：ポインタの並びか（u32）・アドレス（u64）

// graphics pipeline（VR_CREATE_GRAPHICS_PIPELINE）の末尾に付けてよい物。[u32 sType][中身の bytes] を並べる。
// 古い受け手は末尾を読まないので、付けても壊れない
//   VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO：view の印・色の数・色の形式の並び・depth の形式・stencil の形式
