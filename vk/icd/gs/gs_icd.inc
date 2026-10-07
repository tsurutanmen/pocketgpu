// ---- geometry shader を頂点シェーダーで真似る（試作・icd.c の create_graphics_pipelines の前に差し込む） ----
// 受け手の GPU が geometry shader を持たないとき、GS を含む pipeline を「VS と GS を合わせた VS」と PS の pipeline に置き換える。
// 受け手は何も変えない（普通の VS＋PS の pipeline と、普通の vkCmdDraw が届くだけ）。
// 描画は vkCmdDraw(n, 1, fv, fi) → vkCmdDraw(GS の出す頂点の数, n, 0, fv)。くわしくは gs_emu.h
#include "gs_emu.h"

// VS と GS の module の SPIR-V を覚えておく（GS を含む pipeline を作るときに 2 つを合わせる）
struct gs_mod { uint64_t id; uint32_t *code; size_t words; struct gs_mod *next; };
static struct gs_mod *g_gsmods;
// GS を真似た pipeline：描くときの頂点の数と形
struct gs_pipe { uint64_t id; uint32_t m, topo; int dyn_topo; struct gs_pipe *next; };
static struct gs_pipe *g_gspipes;
static SRWLOCK g_gs_lock = SRWLOCK_INIT;

static void dyn1(VkCommandBuffer cbh, VkDynamicState st, uint32_t v);
static VkResult VKAPI_CALL create_shader(VkDevice dv, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out);
static void VKAPI_CALL destroy_shader(VkDevice dv, VkShaderModule h, const VkAllocationCallbacks *a);
static VkResult VKAPI_CALL create_graphics_pipelines(VkDevice dv, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                     const VkAllocationCallbacks *a, VkPipeline *out);

// SPIR-V の最初の入口の段（ExecutionModel）。0 頂点・3 geometry。無ければ ~0
static uint32_t spv_model(const uint32_t *w, size_t n) {
    for (size_t i = 5; i + 1 < n;) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || op == 54) break;
        if (op == 15) return w[i + 1];
        i += wc;
    }
    return ~0u;
}

// create_shader から呼ぶ。GS を持たない GPU のときだけ、VS と GS の中身を写して持つ
static void gs_keep(uint64_t id, const uint32_t *code, size_t bytes) {
    if (g_hw.real.geometryShader || bytes < 20) return;
    uint32_t m = spv_model(code, bytes / 4);
    if (m != 0 && m != 3) return;
    struct gs_mod *e = calloc(1, sizeof *e);
    e->id = id;
    e->words = bytes / 4;
    e->code = malloc(bytes);
    memcpy(e->code, code, bytes);
    AcquireSRWLockExclusive(&g_gs_lock);
    e->next = g_gsmods;
    g_gsmods = e;
    ReleaseSRWLockExclusive(&g_gs_lock);
}

// destroy_shader・destroy_pipeline から呼ぶ。覚えた物を捨てる
static void gs_forget(uint64_t id) {
    if (!g_gsmods && !g_gspipes) return;
    AcquireSRWLockExclusive(&g_gs_lock);
    for (struct gs_mod **p = &g_gsmods; *p; p = &(*p)->next)
        if ((*p)->id == id) { struct gs_mod *e = *p; *p = e->next; free(e->code); free(e); break; }
    // pipeline の覚えは表から外すだけで、free はしない（その pipeline をつないだ command buffer が指したままのことがある）
    for (struct gs_pipe **p = &g_gspipes; *p; p = &(*p)->next)
        if ((*p)->id == id) { *p = (*p)->next; break; }
    ReleaseSRWLockExclusive(&g_gs_lock);
}

// 段の SPIR-V：pNext の VkShaderModuleCreateInfo（maintenance5）か、覚えた module から
static const uint32_t *gs_code(const VkPipelineShaderStageCreateInfo *st, size_t *words) {
    for (const VkBaseInStructure *x = st->pNext; x; x = x->pNext)
        if (x->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *m = (const VkShaderModuleCreateInfo *) x;
            *words = m->codeSize / 4;
            return m->pCode;
        }
    const uint32_t *r = NULL;
    AcquireSRWLockShared(&g_gs_lock);
    for (struct gs_mod *e = g_gsmods; e; e = e->next)
        if (e->id == ID(st->module)) { r = e->code; *words = e->words; break; }
    ReleaseSRWLockShared(&g_gs_lock);
    return r;
}

