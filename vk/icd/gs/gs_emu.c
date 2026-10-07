// geometry shader を、頂点シェーダー 1 本で真似る。
//
// 先例は Mesa の src/poly/nir/poly_nir_lower_gs.c の create_gs_rast_shader。GS を「出力の頂点ごとに走る頂点シェーダー」に直し、
// EmitVertex のたびに「自分の番の頂点か」を見て出力を選ぶ。Mesa は NIR の上でやるが、ここでは SPIR-V の上でやる。
// Mesa は VS を compute で先に走らせて結果を buffer に置くが、ここでは VS の本体を関数として、プリミティブの頂点の数だけ呼ぶ。
//
// 作る VS の中身：
//   1. VS の入力（組み込みと、C1 なら頂点の属性も）を Private にして、値を入れる
//      B ：InstanceIndex・BaseInstance から（頂点の属性は固定の取り出しのまま）
//      C1：記録（GSR_*）と index・頂点の buffer を自分で読む
//   2. VS の本体（もとの main）を関数として呼ぶ。VS の出力は Private にしてあるので、そこに残る
//   3. VS の出力を、GS の入力（これも Private にした配列）の c 番に写す。1〜3 をプリミティブの頂点の数だけ繰り返す
//   4. GS の本体を走らせる。GS の出力は Private の「下書き」にし、EmitVertex のたびに自分の番なら下書きを本当の出力に写す
//      帯：「数えた数 <= k」なら写す（k は VertexIndex）。k 番目が無ければ最後の頂点のままになり、帯の三角は潰れて描かれない
//      並び（list_mode）：帯を数え直し、三角 t の c 番の角（VertexIndex = 3t + c）を、直前 2 つの頂点の写しから選ぶ
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include "gs_emu.h"
#include "spv_table.h"

// ---- SPIR-V の番号（使う物だけ） ----
enum {
    OP_Name = 5, OP_Line = 8, OP_Extension = 10, OP_ExtInstImport = 11, OP_ExtInst = 12, OP_MemoryModel = 14, OP_EntryPoint = 15,
    OP_ExecutionMode = 16, OP_Capability = 17, OP_TypeVoid = 19, OP_TypeBool = 20, OP_TypeInt = 21, OP_TypeFloat = 22,
    OP_TypeVector = 23, OP_TypeMatrix = 24, OP_TypeImage = 25, OP_TypeSampler = 26, OP_TypeSampledImage = 27, OP_TypeArray = 28,
    OP_TypeRuntimeArray = 29, OP_TypeStruct = 30, OP_TypePointer = 32, OP_TypeFunction = 33, OP_Constant = 43, OP_ConstantComposite = 44,
    OP_ConstantNull = 46, OP_SpecConstantOp = 52, OP_Function = 54, OP_FunctionCall = 57, OP_Variable = 59, OP_Load = 61, OP_Store = 62,
    OP_CopyMemory = 63, OP_CopyMemorySized = 64, OP_AccessChain = 65, OP_InBoundsAccessChain = 66, OP_PtrAccessChain = 67,
    OP_InBoundsPtrAccessChain = 70, OP_Decorate = 71, OP_MemberDecorate = 72, OP_DecorationGroup = 73, OP_GroupDecorate = 74,
    OP_GroupMemberDecorate = 75, OP_VectorShuffle = 79, OP_CompositeConstruct = 80, OP_CompositeExtract = 81, OP_CopyObject = 83,
    OP_ImageWrite = 99, OP_ConvertSToF = 111, OP_ConvertUToF = 112, OP_Bitcast = 124, OP_IAdd = 128, OP_ISub = 130, OP_IMul = 132,
    OP_UDiv = 134, OP_FDiv = 136, OP_UMod = 137, OP_LogicalOr = 166, OP_LogicalAnd = 167, OP_Select = 169, OP_IEqual = 170, OP_INotEqual = 171,
    OP_UGreaterThanEqual = 174, OP_ULessThanEqual = 178, OP_ShiftRightLogical = 194, OP_ShiftLeftLogical = 196, OP_BitwiseAnd = 199,
    OP_BitFieldSExtract = 202, OP_BitFieldUExtract = 203, OP_EmitVertex = 218, OP_EndPrimitive = 219, OP_EmitStreamVertex = 220,
    OP_EndStreamPrimitive = 221, OP_LoopMerge = 246, OP_Label = 248, OP_NoLine = 317, OP_ExecutionModeId = 331, OP_DecorateId = 332,
    OP_DecorateString = 5632,
};
enum { SC_Input = 1, SC_Uniform = 2, SC_Output = 3, SC_Private = 6, SC_Function = 7, SC_StorageBuffer = 12 };
enum { DEC_SpecId = 1, DEC_Block = 2, DEC_BufferBlock = 3, DEC_ArrayStride = 6, DEC_BuiltIn = 11, DEC_NonWritable = 24, DEC_Stream = 29,
       DEC_Location = 30, DEC_Component = 31, DEC_Binding = 33, DEC_DescriptorSet = 34, DEC_Offset = 35, DEC_XfbBuffer = 36, DEC_XfbStride = 37 };
enum { BI_Position = 0, BI_PrimitiveId = 7, BI_InvocationId = 8, BI_Layer = 9, BI_ViewportIndex = 10, BI_VertexIndex = 42,
       BI_InstanceIndex = 43, BI_BaseVertex = 4424, BI_BaseInstance = 4425, BI_DrawIndex = 4426 };
enum { GLSL_FMax = 40, GLSL_UnpackHalf2x16 = 62 };

// ---- 語の並びと命令の並び ----
typedef struct { uint32_t *p; size_t n, cap; } wbuf;
typedef struct { uint32_t **v; size_t n, cap; } ilist; // v[i][0] が「語数 << 16 | 命令」

static void wb_put(wbuf *b, uint32_t w) {
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 1024; b->p = realloc(b->p, b->cap * sizeof *b->p); }
    b->p[b->n++] = w;
}
static void wb_ins(wbuf *b, const uint32_t *ins) { for (uint32_t i = 0; i < ins[0] >> 16; i++) wb_put(b, ins[i]); }
static void wb_op(wbuf *b, uint32_t op, uint32_t n, const uint32_t *w) {
    wb_put(b, ((n + 1) << 16) | op);
    for (uint32_t i = 0; i < n; i++) wb_put(b, w[i]);
}
#define OPW(b, op, ...) do { const uint32_t w_[] = { __VA_ARGS__ }; wb_op((b), (op), (uint32_t) (sizeof w_ / sizeof w_[0]), w_); } while (0)

static void il_push(ilist *l, uint32_t *ins) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = realloc(l->v, l->cap * sizeof *l->v); }
    l->v[l->n++] = ins;
}
static uint32_t *dup_ins(const uint32_t *ins) {
    uint32_t wc = ins[0] >> 16, *d = malloc(wc * 4);
    memcpy(d, ins, wc * 4);
    return d;
}
static uint32_t *make_ins(uint32_t op, uint32_t n, const uint32_t *w) {
    uint32_t *d = malloc((n + 1) * 4);
    d[0] = ((n + 1) << 16) | op;
    memcpy(d + 1, w, n * 4);
    return d;
}
#define OPC(ins) ((ins)[0] & 0xffff)
#define WC(ins) ((ins)[0] >> 16)

// ---- 命令表：どの語が id か ----
static const char *op_desc(uint32_t op) {
    size_t lo = 0, hi = SPV_NOPS;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (spv_ops[m].op < op) lo = m + 1; else hi = m;
    }
    return lo < SPV_NOPS && spv_ops[lo].op == op ? spv_ops[lo].o : NULL;
}

typedef void (*idfn)(uint32_t *w, int kind, void *u); // kind：0 id・1 結果の型・2 結果の id
static size_t walk_codes(uint32_t *ins, size_t wc, size_t k, const char *d, idfn fn, void *u);

static size_t walk_one(uint32_t *ins, size_t wc, size_t k, char c, char kl, idfn fn, void *u) {
    if (k >= wc) return wc;
    switch (c) {
    case 't': fn(&ins[k], 1, u); return k + 1;
    case 'r': fn(&ins[k], 2, u); return k + 1;
    case 'i': fn(&ins[k], 0, u); return k + 1;
    case 'n': return wc;
    case 's':
        while (k < wc) {
            uint32_t x = ins[k++];
            if (!(x & 0xff) || !(x & 0xff00) || !(x & 0xff0000) || !(x & 0xff000000)) break;
        }
        return k;
    case 'P': fn(&ins[k], 0, u); return k + 2;
    case 'Q': if (k + 1 < wc) fn(&ins[k + 1], 0, u); return k + 2;
    case 'D': fn(&ins[k], 0, u); if (k + 1 < wc) fn(&ins[k + 1], 0, u); return k + 2;
    case 'e': case 'b': {
        const spv_enum_kind *ek = &spv_enum_kinds[kl - 'A'];
        uint32_t v = ins[k++];
        for (uint32_t j = 0; j < ek->n; j++) {
            uint32_t ev = ek->e[j].v;
            if (c == 'e' ? ev == v : (ev && (v & ev) == ev)) {
                k = walk_codes(ins, wc, k, ek->e[j].p, fn, u);
                if (c == 'e') break;
            }
        }
        return k;
    }
    default: return k + 1; // 'l'
    }
}

static size_t walk_codes(uint32_t *ins, size_t wc, size_t k, const char *d, idfn fn, void *u) {
    for (const char *p = d; *p && k < wc;) {
        char q = 0, c, kl = 0;
        if (*p == '?' || *p == '*') q = *p++;
        c = *p++;
        if (c == 'e' || c == 'b') kl = *p++;
        k = walk_one(ins, wc, k, c, kl, fn, u);
        if (q == '*') while (k < wc) k = walk_one(ins, wc, k, c, kl, fn, u);
    }
    return k;
}

// 命令の中の id の語ごとに fn を呼ぶ。知らない命令なら 0
static int walk_ids(uint32_t *ins, idfn fn, void *u) {
    const char *d = op_desc(OPC(ins));
    if (!d) return 0;
    walk_codes(ins, WC(ins), 1, d, fn, u);
    if (OPC(ins) == OP_SpecConstantOp && WC(ins) > 4) {
        const char *d2 = op_desc(ins[3]);
        if (!d2) return 0;
        while (*d2 == 't' || *d2 == 'r') d2++;
        walk_codes(ins, WC(ins), 4, d2, fn, u);
    }
    return 1;
}

static uint32_t res_id(const uint32_t *ins) {
    const char *d = op_desc(OPC(ins));
    if (!d) return 0;
    if (d[0] == 't' && d[1] == 'r') return ins[2];
    return d[0] == 'r' ? ins[1] : 0;
}

static int is_type_op(uint32_t op) { return (op >= OP_TypeVoid && op <= 39) || op == 4417 || op == 4456 || op == 5358; }
// 同じ中身なら 1 つにまとめないといけない型（SPIR-V は「集まりでもポインターでもない型」の重複を許さない）
static int dedup_type_op(uint32_t op) {
    return (op >= OP_TypeVoid && op <= OP_TypeSampledImage) || op == OP_TypeFunction;
}

// ---- モジュールを節に分ける ----
enum { S_CAP, S_EXT, S_IMP, S_MM, S_EP, S_EM, S_DBG, S_ANN, S_GLOB, S_FUNC, S_N };
typedef struct { ilist s[S_N]; uint32_t ver, gen, bound; } smod;

