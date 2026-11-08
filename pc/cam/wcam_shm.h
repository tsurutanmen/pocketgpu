// 橋渡し（webcam_rx --shm）と DirectShow の仮想カメラ（dshow_cam.dll。Zoom やブラウザの中で動く）の間の口：名前つきの共有メモリ。
// パイプは相手が 1 つだけなので、カメラを同時に開くアプリが 2 つになると困る。共有メモリなら、何本でも同じ絵を読める。
//
//   書く側（1 つ）：webcam_rx。CreateFileMapping で作り、コマごとに次の枠へ NV12 を書いて、最後に枠の seq と頭の seq を進める。
//   読む側（何本でも）：仮想カメラ。OpenFileMapping で開いて、頭の seq の枠を写す。写し終えて枠の seq が変わっていなければ使う。
//   書く側が居なくなったか（heartbeat が 1 秒より古い）、まだ開けないときは、読む側が「待っています」の絵を出す。
//
// camset で足した「制御の欄」（頭の残りの 112 バイトの中。前の版の読む側・書く側は見ないので、混ぜて使っても壊れない）：
//   req：読む側 → 書く側。アプリ（OBS など）が選んだ形（幅・高さ・コマ数・カメラ）。64 ビットを 1 回で書く（何本の読む側が書いても混ざらない）。
//        書く側はこれを見て、iPhone に HELLO を送り直す。req_pid のプロセスが終わっていたら、その頼みは無かったことにする。
//   cur：書く側 → 読む側。いま iPhone が送っている形（ACK の値）。読む側は「既定の形」をこれに合わせる。
//
// cam4k で足した 4K（3840x2160）の口：
//   この共有メモリ（Local\pocketgpu-webcam-v1）の作りは変えない（頭 256 バイト・枠 3 つ・1 枠は 1920x1080 の NV12）。前の版の読む側は今までどおり読める。
//   4K を選んだときだけ、書く側はもう 1 つ共有メモリ（名前の後ろに -4k）を作り、4K の絵をそこへ書く（4K の絵は、こちらの口にも 1920x1080 に縮めた絵を書く）。
//   別の名前にしたのは、読む側（OBS など）が前の小さい共有メモリを開いたままでも、書く側が 4K の口を作れるようにするため。
//   - 頭の ext_state：1 なら 4K の口がある（書く側が書く。0 は無し）。前の版の書く側は 0 のまま
//   - 頭の cur_full：cur と同じ詰め方で、1920x1080 に収めない本当の形（4K の口があるときだけ書く）
//   - 枠の flags の WCAM_SLOT_EXT：この枠の絵は 4K の口の同じ番号の枠に本物がある（枠のこちらは縮めた絵）
//   - 4K の口の頭 wcam_ext_head：magic 'IW4K'・枠 3 つ・各枠に seq・幅・高さ・時刻
// 仮想カメラが 4K の形を名乗るかどうかは、レジストリ HKCU\Software\PocketGPU\VirtualCamera の MaxHeight（DWORD。2160 なら名乗る。無ければ 1080）。
//   webcam_rx が起動のとき（--vcam-max か環境変数 POCKETGPU_VCAM_MAX）に書く。camera_setup vcam-max でも書ける。
//
// 名前は Local\（このログオンの中だけ）。時計は QueryPerformanceCounter の µs（同じ PC の中なのでプロセスをまたいで比べられる）。
#pragma once
#include <stdint.h>

#ifndef WCAM_SHM_NAME
#define WCAM_SHM_NAME L"Local\\pocketgpu-webcam-v1"
#endif
#define WCAM_SHM_MAGIC 0x31435749u // 'IWC1'
#define WCAM_SHM_SLOTS 3u
#define WCAM_SHM_MAXW 1920u
#define WCAM_SHM_MAXH 1080u
#define WCAM_SHM_SLOT_BYTES (WCAM_SHM_MAXW * WCAM_SHM_MAXH * 3u / 2u)

typedef struct {
    volatile int64_t seq;   // この枠に入っているコマの番号。書いている間は 0
    uint32_t width, height; // NV12（詰めてある。Y が w*h、そのあと UV が w*h/2）
    uint32_t fps, flags;    // flags：WCAM_SLOT_EXT
    uint64_t pts_us;        // 撮った時刻（PC の QPC・µs）
} wcam_shm_slot;
#define WCAM_SLOT_EXT 1u    // cam4k：本物の絵は 4K の口にある（この枠の絵は 1920x1080 に収めた縮小）

