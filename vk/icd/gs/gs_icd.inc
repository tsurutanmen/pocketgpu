// ---- geometry shader を頂点シェーダーで真似る（試作・icd.c の create_graphics_pipelines の前に差し込む） ----
// 受け手の GPU が geometry shader を持たないとき、GS を含む pipeline を「VS と GS を合わせた VS」と PS の pipeline に置き換える。
// 受け手は何も変えない（普通の VS＋PS の pipeline と、普通の vkCmdDraw・descriptor set が届くだけ）。くわしくは gs_emu.h
//
// 形は 2 つ。
// ・B：点を受ける GS・インスタンスごとの頂点の入力が無い・帯の途中で切らない。番号なしでインスタンス 1 つの描画だけに使う。
//   vkCmdDraw(n, 1, fv, fi) → vkCmdDraw(GS の出す頂点の数, n, 0, fv)
// ・C1：それ以外（番号つき・インスタンスつき・線や三角を受ける GS・GS の instancing・帯の途中の RestartStrip）。
//   作った VS は index と頂点の buffer を storage buffer として自分で読む。描画ごとの値は「記録」（256 バイト）に書き、
//   アプリの pipeline layout の後ろに 1 つ足した set（binding 0 記録・1 index・2+b 頂点の buffer）で読ませる。
//   そのため、受け手が GS を持たないときは、頂点・index の buffer に STORAGE_BUFFER の使い道を足し、
//   pipeline layout を作るたびに「後ろに set を 1 つ足した layout」も作っておく。
#include "gs_emu.h"

// VS と GS の module の SPIR-V を覚えておく（GS を含む pipeline を作るときに 2 つを合わせる）
struct gs_mod { uint64_t id; uint32_t *code; size_t words; struct gs_mod *next; };
static struct gs_mod *g_gsmods;
// アプリの pipeline layout と、後ろに set を 1 つ足した layout
struct gs_layout { uint64_t id; uint32_t nsets; uint64_t ext; struct gs_layout *next; };
static struct gs_layout *g_gslayouts;
// GS を真似た pipeline
struct gs_pipe {
    uint64_t id;                // アプリに返した pipeline（B があれば B、無ければ C1）
    uint64_t pull, pull_layout; // C1 の pipeline と layout（無ければ 0）
    uint32_t set;               // C1 の set の番号
    uint32_t m, topo;           // アプリに返した pipeline の 1 インスタンスの頂点の数と形
    uint32_t pm, ptopo;         // C1 の 1 インスタンスの頂点の数と形
    uint32_t nv, inv, app_topo; // 入力のプリミティブの頂点の数・GS の instancing・アプリの pipeline の形
    int dyn_topo, has_b;
    uint32_t vb_used, stride[GS_MAX_VB];
    struct gs_pipe *next;
};
static struct gs_pipe *g_gspipes;
static SRWLOCK g_gs_lock = SRWLOCK_INIT;
// C1 の set の形（装置ごと）
static struct { VkDevice dv; VkDescriptorSetLayout l; } g_gs_dsl[4];
static _Thread_local int g_gs_inner; // 自分で pipeline layout を作っているあいだは覚えない

// command buffer ごとの物：アプリがつないだ頂点・index の buffer、記録を書く一時の buffer、descriptor set
struct gs_chunk { VkBuffer b; VkDeviceMemory m; uint8_t *p; uint32_t used, size; int reg; struct gs_chunk *next; };
struct gs_dpool { VkDescriptorPool p; uint32_t left; struct gs_dpool *next; };
struct gs_cbst {
    struct { struct buf *b; uint64_t off, stride; int has_stride; } vb[GS_MAX_VB];
    struct buf *ib;
    uint64_t iboff;
    uint32_t itype;
    struct gs_chunk *chunks, *cur;
    struct gs_dpool *pools, *curpool;
    VkDescriptorSet set; // いま使える set と、その中身
    struct gs_chunk *set_chunk;
    struct buf *set_ib, *set_vb[GS_MAX_VB];
    uint32_t set_mask;
    struct { uint64_t layout, set; uint32_t ndyn, dyn[8]; } app[8]; // アプリがつないだ set（GRAPHICS）
};
enum { GS_CHUNK = 65536, GS_POOL_SETS = 64 };