static int sec_of(uint32_t op) {
    switch (op) {
    case OP_Capability: return S_CAP;
    case OP_Extension: return S_EXT;
    case OP_ExtInstImport: return S_IMP;
    case OP_MemoryModel: return S_MM;
    case OP_EntryPoint: return S_EP;
    case OP_ExecutionMode: case OP_ExecutionModeId: return S_EM;
    case 2: case 3: case 4: case OP_Name: case 6: case 7: case 330: return S_DBG;
    case OP_Decorate: case OP_MemberDecorate: case OP_DecorationGroup: case OP_GroupDecorate: case OP_GroupMemberDecorate:
    case OP_DecorateId: case OP_DecorateString: case 5633: return S_ANN;
    }
    return S_GLOB;
}

static int split(smod *m, const uint32_t *w, size_t n) {
    if (n < 5 || w[0] != 0x07230203u) return 0;
    m->ver = w[1];
    m->gen = w[2];
    m->bound = w[3];
    int infn = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16;
        if (!wc || i + wc > n) return 0;
        if ((w[i] & 0xffff) == OP_Function) infn = 1;
        il_push(&m->s[infn ? S_FUNC : sec_of(w[i] & 0xffff)], dup_ins(&w[i]));
        i += wc;
    }
    return 1;
}

static void smod_free(smod *m) {
    for (int s = 0; s < S_N; s++) {
        for (size_t i = 0; i < m->s[s].n; i++) free(m->s[s].v[i]);
        free(m->s[s].v);
    }
}

uint32_t gs_emu_input_prim(const uint32_t *w, size_t n) {
    if (n < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n || op == OP_Function) break;
        if (op == OP_ExecutionMode && wc >= 3 && w[i + 2] >= 19 && w[i + 2] <= 23) return w[i + 2];
        i += wc;
    }
    return 0;
}

// ---- 作業のまとめ ----
typedef struct { uint32_t var, builtin, loc, comp, type; } iovar;
enum { MAX_IO = 48 };

typedef struct {
    gs_emu_info *info;
    const gs_emu_cfg *cfg;
    int bad, pull;
    uint32_t ver, next, cap, base;
    uint32_t **def;     // id → 定義の命令
    uint8_t *conv;      // Private に変える変数
    uint32_t *plainmap; // 型 → 飾り（Block・BuiltIn）を外した写し
    uint32_t *sel;      // GS の出力（下書きにした変数）→ 本当の出力の変数
    uint32_t *root;     // ポインター → もとの変数
    smod g, v;          // v は id を付け替えた後
    ilist newglob, convvars, newvars, newann, newdbg, newimp, types;
    wbuf fn;
    uint32_t t_void, t_bool, t_u32, t_i32, t_f32, t_bi, t_v2f;
    uint32_t c_u0, c_u1, glsl;
    uint32_t in_vi, in_ii, in_bi, v_cnt, v_end, v_k; // v_k：受け持つ頂点（描かないプリミティブなら ~0）
    uint32_t gs_main, vs_main;
    int strip, pro_done;
    uint32_t nv, inv, outk;  // 入力の頂点の数・GS の instancing・出す形（0 点・1 線・2 三角）
    int list;                // 並びに直す
    uint32_t rec_var, ib_var, vb_var[GS_MAX_VB];
    uint32_t v_t, v_c, v_s, v_tc; // 並び：受け持つ三角・角、帯の中の頂点の数、できた三角の数
    uint32_t prev1[MAX_IO], prev2[MAX_IO];
    iovar vs_in[MAX_IO], vs_out[MAX_IO], gs_in[MAX_IO], gs_out[MAX_IO];
    int n_vs_in, n_vs_out, n_gs_in, n_gs_out;
} ctx;

static void fail(ctx *c, const char *fmt, ...) {
    if (c->bad) return;
    c->bad = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->info->why, sizeof c->info->why, fmt, ap);
    va_end(ap);
}

static uint32_t spv_new_id(ctx *c) {
    if (c->next + 1 >= c->cap) { fail(c, "id が足りない"); return c->cap - 1; }
    return c->next++;
}

static void set_def(ctx *c, uint32_t *ins) {
    uint32_t r = res_id(ins);
    if (r && r < c->cap) c->def[r] = ins;
}

static uint32_t *D(ctx *c, uint32_t id) { return id && id < c->cap ? c->def[id] : NULL; }

// 型・定数を足す（全体の宣言の節の、GS と VS の後ろに並ぶ）
static uint32_t add_glob(ctx *c, uint32_t op, uint32_t n, const uint32_t *w) {
    uint32_t *ins = make_ins(op, n, w);
    il_push(&c->newglob, ins);
    set_def(c, ins);
    if (is_type_op(op)) il_push(&c->types, ins);
    return res_id(ins);
}

// 同じ型があればそれを、無ければ作る。w は結果の id より後ろの語
static uint32_t find_type(ctx *c, uint32_t op, uint32_t n, const uint32_t *w) {
    for (size_t i = 0; i < c->types.n; i++) {
        uint32_t *t = c->types.v[i];
        if (OPC(t) == op && WC(t) == n + 2 && !memcmp(t + 2, w, n * 4)) return t[1];
    }
    return 0;
}
static uint32_t get_type(ctx *c, uint32_t op, uint32_t n, const uint32_t *w) {
    uint32_t id = find_type(c, op, n, w);
    if (id) return id;
    uint32_t tmp[16];
    tmp[0] = spv_new_id(c);
    memcpy(tmp + 1, w, n * 4);
    return add_glob(c, op, n + 1, tmp);
}
static uint32_t ty_ptr(ctx *c, uint32_t sc, uint32_t t) { uint32_t w[2] = { sc, t }; return get_type(c, OP_TypePointer, 2, w); }
static uint32_t ty_vec(ctx *c, uint32_t t, uint32_t n) { uint32_t w[2] = { t, n }; return get_type(c, OP_TypeVector, 2, w); }

// 32 ビットの定数。同じ型と値の OpConstant があればそれを使う
static uint32_t k32(ctx *c, uint32_t type, uint32_t v) {
    ilist *ls[3] = { &c->g.s[S_GLOB], &c->v.s[S_GLOB], &c->newglob };
    for (int m = 0; m < 3; m++)
        for (size_t i = 0; i < ls[m]->n; i++) {
            uint32_t *t = ls[m]->v[i];
            if (OPC(t) == OP_Constant && WC(t) == 4 && t[1] == type && t[3] == v) return t[2];
        }
    uint32_t w[3] = { type, spv_new_id(c), v };
    return add_glob(c, OP_Constant, 3, w);
}
static uint32_t ku(ctx *c, uint32_t v) { return k32(c, c->t_u32, v); }
static uint32_t kf(ctx *c, float f) { uint32_t b; memcpy(&b, &f, 4); return k32(c, c->t_f32, b); }
static uint32_t const_value(ctx *c, uint32_t id) {
    uint32_t *d = D(c, id);
    return d && OPC(d) == OP_Constant && WC(d) >= 4 ? d[3] : 0;
}

static uint32_t ptr_pointee(ctx *c, uint32_t pt) { uint32_t *d = D(c, pt); return d && OPC(d) == OP_TypePointer ? d[3] : 0; }
static uint32_t ptr_class(ctx *c, uint32_t pt) { uint32_t *d = D(c, pt); return d && OPC(d) == OP_TypePointer ? d[2] : ~0u; }

// ---- 飾りを探す（GS と VS の両方の飾りの節から） ----
static int deco(ctx *c, uint32_t id, uint32_t dec, uint32_t *val) {
    smod *ms[2] = { &c->g, &c->v };
    for (int m = 0; m < 2; m++)
        for (size_t i = 0; i < ms[m]->s[S_ANN].n; i++) {
            uint32_t *a = ms[m]->s[S_ANN].v[i];
            if (OPC(a) == OP_Decorate && a[1] == id && a[2] == dec) {
                if (val) *val = WC(a) > 3 ? a[3] : 0;
                return 1;
            }
        }
    return 0;
}
static int member_builtin(ctx *c, uint32_t st, uint32_t mem, uint32_t *b) {
    smod *ms[2] = { &c->g, &c->v };
    for (int m = 0; m < 2; m++)
        for (size_t i = 0; i < ms[m]->s[S_ANN].n; i++) {
            uint32_t *a = ms[m]->s[S_ANN].v[i];
            if (OPC(a) == OP_MemberDecorate && a[1] == st && a[2] == mem && a[3] == DEC_BuiltIn) { *b = a[4]; return 1; }
        }
    return 0;
}
static int io_block(ctx *c, uint32_t st) {
    uint32_t *d = D(c, st), b;
    if (!d || OPC(d) != OP_TypeStruct) return 0;
    if (deco(c, st, DEC_Block, NULL) || deco(c, st, DEC_BufferBlock, NULL)) return 1;
    for (uint32_t m = 0; m + 2 < WC(d); m++) if (member_builtin(c, st, m, &b)) return 1;
    return 0;
}

// Block・BuiltIn の飾りを持つ構造体を、飾りの無い写しに置き換えた型（Private の変数に使う）
static uint32_t plain(ctx *c, uint32_t t) {
    if (!t || t >= c->cap) return t;
    if (c->plainmap[t]) return c->plainmap[t];
    uint32_t *d = D(c, t), r = t;
    if (d && OPC(d) == OP_TypeStruct && WC(d) - 2 < 60) {
        uint32_t n = WC(d) - 2, w[64];
        int changed = io_block(c, t);
        for (uint32_t i = 0; i < n; i++) {
            w[1 + i] = plain(c, d[2 + i]);
            if (w[1 + i] != d[2 + i]) changed = 1;
        }
        if (changed) { w[0] = spv_new_id(c); r = add_glob(c, OP_TypeStruct, n + 1, w); }
    } else if (d && OPC(d) == OP_TypeArray) {
        uint32_t e = plain(c, d[2]);
        if (e != d[2]) { uint32_t w[3] = { spv_new_id(c), e, d[3] }; r = add_glob(c, OP_TypeArray, 3, w); }
    }
    c->plainmap[t] = r;
    return r;
}

// ---- VS を読み込む：id を GS の後ろへずらし、同じ型は GS の物にまとめる ----
typedef struct { ctx *c; const uint32_t *map; uint32_t n; int keep_res; } remap_u;
static void remap_cb(uint32_t *w, int kind, void *u_) {
    remap_u *u = u_;
    if (kind == 2 && u->keep_res) return;
    if (*w >= u->n) { fail(u->c, "VS の id %u が範囲の外", *w); return; }
    *w = u->map[*w];
}

static int str_eq(const uint32_t *a, const uint32_t *b, uint32_t na, uint32_t nb) { return na == nb && !memcmp(a, b, na * 4); }

