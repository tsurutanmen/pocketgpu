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
#define VR_CAP_D3D 512u              // DirectX（DXVK）の土台：VR_CREATE_BUFFER_VIEW・texel buffer の descriptor・VR_GET_IMAGE_SUBRESOURCE_LAYOUT
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
};

// graphics pipeline（VR_CREATE_GRAPHICS_PIPELINE）の末尾に付けてよい物。[u32 sType][中身の bytes] を並べる。
// 古い受け手は末尾を読まないので、付けても壊れない
//   VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO：view の印・色の数・色の形式の並び・depth の形式・stencil の形式