static void dyn1(VkCommandBuffer cbh, VkDynamicState st, uint32_t v);
static VkResult VKAPI_CALL create_shader(VkDevice dv, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out);
static void VKAPI_CALL destroy_shader(VkDevice dv, VkShaderModule h, const VkAllocationCallbacks *a);
static VkResult VKAPI_CALL create_graphics_pipelines(VkDevice dv, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                     const VkAllocationCallbacks *a, VkPipeline *out);
static VkResult VKAPI_CALL create_dsl(VkDevice dv, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out);
static VkResult VKAPI_CALL create_playout(VkDevice dv, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out);
static void VKAPI_CALL destroy_playout_rpc(VkDevice dv, VkPipelineLayout h, const VkAllocationCallbacks *a);
static void VKAPI_CALL destroy_pipeline_rpc(VkDevice dv, VkPipeline h, const VkAllocationCallbacks *a);
static VkResult VKAPI_CALL create_dpool(VkDevice dv, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out);
static void VKAPI_CALL destroy_dpool(VkDevice dv, VkDescriptorPool pp, const VkAllocationCallbacks *a);
static VkResult VKAPI_CALL reset_dpool(VkDevice dv, VkDescriptorPool pp, VkDescriptorPoolResetFlags f);
static VkResult VKAPI_CALL alloc_dsets(VkDevice dv, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out);
static void VKAPI_CALL update_dsets(VkDevice dv, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c);
static uint32_t pick_mem(uint32_t bits, VkMemoryPropertyFlags want);
static void cb_reads(struct cb *c, struct buf *bf, uint64_t off, uint64_t len);

static int gs_on(void) { return !g_hw.real.geometryShader; }

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
    if (!gs_on() || bytes < 20) return;
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

// destroy_shader・destroy_pipeline から呼ぶ。覚えた物を捨てる（C1 の pipeline も消す）
static void gs_forget_pipe(VkDevice dv, uint64_t id) {
    if (!g_gsmods && !g_gspipes) return;
    uint64_t extra = 0;
    AcquireSRWLockExclusive(&g_gs_lock);
    for (struct gs_mod **p = &g_gsmods; *p; p = &(*p)->next)
        if ((*p)->id == id) { struct gs_mod *e = *p; *p = e->next; free(e->code); free(e); break; }
    // pipeline の覚えは表から外すだけで、free はしない（その pipeline をつないだ command buffer が指したままのことがある）
    for (struct gs_pipe **p = &g_gspipes; *p; p = &(*p)->next)
        if ((*p)->id == id) { if ((*p)->pull && (*p)->pull != id) extra = (*p)->pull; *p = (*p)->next; break; }
    ReleaseSRWLockExclusive(&g_gs_lock);
    if (extra && dv) destroy_pipeline_rpc(dv, (VkPipeline) (uintptr_t) extra, NULL);
}

// 頂点・index の buffer は、作った VS が storage buffer として読むので、その使い道を足す
static uint32_t gs_buffer_usage(uint32_t usage) {
    if (gs_on() && (usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT))) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    return usage;
}

// C1 の set の形（binding 0 記録・1 index・2+b 頂点の buffer）
static VkDescriptorSetLayout gs_dsl(VkDevice dv) {
    VkDescriptorSetLayout r = VK_NULL_HANDLE;
    AcquireSRWLockExclusive(&g_gs_lock);
    int k = 0;
    for (; k < 4 && g_gs_dsl[k].dv; k++) if (g_gs_dsl[k].dv == dv) { r = g_gs_dsl[k].l; break; }
    if (!r && k < 4) {
        VkDescriptorSetLayoutBinding b[2 + GS_MAX_VB];
        for (uint32_t i = 0; i < 2 + GS_MAX_VB; i++)
            b[i] = (VkDescriptorSetLayoutBinding) { i, i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
        VkDescriptorSetLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2 + GS_MAX_VB, .pBindings = b };
        if (create_dsl(dv, &ci, NULL, &r) == VK_SUCCESS) { g_gs_dsl[k].dv = dv; g_gs_dsl[k].l = r; }
    }
    ReleaseSRWLockExclusive(&g_gs_lock);
    return r;
}

// destroy_device から呼ぶ。消した装置の C1 の set の形を忘れる。
// 覚えは装置の番地で引くので、残したままだと、次の装置が同じ番地に作られたとき（Unreal は試しの装置を壊してから作り直す）、
// 受け手ではもう消えた形で set を取ろうとする。受け手は set を取れず、空の set をつないだ command buffer を
// iPhone の MoltenVK が submit で読んで落ちた（10/10 19:35・19:42 の bindDescriptorSets・set 1）
static void gs_forget_dev(VkDevice dv) {
    struct dsl *old = NULL;
    AcquireSRWLockExclusive(&g_gs_lock);
    int n = 0;
    while (n < 4 && g_gs_dsl[n].dv) n++;
    for (int k = 0; k < n; k++)
        if (g_gs_dsl[k].dv == dv) {
            old = (struct dsl *) g_gs_dsl[k].l;
            // 探す所は空きで止まるので、最後の物を空いた所へ詰める
            g_gs_dsl[k] = g_gs_dsl[n - 1];
            g_gs_dsl[n - 1].dv = VK_NULL_HANDLE;
            g_gs_dsl[n - 1].l = VK_NULL_HANDLE;
            break;
        }
    ReleaseSRWLockExclusive(&g_gs_lock);
    if (old) {
        say("geometry shader の真似：消した装置の set の形を忘れた");
        dsl_unref(old);
    }
}