typedef struct {
    uint32_t magic, version, head_bytes, slot_bytes;
    uint32_t nslots, max_w, max_h, writer_pid;
    volatile int64_t seq;          // 最後に書き終えたコマの番号（1 から）。枠は seq % nslots
    volatile int64_t heartbeat_us; // 書く側が最後に生きていた時刻（QPC・µs）。0 は「書く側が終わった」
    wcam_shm_slot slot[WCAM_SHM_SLOTS];
    // ---- ここから camset の制御の欄（前の版では pad の中。0 のままなら「頼み無し」）
    volatile int64_t req;          // 読む側 → 書く側（WCAM_REQ_MAKE で詰めた形）。0 は頼み無し
    volatile int64_t req_us;       // req を書いた時刻
    volatile uint32_t req_pid;     // req を書いた読む側のプロセス
    volatile uint32_t writer_caps; // 書く側ができること（WCAM_CAP_*）。前の版の webcam_rx は 0
    volatile int64_t cur;          // 書く側 → 読む側：いま iPhone が送っている形（同じ詰め方。serial は ACK の数）。1920x1080 に収めた形
    volatile uint32_t cur_kbps, cur_flags; // ACK の kbps と flags
    // ---- cam4k（前の版では pad の中。0 のままなら 4K の口は無い）
    volatile uint32_t ext_state;   // 1：4K の口がある（書く側が書く）
    uint32_t ext_pad;
    volatile int64_t cur_full;     // cur の、収めない本当の形（ext_state が 1 のときだけ。0 は無し）
    uint8_t pad[256 - 48 - WCAM_SHM_SLOTS * 32 - 40 - 16];
} wcam_shm_head; // 256 バイト。このあと枠が nslots 個（slot_bytes ずつ）

#define WCAM_CAP_FOLLOW 1u  // 書く側は req を読んで iPhone に頼み直す
#define WCAM_CAP_4K 2u      // cam4k：書く側は 4K の口を持てる（ext_state が 1 のときに使える）

// ---- cam4k：4K の口
#define WCAM_EXT_SUFFIX L"-4k"
#define WCAM_EXT_MAGIC 0x4b345749u // 'IW4K'
#define WCAM_EXT_MAXW 3840u
#define WCAM_EXT_MAXH 2160u
#define WCAM_EXT_SLOTS 3u
#define WCAM_EXT_SLOT_BYTES (WCAM_EXT_MAXW * WCAM_EXT_MAXH * 3u / 2u)
typedef struct {
    uint32_t magic, version, head_bytes, slot_bytes;
    uint32_t nslots, max_w, max_h, writer_pid;
    wcam_shm_slot slot[WCAM_EXT_SLOTS]; // 枠ごとの seq・幅・高さ・時刻（頭の seq と同じ番号を書く）
    uint8_t pad[256 - 32 - WCAM_EXT_SLOTS * 32];
} wcam_ext_head; // 256 バイト。このあと枠が nslots 個（slot_bytes ずつ）
#define WCAM_EXT_TOTAL (sizeof(wcam_ext_head) + (size_t) WCAM_EXT_SLOTS * WCAM_EXT_SLOT_BYTES)
#define WCAM_EXT_FRAME(head, i) ((uint8_t *) (head) + sizeof(wcam_ext_head) + (size_t) (i) * (head)->slot_bytes)

// 仮想カメラが名乗る一番大きい高さ（レジストリ）
#define WCAM_VCAM_REGKEY L"Software\\PocketGPU\\VirtualCamera"
#define WCAM_VCAM_REGKEY_TEST L"Software\\PocketGPU\\VirtualCamera-Test" // 試験用の名前の共有メモリ（PGPU_TEST_SHM）のときと、試験用の DLL が使う
#define WCAM_VCAM_REGVAL L"MaxHeight"

// 形を 64 ビットに詰める：幅 16・高さ 16・コマ数 8・カメラ 8（0 は指定なし、1〜4 がカメラ 0〜3）・serial 16（書くたびに変える）
#define WCAM_REQ_MAKE(w, h, fps, cam1, serial) \
    ((int64_t) (((uint64_t) (w) & 0xffff) | (((uint64_t) (h) & 0xffff) << 16) | (((uint64_t) (fps) & 0xff) << 32) | (((uint64_t) (cam1) & 0xff) << 40) | (((uint64_t) (serial) & 0xffff) << 48)))
#define WCAM_REQ_W(r) ((uint32_t) ((uint64_t) (r) & 0xffff))
#define WCAM_REQ_H(r) ((uint32_t) (((uint64_t) (r) >> 16) & 0xffff))
#define WCAM_REQ_FPS(r) ((uint32_t) (((uint64_t) (r) >> 32) & 0xff))
#define WCAM_REQ_CAM1(r) ((uint32_t) (((uint64_t) (r) >> 40) & 0xff))
#define WCAM_REQ_SERIAL(r) ((uint32_t) (((uint64_t) (r) >> 48) & 0xffff))
#define WCAM_REQ_SAME(a, b) ((((uint64_t) (a)) & 0xffffffffffffull) == (((uint64_t) (b)) & 0xffffffffffffull)) // serial を除いて同じ

#define WCAM_SHM_TOTAL (sizeof(wcam_shm_head) + (size_t) WCAM_SHM_SLOTS * WCAM_SHM_SLOT_BYTES)
#define WCAM_SHM_FRAME(head, i) ((uint8_t *) (head) + sizeof(wcam_shm_head) + (size_t) (i) * (head)->slot_bytes)

static inline int64_t wcam_shm_now_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t) ((double) c.QuadPart * 1e6 / (double) f.QuadPart);
}