static void load_vs(ctx *c, const uint32_t *w, size_t n) {
    smod raw = { 0 };
    if (!split(&raw, w, n)) { fail(c, "VS の SPIR-V が読めない"); return; }
    uint32_t vb = raw.bound, *map = calloc(vb, 4), *spec = calloc(vb, 4);
    uint8_t *drop = calloc(vb, 1);
    for (uint32_t i = 1; i < vb; i++) map[i] = c->base + i;
    // GLSL.std.450 などは、GS と同じ名前の物を使う
    for (size_t i = 0; i < raw.s[S_IMP].n; i++) {
        uint32_t *a = raw.s[S_IMP].v[i];
        for (size_t j = 0; j < c->g.s[S_IMP].n; j++) {
            uint32_t *b = c->g.s[S_IMP].v[j];
            if (a[1] < vb && str_eq(a + 2, b + 2, WC(a) - 2, WC(b) - 2)) { map[a[1]] = b[1]; drop[a[1]] = 1; }
        }
    }
    // SpecId（DXVK 1.10 は binding ごとに 1 つ持つ）
    for (size_t i = 0; i < raw.s[S_ANN].n; i++) {
        uint32_t *a = raw.s[S_ANN].v[i];
        if (OPC(a) == OP_Decorate && a[2] == DEC_SpecId && a[1] < vb) spec[a[1]] = a[3] + 1;
    }
    // 型・定数・変数を前から順に。参照する id はもう決まっている
    for (size_t i = 0; i < raw.s[S_GLOB].n && !c->bad; i++) {
        uint32_t *ins = dup_ins(raw.s[S_GLOB].v[i]), op = OPC(ins), r = res_id(ins);
        if (op == OP_Line || op == OP_NoLine) { free(ins); continue; }
        remap_u u = { c, map, vb, 1 };
        if (!walk_ids(ins, remap_cb, &u)) { fail(c, "VS に知らない命令 %u", op); free(ins); break; }
        if (r && r < vb && dedup_type_op(op)) {
            uint32_t f = find_type(c, op, WC(ins) - 2, ins + 2);
            if (f) { map[r] = f; drop[r] = 1; free(ins); continue; }
        }
        if (r && r < vb && spec[r]) {
            // 同じ SpecId を GS も持つなら、中身が同じときだけ 1 つにまとめる
            for (size_t j = 0; j < c->g.s[S_ANN].n; j++) {
                uint32_t *a = c->g.s[S_ANN].v[j], *gd;
                if (OPC(a) != OP_Decorate || a[2] != DEC_SpecId || a[3] + 1 != spec[r]) continue;
                gd = D(c, a[1]);
                if (gd && OPC(gd) == op && WC(gd) == WC(ins) && gd[1] == ins[1] && !memcmp(gd + 3, ins + 3, (WC(ins) - 3) * 4)) {
                    map[r] = a[1];
                    drop[r] = 1;
                } else fail(c, "SpecId %u が VS と GS でぶつかる", spec[r] - 1);
            }
            if (drop[r]) { free(ins); continue; }
        }
        if (r) {
            remap_u ur = { c, map, vb, 0 };
            uint32_t *rp = op_desc(op)[0] == 't' ? &ins[2] : &ins[1];
            remap_cb(rp, 2, &ur);
        }
        il_push(&c->v.s[S_GLOB], ins);
        set_def(c, ins);
        if (is_type_op(op)) il_push(&c->types, ins);
    }
    // 残りの節。VS の名前などの調べ用の物は落とす（OpString を落とすので OpLine も落とす）
    for (int s = 0; s < S_N && !c->bad; s++) {
        if (s == S_GLOB || s == S_DBG) continue;
        for (size_t i = 0; i < raw.s[s].n; i++) {
            uint32_t *a = raw.s[s].v[i];
            if (s == S_IMP && drop[a[1]]) continue;
            if (s == S_ANN && (OPC(a) == OP_DecorationGroup || OPC(a) == OP_GroupDecorate || OPC(a) == OP_GroupMemberDecorate)) {
                fail(c, "VS に decoration group がある");
                break;
            }
            if (s == S_ANN && a[1] < vb && drop[a[1]]) continue;
            if (s == S_FUNC && (OPC(a) == OP_Line || OPC(a) == OP_NoLine)) continue;
            uint32_t *ins = dup_ins(a);
            remap_u u = { c, map, vb, 0 };
            if (!walk_ids(ins, remap_cb, &u)) { fail(c, "VS に知らない命令 %u", OPC(ins)); free(ins); break; }
            il_push(&c->v.s[s], ins);
            set_def(c, ins);
        }
    }
    c->v.ver = raw.ver;
    c->v.bound = vb;
    smod_free(&raw);
    free(map);
    free(spec);
    free(drop);
}

// ---- 調べる ----
static void find_entry(ctx *c, smod *m, uint32_t model, uint32_t *main_id, const char *who) {
    int n = 0;
    for (size_t i = 0; i < m->s[S_EP].n; i++) {
        uint32_t *e = m->s[S_EP].v[i];
        if (e[1] == model) { *main_id = e[2]; n++; } else n += 100;
    }
    if (n != 1) fail(c, "%s の入口が 1 つでない", who);
}

static void collect_io(ctx *c, smod *m, uint32_t sc, iovar *out, int *nout, int max) {
    for (size_t i = 0; i < m->s[S_GLOB].n; i++) {
        uint32_t *d = m->s[S_GLOB].v[i];
        if (OPC(d) != OP_Variable || d[3] != sc) continue;
        if (*nout >= max) { fail(c, "入出力の変数が多すぎる"); return; }
        iovar *v = &out[(*nout)++];
        memset(v, 0, sizeof *v);
        v->var = d[2];
        v->type = ptr_pointee(c, d[1]);
        v->builtin = ~0u;
        deco(c, v->var, DEC_BuiltIn, &v->builtin);
        v->loc = ~0u;
        deco(c, v->var, DEC_Location, &v->loc);
        deco(c, v->var, DEC_Component, &v->comp);
    }
}

// 書いてはいけない所（buffer・画像など）に書く命令が無いか。何度も走らせるので、書く VS・GS は真似られない
static void check_side_effects(ctx *c, smod *m, const char *who) {
    for (size_t i = 0; i < m->s[S_FUNC].n && !c->bad; i++) {
        uint32_t *ins = m->s[S_FUNC].v[i], op = OPC(ins);
        if (op == OP_ImageWrite || (op >= 227 && op <= 242) || op == 318 || op == 319 || op == 5614 || op == 5615 || op == 6035) {
            fail(c, "%s が buffer や画像に書く（命令 %u）", who, op);
            return;
        }
        if (op == OP_Store || op == OP_CopyMemory || op == OP_CopyMemorySized) {
            uint32_t *p = D(c, ins[1]);
            uint32_t sc = !p ? ~0u : OPC(p) == OP_Variable ? p[3] : ptr_class(c, p[1]);
            if (sc != SC_Function && sc != SC_Private && sc != SC_Output) fail(c, "%s が buffer に書く（記憶の種類 %u）", who, sc);
        }
    }
}

// 変える変数を指すポインターが、読み書きと access chain 以外に使われていないか
typedef struct { ctx *c; int hit; } use_u;
static void use_cb(uint32_t *w, int kind, void *u_) {
    use_u *u = u_;
    if (kind == 0 && *w < u->c->cap && u->c->root[*w] && u->c->conv[u->c->root[*w]]) u->hit = 1;
}
static int chain_op(uint32_t op) {
    return op == OP_AccessChain || op == OP_InBoundsAccessChain || op == OP_PtrAccessChain || op == OP_InBoundsPtrAccessChain || op == OP_CopyObject;
}

static void compute_roots(ctx *c) {
    smod *ms[2] = { &c->g, &c->v };
    for (int m = 0; m < 2; m++)
        for (size_t i = 0; i < ms[m]->s[S_GLOB].n; i++) {
            uint32_t *d = ms[m]->s[S_GLOB].v[i];
            if (OPC(d) == OP_Variable) c->root[d[2]] = d[2];
        }
    for (int pass = 0, changed = 1; changed && pass < 16; pass++) {
        changed = 0;
        for (int m = 0; m < 2; m++)
            for (size_t i = 0; i < ms[m]->s[S_FUNC].n; i++) {
                uint32_t *d = ms[m]->s[S_FUNC].v[i];
                if (!chain_op(OPC(d)) || d[2] >= c->cap || d[3] >= c->cap) continue;
                if (c->root[d[3]] && c->root[d[2]] != c->root[d[3]]) { c->root[d[2]] = c->root[d[3]]; changed = 1; }
            }
    }
    for (int m = 0; m < 2; m++)
        for (size_t i = 0; i < ms[m]->s[S_FUNC].n && !c->bad; i++) {
            uint32_t *d = ms[m]->s[S_FUNC].v[i], op = OPC(d);
            if (chain_op(op) || op == OP_Load || op == OP_Store || op == OP_CopyMemory) continue;
            use_u u = { c, 0 };
            walk_ids(d, use_cb, &u);
            if (u.hit) fail(c, "入出力の変数を指すポインターを、命令 %u に渡している", op);
        }
}

// ---- 関数の中に書く物 ----
static uint32_t op1(ctx *c, uint32_t op, uint32_t type, uint32_t a) { uint32_t x = spv_new_id(c); OPW(&c->fn, op, type, x, a); return x; }
static uint32_t op2(ctx *c, uint32_t op, uint32_t type, uint32_t a, uint32_t b) { uint32_t x = spv_new_id(c); OPW(&c->fn, op, type, x, a, b); return x; }
static uint32_t op3(ctx *c, uint32_t op, uint32_t type, uint32_t a, uint32_t b, uint32_t d) { uint32_t x = spv_new_id(c); OPW(&c->fn, op, type, x, a, b, d); return x; }
static uint32_t ext1(ctx *c, uint32_t type, uint32_t inst, uint32_t a) { uint32_t x = spv_new_id(c); OPW(&c->fn, OP_ExtInst, type, x, c->glsl, inst, a); return x; }
static uint32_t ext2(ctx *c, uint32_t type, uint32_t inst, uint32_t a, uint32_t b) { uint32_t x = spv_new_id(c); OPW(&c->fn, OP_ExtInst, type, x, c->glsl, inst, a, b); return x; }
static uint32_t uadd(ctx *c, uint32_t a, uint32_t b) { return op2(c, OP_IAdd, c->t_u32, a, b); }
static uint32_t umul(ctx *c, uint32_t a, uint32_t b) { return op2(c, OP_IMul, c->t_u32, a, b); }
static uint32_t ueq(ctx *c, uint32_t a, uint32_t b) { return op2(c, OP_IEqual, c->t_bool, a, b); }

static void store_bitcast(ctx *c, uint32_t var, uint32_t val, uint32_t from_type) {
    uint32_t t = ptr_pointee(c, D(c, var)[1]);
    if (t != from_type) val = op1(c, OP_Bitcast, t, val);
    OPW(&c->fn, OP_Store, var, val);
}

// access chain（path は定数の id の並び）。結果の型は sc の記憶の種類の type へのポインター
static uint32_t chain(ctx *c, uint32_t sc, uint32_t type, uint32_t base, const uint32_t *path, int n) {
    if (!n) return base;
    uint32_t w[16], id = spv_new_id(c);
    w[0] = ty_ptr(c, sc, type);
    w[1] = id;
    w[2] = base;
    for (int i = 0; i < n && i < 12; i++) w[3 + i] = path[i];
    wb_op(&c->fn, OP_AccessChain, 3 + (uint32_t) n, w);
    return id;
}

static uint32_t load(ctx *c, uint32_t type, uint32_t ptr) {
    uint32_t x = spv_new_id(c);
    OPW(&c->fn, OP_Load, type, x, ptr);
    return x;
}
static uint32_t load_u32(ctx *c, uint32_t var) {
    uint32_t t = ptr_pointee(c, D(c, var)[1]), x = load(c, t, var);
    return t == c->t_u32 ? x : op1(c, OP_Bitcast, c->t_u32, x);
}

// スカラーの条件で選ぶ（1.4 より前の OpSelect は、ベクトルを選ぶとき条件も同じ長さの bool のベクトル）
static uint32_t vsel(ctx *c, uint32_t type, uint32_t cond, uint32_t a, uint32_t b) {
    uint32_t *d = D(c, type);
    if (d && OPC(d) == OP_TypeVector && c->ver < 0x10400) {
        uint32_t w[8] = { ty_vec(c, c->t_bool, d[3]), spv_new_id(c) };
        for (uint32_t i = 0; i < d[3]; i++) w[2 + i] = cond;
        wb_op(&c->fn, OP_CompositeConstruct, 2 + d[3], w);
        cond = w[1];
    }
    return op3(c, OP_Select, type, cond, a, b);
}