// create_playout から呼ぶ。後ろに C1 の set を足した layout も作る（アプリが先に set の形を消しても困らないように、今作る）
static void gs_note_layout(VkDevice dv, uint64_t id, const VkPipelineLayoutCreateInfo *ci) {
    if (!gs_on() || g_gs_inner || ci->setLayoutCount >= 7) return;
    VkDescriptorSetLayout sl[8];
    for (uint32_t i = 0; i < ci->setLayoutCount; i++) {
        sl[i] = ci->pSetLayouts[i];
        if (!sl[i]) return;
    }
    if (!(sl[ci->setLayoutCount] = gs_dsl(dv))) return;
    VkPipelineLayoutCreateInfo c2 = *ci;
    c2.pNext = NULL;
    c2.setLayoutCount = ci->setLayoutCount + 1;
    c2.pSetLayouts = sl;
    VkPipelineLayout ext = VK_NULL_HANDLE;
    g_gs_inner = 1;
    VkResult r = create_playout(dv, &c2, NULL, &ext);
    g_gs_inner = 0;
    if (r != VK_SUCCESS) return;
    struct gs_layout *e = calloc(1, sizeof *e);
    *e = (struct gs_layout) { id, ci->setLayoutCount, ID(ext), NULL };
    AcquireSRWLockExclusive(&g_gs_lock);
    e->next = g_gslayouts;
    g_gslayouts = e;
    ReleaseSRWLockExclusive(&g_gs_lock);
}

static void gs_forget_layout(VkDevice dv, uint64_t id) {
    if (!g_gslayouts) return;
    struct gs_layout *e = NULL;
    AcquireSRWLockExclusive(&g_gs_lock);
    for (struct gs_layout **p = &g_gslayouts; *p; p = &(*p)->next)
        if ((*p)->id == id) { e = *p; *p = e->next; break; }
    ReleaseSRWLockExclusive(&g_gs_lock);
    if (e) { destroy_playout_rpc(dv, (VkPipelineLayout) (uintptr_t) e->ext, NULL); free(e); }
}

static int gs_find_layout(uint64_t id, uint64_t *ext, uint32_t *nsets) {
    int ok = 0;
    AcquireSRWLockShared(&g_gs_lock);
    for (struct gs_layout *e = g_gslayouts; e; e = e->next)
        if (e->id == id) { *ext = e->ext; *nsets = e->nsets; ok = 1; break; }
    ReleaseSRWLockShared(&g_gs_lock);
    return ok;
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
    return gs_on() && (f & VK_SHADER_STAGE_GEOMETRY_BIT) ? f | VK_SHADER_STAGE_VERTEX_BIT : f;
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

// 作った VS の module で pipeline を作る。vi・ia・layout を差し替える
static VkResult gs_make(VkDevice dv, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *c, const VkAllocationCallbacks *a,
                        const VkPipelineShaderStageCreateInfo *vs, const VkPipelineShaderStageCreateInfo *ps, const VkSpecializationInfo *si,
                        const uint32_t *code, size_t nw, const VkPipelineVertexInputStateCreateInfo *vi, uint32_t topo, uint64_t layout, VkPipeline *out) {
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = code };
    VkShaderModule mod = VK_NULL_HANDLE;
    create_shader(dv, &smi, NULL, &mod);
    VkPipelineShaderStageCreateInfo st[2];
    uint32_t ns = 1;
    st[0] = *vs;
    st[0].pNext = NULL;
    st[0].module = mod;
    st[0].pName = "main";
    st[0].pSpecializationInfo = si->mapEntryCount ? si : NULL;
    if (ps) st[ns++] = *ps;
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topo };
    VkGraphicsPipelineCreateInfo c2 = *c;
    c2.stageCount = ns;
    c2.pStages = st;
    c2.pInputAssemblyState = &ia;
    c2.pVertexInputState = vi;
    c2.pTessellationState = NULL;
    c2.layout = (VkPipelineLayout) (uintptr_t) layout;
    VkResult r = create_graphics_pipelines(dv, cache, 1, &c2, a, out);
    destroy_shader(dv, mod, NULL);
    return r;
}