// GS を持たない GPU では、GEOMETRY の段の資源と push constant を VERTEX でも見えるようにする（作った VS が読むので）
static uint32_t gs_stages(uint32_t f) {
    return !g_hw.real.geometryShader && (f & VK_SHADER_STAGE_GEOMETRY_BIT) ? f | VK_SHADER_STAGE_VERTEX_BIT : f;
}

// specialization を合わせる（VS の物を先に。同じ番号は VS の値を使う）
static void gs_merge_spec(const VkSpecializationInfo *a, const VkSpecializationInfo *b, VkSpecializationInfo *o,
                          VkSpecializationMapEntry *me, uint8_t *data, size_t cap) {
    memset(o, 0, sizeof *o);
    o->pMapEntries = me;
    o->pData = data;
    const VkSpecializationInfo *s[2] = { a, b };
    for (int k = 0; k < 2; k++) {
        if (!s[k]) continue;
        for (uint32_t i = 0; i < s[k]->mapEntryCount && o->mapEntryCount < 128; i++) {
            const VkSpecializationMapEntry *e = &s[k]->pMapEntries[i];
            int dupe = 0;
            for (uint32_t j = 0; j < o->mapEntryCount; j++) dupe |= me[j].constantID == e->constantID;
            if (dupe || o->dataSize + e->size > cap || e->offset + e->size > s[k]->dataSize) continue;
            me[o->mapEntryCount] = (VkSpecializationMapEntry) { e->constantID, (uint32_t) o->dataSize, e->size };
            memcpy(data + o->dataSize, (const uint8_t *) s[k]->pData + e->offset, e->size);
            o->dataSize += e->size;
            o->mapEntryCount++;
        }
    }
}

static VkResult gs_emu_pipeline(VkDevice dv, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *c, const VkAllocationCallbacks *a, VkPipeline *out) {
    *out = VK_NULL_HANDLE;
    const VkPipelineShaderStageCreateInfo *vs = NULL, *gs = NULL;
    VkPipelineShaderStageCreateInfo st[8];
    uint32_t ns = 1;
    for (uint32_t k = 0; k < c->stageCount; k++) {
        const VkPipelineShaderStageCreateInfo *s = &c->pStages[k];
        if (s->stage == VK_SHADER_STAGE_VERTEX_BIT) vs = s;
        else if (s->stage == VK_SHADER_STAGE_GEOMETRY_BIT) gs = s;
        else if (s->stage == VK_SHADER_STAGE_FRAGMENT_BIT && ns < 8) st[ns++] = *s;
        else { say("geometry shader と一緒の段 0x%x は真似られない", s->stage); return VK_ERROR_UNKNOWN; }
    }
    size_t nv = 0, ng = 0;
    const uint32_t *cv = vs ? gs_code(vs, &nv) : NULL, *cg = gs ? gs_code(gs, &ng) : NULL;
    if (!cv || !cg) { say("geometry shader を真似る：VS か GS の SPIR-V が見つからない"); return VK_ERROR_UNKNOWN; }
    uint32_t *code = NULL;
    size_t nw = 0;
    gs_emu_info info;
    if (!gs_emu_build(cv, nv, cg, ng, &code, &nw, &info)) { say("geometry shader を真似られない：%s", info.why); return VK_ERROR_UNKNOWN; }
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = code };
    VkShaderModule mod = VK_NULL_HANDLE;
    create_shader(dv, &smi, NULL, &mod);
    free(code);
    // 段：作った VS（main）と PS
    VkSpecializationMapEntry me[128];
    uint8_t data[2048];
    VkSpecializationInfo si;
    gs_merge_spec(vs->pSpecializationInfo, gs->pSpecializationInfo, &si, me, data, sizeof data);
    st[0] = *vs;
    st[0].pNext = NULL;
    st[0].module = mod;
    st[0].pName = "main";
    st[0].pSpecializationInfo = si.mapEntryCount ? &si : NULL;
    // 形：GS の出す形にする。インスタンス 1 つが元の点 1 つ
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = info.topology };
    // 頂点の属性は、インスタンスごとに 1 つ読む（firstInstance に元の firstVertex を入れて描くので、同じ要素を読む）
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkVertexInputBindingDescription vb[32];
    if (c->pVertexInputState) {
        vi = *c->pVertexInputState;
        if (vi.pNext || vi.vertexBindingDescriptionCount > 32) { say("geometry shader を真似る：頂点の入力の形が読めない"); destroy_shader(dv, mod, NULL); return VK_ERROR_UNKNOWN; }
        for (uint32_t k = 0; k < vi.vertexBindingDescriptionCount; k++) {
            vb[k] = vi.pVertexBindingDescriptions[k];
            if (info.vertex_input && vb[k].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
                say("geometry shader を真似る：インスタンスごとの頂点の入力はまだ");
                destroy_shader(dv, mod, NULL);
                return VK_ERROR_UNKNOWN;
            }
            vb[k].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
        }
        vi.pVertexBindingDescriptions = vb;
    }
    int dyn_topo = 0;
    const VkPipelineDynamicStateCreateInfo *dy = c->pDynamicState;
    for (uint32_t k = 0; dy && k < dy->dynamicStateCount; k++) {
        if (dy->pDynamicStates[k] == VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) dyn_topo = 1;
        if (dy->pDynamicStates[k] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) { say("geometry shader を真似る：動的な頂点の入力はまだ"); destroy_shader(dv, mod, NULL); return VK_ERROR_UNKNOWN; }
    }
    VkGraphicsPipelineCreateInfo c2 = *c;
    c2.stageCount = ns;
    c2.pStages = st;
    c2.pInputAssemblyState = &ia;
    c2.pVertexInputState = c->pVertexInputState ? &vi : NULL;
    VkResult r = create_graphics_pipelines(dv, cache, 1, &c2, a, out);
    destroy_shader(dv, mod, NULL);
    if (r != VK_SUCCESS) return r;
    struct gs_pipe *p = calloc(1, sizeof *p);
    *p = (struct gs_pipe) { ID(*out), info.max_vertices, info.topology, dyn_topo, NULL };
    AcquireSRWLockExclusive(&g_gs_lock);
    p->next = g_gspipes;
    g_gspipes = p;
    ReleaseSRWLockExclusive(&g_gs_lock);
    say("geometry shader を頂点シェーダーで真似た：頂点 %u 個・形 %u・動的な形 %d・頂点の属性 %d", info.max_vertices, info.topology, dyn_topo, info.vertex_input);
    return VK_SUCCESS;
}