// 値の形を合わせる（vecN → vecM・スカラー ↔ ベクトル）。合わせられなければ 0
static uint32_t convert(ctx *c, uint32_t val, uint32_t from, uint32_t to) {
    if (from == to) return val;
    uint32_t *a = D(c, from), *b = D(c, to);
    if (!a || !b) return 0;
    uint32_t as = OPC(a) == OP_TypeVector ? a[2] : from, bs = OPC(b) == OP_TypeVector ? b[2] : to;
    uint32_t an = OPC(a) == OP_TypeVector ? a[3] : 1, bn = OPC(b) == OP_TypeVector ? b[3] : 1;
    if (as != bs) return 0;
    uint32_t x = spv_new_id(c);
    if (bn == 1) { OPW(&c->fn, OP_CompositeExtract, to, x, val, 0); return x; }
    if (an == 1) {
        uint32_t w[8] = { to, x };
        for (uint32_t i = 0; i < bn; i++) w[2 + i] = val;
        wb_op(&c->fn, OP_CompositeConstruct, 2 + bn, w);
        return x;
    }
    uint32_t w[10] = { to, x, val, val };
    for (uint32_t i = 0; i < bn; i++) w[4 + i] = i < an ? i : 0;
    wb_op(&c->fn, OP_VectorShuffle, 4 + bn, w);
    return x;
}

// VS の出力のうち、組み込み b を持つ物を読む（単独の変数でも、構造体の中の 1 つでも）。無ければ 0
static uint32_t load_vs_builtin(ctx *c, uint32_t b, uint32_t *type) {
    for (int i = 0; i < c->n_vs_out; i++) {
        iovar *o = &c->vs_out[i];
        if (o->builtin == b) { *type = o->type; return load(c, o->type, o->var); }
        uint32_t *d = D(c, o->type), mb;
        if (d && OPC(d) == OP_TypeStruct)
            for (uint32_t m = 0; m + 2 < WC(d); m++)
                if (member_builtin(c, o->type, m, &mb) && mb == b) {
                    uint32_t idx = ku(c, m);
                    *type = d[2 + m];
                    return load(c, d[2 + m], chain(c, SC_Private, d[2 + m], o->var, &idx, 1));
                }
    }
    return 0;
}

// GS の出力のうち、組み込みの Position の本当の出力（ポインター）と型。無ければ 0
static uint32_t sel_position(ctx *c, uint32_t *type) {
    for (int i = 0; i < c->n_gs_out; i++) {
        iovar *o = &c->gs_out[i];
        uint32_t s = c->sel[o->var];
        if (o->builtin == BI_Position) { *type = o->type; return s; }
        uint32_t *d = D(c, o->type), mb;
        if (d && OPC(d) == OP_TypeStruct)
            for (uint32_t m = 0; m + 2 < WC(d); m++)
                if (member_builtin(c, o->type, m, &mb) && mb == BI_Position) {
                    uint32_t idx = ku(c, m);
                    *type = d[2 + m];
                    return chain(c, SC_Output, d[2 + m], s, &idx, 1);
                }
    }
    return 0;
}

// VS の出力を、GS の入力の配列の vi 番に写す
static void copy_vs_to_gs(ctx *c, uint32_t vi) {
    uint32_t idx = ku(c, vi);
    for (int i = 0; i < c->n_gs_in; i++) {
        iovar *g = &c->gs_in[i];
        if (g->builtin == BI_PrimitiveId || g->builtin == BI_InvocationId) continue;
        uint32_t *arr = D(c, g->type);
        if (!arr || OPC(arr) != OP_TypeArray) continue;
        uint32_t elem = arr[2], *ed = D(c, elem), t = 0, val;
        if (g->loc != ~0u) {
            for (int j = 0; j < c->n_vs_out; j++) {
                iovar *o = &c->vs_out[j];
                if (o->loc != g->loc || o->comp != g->comp) continue;
                val = convert(c, load(c, o->type, o->var), o->type, elem);
                if (val) OPW(&c->fn, OP_Store, chain(c, SC_Private, elem, g->var, &idx, 1), val);
                break;
            }
        } else if (g->builtin != ~0u) {
            val = load_vs_builtin(c, g->builtin, &t);
            if (val && (val = convert(c, val, t, elem))) OPW(&c->fn, OP_Store, chain(c, SC_Private, elem, g->var, &idx, 1), val);
        } else if (ed && OPC(ed) == OP_TypeStruct) {
            // gl_in[] のような組み込みの構造体（DXVK 1.10）
            for (uint32_t m = 0; m + 2 < WC(ed); m++) {
                uint32_t mb, path[2] = { idx, ku(c, m) };
                if (!member_builtin(c, elem, m, &mb)) continue;
                val = load_vs_builtin(c, mb, &t);
                if (val && (val = convert(c, val, t, ed[2 + m]))) OPW(&c->fn, OP_Store, chain(c, SC_Private, ed[2 + m], g->var, path, 2), val);
            }
        }
    }
}

// GS の組み込みの入力（PrimitiveId・InvocationId）
static void set_gs_builtins(ctx *c, uint32_t prim, uint32_t inv_id) {
    for (int i = 0; i < c->n_gs_in; i++) {
        iovar *g = &c->gs_in[i];
        if (g->builtin == BI_PrimitiveId) store_bitcast(c, g->var, prim, c->t_u32);
        if (g->builtin == BI_InvocationId) store_bitcast(c, g->var, inv_id, c->t_u32);
    }
}

// 数える物と、位置の初め（何も選ばれなければ画面の外の 1 点）
static void init_counters(ctx *c) {
    OPW(&c->fn, OP_Store, c->v_cnt, c->c_u0);
    OPW(&c->fn, OP_Store, c->v_end, c->c_u0);
    if (c->list) {
        OPW(&c->fn, OP_Store, c->v_s, c->c_u0);
        OPW(&c->fn, OP_Store, c->v_tc, c->c_u0);
    }
    uint32_t pt = 0, pp = sel_position(c, &pt), *pd = D(c, pt);
    if (pp && pd && OPC(pd) == OP_TypeVector && pd[3] == 4) {
        uint32_t f = pd[2], two = k32(c, f, 0x40000000u), z = k32(c, f, 0), one = k32(c, f, 0x3f800000u);
        uint32_t w[6] = { pt, spv_new_id(c), two, two, z, one };
        uint32_t outside = add_glob(c, OP_ConstantComposite, 6, w);
        OPW(&c->fn, OP_Store, pp, outside);
    }
}

// ---- B：本体の前に足す物 ----
static void prologue_fixed(ctx *c) {
    uint32_t ii = 0, bi = 0, zero_bi = k32(c, c->t_bi, 0);
    if (c->in_ii) ii = load(c, c->t_bi, c->in_ii);
    if (c->in_bi) bi = load(c, c->t_bi, c->in_bi);
    // 1. VS の組み込みの入力。元の点の番号は InstanceIndex、元の firstVertex は BaseInstance に入っている
    for (int i = 0; i < c->n_vs_in; i++) {
        iovar *v = &c->vs_in[i];
        if (v->builtin == ~0u) continue;
        uint32_t val = v->builtin == BI_VertexIndex ? ii : v->builtin == BI_BaseVertex ? bi : zero_bi;
        store_bitcast(c, v->var, val, c->t_bi);
    }
    // 2. VS の本体を呼ぶ
    OPW(&c->fn, OP_FunctionCall, c->t_void, spv_new_id(c), c->vs_main);
    OPW(&c->fn, OP_Store, c->v_k, load_u32(c, c->in_vi));
    // 3. VS の出力を GS の入力（1 要素の配列）の 0 番に写す
    copy_vs_to_gs(c, 0);
    for (int i = 0; i < c->n_gs_in; i++) {
        iovar *g = &c->gs_in[i];
        if (g->builtin == BI_PrimitiveId) store_bitcast(c, g->var, op2(c, OP_ISub, c->t_bi, ii, bi), c->t_bi);
        if (g->builtin == BI_InvocationId) store_bitcast(c, g->var, zero_bi, c->t_bi);
    }
    init_counters(c);
}

// ---- C1：記録と buffer を読む ----
static uint32_t rec_word(ctx *c, uint32_t w) {
    uint32_t path[3] = { ku(c, 0), ku(c, w / 4), ku(c, w % 4) };
    return load(c, c->t_u32, chain(c, SC_Uniform, c->t_u32, c->rec_var, path, 3));
}
static uint32_t sb_word(ctx *c, uint32_t var, uint32_t byte) {
    uint32_t path[2] = { ku(c, 0), op2(c, OP_ShiftRightLogical, c->t_u32, byte, ku(c, 2)) };
    return load(c, c->t_u32, chain(c, SC_StorageBuffer, c->t_u32, var, path, 2));
}

// 頂点の形式
enum { K_F32, K_F16, K_UNORM, K_SNORM, K_UINT, K_SINT, K_USCALED, K_SSCALED };
typedef struct { uint8_t n, cs, kind, bgra, pack; } fmti;
static int fmt_info(uint32_t f, fmti *o) {
    static const uint8_t k8[7] = { K_UNORM, K_SNORM, K_USCALED, K_SSCALED, K_UINT, K_SINT, K_UNORM };
    static const uint8_t k16[7] = { K_UNORM, K_SNORM, K_USCALED, K_SSCALED, K_UINT, K_SINT, K_F16 };
    memset(o, 0, sizeof *o);
    if (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SINT) { *o = (fmti) { 1, 1, k8[f - VK_FORMAT_R8_UNORM], 0, 0 }; return 1; }
    if (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SINT) { *o = (fmti) { 2, 1, k8[f - VK_FORMAT_R8G8_UNORM], 0, 0 }; return 1; }
    if (f >= VK_FORMAT_R8G8B8A8_UNORM && f <= VK_FORMAT_R8G8B8A8_SINT) { *o = (fmti) { 4, 1, k8[f - VK_FORMAT_R8G8B8A8_UNORM], 0, 0 }; return 1; }
    if (f >= VK_FORMAT_B8G8R8A8_UNORM && f <= VK_FORMAT_B8G8R8A8_SINT) { *o = (fmti) { 4, 1, k8[f - VK_FORMAT_B8G8R8A8_UNORM], 1, 0 }; return 1; }
    if (f >= VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32) {
        *o = (fmti) { 4, 4, k8[f - VK_FORMAT_A2B10G10R10_UNORM_PACK32], 0, 1 };
        return 1;
    }
    if (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT) { *o = (fmti) { 1, 2, k16[f - VK_FORMAT_R16_UNORM], 0, 0 }; return 1; }
    if (f >= VK_FORMAT_R16G16_UNORM && f <= VK_FORMAT_R16G16_SFLOAT) { *o = (fmti) { 2, 2, k16[f - VK_FORMAT_R16G16_UNORM], 0, 0 }; return 1; }
    if (f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT) { *o = (fmti) { 4, 2, k16[f - VK_FORMAT_R16G16B16A16_UNORM], 0, 0 }; return 1; }
    static const uint8_t k32k[3] = { K_UINT, K_SINT, K_F32 };
    if (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32G32B32A32_SFLOAT) {
        uint32_t i = f - VK_FORMAT_R32_UINT;
        *o = (fmti) { (uint8_t) (i / 3 + 1), 4, k32k[i % 3], 0, 0 };
        return 1;
    }
    return 0;
}