static VkResult gs_emu_pipeline(VkDevice dv, VkPipelineCache cache, const VkGraphicsPipelineCreateInfo *c, const VkAllocationCallbacks *a, VkPipeline *out) {
    *out = VK_NULL_HANDLE;
    const VkPipelineShaderStageCreateInfo *vs = NULL, *gs = NULL, *ps = NULL;
    for (uint32_t k = 0; k < c->stageCount; k++) {
        const VkPipelineShaderStageCreateInfo *s = &c->pStages[k];
        if (s->stage == VK_SHADER_STAGE_VERTEX_BIT) vs = s;
        else if (s->stage == VK_SHADER_STAGE_GEOMETRY_BIT) gs = s;
        else if (s->stage == VK_SHADER_STAGE_FRAGMENT_BIT) ps = s;
        else { say("geometry shader と一緒の段 0x%x は真似られない", s->stage); return VK_ERROR_UNKNOWN; }
    }
    size_t nv = 0, ng = 0;
    const uint32_t *cv = vs ? gs_code(vs, &nv) : NULL, *cg = gs ? gs_code(gs, &ng) : NULL;
    if (!cv || !cg) { say("geometry shader を真似る：VS か GS の SPIR-V が見つからない"); return VK_ERROR_UNKNOWN; }
    VkSpecializationMapEntry me[128];
    uint8_t data[2048];
    VkSpecializationInfo si;
    gs_merge_spec(vs->pSpecializationInfo, gs->pSpecializationInfo, &si, me, data, sizeof data);
    // 動的な状態
    int dyn_topo = 0;
    const VkPipelineDynamicStateCreateInfo *dy = c->pDynamicState;
    for (uint32_t k = 0; dy && k < dy->dynamicStateCount; k++) {
        if (dy->pDynamicStates[k] == VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) dyn_topo = 1;
        if (dy->pDynamicStates[k] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) { say("geometry shader を真似る：動的な頂点の入力はまだ"); return VK_ERROR_UNKNOWN; }
    }
    const VkPipelineVertexInputStateCreateInfo *avi = c->pVertexInputState;
    if (avi && (avi->pNext || avi->vertexBindingDescriptionCount > 32 || avi->vertexAttributeDescriptionCount > 32)) {
        say("geometry shader を真似る：頂点の入力の形が読めない");
        return VK_ERROR_UNKNOWN;
    }
    struct gs_pipe p = { .dyn_topo = dyn_topo, .app_topo = c->pInputAssemblyState ? c->pInputAssemblyState->topology : 0 };
    int inst_rate = 0;
    for (uint32_t k = 0; avi && k < avi->vertexBindingDescriptionCount; k++) {
        const VkVertexInputBindingDescription *b = &avi->pVertexBindingDescriptions[k];
        inst_rate |= b->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE;
        if (b->binding < GS_MAX_VB) p.stride[b->binding] = b->stride;
    }
    // B：点を受ける GS で、インスタンスごとの頂点の入力が無いとき
    uint32_t *code = NULL;
    size_t nw = 0;
    gs_emu_info info;
    VkPipeline fixed = VK_NULL_HANDLE, pull = VK_NULL_HANDLE;
    char why_b[160] = "", why_c[160] = "";
    if (gs_emu_input_prim(cg, ng) == 19 && !inst_rate) {
        if (gs_emu_build2(cv, nv, cg, ng, NULL, &code, &nw, &info)) {
            // 頂点の属性は、インスタンスごとに 1 つ読む（firstInstance に元の firstVertex を入れて描くので、同じ要素を読む）
            VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkVertexInputBindingDescription vb[32];
            if (avi) {
                vi = *avi;
                for (uint32_t k = 0; k < vi.vertexBindingDescriptionCount; k++) {
                    vb[k] = vi.pVertexBindingDescriptions[k];
                    vb[k].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
                }
                vi.pVertexBindingDescriptions = vb;
            }
            if (gs_make(dv, cache, c, a, vs, ps, &si, code, nw, avi ? &vi : NULL, info.topology, ID(c->layout), &fixed) == VK_SUCCESS) {
                p.m = info.draw_vertices;
                p.topo = info.topology;
                p.has_b = 1;
            } else snprintf(why_b, sizeof why_b, "pipeline を作れない");
            free(code);
            code = NULL;
        } else snprintf(why_b, sizeof why_b, "%s", info.why);
    } else snprintf(why_b, sizeof why_b, inst_rate ? "インスタンスごとの頂点の入力がある" : "点を受ける GS でない");
    // C1：index と頂点の buffer を自分で読む
    uint64_t ext = 0;
    uint32_t nsets = 0;
    if (!gs_find_layout(ID(c->layout), &ext, &nsets)) snprintf(why_c, sizeof why_c, "set を足した pipeline layout が無い");
    else {
        gs_emu_cfg cfg = { .pull = 1, .set = nsets };
        for (uint32_t k = 0; avi && k < avi->vertexAttributeDescriptionCount; k++) {
            const VkVertexInputAttributeDescription *d = &avi->pVertexAttributeDescriptions[k];
            cfg.attr[cfg.nattr++] = (gs_attr) { d->location, d->binding, d->format, d->offset };
        }
        for (uint32_t k = 0; avi && k < avi->vertexBindingDescriptionCount; k++) {
            const VkVertexInputBindingDescription *d = &avi->pVertexBindingDescriptions[k];
            cfg.bind[cfg.nbind++] = (gs_vbind) { d->binding, d->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE, 1 };
        }
        if (gs_emu_build2(cv, nv, cg, ng, &cfg, &code, &nw, &info)) {
            VkPipelineVertexInputStateCreateInfo vi0 = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            if (gs_make(dv, cache, c, a, vs, ps, &si, code, nw, &vi0, info.topology, ext, &pull) == VK_SUCCESS) {
                p.pull = ID(pull);
                p.pull_layout = ext;
                p.set = nsets;
                p.pm = info.draw_vertices;
                p.ptopo = info.topology;
                p.nv = info.in_verts;
                p.inv = info.invocations;
                p.vb_used = info.used_bindings;
            } else snprintf(why_c, sizeof why_c, "pipeline を作れない");
            free(code);
        } else snprintf(why_c, sizeof why_c, "%s", info.why);
    }
    if (!fixed && !pull) { say("geometry shader を真似られない：B %s・C1 %s", why_b, why_c); return VK_ERROR_UNKNOWN; }
    if (!fixed) { p.m = p.pm; p.topo = p.ptopo; }
    p.id = fixed ? ID(fixed) : ID(pull);
    struct gs_pipe *e = calloc(1, sizeof *e);
    *e = p;
    AcquireSRWLockExclusive(&g_gs_lock);
    e->next = g_gspipes;
    g_gspipes = e;
    ReleaseSRWLockExclusive(&g_gs_lock);
    *out = (VkPipeline) (uintptr_t) p.id;
    say("geometry shader を頂点シェーダーで真似た：B %s・C1 %s（頂点 %u・形 %u・入力の頂点 %u・instancing %u・並び %d・set %u）",
        fixed ? "あり" : why_b, pull ? "あり" : why_c, p.pm, p.ptopo, p.nv, p.inv, pull ? info.list_mode : 0, p.set);
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

// ---- command buffer ごとの物 ----
static struct gs_cbst *gs_st(struct cb *c) {
    if (!c->gsx) c->gsx = calloc(1, sizeof *c->gsx);
    return c->gsx;
}

// cmd_bind_vbs・cmd_bind_vbs2 から呼ぶ
static void gs_track_vb(struct cb *c, uint32_t first, uint32_t n, const VkBuffer *bs, const VkDeviceSize *offs, const VkDeviceSize *strides) {
    if (!gs_on()) return;
    struct gs_cbst *st = gs_st(c);
    for (uint32_t i = 0; i < n && first + i < GS_MAX_VB; i++) {
        st->vb[first + i].b = (struct buf *) bs[i];
        st->vb[first + i].off = offs[i];
        if (strides) { st->vb[first + i].stride = strides[i]; st->vb[first + i].has_stride = 1; }
    }
}

// cmd_bind_ib から呼ぶ
static void gs_track_ib(struct cb *c, VkBuffer bb, VkDeviceSize off, VkIndexType t) {
    if (!gs_on()) return;
    struct gs_cbst *st = gs_st(c);
    st->ib = (struct buf *) bb;
    st->iboff = off;
    st->itype = t;
}

// cmd_bind_sets から呼ぶ。C1 の set を差したあとに、アプリの同じ番号の set を戻すため
static void gs_track_sets(struct cb *c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *sets,
                          uint32_t nd, const uint32_t *dyn) {
    if (!gs_on() || bp != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    struct gs_cbst *st = gs_st(c);
    uint32_t di = 0;
    for (uint32_t i = 0; i < n; i++) {
        const struct dset *s = (const struct dset *) sets[i];
        uint32_t nds = 0;
        for (uint32_t k = 0; s && s->layout && k < s->layout->nt; k++)
            if (s->layout->t[k].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || s->layout->t[k].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                nds += s->layout->t[k].count;
        if (first + i < 8) {
            st->app[first + i].layout = ID(l);
            st->app[first + i].set = ID(sets[i]);
            st->app[first + i].ndyn = nds <= 8 && di + nds <= nd ? nds : 0;
            for (uint32_t k = 0; k < st->app[first + i].ndyn; k++) st->app[first + i].dyn[k] = dyn[di + k];
        }
        di += nds;
    }
}

// 記録の場所を 1 つ取る（256 バイト。dynamic offset の並びの単位の最大）
static uint8_t *gs_rec(struct cb *c, struct gs_cbst *st, struct gs_chunk **ch, uint32_t *off) {
    VkDevice dv = (VkDevice) c->d;
    struct gs_chunk *k = st->cur;
    if (k && k->used + GS_REC_BYTES > k->size) { k = k->next; if (k) k->used = 0; }
    if (!k) {
        k = calloc(1, sizeof *k);
        k->size = GS_CHUNK;
        VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GS_CHUNK,
                                  .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        VkMemoryRequirements req;
        if (create_buffer(dv, &bc, NULL, &k->b) != VK_SUCCESS) { free(k); return NULL; }
        buffer_reqs(dv, k->b, &req);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                     .memoryTypeIndex = pick_mem(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        if (alloc_memory(dv, &mai, NULL, &k->m) != VK_SUCCESS) { destroy_buffer(dv, k->b, NULL); free(k); return NULL; }
        bind_buffer(dv, k->b, k->m, 0);
        map_memory(dv, k->m, 0, VK_WHOLE_SIZE, 0, (void **) &k->p);
        if (!k->p) { destroy_buffer(dv, k->b, NULL); free_memory(dv, k->m, NULL); free(k); return NULL; }
        // 続きに足す
        struct gs_chunk **pp = &st->chunks;
        while (*pp) pp = &(*pp)->next;
        *pp = k;
    }
    st->cur = k;
    if (!k->reg) { cb_reads(c, (struct buf *) k->b, 0, k->size); k->reg = 1; }
    *ch = k;
    *off = k->used;
    k->used += GS_REC_BYTES;
    memset(k->p + *off, 0, GS_REC_BYTES);
    return k->p + *off;
}

// C1 の set（記録の buffer・index と頂点の buffer）。中身が同じなら前の物を使う
static VkDescriptorSet gs_set(struct cb *c, struct gs_cbst *st, const struct gs_pipe *g, struct gs_chunk *ch) {
    VkDevice dv = (VkDevice) c->d;
    struct buf *ib = st->ib ? st->ib : (struct buf *) ch->b, *vb[GS_MAX_VB] = { 0 };
    int same = st->set && st->set_chunk == ch && st->set_ib == ib && (st->set_mask & g->vb_used) == g->vb_used;
    for (uint32_t b = 0; b < GS_MAX_VB; b++) {
        if (!(g->vb_used >> b & 1)) continue;
        vb[b] = st->vb[b].b ? st->vb[b].b : (struct buf *) ch->b;
        if (st->set_vb[b] != vb[b]) same = 0;
    }
    if (same) return st->set;
    VkDescriptorSetLayout l = gs_dsl(dv);
    if (!l) return VK_NULL_HANDLE; // 形を作れない（装置が 5 つ以上など）。alloc_dsets に空の形を渡さない
    struct gs_dpool *p = st->curpool;
    if (p && !p->left) {
        p = p->next;
        if (p && p->left != GS_POOL_SETS) { reset_dpool(dv, p->p, 0); p->left = GS_POOL_SETS; }
    }
    if (!p) {
        p = calloc(1, sizeof *p);
        VkDescriptorPoolSize sz[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, GS_POOL_SETS }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, GS_POOL_SETS * (1 + GS_MAX_VB) } };
        VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = GS_POOL_SETS, .poolSizeCount = 2, .pPoolSizes = sz };
        if (create_dpool(dv, &pci, NULL, &p->p) != VK_SUCCESS) { free(p); return VK_NULL_HANDLE; }
        p->left = GS_POOL_SETS;
        struct gs_dpool **pp = &st->pools;
        while (*pp) pp = &(*pp)->next;
        *pp = p;
    }
    st->curpool = p;
    VkDescriptorSet s = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = p->p, .descriptorSetCount = 1, .pSetLayouts = &l };
    if (alloc_dsets(dv, &ai, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    p->left--;
    VkDescriptorBufferInfo bi[2 + GS_MAX_VB];
    VkWriteDescriptorSet w[2 + GS_MAX_VB];
    uint32_t nw = 0;
    bi[0] = (VkDescriptorBufferInfo) { ch->b, 0, GS_REC_BYTES };
    bi[1] = (VkDescriptorBufferInfo) { (VkBuffer) ib, 0, VK_WHOLE_SIZE };
    for (uint32_t b = 0; b < 2 + GS_MAX_VB; b++) {
        if (b >= 2) {
            if (!(g->vb_used >> (b - 2) & 1)) continue;
            bi[b] = (VkDescriptorBufferInfo) { (VkBuffer) vb[b - 2], 0, VK_WHOLE_SIZE };
        }
        w[nw++] = (VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s, .dstBinding = b, .descriptorCount = 1,
                                           .descriptorType = b ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &bi[b] };
    }
    update_dsets(dv, nw, w, 0, NULL);
    st->set = s;
    st->set_chunk = ch;
    st->set_ib = ib;
    st->set_mask = g->vb_used;
    memcpy(st->set_vb, vb, sizeof vb);
    return s;
}

static void gs_put_bind_set(struct cb *c, uint64_t layout, uint32_t first, uint64_t set, uint32_t nd, const uint32_t *dyn) {
    vr_u32(&c->s, VR_CMD_BIND_DESCRIPTOR_SETS);
    vr_u32(&c->s, VK_PIPELINE_BIND_POINT_GRAPHICS);
    vr_u64(&c->s, layout);
    vr_u32(&c->s, first);
    vr_u32(&c->s, 1);
    vr_u64(&c->s, set);
    vr_u32(&c->s, nd);
    for (uint32_t i = 0; i < nd; i++) vr_u32(&c->s, dyn[i]);
}

static void gs_put_pipeline(struct cb *c, uint64_t p) {
    vr_u32(&c->s, VR_CMD_BIND_PIPELINE);
    vr_u32(&c->s, VK_PIPELINE_BIND_POINT_GRAPHICS);
    vr_u64(&c->s, p);
}

static void gs_once(const char *what) {
    static const char *seen[16];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == what) return;
    if (n < 16) seen[n++] = what;
    say("geometry shader を真似る：%s（飛ばす）", what);
}

// C1 の描画。indexed なら first は firstIndex、そうでなければ firstVertex
static void gs_pull_draw(struct cb *c, const struct gs_pipe *g, int indexed, uint32_t count, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) {
    if (!g->pull) { gs_once("この GS は C1 で作れていないので、番号つき・インスタンスつきの描画はできない"); return; }
    if (!count || !ic) return;
    struct gs_cbst *st = gs_st(c);
    uint32_t topo = g->dyn_topo && c->app_topo_set ? c->app_topo : g->app_topo, nv = 0, cls = GS_TOPO_LIST, P = 0;
    switch (topo) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: nv = 1; P = count; break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: nv = 2; P = count / 2; break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: nv = 2; cls = GS_TOPO_STRIP; P = count >= 2 ? count - 1 : 0; break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: nv = 3; P = count / 3; break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: nv = 3; cls = GS_TOPO_STRIP; P = count >= 3 ? count - 2 : 0; break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: nv = 3; cls = GS_TOPO_FAN; P = count >= 3 ? count - 2 : 0; break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: nv = 4; P = count / 4; break;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: nv = 4; cls = GS_TOPO_STRIP; P = count >= 4 ? count - 3 : 0; break;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY: nv = 6; P = count / 6; break;
    default: gs_once("この入力の形（隣つきの三角の帯・パッチ）はまだ"); return;
    }
    if (nv != g->nv) { gs_once("描画の形が GS の入力の形と合わない"); return; }
    if (!P) return;
    uint32_t isz = st->itype == VK_INDEX_TYPE_UINT32 ? 4 : st->itype == VK_INDEX_TYPE_UINT8_EXT ? 1 : 2;
    if (indexed && !st->ib) { gs_once("index buffer がつないでいない"); return; }
    struct gs_chunk *ch = NULL;
    uint32_t off = 0;
    uint32_t *r = (uint32_t *) gs_rec(c, st, &ch, &off);
    if (!r) { gs_once("記録の buffer を作れない"); return; }
    r[GSR_FLAGS] = (indexed ? 1u : 0u) | (indexed && cls == GS_TOPO_STRIP ? 2u : 0u); // 帯の番号つきは restart の値を見る（DXVK は帯で restart を有効にする）
    r[GSR_IB] = indexed ? (uint32_t) (st->iboff + (uint64_t) first * isz) : 0;
    r[GSR_VBASE] = indexed ? (uint32_t) vo : first;
    r[GSR_FINST] = fi;
    r[GSR_PRIMS] = P;
    r[GSR_ISZ] = isz;
    r[GSR_IMASK] = isz == 4 ? 0xffffffffu : isz == 2 ? 0xffffu : 0xffu;
    r[GSR_TOPO] = cls;
    for (uint32_t b = 0; b < GS_MAX_VB; b++) {
        if (!(g->vb_used >> b & 1)) continue;
        r[GSR_VB + 2 * b] = (uint32_t) st->vb[b].off;
        r[GSR_VB + 2 * b + 1] = (uint32_t) (st->vb[b].has_stride ? st->vb[b].stride : g->stride[b]);
    }
    VkDescriptorSet set = gs_set(c, st, g, ch);
    if (!set) { gs_once("descriptor set を作れない"); return; }
    uint64_t inst = (uint64_t) P * ic * (g->inv ? g->inv : 1);
    if (inst > 0xffffffffull) { gs_once("インスタンスが多すぎる"); return; }
    if (g->pull != g->id) gs_put_pipeline(c, g->pull);
    if (g->dyn_topo && g->ptopo != g->topo) dyn1((VkCommandBuffer) c, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, g->ptopo);
    gs_put_bind_set(c, g->pull_layout, g->set, ID(set), 1, &off);
    vr_u32(&c->s, VR_CMD_DRAW);
    vr_u32(&c->s, g->pm);
    vr_u32(&c->s, (uint32_t) inst);
    vr_u32(&c->s, 0);
    vr_u32(&c->s, 0);
    if (g->pull != g->id) gs_put_pipeline(c, g->id);
    if (g->dyn_topo && g->ptopo != g->topo) dyn1((VkCommandBuffer) c, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, g->topo);
    // アプリが同じ番号に set をつないでいたら戻す
    if (g->set < 8 && st->app[g->set].set) gs_put_bind_set(c, st->app[g->set].layout, g->set, st->app[g->set].set, st->app[g->set].ndyn, st->app[g->set].dyn);
}

