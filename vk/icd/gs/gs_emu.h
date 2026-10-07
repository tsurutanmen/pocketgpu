// geometry shader を持たない GPU（MoltenVK）のために、VS と GS の SPIR-V から「GS を真似る VS」を 1 本作る。
// 第 1 段は「点を受けて、三角の帯・線の帯・点を出す GS」だけ（粒を四角に広げる形。FluidCS11 の粒）。
//
// 描き方：元の描画 vkCmdDraw(n, 1, fv, fi) を、vkCmdDraw(max_vertices, n, 0, fv) に変える。
//   インスタンス 1 つが元の点 1 つ。頂点番号 k が「GS が k 番目に出す頂点」。
//   firstInstance に fv を入れるので、作った VS の中では InstanceIndex が元の VertexIndex と同じ値になる。
//   帯（strip）はインスタンスごとに切れるので、点ごとの四角が隣とつながらない。
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t max_vertices;    // GS の OutputVertices。描画の頂点の数になる
    uint32_t topology;        // 作った pipeline の VkPrimitiveTopology（0 点・2 線の帯・4 三角の帯）
    int vertex_input;         // VS が頂点の属性（Location の入力）を読む。読むなら binding を INSTANCE に変える
    int writes_layer;         // GS が gl_Layer か gl_ViewportIndex を書く（VS から書ける GPU が要る）
    int restart_mid;          // EndPrimitive が最後の Emit より前にある。2 本目からの帯は描かない
    char why[160];            // 作れなかったときの理由
} gs_emu_info;

// 作れたら 1。out は malloc した語の並び（free で返す）
int gs_emu_build(const uint32_t *vs, size_t vs_words, const uint32_t *gs, size_t gs_words,
                 uint32_t **out, size_t *out_words, gs_emu_info *info);