// 頂点の buffer の byte 番地 base から、形式 fmt の 1 つを vec4 に読む。*vt に vec4 の型
static uint32_t fetch(ctx *c, uint32_t fmt, uint32_t var, uint32_t base, uint32_t *vt) {
    fmti f;
    if (!fmt_info(fmt, &f)) { fail(c, "頂点の形式 %u はまだ読めない", fmt); return 0; }
    int sgn = f.kind == K_SNORM || f.kind == K_SINT || f.kind == K_SSCALED;
    uint32_t ct = f.kind == K_UINT ? c->t_u32 : f.kind == K_SINT ? c->t_i32 : c->t_f32, comp[4], word0 = 0;
    if (f.pack) word0 = sb_word(c, var, base);
    for (uint32_t i = 0; i < f.n; i++) {
        uint32_t src, shift, bits;
        if (f.pack) {
            src = word0;
            shift = ku(c, i * 10);
            bits = i == 3 ? 2 : 10;
        } else {
            uint32_t byte = i ? uadd(c, base, ku(c, i * f.cs)) : base;
            src = sb_word(c, var, byte);
            shift = op2(c, OP_ShiftLeftLogical, c->t_u32, op2(c, OP_BitwiseAnd, c->t_u32, byte, ku(c, 3)), ku(c, 3));
            bits = f.cs * 8;
        }
        uint32_t raw = 0, sraw = 0, val = 0;
        if (bits == 32) {
            raw = src;
            if (sgn) sraw = op1(c, OP_Bitcast, c->t_i32, src);
        } else if (sgn) {
            sraw = op3(c, OP_BitFieldSExtract, c->t_i32, op1(c, OP_Bitcast, c->t_i32, src), shift, ku(c, bits));
        } else {
            raw = op3(c, OP_BitFieldUExtract, c->t_u32, src, shift, ku(c, bits));
        }
        switch (f.kind) {
        case K_F32: val = op1(c, OP_Bitcast, c->t_f32, raw); break;
        case K_F16: val = op2(c, OP_CompositeExtract, c->t_f32, ext1(c, c->t_v2f, GLSL_UnpackHalf2x16, raw), 0); break;
        case K_UNORM: val = op2(c, OP_FDiv, c->t_f32, op1(c, OP_ConvertUToF, c->t_f32, raw), kf(c, (float) ((1u << bits) - 1))); break;
        case K_SNORM:
            val = op2(c, OP_FDiv, c->t_f32, op1(c, OP_ConvertSToF, c->t_f32, sraw), kf(c, (float) ((1u << (bits - 1)) - 1)));
            val = ext2(c, c->t_f32, GLSL_FMax, val, kf(c, -1.0f));
            break;
        case K_USCALED: val = op1(c, OP_ConvertUToF, c->t_f32, raw); break;
        case K_SSCALED: val = op1(c, OP_ConvertSToF, c->t_f32, sraw); break;
        case K_UINT: val = raw; break;
        case K_SINT: val = sraw; break;
        }
        comp[i] = val;
    }
    if (f.bgra) { uint32_t t = comp[0]; comp[0] = comp[2]; comp[2] = t; }
    for (uint32_t i = f.n; i < 4; i++) {
        uint32_t one = ct == c->t_f32 ? 0x3f800000u : 1u;
        comp[i] = k32(c, ct, i == 3 ? one : 0);
    }
    *vt = ty_vec(c, ct, 4);
    uint32_t x = spv_new_id(c);
    OPW(&c->fn, OP_CompositeConstruct, *vt, x, comp[0], comp[1], comp[2], comp[3]);
    return x;
}

static const gs_attr *find_attr(const gs_emu_cfg *cfg, uint32_t loc) {
    for (uint32_t i = 0; i < cfg->nattr; i++) if (cfg->attr[i].location == loc) return &cfg->attr[i];
    return NULL;
}
static const gs_vbind *find_bind(const gs_emu_cfg *cfg, uint32_t b) {
    for (uint32_t i = 0; i < cfg->nbind; i++) if (cfg->bind[i].binding == b) return &cfg->bind[i];
    return NULL;
}

// 型の数の部分（スカラーの型と成分の数）
static uint32_t scalar_of(ctx *c, uint32_t t, uint32_t *n) {
    uint32_t *d = D(c, t);
    if (d && OPC(d) == OP_TypeVector) { *n = d[3]; return d[2]; }
    *n = 1;
    return t;
}

// VS の頂点の属性 1 つ（Location の入力）を読んで入れる
static void load_attr(ctx *c, iovar *v, uint32_t vtx, uint32_t finst, uint32_t j) {
    uint32_t vt = plain(c, v->type), n, st = scalar_of(c, vt, &n), *sd = D(c, st);
    if (!sd || (OPC(sd) != OP_TypeInt && OPC(sd) != OP_TypeFloat)) { fail(c, "VS の入力 Location %u の型が読めない", v->loc); return; }
    const gs_attr *a = find_attr(c->cfg, v->loc);
    const gs_vbind *b = a ? find_bind(c->cfg, a->binding) : NULL;
    if (!a || !b || a->binding >= GS_MAX_VB || !c->vb_var[a->binding]) {
        uint32_t w[2] = { vt, spv_new_id(c) };
        OPW(&c->fn, OP_Store, v->var, add_glob(c, OP_ConstantNull, 2, w));
        return;
    }
    uint32_t elem = vtx;
    if (b->rate == 1) elem = b->divisor == 0 ? finst : uadd(c, finst, b->divisor == 1 ? j : op2(c, OP_UDiv, c->t_u32, j, ku(c, b->divisor)));
    uint32_t boff = rec_word(c, GSR_VB + 2 * a->binding), stride = rec_word(c, GSR_VB + 2 * a->binding + 1);
    uint32_t base = uadd(c, uadd(c, boff, umul(c, elem, stride)), ku(c, a->offset)), v4t = 0;
    uint32_t v4 = fetch(c, a->format, c->vb_var[a->binding], base, &v4t);
    if (!v4) return;
    uint32_t n4, s4 = scalar_of(c, v4t, &n4), x;
    (void) n4;
    if (v->comp + n > 4) { fail(c, "頂点の属性の成分が 4 を超える"); return; }
    if (n == 1) x = op2(c, OP_CompositeExtract, s4, v4, v->comp);
    else {
        uint32_t w[10] = { ty_vec(c, s4, n), spv_new_id(c), v4, v4 };
        for (uint32_t i = 0; i < n; i++) w[4 + i] = v->comp + i;
        wb_op(&c->fn, OP_VectorShuffle, 4 + n, w);
        x = w[1];
    }
    if (st != s4) x = op1(c, OP_Bitcast, vt, x);
    OPW(&c->fn, OP_Store, v->var, x);
}

static void prologue_pull(ctx *c) {
    uint32_t u = c->t_u32, VI = load_u32(c, c->in_vi), II = load_u32(c, c->in_ii);
    uint32_t flags = rec_word(c, GSR_FLAGS), ibb = rec_word(c, GSR_IB), vbase = rec_word(c, GSR_VBASE), finst = rec_word(c, GSR_FINST);
    uint32_t P = rec_word(c, GSR_PRIMS), isz = rec_word(c, GSR_ISZ), imask = rec_word(c, GSR_IMASK), topo = rec_word(c, GSR_TOPO);
    // インスタンスの番号 = (元のインスタンス × プリミティブの数 + プリミティブ) × GS の instancing + GS の instancing の番号
    uint32_t inv_id = c->c_u0, t2 = II;
    if (c->inv > 1) { inv_id = op2(c, OP_UMod, u, II, ku(c, c->inv)); t2 = op2(c, OP_UDiv, u, II, ku(c, c->inv)); }
    uint32_t prim = op2(c, OP_UMod, u, t2, P), j = op2(c, OP_UDiv, u, t2, P), inst = uadd(c, finst, j);
    uint32_t indexed = op2(c, OP_INotEqual, c->t_bool, op2(c, OP_BitwiseAnd, u, flags, c->c_u1), c->c_u0);
    uint32_t restart = op2(c, OP_INotEqual, c->t_bool, op2(c, OP_BitwiseAnd, u, flags, ku(c, 2)), c->c_u0), skip = 0;
    uint32_t is_strip = ueq(c, topo, ku(c, GS_TOPO_STRIP)), is_fan = ueq(c, topo, ku(c, GS_TOPO_FAN));
    uint32_t odd = c->nv == 3 ? op2(c, OP_INotEqual, c->t_bool, op2(c, OP_BitwiseAnd, u, prim, c->c_u1), c->c_u0) : 0;
    uint32_t base_list = c->nv > 1 ? umul(c, prim, ku(c, c->nv)) : prim;
    for (uint32_t vi = 0; vi < c->nv && !c->bad; vi++) {
        // 入力の流れの中の場所
        uint32_t pos = vi ? uadd(c, base_list, ku(c, vi)) : base_list;
        if (c->nv == 3) {
            // 三角の帯：i 番の三角は (i, i+1+i%2, i+2-i%2)。扇：(i+1, i+2, 0)（Vulkan の決まり）
            uint32_t ps = vi == 0 ? prim : uadd(c, prim, vsel(c, u, odd, ku(c, vi == 1 ? 2 : 1), ku(c, vi == 1 ? 1 : 2)));
            uint32_t pf = vi == 2 ? c->c_u0 : uadd(c, prim, ku(c, vi + 1));
            pos = vsel(c, u, is_strip, ps, vsel(c, u, is_fan, pf, pos));
        } else if (c->nv == 2 || c->nv == 4) {
            pos = vsel(c, u, is_strip, vi ? uadd(c, prim, ku(c, vi)) : prim, pos);
        }
        // index（番号なしのときは 0 番地を読んで捨てる）
        uint32_t ibyte = vsel(c, u, indexed, uadd(c, ibb, umul(c, pos, isz)), c->c_u0);
        uint32_t w = sb_word(c, c->ib_var, ibyte);
        uint32_t sh = op2(c, OP_ShiftLeftLogical, u, op2(c, OP_BitwiseAnd, u, ibyte, ku(c, 3)), ku(c, 3));
        uint32_t raw = op2(c, OP_BitwiseAnd, u, op2(c, OP_ShiftRightLogical, u, w, sh), imask);
        // 帯の primitive restart の値：そのプリミティブは描かない。範囲の外を読まないように 0 番を読む
        uint32_t cut = op2(c, OP_LogicalAnd, c->t_bool, restart, ueq(c, raw, imask));
        skip = skip ? op2(c, OP_LogicalOr, c->t_bool, skip, cut) : cut;
        raw = vsel(c, u, cut, c->c_u0, raw);
        uint32_t vtx = uadd(c, vsel(c, u, indexed, raw, pos), vbase);
        // VS の入力
        for (int i = 0; i < c->n_vs_in && !c->bad; i++) {
            iovar *v = &c->vs_in[i];
            if (v->builtin == ~0u) { load_attr(c, v, vtx, finst, j); continue; }
            uint32_t val = v->builtin == BI_VertexIndex ? vtx : v->builtin == BI_InstanceIndex ? inst : v->builtin == BI_BaseVertex ? vbase :
                           v->builtin == BI_BaseInstance ? finst : c->c_u0;
            store_bitcast(c, v->var, val, u);
        }
        OPW(&c->fn, OP_FunctionCall, c->t_void, spv_new_id(c), c->vs_main);
        copy_vs_to_gs(c, vi);
    }
    // 受け持つ頂点。描かないプリミティブなら、どの Emit も選ばない
    uint32_t none = ku(c, ~0u);
    if (c->list) {
        uint32_t per = ku(c, c->outk == 2 ? 3 : 2);
        OPW(&c->fn, OP_Store, c->v_t, vsel(c, u, skip, none, op2(c, OP_UDiv, u, VI, per)));
        OPW(&c->fn, OP_Store, c->v_c, op2(c, OP_UMod, u, VI, per));
    }
    OPW(&c->fn, OP_Store, c->v_k, vsel(c, u, skip, none, VI));
    set_gs_builtins(c, prim, inv_id);
    init_counters(c);
}