static const struct gs_pipe *gs_find(uint64_t id) {
    const struct gs_pipe *r = NULL;
    AcquireSRWLockShared(&g_gs_lock);
    for (struct gs_pipe *p = g_gspipes; p; p = p->next) if (p->id == id) { r = p; break; }
    ReleaseSRWLockShared(&g_gs_lock);
    return r;
}

// cmd_bind_pipeline から呼ぶ。GS を真似た pipeline なら覚え、形が動的なら GS の形にする（外れたらアプリの形に戻す）
static void gs_bind(struct cb *c, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    const struct gs_pipe *g = g_gspipes ? gs_find(ID(p)) : NULL, *was = c->gs;
    if (g && g->dyn_topo) dyn1((VkCommandBuffer) c, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, g->topo);
    else if (!g && was && was->dyn_topo && c->app_topo_set) dyn1((VkCommandBuffer) c, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, c->app_topo);
    c->gs = g;
}

// cmd_set_topology の代わり
static void gs_set_topology(struct cb *c, VkPrimitiveTopology t) {
    const struct gs_pipe *g = c->gs;
    c->app_topo = t;
    c->app_topo_set = 1;
    dyn1((VkCommandBuffer) c, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, g ? g->topo : (uint32_t) t);
}

// cmd_draw から呼ぶ。GS を真似た pipeline なら描き方を変えて 1 を返す
static int gs_draw(struct cb *c, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    const struct gs_pipe *g = c->gs;
    (void) fi;
    if (!g) return 0;
    if (ic != 1) {
        static int once;
        if (!once++) say("geometry shader を真似る：インスタンスつきの描画はまだ（飛ばす）");
        return 1;
    }
    vr_u32(&c->s, VR_CMD_DRAW);
    vr_u32(&c->s, g->m); // 頂点：GS が出す k 番目
    vr_u32(&c->s, vc);   // インスタンス：元の点
    vr_u32(&c->s, 0);
    vr_u32(&c->s, fv);   // 作った VS の中で InstanceIndex が元の VertexIndex と同じになる
    return 1;
}

// 番号つき・間接の描画は、まだ真似ない（飛ばす）
static int gs_skip(struct cb *c, const char *what) {
    if (!c->gs) return 0;
    static int once;
    if (!once++) say("geometry shader を真似る：%s の描画はまだ（飛ばす）", what);
    return 1;
}