// cmd_draw から呼ぶ。GS を真似た pipeline なら描き方を変えて 1 を返す
static int gs_draw(struct cb *c, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    const struct gs_pipe *g = c->gs;
    if (!g) return 0;
    if (ic == 1 && g->has_b) {
        vr_u32(&c->s, VR_CMD_DRAW);
        vr_u32(&c->s, g->m); // 頂点：GS が出す k 番目
        vr_u32(&c->s, vc);   // インスタンス：元の点
        vr_u32(&c->s, 0);
        vr_u32(&c->s, fv);   // 作った VS の中で InstanceIndex が元の VertexIndex と同じになる
        return 1;
    }
    gs_pull_draw(c, g, 0, vc, ic, fv, 0, fi);
    return 1;
}

// cmd_draw_indexed から呼ぶ
static int gs_draw_indexed(struct cb *c, uint32_t n, uint32_t ic, uint32_t first, int32_t vo, uint32_t fi) {
    const struct gs_pipe *g = c->gs;
    if (!g) return 0;
    gs_pull_draw(c, g, 1, n, ic, first, vo, fi);
    return 1;
}

// 間接の描画は、まだ真似ない（飛ばす）
static int gs_skip(struct cb *c, const char *what) {
    if (!c->gs) return 0;
    (void) what;
    gs_once("間接の描画はまだ");
    return 1;
}