// 下書き x から本当の出力 s へ、take が真なら写す（葉＝スカラーかベクトルごと）
static void select_tree(ctx *c, uint32_t take, uint32_t x, uint32_t s, uint32_t type, uint32_t *path, int depth) {
    uint32_t *d = D(c, type);
    if (!d || depth > 8) { fail(c, "出力の型が読めない"); return; }
    uint32_t op = OPC(d);
    if (op == OP_TypeStruct || op == OP_TypeArray || op == OP_TypeMatrix) {
        uint32_t n = op == OP_TypeStruct ? WC(d) - 2 : op == OP_TypeArray ? const_value(c, d[3]) : d[3];
        for (uint32_t i = 0; i < n && !c->bad; i++) {
            path[depth] = ku(c, i);
            select_tree(c, take, x, s, op == OP_TypeStruct ? d[2 + i] : d[2], path, depth + 1);
        }
        return;
    }
    uint32_t px = chain(c, SC_Private, type, x, path, depth), ps = chain(c, SC_Output, type, s, path, depth);
    uint32_t a = load(c, type, px), b = load(c, type, ps);
    OPW(&c->fn, OP_Store, ps, vsel(c, type, take, a, b));
}

// 並び：三角 t の角を、直前 2 つ（p2・p1）と今（x）から選んで、take なら写す
// 帯の j 番の三角は (w_j, w_{j+1+j%2}, w_{j+2-j%2})。線は (w_j, w_{j+1})
static void select_list(ctx *c, uint32_t take, uint32_t odd, uint32_t c0, uint32_t c1, int k, uint32_t x, uint32_t s, uint32_t type,
                        uint32_t plt, uint32_t *path, int depth) {
    uint32_t *d = D(c, type);
    if (!d || depth > 8) { fail(c, "出力の型が読めない"); return; }
    uint32_t op = OPC(d);
    if (op == OP_TypeStruct || op == OP_TypeArray || op == OP_TypeMatrix) {
        uint32_t n = op == OP_TypeStruct ? WC(d) - 2 : op == OP_TypeArray ? const_value(c, d[3]) : d[3];
        uint32_t *pd = D(c, plt);
        for (uint32_t i = 0; i < n && !c->bad; i++) {
            path[depth] = ku(c, i);
            uint32_t et = op == OP_TypeStruct ? d[2 + i] : d[2], pet = !pd ? 0 : OPC(pd) == OP_TypeStruct ? pd[2 + i] : pd[2];
            select_list(c, take, odd, c0, c1, k, x, s, et, pet, path, depth + 1);
        }
        return;
    }
    uint32_t cur = load(c, type, chain(c, SC_Private, type, x, path, depth));
    uint32_t p1 = load(c, type, chain(c, SC_Private, type, c->prev1[k], path, depth)), v;
    uint32_t ps = chain(c, SC_Output, type, s, path, depth), o = load(c, type, ps);
    if (c->outk == 2) {
        uint32_t p2 = load(c, type, chain(c, SC_Private, type, c->prev2[k], path, depth));
        uint32_t x1 = vsel(c, type, odd, cur, p1), x2 = vsel(c, type, odd, p1, cur);
        v = vsel(c, type, c0, p2, vsel(c, type, c1, x1, x2));
    } else {
        v = vsel(c, type, c0, p1, cur);
    }
    OPW(&c->fn, OP_Store, ps, vsel(c, type, take, v, o));
}

// EmitVertex の代わり
static void emit_vertex(ctx *c) {
    if (c->list) {
        uint32_t u = c->t_u32, s = load(c, u, c->v_s), tc = load(c, u, c->v_tc), need = ku(c, c->outk == 2 ? 2 : 1);
        uint32_t forms = op2(c, OP_UGreaterThanEqual, c->t_bool, s, need);
        uint32_t take = op2(c, OP_LogicalAnd, c->t_bool, forms, ueq(c, tc, load(c, u, c->v_t)));
        uint32_t cc = load(c, u, c->v_c), c0 = ueq(c, cc, c->c_u0), c1 = ueq(c, cc, c->c_u1), odd = 0;
        if (c->outk == 2) odd = op2(c, OP_INotEqual, c->t_bool, op2(c, OP_BitwiseAnd, u, s, c->c_u1), c->c_u0); // (s-2) と s の偶奇は同じ
        for (int i = 0; i < c->n_gs_out && !c->bad; i++) {
            uint32_t path[16], pt = plain(c, c->gs_out[i].type);
            select_list(c, take, odd, c0, c1, i, c->gs_out[i].var, c->sel[c->gs_out[i].var], c->gs_out[i].type, pt, path, 0);
        }
        // 直前の頂点の写しをずらす
        for (int i = 0; i < c->n_gs_out && !c->bad; i++) {
            uint32_t pt = plain(c, c->gs_out[i].type);
            if (c->outk == 2) OPW(&c->fn, OP_Store, c->prev2[i], load(c, pt, c->prev1[i]));
            OPW(&c->fn, OP_Store, c->prev1[i], load(c, pt, c->gs_out[i].var));
        }
        OPW(&c->fn, OP_Store, c->v_tc, uadd(c, tc, vsel(c, u, forms, c->c_u1, c->c_u0)));
        OPW(&c->fn, OP_Store, c->v_s, uadd(c, s, c->c_u1));
        return;
    }
    uint32_t cnt = load(c, c->t_u32, c->v_cnt), k = load(c, c->t_u32, c->v_k);
    uint32_t e = load(c, c->t_u32, c->v_end);
    // 帯は「数えた数 <= k」：k 番目まで写し続けるので、k 番目が無ければ最後の頂点が残る。点は「== k」だけ
    uint32_t t1 = op2(c, c->strip ? OP_ULessThanEqual : OP_IEqual, c->t_bool, cnt, k);
    uint32_t take = op2(c, OP_LogicalAnd, c->t_bool, t1, ueq(c, e, c->c_u0));
    for (int i = 0; i < c->n_gs_out && !c->bad; i++) {
        uint32_t path[16];
        select_tree(c, take, c->gs_out[i].var, c->sel[c->gs_out[i].var], c->gs_out[i].type, path, 0);
    }
    OPW(&c->fn, OP_Store, c->v_cnt, uadd(c, cnt, c->c_u1));
}

static void emit_funcs(ctx *c, smod *m, int is_gs) {
    int in_main = 0, need_pro = 0;
    for (size_t i = 0; i < m->s[S_FUNC].n && !c->bad; i++) {
        uint32_t *ins = m->s[S_FUNC].v[i], op = OPC(ins);
        if (is_gs) {
            if (op == OP_Function) in_main = ins[2] == c->gs_main;
            if (need_pro && op != OP_Variable && op != OP_Line && op != OP_NoLine) {
                if (c->pull) prologue_pull(c); else prologue_fixed(c);
                need_pro = 0;
            }
            if (op == OP_Label && in_main && !c->pro_done) { wb_ins(&c->fn, ins); need_pro = c->pro_done = 1; continue; }
            if (op == OP_EmitVertex || op == OP_EmitStreamVertex) { emit_vertex(c); continue; }
            if (op == OP_EndPrimitive || op == OP_EndStreamPrimitive) {
                OPW(&c->fn, OP_Store, c->list ? c->v_s : c->v_end, c->list ? c->c_u0 : c->c_u1);
                continue;
            }
        }
        if (chain_op(op) && ins[3] < c->cap && c->root[ins[3]] && c->conv[c->root[ins[3]]]) {
            // 変えた変数の中を指すポインターは Private にする
            uint32_t *x = dup_ins(ins);
            x[1] = ty_ptr(c, SC_Private, plain(c, ptr_pointee(c, ins[1])));
            wb_ins(&c->fn, x);
            free(x);
            continue;
        }
        wb_ins(&c->fn, ins);
    }
}

// ---- 名前（調べ用）・飾り ----
static void add_name(ctx *c, uint32_t id, const char *s) {
    uint32_t w[16] = { id }, n = (uint32_t) strlen(s) / 4 + 1;
    memset(w + 1, 0, 15 * 4);
    memcpy(w + 1, s, strlen(s));
    il_push(&c->newdbg, make_ins(OP_Name, 1 + n, w));
}

// GS の出力のうち、PrimitiveId の組み込みの物か
static int gs_prim_out(ctx *c, uint32_t var) {
    for (int i = 0; i < c->n_gs_out; i++)
        if (c->gs_out[i].var == var && c->gs_out[i].builtin == BI_PrimitiveId) return 1;
    return 0;
}

static uint32_t new_var(ctx *c, uint32_t ptype, uint32_t sc, const char *name) {
    uint32_t w[3] = { ptype, spv_new_id(c), sc };
    il_push(&c->newvars, make_ins(OP_Variable, 3, w));
    set_def(c, c->newvars.v[c->newvars.n - 1]);
    if (name) add_name(c, w[1], name);
    return w[1];
}
static void add_deco(ctx *c, uint32_t id, uint32_t dec, uint32_t val) { uint32_t w[3] = { id, dec, val }; il_push(&c->newann, make_ins(OP_Decorate, 3, w)); }
static void add_deco0(ctx *c, uint32_t id, uint32_t dec) { uint32_t w[2] = { id, dec }; il_push(&c->newann, make_ins(OP_Decorate, 2, w)); }
static void add_mdeco(ctx *c, uint32_t st, uint32_t m, uint32_t dec, int has, uint32_t val) {
    uint32_t w[4] = { st, m, dec, val };
    il_push(&c->newann, make_ins(OP_MemberDecorate, has ? 4 : 3, w));
}

static int has_cap(ilist *l, uint32_t cap) {
    for (size_t i = 0; i < l->n; i++) if (l->v[i][1] == cap) return 1;
    return 0;
}

// C1 の資源：記録（uniform）・index と頂点の buffer（storage）
static void make_pull_vars(ctx *c) {
    uint32_t set = c->cfg->set;
    // 記録：struct { uvec4 r[16]; }
    uint32_t v4u = ty_vec(c, c->t_u32, 4), n16 = ku(c, GSR_WORDS / 4);
    uint32_t wa[3] = { spv_new_id(c), v4u, n16 }, arr = add_glob(c, OP_TypeArray, 3, wa);
    uint32_t ws[2] = { spv_new_id(c), arr }, st = add_glob(c, OP_TypeStruct, 2, ws);
    add_deco(c, arr, DEC_ArrayStride, 16);
    add_deco0(c, st, DEC_Block);
    add_mdeco(c, st, 0, DEC_Offset, 1, 0);
    c->rec_var = new_var(c, ty_ptr(c, SC_Uniform, st), SC_Uniform, "gs_emu_record");
    add_deco(c, c->rec_var, DEC_DescriptorSet, set);
    add_deco(c, c->rec_var, DEC_Binding, 0);
    // buffer：struct { uint d[]; }（読むだけ）
    uint32_t wr[2] = { spv_new_id(c), c->t_u32 }, ra = add_glob(c, OP_TypeRuntimeArray, 2, wr);
    uint32_t wb[2] = { spv_new_id(c), ra }, sb = add_glob(c, OP_TypeStruct, 2, wb);
    add_deco(c, ra, DEC_ArrayStride, 4);
    add_deco0(c, sb, DEC_Block);
    add_mdeco(c, sb, 0, DEC_Offset, 1, 0);
    add_mdeco(c, sb, 0, DEC_NonWritable, 0, 0);
    uint32_t psb = ty_ptr(c, SC_StorageBuffer, sb);
    c->ib_var = new_var(c, psb, SC_StorageBuffer, "gs_emu_index");
    add_deco(c, c->ib_var, DEC_DescriptorSet, set);
    add_deco(c, c->ib_var, DEC_Binding, 1);
    for (int i = 0; i < c->n_vs_in; i++) {
        iovar *v = &c->vs_in[i];
        if (v->builtin != ~0u) continue;
        const gs_attr *a = find_attr(c->cfg, v->loc);
        if (!a) continue;
        if (a->binding >= GS_MAX_VB) { fail(c, "頂点の binding %u は多すぎる", a->binding); return; }
        if (c->vb_var[a->binding]) continue;
        char nm[32];
        snprintf(nm, sizeof nm, "gs_emu_vb%u", a->binding);
        c->vb_var[a->binding] = new_var(c, psb, SC_StorageBuffer, nm);
        add_deco(c, c->vb_var[a->binding], DEC_DescriptorSet, set);
        add_deco(c, c->vb_var[a->binding], DEC_Binding, 2 + a->binding);
        c->info->used_bindings |= 1u << a->binding;
    }
}

