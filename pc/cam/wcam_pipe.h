// 橋渡し（webcam_rx）と仮想カメラの media source（Frame Server の中）の間の口。Winsock を使わない側（vcam_source）も読むので別にした。
#pragma once
#include <stdint.h>

// 仮想カメラの口（PC の中）。橋渡し（webcam_rx）がサーバー、仮想カメラの media source（Frame Server の中）がクライアント
#define WCAM_PIPE_NAME L"\\\\.\\pipe\\iphone-webcam-nv12"
#define WCAM_PIPE_MAGIC 0x32564e49u // 'INV2'
typedef struct { uint32_t magic, width, height, fps; uint64_t pts_us; uint32_t flags, reserved; } wc_pipe_head; // 32 バイト。このあと NV12 が width*height*3/2 バイト（詰めてある）

