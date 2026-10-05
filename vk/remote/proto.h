// ドライバと受け手の取り決め。番号を変えたら VR_PROTO_VERSION を上げる
#pragma once
#include <stdint.h>

#define VR_PROTO_VERSION 2
#define VR_DEFAULT_PORT 50060

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
    VR_CMD_RESET_QUERY_POOL,      // （まだ使わない）
};