// cb_clear から呼ぶ。記録の buffer と pool は次の記録で使い回す
static void gs_cb_clear(struct cb *c) {
    struct gs_cbst *st = c->gsx;
    if (!st) return;
    VkDevice dv = (VkDevice) c->d;
    for (struct gs_chunk *k = st->chunks; k; k = k->next) { k->used = 0; k->reg = 0; }
    for (struct gs_dpool *p = st->pools; p; p = p->next)
        if (p->left != GS_POOL_SETS) { reset_dpool(dv, p->p, 0); p->left = GS_POOL_SETS; }
    struct gs_chunk *chunks = st->chunks;
    struct gs_dpool *pools = st->pools;
    memset(st, 0, sizeof *st);
    st->chunks = chunks;
    st->cur = chunks;
    st->pools = pools;
    st->curpool = pools;
}

// cb_free から呼ぶ
static void gs_cb_free(struct cb *c) {
    struct gs_cbst *st = c->gsx;
    if (!st) return;
    VkDevice dv = (VkDevice) c->d;
    for (struct gs_chunk *k = st->chunks, *nx; k; k = nx) { nx = k->next; destroy_buffer(dv, k->b, NULL); free_memory(dv, k->m, NULL); free(k); }
    for (struct gs_dpool *p = st->pools, *nx; p; p = nx) { nx = p->next; destroy_dpool(dv, p->p, NULL); free(p); }
    free(st);
    c->gsx = NULL;
}
