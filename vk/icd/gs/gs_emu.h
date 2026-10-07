// geometry shader を持たない GPU（MoltenVK）のために、VS と GS の SPIR-V から「GS を真似る VS」を 1 本作る。
//
// 形は 2 つ。
// ・B（固定の取り出し）：点を受ける GS で、番号なし・インスタンス 1 つの描画だけ。
//   vkCmdDraw(n, 1, fv, fi) → vkCmdDraw(GS の出す頂点の数, n, 0, fv)。インスタンス 1 つが元の点 1 つ。頂点の属性は binding を INSTANCE にして読む。
// ・C1（自分で読む＝pull）：入力の形は点・線・三角・隣つき。番号つき・インスタンスつき・GS の instancing も。
//   作った VS は、頂点の属性と index を storage buffer として自分で読み、元の VS をプリミティブの頂点の数だけ呼んでから GS の本体を走らせる。
//   描画は vkCmdDraw(1 インスタンスの頂点の数, プリミティブの数 × インスタンスの数 × GS の instancing, 0, 0)。
//   描画ごとの値（index の場所・BaseVertex・各 binding の場所と間隔など）は、ドライバが書く小さな記録（下の GSR_*）を
//   dynamic な uniform buffer として読む。descriptor set は cfg->set 番に足す（binding 0 記録・1 index・2+b 頂点の buffer b）。
//   帯の途中で RestartStrip する GS は、出力を三角（線）の並びに直す（list_mode）。そのときの頂点の数は 3×(M−2)（線は 2×(M−1)）
#pragma once
#include <stddef.h>
#include <stdint.h>

// 記録（1 つの描画ごと・32 ビットの語の並び・256 バイト）
enum {
    GSR_FLAGS = 0,  // 1：番号つき
    GSR_IB = 1,     // index buffer の中で、最初の index の場所（バイト）
    GSR_VBASE = 2,  // 番号つきなら vertexOffset、番号なしなら firstVertex
    GSR_FINST = 3,  // firstInstance
    GSR_PRIMS = 4,  // 1 インスタンスあたりのプリミティブの数
    GSR_ISZ = 5,    // index 1 つのバイト数（1・2・4）
    GSR_IMASK = 6,  // index の値の取り出しの印（0xff・0xffff・0xffffffff）
    GSR_TOPO = 7,   // 0 並び・1 帯・2 扇（GS_TOPO_*）
    GSR_VB = 8,     // 8+2b：binding b の場所（バイト）、9+2b：間隔
    GSR_WORDS = 64,
};
enum { GS_TOPO_LIST = 0, GS_TOPO_STRIP = 1, GS_TOPO_FAN = 2 };
enum { GS_MAX_VB = 16, GS_REC_BYTES = 256 };

typedef struct { uint32_t location, binding, format, offset; } gs_attr;
typedef struct { uint32_t binding, rate, divisor; } gs_vbind; // rate：0 頂点・1 インスタンス

typedef struct {
    int pull;          // 0：B・1：C1
    uint32_t set;      // C1：足す descriptor set の番号
    uint32_t nattr, nbind;
    gs_attr attr[32];
    gs_vbind bind[32];
} gs_emu_cfg;

typedef struct {
    uint32_t max_vertices;    // GS の OutputVertices
    uint32_t draw_vertices;   // 作った描画の 1 インスタンスの頂点の数
    uint32_t topology;        // 作った pipeline の VkPrimitiveTopology
    uint32_t in_verts;        // 入力のプリミティブ 1 つの頂点の数（1・2・3・4・6）
    uint32_t invocations;     // GS の instancing の数
    uint32_t used_bindings;   // C1：VS が読む頂点の binding の印
    int vertex_input;         // VS が頂点の属性（Location の入力）を読む
    int writes_layer;         // GS が gl_Layer か gl_ViewportIndex を書く（VS から書ける GPU が要る）
    int restart_mid;          // EndPrimitive のあとにまた Emit することがある
    int list_mode;            // 出力を並びに直した
    char why[160];            // 作れなかったときの理由
} gs_emu_info;

// 作れたら 1。out は malloc した語の並び（free で返す）。cfg が NULL なら B
int gs_emu_build2(const uint32_t *vs, size_t vs_words, const uint32_t *gs, size_t gs_words, const gs_emu_cfg *cfg,
                  uint32_t **out, size_t *out_words, gs_emu_info *info);
int gs_emu_build(const uint32_t *vs, size_t vs_words, const uint32_t *gs, size_t gs_words,
                 uint32_t **out, size_t *out_words, gs_emu_info *info);
// GS の SPIR-V から、入力の形（ExecutionMode の 19〜23）だけを読む。読めなければ 0
uint32_t gs_emu_input_prim(const uint32_t *gs, size_t gs_words);