int gs_emu_build2(const uint32_t *vs, size_t vs_words, const uint32_t *gs, size_t gs_words, const gs_emu_cfg *cfg,
                  uint32_t **out, size_t *out_words, gs_emu_info *info) {
    ctx cc = { 0 }, *c = &cc;
    memset(info, 0, sizeof *info);
    c->info = info;
    c->cfg = cfg;
    c->pull = cfg && cfg->pull;
    *out = NULL;
    *out_words = 0;
    if (!split(&c->g, gs, gs_words)) { snprintf(info->why, sizeof info->why, "GS の SPIR-V が読めない"); return 0; }
    if (vs_words < 5 || vs[0] != 0x07230203u) { smod_free(&c->g); snprintf(info->why, sizeof info->why, "VS の SPIR-V が読めない"); return 0; }
    c->ver = c->g.ver > vs[1] ? c->g.ver : vs[1];
    c->base = c->g.bound - 1;
    c->next = c->g.bound + vs[3];
    c->cap = c->next + 262144;
    c->def = calloc(c->cap, sizeof *c->def);
    c->conv = calloc(c->cap, 1);
    c->plainmap = calloc(c->cap, 4);
    c->sel = calloc(c->cap, 4);
    c->root = calloc(c->cap, 4);
    for (int s = 0; s < S_N; s++)
        for (size_t i = 0; i < c->g.s[s].n; i++) {
            set_def(c, c->g.s[s].v[i]);
            if (s == S_GLOB && is_type_op(OPC(c->g.s[s].v[i]))) il_push(&c->types, c->g.s[s].v[i]);
        }
    load_vs(c, vs, vs_words);

    // 入口と形
    uint32_t vs_main_raw = 0;
    find_entry(c, &c->g, 3, &c->gs_main, "GS");
    find_entry(c, &c->v, 0, &vs_main_raw, "VS");
    c->vs_main = vs_main_raw;
    if (c->g.s[S_MM].n != 1 || c->v.s[S_MM].n != 1 || memcmp(c->g.s[S_MM].v[0], c->v.s[S_MM].v[0], 12)) fail(c, "VS と GS の memory model が違う");
    uint32_t inprim = ~0u, outprim = ~0u, inv = 1;
    for (size_t i = 0; i < c->g.s[S_EM].n; i++) {
        uint32_t *e = c->g.s[S_EM].v[i];
        if (OPC(e) != OP_ExecutionMode || e[1] != c->gs_main) continue;
        switch (e[2]) {
        case 0: inv = e[3]; break;
        case 11: fail(c, "GS が transform feedback を使う"); break;
        case 19: case 20: case 21: case 22: case 23: inprim = e[2]; break;
        case 26: info->max_vertices = e[3]; break;
        case 27: case 28: case 29: outprim = e[2]; break;
        }
    }
    // 入力の形：19 点・20 線・21 隣つきの線・22 三角・23 隣つきの三角
    static const uint32_t nvs[5] = { 1, 2, 4, 3, 6 };
    if (inprim < 19 || inprim > 23) fail(c, "GS の入力の形 %u が分からない", inprim);
    else c->nv = nvs[inprim - 19];
    if (!c->pull && inprim != 19) fail(c, "B は点を受ける GS だけ（入力の形 %u）", inprim);
    if (!c->pull && inv != 1) fail(c, "B は GS の instancing（%u 回）を扱わない", inv);
    if (!inv || inv > 32) fail(c, "GS の instancing が %u 回", inv);
    if (!info->max_vertices || info->max_vertices > 1024) fail(c, "OutputVertices が %u", info->max_vertices);
    c->inv = inv;
    c->outk = outprim == 29 ? 2 : outprim == 28 ? 1 : 0;
    c->strip = outprim != 27;
    info->in_verts = c->nv;
    info->invocations = inv;

    // 入出力の変数
    collect_io(c, &c->v, SC_Input, c->vs_in, &c->n_vs_in, MAX_IO);
    collect_io(c, &c->v, SC_Output, c->vs_out, &c->n_vs_out, MAX_IO);
    collect_io(c, &c->g, SC_Input, c->gs_in, &c->n_gs_in, MAX_IO);
    collect_io(c, &c->g, SC_Output, c->gs_out, &c->n_gs_out, MAX_IO);
    for (int i = 0; i < c->n_vs_in; i++) {
        iovar *v = &c->vs_in[i];
        if (v->builtin == ~0u) {
            info->vertex_input = 1;
            if (c->pull) c->conv[v->var] = 1; // C1 では頂点の属性も自分で読む
            continue;
        }
        if (v->builtin != BI_VertexIndex && v->builtin != BI_InstanceIndex && v->builtin != BI_BaseVertex &&
            v->builtin != BI_BaseInstance && v->builtin != BI_DrawIndex) fail(c, "VS の組み込みの入力 %u は知らない", v->builtin);
        c->conv[v->var] = 1;
    }
    for (int i = 0; i < c->n_vs_out; i++) c->conv[c->vs_out[i].var] = 1;
    for (int i = 0; i < c->n_gs_in; i++) c->conv[c->gs_in[i].var] = 1;
    for (int i = 0; i < c->n_gs_out; i++) {
        iovar *o = &c->gs_out[i];
        uint32_t *d = D(c, o->type), mb;
        c->conv[o->var] = 1;
        if (o->builtin == BI_Layer || o->builtin == BI_ViewportIndex) info->writes_layer = 1;
        if (d && OPC(d) == OP_TypeStruct)
            for (uint32_t m = 0; m + 2 < WC(d); m++)
                if (member_builtin(c, o->type, m, &mb) && (mb == BI_Layer || mb == BI_ViewportIndex)) info->writes_layer = 1;
    }
    check_side_effects(c, &c->g, "GS");
    check_side_effects(c, &c->v, "VS");
    compute_roots(c);
    // EmitStreamVertex は 0 番の流れだけ。EndPrimitive のあとにまた Emit するか（並びに直すか）。
    // 文の順で Emit が Cut より後にあるか、Cut がループの中にあれば、そうみなす。
    // ループの中かは、OpLoopMerge から、その merge の block の OpLabel までの間か、で見る（DXVK は block を構造の順に並べる）
    {
        int ended = 0;
        uint32_t loops[64], nl = 0;
        for (size_t i = 0; i < c->g.s[S_FUNC].n && !c->bad; i++) {
            uint32_t *ins = c->g.s[S_FUNC].v[i], op = OPC(ins);
            if (op == OP_Function) ended = 0, nl = 0;
            if (op == OP_LoopMerge && nl < 64) loops[nl++] = ins[1];
            if (op == OP_Label) for (uint32_t k = 0; k < nl; k++) if (loops[k] == ins[1]) { loops[k] = loops[--nl]; break; }
            if ((op == OP_EmitStreamVertex || op == OP_EndStreamPrimitive) && const_value(c, ins[1]) != 0) fail(c, "GS が 0 番以外の流れに出す");
            if (op == OP_EndPrimitive || op == OP_EndStreamPrimitive) { ended = 1; if (nl) info->restart_mid = 1; }
            if ((op == OP_EmitVertex || op == OP_EmitStreamVertex) && ended) info->restart_mid = 1;
        }
    }
    if (info->restart_mid && c->outk) {
        if (!c->pull) fail(c, "B は帯の途中の RestartStrip を扱わない");
        c->list = 1;
    }
    info->list_mode = c->list;
    if (c->list && c->outk == 2 && info->max_vertices < 3) fail(c, "OutputVertices が %u で三角にならない", info->max_vertices);
    info->draw_vertices = !c->list ? info->max_vertices : c->outk == 2 ? 3 * (info->max_vertices - 2) : 2 * (info->max_vertices - 1);
    info->topology = c->outk == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                   : c->outk == 1 ? (c->list ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_STRIP)
                                  : (c->list ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    if (c->bad) goto done;

    // よく使う型
    c->t_void = get_type(c, OP_TypeVoid, 0, NULL);
    c->t_bool = get_type(c, OP_TypeBool, 0, NULL);
    { uint32_t w[2] = { 32, 0 }; c->t_u32 = get_type(c, OP_TypeInt, 2, w); }
    { uint32_t w[2] = { 32, 1 }; c->t_i32 = get_type(c, OP_TypeInt, 2, w); }
    { uint32_t w[1] = { 32 }; c->t_f32 = get_type(c, OP_TypeFloat, 1, w); }
    c->t_v2f = ty_vec(c, c->t_f32, 2);
    c->t_bi = c->t_u32;
    for (int i = 0; i < c->n_vs_in; i++)
        if (c->vs_in[i].builtin != ~0u) { c->t_bi = c->vs_in[i].type; break; }
    c->c_u0 = ku(c, 0);
    c->c_u1 = ku(c, 1);
    // GLSL.std.450（無ければ足す）
    {
        static const char nm[] = "GLSL.std.450";
        uint32_t w[8] = { 0 };
        memcpy(w + 1, nm, sizeof nm);
        for (size_t i = 0; i < c->g.s[S_IMP].n && !c->glsl; i++)
            if (str_eq(c->g.s[S_IMP].v[i] + 2, w + 1, WC(c->g.s[S_IMP].v[i]) - 2, 4)) c->glsl = c->g.s[S_IMP].v[i][1];
        for (size_t i = 0; i < c->v.s[S_IMP].n && !c->glsl; i++)
            if (str_eq(c->v.s[S_IMP].v[i] + 2, w + 1, WC(c->v.s[S_IMP].v[i]) - 2, 4)) c->glsl = c->v.s[S_IMP].v[i][1];
        if (!c->glsl) {
            w[0] = c->glsl = spv_new_id(c);
            il_push(&c->newimp, make_ins(OP_ExtInstImport, 5, w));
        }
    }

    // 変える変数を Private にして、宣言を後ろへ移す（新しいポインターの型より後に置くため）
    for (size_t i = 0; i < c->g.s[S_GLOB].n + c->v.s[S_GLOB].n; i++) {
        uint32_t *d = i < c->g.s[S_GLOB].n ? c->g.s[S_GLOB].v[i] : c->v.s[S_GLOB].v[i - c->g.s[S_GLOB].n];
        if (OPC(d) != OP_Variable || !c->conv[d[2]]) continue;
        uint32_t w[3] = { ty_ptr(c, SC_Private, plain(c, ptr_pointee(c, d[1]))), d[2], SC_Private };
        uint32_t *x = make_ins(OP_Variable, 3, w);
        il_push(&c->convvars, x);
        c->def[d[2]] = x;
    }
    // GS の出力ごとに、本当の出力の変数を作る（飾りはあとで付け替える）。並びなら直前 2 つの写しも
    for (int i = 0; i < c->n_gs_out; i++) {
        uint32_t *d = NULL;
        for (size_t j = 0; j < c->g.s[S_GLOB].n; j++)
            if (OPC(c->g.s[S_GLOB].v[j]) == OP_Variable && c->g.s[S_GLOB].v[j][2] == c->gs_out[i].var) d = c->g.s[S_GLOB].v[j];
        // PrimitiveId の出力は VS からは出せない（Geometry の capability が要る。残すと NVIDIA は GPU の例外で止まる）。
        // 作る VS では Private の変数にして捨てる。PS が読めば、GPU が数えた番号（作った描画の中の番号）になる
        if (c->gs_out[i].builtin == BI_PrimitiveId)
            c->sel[c->gs_out[i].var] = new_var(c, ty_ptr(c, SC_Private, plain(c, c->gs_out[i].type)), SC_Private, NULL);
        else
            c->sel[c->gs_out[i].var] = new_var(c, d[1], SC_Output, NULL);
        if (c->list) {
            uint32_t pp = ty_ptr(c, SC_Private, plain(c, c->gs_out[i].type));
            c->prev1[i] = new_var(c, pp, SC_Private, NULL);
            if (c->outk == 2) c->prev2[i] = new_var(c, pp, SC_Private, NULL);
        }
    }
    // 組み込みの入力（作る VS の）
    uint32_t pin = ty_ptr(c, SC_Input, c->t_bi);
    int need_ii = c->pull, need_bi = 0;
    if (!c->pull) {
        for (int i = 0; i < c->n_vs_in; i++) {
            if (c->vs_in[i].builtin == BI_VertexIndex) need_ii = 1;
            if (c->vs_in[i].builtin == BI_BaseVertex) need_bi = 1;
        }
        for (int i = 0; i < c->n_gs_in; i++) if (c->gs_in[i].builtin == BI_PrimitiveId) need_ii = need_bi = 1;
    }
    c->in_vi = new_var(c, pin, SC_Input, "gs_emu_vertex_index");
    add_deco(c, c->in_vi, DEC_BuiltIn, BI_VertexIndex);
    if (need_ii) { c->in_ii = new_var(c, pin, SC_Input, "gs_emu_instance_index"); add_deco(c, c->in_ii, DEC_BuiltIn, BI_InstanceIndex); }
    if (need_bi) { c->in_bi = new_var(c, pin, SC_Input, "gs_emu_base_instance"); add_deco(c, c->in_bi, DEC_BuiltIn, BI_BaseInstance); }
    uint32_t pu = ty_ptr(c, SC_Private, c->t_u32);
    c->v_cnt = new_var(c, pu, SC_Private, "gs_emu_count");
    c->v_end = new_var(c, pu, SC_Private, "gs_emu_ended");
    c->v_k = new_var(c, pu, SC_Private, "gs_emu_target");
    if (c->list) {
        c->v_t = new_var(c, pu, SC_Private, "gs_emu_prim_out");
        c->v_c = new_var(c, pu, SC_Private, "gs_emu_corner");
        c->v_s = new_var(c, pu, SC_Private, "gs_emu_strip_len");
        c->v_tc = new_var(c, pu, SC_Private, "gs_emu_prims_made");
    }
    if (c->pull) make_pull_vars(c);
    if (c->bad) goto done;

    // 関数
    emit_funcs(c, &c->g, 1);
    emit_funcs(c, &c->v, 0);
    if (c->bad) goto done;

    // ---- 組み立て ----
    {
        wbuf o = { 0 };
        uint32_t hdr[5] = { 0x07230203u, c->ver, c->g.gen, 0, 0 };
        for (int i = 0; i < 5; i++) wb_put(&o, hdr[i]);
        // capability：GS だけの物を外し、要る物を足す
        ilist caps = { 0 };
        smod *ms[2] = { &c->g, &c->v };
        for (int m = 0; m < 2; m++)
            for (size_t i = 0; i < ms[m]->s[S_CAP].n; i++) {
                uint32_t cap = ms[m]->s[S_CAP].v[i][1];
                if (cap == 2 || cap == 24 || cap == 53 || cap == 54 || cap == 57 || has_cap(&caps, cap)) continue;
                il_push(&caps, ms[m]->s[S_CAP].v[i]);
            }
        uint32_t extra[4], ne = 0;
        if (c->in_bi && !has_cap(&caps, 4427)) extra[ne++] = 4427; // DrawParameters（BaseInstance）
        if (info->writes_layer) {
            if (c->ver >= 0x10500) { extra[ne++] = 69; extra[ne++] = 70; } // ShaderLayer・ShaderViewportIndex
            else extra[ne++] = 5254;                                       // ShaderViewportIndexLayerEXT
        }
        for (size_t i = 0; i < caps.n; i++) wb_ins(&o, caps.v[i]);
        for (uint32_t i = 0; i < ne; i++) OPW(&o, OP_Capability, extra[i]);
        free(caps.v);
        // extension（同じ名前は 1 つ）
        ilist exts = { 0 };
        for (int m = 0; m < 2; m++)
            for (size_t i = 0; i < ms[m]->s[S_EXT].n; i++) {
                uint32_t *a = ms[m]->s[S_EXT].v[i];
                int dupe = 0;
                for (size_t j = 0; j < exts.n; j++) dupe |= str_eq(a + 1, exts.v[j] + 1, WC(a) - 1, WC(exts.v[j]) - 1);
                if (!dupe) il_push(&exts, a);
            }
        for (size_t i = 0; i < exts.n; i++) wb_ins(&o, exts.v[i]);
        const char *addext[3] = { info->writes_layer && c->ver < 0x10500 ? "SPV_EXT_shader_viewport_index_layer" : NULL,
                                  c->in_bi && c->ver < 0x10300 ? "SPV_KHR_shader_draw_parameters" : NULL,
                                  c->pull && c->ver < 0x10300 ? "SPV_KHR_storage_buffer_storage_class" : NULL };
        for (int k = 0; k < 3; k++) {
            if (!addext[k]) continue;
            uint32_t w[16] = { 0 };
            memcpy(w, addext[k], strlen(addext[k]));
            int dupe = 0;
            for (size_t j = 0; j < exts.n; j++) dupe |= str_eq(w, exts.v[j] + 1, (uint32_t) strlen(addext[k]) / 4 + 1, WC(exts.v[j]) - 1);
            if (!dupe) wb_op(&o, OP_Extension, (uint32_t) strlen(addext[k]) / 4 + 1, w);
        }
        free(exts.v);
        for (int m = 0; m < 2; m++) for (size_t i = 0; i < ms[m]->s[S_IMP].n; i++) wb_ins(&o, ms[m]->s[S_IMP].v[i]);
        for (size_t i = 0; i < c->newimp.n; i++) wb_ins(&o, c->newimp.v[i]);
        wb_ins(&o, c->g.s[S_MM].v[0]);
        // 入口：Vertex・GS の main。1.4 からは使う全体の変数を全部並べる
        {
            wbuf ep = { 0 };
            wb_put(&ep, 0);
            wb_put(&ep, c->gs_main);
            wb_put(&ep, 0x6e69616d); // "main"
            wb_put(&ep, 0);
            ilist *vl[5] = { &c->g.s[S_GLOB], &c->v.s[S_GLOB], &c->newglob, &c->convvars, &c->newvars };
            for (int m = 0; m < 5; m++)
                for (size_t i = 0; i < vl[m]->n; i++) {
                    uint32_t *d = vl[m]->v[i];
                    if (OPC(d) != OP_Variable || d[3] == SC_Function) continue;
                    if ((m == 0 || m == 1) && c->conv[d[2]]) continue; // 後ろへ移した物
                    if (c->ver < 0x10400 && d[3] != SC_Input && d[3] != SC_Output) continue;
                    wb_put(&ep, d[2]);
                }
            wb_op(&o, OP_EntryPoint, (uint32_t) ep.n, ep.p);
            free(ep.p);
        }
        // execution mode：GS の形の物を外す。VS の物は入口を付け替え、同じ物は 1 つ
        {
            ilist ems = { 0 };
            for (int m = 0; m < 2; m++)
                for (size_t i = 0; i < ms[m]->s[S_EM].n; i++) {
                    uint32_t *e = ms[m]->s[S_EM].v[i], mode = e[2];
                    if (m == 0 && (mode == 0 || (mode >= 19 && mode <= 23) || (mode >= 26 && mode <= 29))) continue;
                    uint32_t *x = dup_ins(e);
                    if (x[1] == c->vs_main) x[1] = c->gs_main;
                    int dupe = 0;
                    for (size_t j = 0; j < ems.n; j++) dupe |= WC(ems.v[j]) == WC(x) && !memcmp(ems.v[j], x, WC(x) * 4);
                    if (dupe) free(x); else il_push(&ems, x);
                }
            for (size_t i = 0; i < ems.n; i++) { wb_ins(&o, ems.v[i]); free(ems.v[i]); }
            free(ems.v);
        }
        // 調べ用の名前など：GS の物と新しい物
        for (size_t i = 0; i < c->g.s[S_DBG].n; i++) wb_ins(&o, c->g.s[S_DBG].v[i]);
        for (size_t i = 0; i < c->newdbg.n; i++) wb_ins(&o, c->newdbg.v[i]);
        // 飾り：Private にした変数の物は落とす。GS の出力の物は本当の出力へ付け替える
        for (int m = 0; m < 2; m++)
            for (size_t i = 0; i < ms[m]->s[S_ANN].n; i++) {
                uint32_t *a = ms[m]->s[S_ANN].v[i], op = OPC(a);
                if ((op == OP_Decorate || op == OP_DecorateId || op == OP_DecorateString) && a[1] < c->cap && c->conv[a[1]]) {
                    if (!c->sel[a[1]] || a[2] == DEC_Stream || a[2] == DEC_Offset || a[2] == DEC_XfbBuffer || a[2] == DEC_XfbStride) continue;
                    if (gs_prim_out(c, a[1])) continue; // Private にした PrimitiveId の出力には飾りを付けない
                    uint32_t *x = dup_ins(a);
                    x[1] = c->sel[a[1]];
                    wb_ins(&o, x);
                    free(x);
                    continue;
                }
                if (op == OP_DecorationGroup || op == OP_GroupDecorate || op == OP_GroupMemberDecorate) { fail(c, "decoration group がある"); break; }
                wb_ins(&o, a);
            }
        for (size_t i = 0; i < c->newann.n; i++) wb_ins(&o, c->newann.v[i]);
        // 全体の宣言：GS・VS（Private にした変数を除く）・新しい型と定数・Private にした変数・新しい変数
        for (int m = 0; m < 2; m++)
            for (size_t i = 0; i < ms[m]->s[S_GLOB].n; i++) {
                uint32_t *d = ms[m]->s[S_GLOB].v[i];
                if (OPC(d) == OP_Variable && c->conv[d[2]]) continue;
                wb_ins(&o, d);
            }
        for (size_t i = 0; i < c->newglob.n; i++) wb_ins(&o, c->newglob.v[i]);
        for (size_t i = 0; i < c->convvars.n; i++) wb_ins(&o, c->convvars.v[i]);
        for (size_t i = 0; i < c->newvars.n; i++) wb_ins(&o, c->newvars.v[i]);
        for (size_t i = 0; i < c->fn.n; i++) wb_put(&o, c->fn.p[i]);
        o.p[3] = c->next;
        if (!c->bad) { *out = o.p; *out_words = o.n; } else free(o.p);
    }

done:;
    int ok = !c->bad;
    smod_free(&c->g);
    smod_free(&c->v);
    ilist *ls[6] = { &c->newglob, &c->convvars, &c->newvars, &c->newann, &c->newdbg, &c->newimp };
    for (int m = 0; m < 6; m++) {
        for (size_t i = 0; i < ls[m]->n; i++) free(ls[m]->v[i]);
        free(ls[m]->v);
    }
    free(c->types.v);
    free(c->fn.p);
    free(c->def);
    free(c->conv);
    free(c->plainmap);
    free(c->sel);
    free(c->root);
    return ok;
}

int gs_emu_build(const uint32_t *vs, size_t vs_words, const uint32_t *gs, size_t gs_words,
                 uint32_t **out, size_t *out_words, gs_emu_info *info) {
    return gs_emu_build2(vs, vs_words, gs, gs_words, NULL, out, out_words, info);
}
