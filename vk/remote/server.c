// Vulkan の命令の受け手。PC のドライバから届いた命令を、ここの本物の Vulkan で実行する。
// Windows では試験用に vulkan-1.dll（RTX 3060）で、iPhone では MoltenVK（Metal）で動く。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "proto.h"
#include "wire.h"
// 命令の並びを縮める LZ4（BSD-2）。この 1 つのファイルの中に取り込む
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "../third_party/lz4/lz4.c"
#pragma GCC diagnostic pop

#ifdef _WIN32
#include <windows.h>
#define COBJMACROS
#include <wincodec.h>
#endif
#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <os/log.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif
#endif
#include <stdarg.h>
#include <stddef.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#endif

// 記録。iPhone ではアプリの中の printf が見えないので os_log に出す（PC から USB で読める）
static void vlog(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
#ifdef __APPLE__
    os_log(OS_LOG_DEFAULT, "vk: %{public}s", line);
    // アプリの記録にも足す（PC から USB の 50059 番で読める。os_log は USB の記録の中継に出てこない）
    extern void rpc_bridge_log_line(const char *text);
    char with_nl[520];
    snprintf(with_nl, sizeof with_nl, "vk: %s\n", line);
    rpc_bridge_log_line(with_nl);
#else
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
#endif
}

// ---- 本物の Vulkan ----
static PFN_vkGetInstanceProcAddr gipa;
static VkInstance g_inst;
static VkPhysicalDevice g_pd;
static int g_portability; // MoltenVK のときは portability_subset を有効にする必要がある

#define VKFUNCS(X) \
    X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueWaitIdle) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkFreeDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdDispatchBase) \
    X(vkCmdDispatchIndirect) X(vkCmdCopyBuffer) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) X(vkCmdPipelineBarrier) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkGetFenceStatus) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkQueueSubmit) \
    X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkSignalSemaphore) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCreateGraphicsPipelines) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdNextSubpass) X(vkCmdDraw) \
    X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdClearColorImage) \
    X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkGetPhysicalDeviceImageFormatProperties) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCmdSetLineWidth) X(vkCmdSetDepthBias) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBounds) X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdClearAttachments) X(vkCmdClearDepthStencilImage) \
    X(vkCmdResolveImage) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkCmdResetQueryPool) \
    X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdWriteTimestamp) X(vkCmdCopyQueryPoolResults) X(vkGetPhysicalDeviceFormatProperties2) \
    X(vkCreateBufferView) X(vkDestroyBufferView) X(vkGetImageSubresourceLayout)

// 1.2・1.3 の入口。本体の名前か拡張の名前で読む。GPU が持っていなければ NULL のまま（そのときは caps で名乗らない）
#define VKFUNCS_OPT(X) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdPipelineBarrier2) X(vkCreateRenderPass2) X(vkResetQueryPool) \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount) \
    X(vkCmdBindVertexBuffers2) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetRasterizerDiscardEnable) \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkGetBufferDeviceAddress)

#define DECL(n) static PFN_##n n;
VKFUNCS(DECL)
VKFUNCS_OPT(DECL)
#undef DECL

static uint32_t g_caps = VR_CAP_DELTA; // 受け手ができること（あいさつで名乗る）
// 空の descriptor（robustness2 の nullDescriptor）を GPU が持つか。持てば装置を作るときに有効にする。
// 持たなければ（MoltenVK）、空の頂点バッファだけは 0 で埋めた代わりのバッファに差し替える
static int g_nulldesc_gpu;
static int g_divisor; // GPU が VK_EXT_vertex_attribute_divisor を持つ
// 調べ用：VKR_LIKE_IOS=1 で、PC の代役が iPhone の GPU のふりをする（BC・geometry shader・cull distance・空の descriptor を持たない）。
// 持たない機能を有効にして装置を作ろうとすると、MoltenVK と同じように断る
static int like_ios(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("VKR_LIKE_IOS"); v = e && *e == '1'; }
    return v;
}

// iPhone のふりで持たない形式。iPhone の GPU（Apple の GPU）には 24 ビットの深度の形式が無い（Metal の Depth24Unorm_Stencil8 は
// macOS の一部の GPU だけ）。DXVK はこれを見て 32 ビット浮動小数の深度に置き換える
static int like_ios_no_format(VkFormat f) {
    return like_ios() && (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM_S8_UINT);
}

// iPhone のふりで頂点に使えない形式。10/7 の iPhone 17 Pro の一覧で bufferFeatures に VERTEX_BUFFER が無かった物
// （DirectX の R10G10B10A2_UINT＝A2B10G10R10_UINT、BGRA の SNORM・SCALED・整数、A2R10G10B10、3 成分の BGR、64 ビット）。
// iPhone の vfmt の場面では、この形式の属性がある pipeline を作れなかった（-3）。ふりでも作らない
static int like_ios_no_vertex(VkFormat f) {
    return like_ios() && ((f >= VK_FORMAT_B8G8R8_UNORM && f <= VK_FORMAT_B8G8R8_SINT) || (f >= VK_FORMAT_B8G8R8A8_SNORM && f <= VK_FORMAT_B8G8R8A8_SINT) ||
                          (f >= VK_FORMAT_A2R10G10B10_UNORM_PACK32 && f <= VK_FORMAT_A2R10G10B10_SINT_PACK32) ||
                          (f >= VK_FORMAT_A2B10G10R10_USCALED_PACK32 && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32) ||
                          (f >= VK_FORMAT_R64_UINT && f <= VK_FORMAT_R64G64B64A64_SFLOAT));
}

// iPhone のふりで隠す機能。10/7 の iPhone 17 Pro（MoltenVK 1.4.2）の DXVK の記録と比べて、iPhone に無かった物
static void like_ios_mask(VkPhysicalDeviceFeatures *f) {
    if (!like_ios()) return;
    f->textureCompressionBC = f->geometryShader = f->shaderCullDistance = VK_FALSE;
    f->logicOp = f->shaderFloat64 = f->variableMultisampleRate = VK_FALSE;
}
static VkBuffer g_dummy_vb;
static VkDeviceMemory g_dummy_mem;
static VkSampler g_dummy_smp; // 空の sampler の代わり（ドライバの取りこぼしで、アプリごと落ちないように）
static VkBuffer dummy_vb(void);
static int g_fmt3;                     // VkFormatProperties3 を答えられる

// ---- PC が振った番号 → ここの本物の Vulkan の物 ----
// aux：バッファならつないだメモリの番号。list：descriptor set なら中のバッファの番号の並び
typedef struct { uint64_t *ids; uint32_t n, cap; } idlist;
typedef struct { uint64_t id, h; uint32_t kind; void *map; uint64_t size; uint64_t aux; idlist *list;
                 uint8_t *last; } ent; // last：ドライバが持っているはずの中身。VR_MEM_READ_DELTA で使う

static void idlist_add(idlist **l, uint64_t id) {
    if (!*l) *l = calloc(1, sizeof **l);
    idlist *x = *l;
    for (uint32_t i = 0; i < x->n; i++) if (x->ids[i] == id) return;
    if (x->n == x->cap) { x->cap = x->cap ? x->cap * 2 : 8; x->ids = realloc(x->ids, x->cap * sizeof *x->ids); }
    x->ids[x->n++] = id;
}

static void idlist_free(idlist **l) {
    if (*l) { free((*l)->ids); free(*l); *l = NULL; }
}
static ent *g_tab;
static size_t g_cap, g_cnt;

enum { K_DEVICE = 1, K_QUEUE, K_BUFFER, K_MEMORY, K_SHADER, K_DSL, K_PLAYOUT, K_PIPELINE, K_DPOOL, K_DSET, K_CPOOL, K_CB, K_FENCE, K_SEM,
       K_IMAGE, K_VIEW, K_RPASS, K_FB, K_SAMPLER, K_QPOOL, K_BVIEW };

static size_t slot(uint64_t id) { return (size_t) ((id * 0x9E3779B97F4A7C15ull) >> 20) & (g_cap - 1); }

static void tab_grow(void) {
    size_t old = g_cap;
    ent *o = g_tab;
    g_cap = g_cap ? g_cap * 2 : 1024;
    g_tab = calloc(g_cap, sizeof *g_tab);
    g_cnt = 0;
    for (size_t i = 0; i < old; i++)
        if (o[i].id) {
            size_t s = slot(o[i].id);
            while (g_tab[s].id) s = (s + 1) & (g_cap - 1);
            g_tab[s] = o[i];
            g_cnt++;
        }
    free(o);
}

static void put(uint64_t id, uint64_t h, uint32_t kind) {
    if ((g_cnt + 1) * 2 > g_cap) tab_grow();
    size_t s = slot(id);
    while (g_tab[s].id && g_tab[s].id != id) s = (s + 1) & (g_cap - 1);
    if (!g_tab[s].id) g_cnt++;
    if (g_tab[s].id == id) { idlist_free(&g_tab[s].list); free(g_tab[s].last); }
    g_tab[s] = (ent) { id, h, kind, NULL, 0, 0, NULL, NULL };
}

static ent *find(uint64_t id) {
    if (!id || !g_cap) return NULL;
    size_t s = slot(id);
    while (g_tab[s].id) {
        if (g_tab[s].id == id) return &g_tab[s];
        s = (s + 1) & (g_cap - 1);
    }
    return NULL;
}

static uint64_t H(uint64_t id) { ent *e = find(id); return e ? e->h : 0; }

static void del(uint64_t id) {
    // 開番地法なので、消したあとの並びを詰め直す
    if (!id || !g_cap) return;
    size_t s = slot(id);
    while (g_tab[s].id && g_tab[s].id != id) s = (s + 1) & (g_cap - 1);
    if (!g_tab[s].id) return;
    free(g_tab[s].last);
    g_tab[s].last = NULL;
    g_tab[s].id = 0;
    g_cnt--;
    for (size_t j = (s + 1) & (g_cap - 1); g_tab[j].id; j = (j + 1) & (g_cap - 1)) {
        ent e = g_tab[j];
        g_tab[j].id = 0;
        g_cnt--;
        size_t k = slot(e.id);
        while (g_tab[k].id) k = (k + 1) & (g_cap - 1);
        g_tab[k] = e;
        g_cnt++;
    }
}

// 番号を引くときは種類も確かめる。違う種類の番号や消した番号なら 0 にして、ドライバに渡さない
// （64 ビットでは Vulkan のハンドルの型がそれぞれ別の型なので、型から種類を決められる）
#define KIND_OF(T) _Generic((T) 0, VkBuffer: K_BUFFER, VkBufferView: K_BVIEW, VkCommandPool: K_CPOOL, VkDescriptorPool: K_DPOOL, \
    VkDescriptorSet: K_DSET, VkDescriptorSetLayout: K_DSL, VkDeviceMemory: K_MEMORY, VkFence: K_FENCE, VkFramebuffer: K_FB, \
    VkImage: K_IMAGE, VkImageView: K_VIEW, VkPipeline: K_PIPELINE, VkPipelineLayout: K_PLAYOUT, VkQueryPool: K_QPOOL, \
    VkRenderPass: K_RPASS, VkSampler: K_SAMPLER, VkSemaphore: K_SEM, VkShaderModule: K_SHADER, default: 0)
static uint64_t HK(uint64_t id, uint32_t kind) {
    ent *e = find(id);
    return e && (!kind || e->kind == kind) ? e->h : 0;
}
#define DEV(id) ((VkDevice) (uintptr_t) HK(id, K_DEVICE))
#define OBJ(T, id) ((T) (uintptr_t) HK(id, KIND_OF(T)))

// ---- 受け手ができることを調べる ----
// 本物の GPU が持っているか（版・拡張・機能）を見て、入口を読めたものだけを caps で名乗る。
// iPhone の MoltenVK は 1.3 の一部しか持たないことがあるので、名乗る前に必ず確かめる
static int has_ext(const VkExtensionProperties *ep, uint32_t ne, const char *name) {
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, name)) return 1;
    return 0;
}

// 本体の名前で読めればそれを、無ければ拡張の名前（末尾 suffix）で読む
static PFN_vkVoidFunction load_opt(int core, const char *name, const char *suffix) {
    PFN_vkVoidFunction f = core ? gipa(g_inst, name) : NULL;
    if (!f && suffix) {
        char n2[96];
        snprintf(n2, sizeof n2, "%s%s", name, suffix);
        f = gipa(g_inst, n2);
    }
    return f;
}

static void detect_caps(const VkExtensionProperties *ep, uint32_t ne, uint32_t api) {
#define CLEAR(n) n = NULL;
    VKFUNCS_OPT(CLEAR)
#undef CLEAR
    int v12 = VK_API_VERSION_MAJOR(api) > 1 || VK_API_VERSION_MINOR(api) >= 2;
    int v13 = VK_API_VERSION_MAJOR(api) > 1 || VK_API_VERSION_MINOR(api) >= 3;
    int e_dr = has_ext(ep, ne, "VK_KHR_dynamic_rendering"), e_s2 = has_ext(ep, ne, "VK_KHR_synchronization2");
    int e_eds = has_ext(ep, ne, "VK_EXT_extended_dynamic_state"), e_eds2 = has_ext(ep, ne, "VK_EXT_extended_dynamic_state2");
    int e_rp2 = has_ext(ep, ne, "VK_KHR_create_renderpass2"), e_hqr = has_ext(ep, ne, "VK_EXT_host_query_reset");
    // 機能を尋ねる（持っている拡張・版の構造体だけを鎖にする）
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceDynamicRenderingFeatures fdr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    VkPhysicalDeviceSynchronization2Features fs2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT feds = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT feds2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT };
    VkPhysicalDeviceHostQueryResetFeatures fhqr = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES };
    VkPhysicalDeviceBufferDeviceAddressFeatures fbda = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    int e_bda = has_ext(ep, ne, "VK_KHR_buffer_device_address");
    void *head = NULL, **link = &head;
#define LINK(cond, s) if (cond) { *link = &s; link = &s.pNext; }
    LINK(v13, f13)
    LINK(e_dr, fdr)
    LINK(e_s2, fs2)
    LINK(e_eds, feds)
    LINK(e_eds2, feds2)
    LINK(v12 || e_hqr, fhqr)
    LINK(v12 || e_bda, fbda)
#undef LINK
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = head };
    if (vkGetPhysicalDeviceFeatures2) vkGetPhysicalDeviceFeatures2(g_pd, &f2);
    uint32_t caps = VR_CAP_DELTA | VR_CAP_CMD10 | VR_CAP_QUERY; // 1.0 の入口だけで済むもの
#define LOAD_OPT(n, core, sfx) n = (PFN_##n) load_opt(core, #n, sfx);
    // dynamic rendering
    int core = v13 && f13.dynamicRendering, ext = e_dr && fdr.dynamicRendering;
    if (core || ext) {
        LOAD_OPT(vkCmdBeginRendering, core, ext ? "KHR" : NULL)
        LOAD_OPT(vkCmdEndRendering, core, ext ? "KHR" : NULL)
        if (vkCmdBeginRendering && vkCmdEndRendering) caps |= VR_CAP_DYNAMIC_RENDERING;
    }
    // synchronization2
    core = v13 && f13.synchronization2, ext = e_s2 && fs2.synchronization2;
    if (core || ext) {
        LOAD_OPT(vkCmdPipelineBarrier2, core, ext ? "KHR" : NULL)
        if (vkCmdPipelineBarrier2) caps |= VR_CAP_SYNC2;
    }
    // render pass 2（1.2 で本体入り）
    if (v12 || e_rp2) {
        LOAD_OPT(vkCreateRenderPass2, v12, e_rp2 ? "KHR" : NULL)
        if (vkCreateRenderPass2) caps |= VR_CAP_RENDER_PASS2;
    }
    // CPU からの query の reset（1.2 で本体入り）
    if (fhqr.hostQueryReset) LOAD_OPT(vkResetQueryPool, v12, e_hqr ? "EXT" : NULL)
    // extended dynamic state 1・2（1.3 で本体入り）
    core = v13, ext = e_eds && feds.extendedDynamicState && e_eds2 && feds2.extendedDynamicState2;
    if (core || ext) {
        const char *sx = ext ? "EXT" : NULL;
        LOAD_OPT(vkCmdSetCullMode, core, sx) LOAD_OPT(vkCmdSetFrontFace, core, sx) LOAD_OPT(vkCmdSetPrimitiveTopology, core, sx)
        LOAD_OPT(vkCmdSetViewportWithCount, core, sx) LOAD_OPT(vkCmdSetScissorWithCount, core, sx) LOAD_OPT(vkCmdBindVertexBuffers2, core, sx)
        LOAD_OPT(vkCmdSetDepthTestEnable, core, sx) LOAD_OPT(vkCmdSetDepthWriteEnable, core, sx) LOAD_OPT(vkCmdSetDepthCompareOp, core, sx)
        LOAD_OPT(vkCmdSetDepthBoundsTestEnable, core, sx) LOAD_OPT(vkCmdSetStencilTestEnable, core, sx) LOAD_OPT(vkCmdSetStencilOp, core, sx)
        LOAD_OPT(vkCmdSetRasterizerDiscardEnable, core, sx) LOAD_OPT(vkCmdSetDepthBiasEnable, core, sx)
        LOAD_OPT(vkCmdSetPrimitiveRestartEnable, core, sx)
        if (vkCmdSetCullMode && vkCmdSetFrontFace && vkCmdSetPrimitiveTopology && vkCmdSetViewportWithCount && vkCmdSetScissorWithCount &&
            vkCmdBindVertexBuffers2 && vkCmdSetDepthTestEnable && vkCmdSetDepthWriteEnable && vkCmdSetDepthCompareOp &&
            vkCmdSetDepthBoundsTestEnable && vkCmdSetStencilTestEnable && vkCmdSetStencilOp && vkCmdSetRasterizerDiscardEnable &&
            vkCmdSetDepthBiasEnable && vkCmdSetPrimitiveRestartEnable)
            caps |= VR_CAP_EXT_DYNAMIC_STATE;
    }
#undef LOAD_OPT
    // 1.3 と名乗れるのは、GPU が 1.3 で、1.3 の必須の入口を全部本体の名前で読めたとき
    const uint32_t need13 = VR_CAP_DYNAMIC_RENDERING | VR_CAP_SYNC2 | VR_CAP_RENDER_PASS2 | VR_CAP_EXT_DYNAMIC_STATE;
    if (v13 && (caps & need13) == need13) caps |= VR_CAP_VK13;
    g_fmt3 = (v13 || has_ext(ep, ne, "VK_KHR_format_feature_flags2")) && vkGetPhysicalDeviceFormatProperties2;
    caps |= VR_CAP_D3D; // buffer view と画像の並び方は 1.0 の入口なので、どの GPU でも使える
    caps |= VR_CAP_PACK | VR_CAP_ASYNC_CREATE | VR_CAP_STATUS | VR_CAP_LINK;
    // 頂点の step rate：GPU が拡張を持てば、pipeline の末尾を読んでつなぐ（MoltenVK も持つ）
    {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
        VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex);
        vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, ex);
        for (uint32_t i = 0; i < ne; i++)
            if (!strcmp(ex[i].extensionName, "VK_EXT_vertex_attribute_divisor")) g_divisor = 1;
        free(ex);
        if (g_divisor) caps |= VR_CAP_DIVISOR;
    }
#if defined(__APPLE__) || defined(_WIN32)
    caps |= VR_CAP_JPEG | VR_CAP_JPEG_PIPE; // 窓に出すコマを JPEG に縮められる（iPhone は ImageIO、PC の代役は WIC）
#endif
    // buffer device address（GPU の中のバッファのアドレス）
    if (fbda.bufferDeviceAddress && (v12 || e_bda)) {
        vkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress) load_opt(v12, "vkGetBufferDeviceAddress", e_bda ? "KHR" : NULL);
        if (vkGetBufferDeviceAddress) caps |= VR_CAP_BDA;
    }
    // 空の descriptor
    g_nulldesc_gpu = 0;
    if (has_ext(ep, ne, "VK_EXT_robustness2") && vkGetPhysicalDeviceFeatures2) {
        VkPhysicalDeviceRobustness2FeaturesEXT r2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &r2 };
        vkGetPhysicalDeviceFeatures2(g_pd, &f2);
        g_nulldesc_gpu = r2.nullDescriptor == VK_TRUE && !like_ios();
    }
    caps |= VR_CAP_NULL_DESC;
    // 調べ用：VKR_CAPS=数 で名乗る印を絞る（古い受け手のふりをする）
    const char *mask = getenv("VKR_CAPS");
    if (mask && *mask) caps &= (uint32_t) strtoul(mask, NULL, 0);
    g_caps = caps;
    vlog("できること（caps）0x%x：1.3 %s・dynamic rendering %s・sync2 %s・render pass 2 %s・extended dynamic state %s・query %s",
         caps, caps & VR_CAP_VK13 ? "○" : "×", caps & VR_CAP_DYNAMIC_RENDERING ? "○" : "×", caps & VR_CAP_SYNC2 ? "○" : "×",
         caps & VR_CAP_RENDER_PASS2 ? "○" : "×", caps & VR_CAP_EXT_DYNAMIC_STATE ? "○" : "×", caps & VR_CAP_QUERY ? "○" : "×");
}

// ---- 起動 ----
// MoltenVK の言い分（なぜ pipeline を作れないかなど）を受け手の記録に流す。iPhone では MoltenVK の記録が USB に出てこないため。
// 1 つの接続で 300 件まで（あふれて受け手が遅くならないように）
static VkDebugUtilsMessengerEXT g_dbg_mess;
static PFN_vkDestroyDebugUtilsMessengerEXT g_dbg_destroy;
static volatile int g_dbg_msgs;
static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void) type; (void) u;
    if (g_dbg_msgs < 300 && d && d->pMessage) {
        g_dbg_msgs++;
        vlog("MoltenVK（%s）：%.440s", sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "誤り" : "注意", d->pMessage);
    }
    return VK_FALSE;
}
static void dbg_stop(void) {
    if (g_dbg_mess && g_dbg_destroy) g_dbg_destroy(g_inst, g_dbg_mess, NULL);
    g_dbg_mess = VK_NULL_HANDLE;
}

static int vk_init(void) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) return 0;
    gipa = (PFN_vkGetInstanceProcAddr) GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    extern PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance, const char *);
    gipa = (PFN_vkGetInstanceProcAddr) vkGetInstanceProcAddr;
#endif
    vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    // 装置で使える版は instance の版と GPU の版の小さい方になる。1.3 の入口を本体の名前で使えるように、できれば 1.3 で作る
    uint32_t iv = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion eiv = (PFN_vkEnumerateInstanceVersion) gipa(NULL, "vkEnumerateInstanceVersion");
    if (eiv) eiv(&iv);
    uint32_t want = VK_API_VERSION_MAJOR(iv) > 1 || VK_API_VERSION_MINOR(iv) >= 3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "iphone-rpc-vk", .apiVersion = want };
    const char *ext[2];
    int want_dbg = 0;
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .ppEnabledExtensionNames = ext };
    // portability_enumeration は Vulkan のローダーが持つ拡張。iPhone のように MoltenVK を直接つなぐと無いので、
    // あるときだけ有効にする（無いのに頼むと vkCreateInstance が失敗する）
#ifdef _WIN32
    // Windows では有効にしない（有効にすると、暗黙のレイヤーが挟まった vkCreateDevice が戻ってこなかった）
    PFN_vkEnumerateInstanceExtensionProperties enum_ext = NULL;
#else
    PFN_vkEnumerateInstanceExtensionProperties enum_ext = (PFN_vkEnumerateInstanceExtensionProperties) gipa(NULL, "vkEnumerateInstanceExtensionProperties");
#endif
    uint32_t nie = 0;
    if (enum_ext) enum_ext(NULL, &nie, NULL);
    VkExtensionProperties *ie = calloc(nie ? nie : 1, sizeof *ie);
    if (enum_ext) enum_ext(NULL, &nie, ie);
    for (uint32_t i = 0; i < nie; i++) {
        if (!strcmp(ie[i].extensionName, "VK_KHR_portability_enumeration")) {
            ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            ext[ci.enabledExtensionCount++] = "VK_KHR_portability_enumeration";
        } else if (!strcmp(ie[i].extensionName, "VK_EXT_debug_utils")) {
            ext[ci.enabledExtensionCount++] = "VK_EXT_debug_utils";
            want_dbg = 1;
        }
    }
    free(ie);
    VkResult ir = vkCreateInstance(&ci, NULL, &g_inst);
    if (ir != VK_SUCCESS) { vlog("vkCreateInstance が失敗 (%d)", ir); return 0; }
    if (want_dbg) {
        PFN_vkCreateDebugUtilsMessengerEXT mk = (PFN_vkCreateDebugUtilsMessengerEXT) gipa(g_inst, "vkCreateDebugUtilsMessengerEXT");
        g_dbg_destroy = (PFN_vkDestroyDebugUtilsMessengerEXT) gipa(g_inst, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = dbg_cb,
        };
        if (!mk || mk(g_inst, &mi, NULL, &g_dbg_mess) != VK_SUCCESS) g_dbg_mess = VK_NULL_HANDLE;
        if (g_dbg_mess) vlog("MoltenVK の言い分を記録に流す");
    }
#define LOAD(n) n = (PFN_##n) gipa(g_inst, #n);
    VKFUNCS(LOAD)
#undef LOAD
    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(g_inst, &n, &g_pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) { vlog("GPU が見つからない (%d, %u 台)", r, n); return 0; }
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, ep);
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, "VK_KHR_portability_subset")) g_portability = 1;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_pd, &p);
    vlog("受け手の GPU: %s", p.deviceName);
    detect_caps(ep, ne, p.apiVersion < want ? p.apiVersion : want);
    free(ep);
    return 1;
}

// ---- 出した仕事が終わるのを受け手の中で待つ（版 3） ----
// 待つのは、出した仕事が使っている物を変えるときだけ（その仕事まで待つ）。
// 関係の無い物を変える命令は待たないので、GPU が計算している間に次の命令を作れる
static VkDevice g_dev;
typedef struct { VkFence f; int own; uint64_t cbs[64]; uint32_t ncb; uint64_t *snap; uint32_t nsnap; } inflight; // snap：調べ用、submit した時点の set
static inflight g_fl[256];
static int g_nfl;
static VkResult g_err;          // GPU を失ったなど。次に返事をするときに伝える
static int g_lost;              // この接続で GPU を失った。切れたあと Vulkan を作り直す
static volatile int g_paused;   // アプリが前にいない間は 1（GPU に新しい仕事を出さない）

// アプリから呼ぶ：前にいないときは 1
void vkr_set_paused(int on) { g_paused = on; }

// ---- インターネット共有の口（Backburner と同じく、USB を有線のネットワークとして使う道） ----
// USB の口（iPhone の中の 127.0.0.1・usbmux）に加えて、インターネット共有の口でも待つ。
// インターネット共有は Wi-Fi でも近くの人がつなげるので、最初に合言葉を出さない相手は断る。
// 合言葉はアプリが起動のたびに作り、USB の口（信頼済みの PC だけがつなげる）で VR_GET_LINK に答えて渡す
static uint8_t g_token[16];
static int g_token_ok;
static char g_extra_host[64];
static int g_main_port = VR_DEFAULT_PORT;
static volatile int g_cur_remote; // いま相手をしているのがインターネット共有の口から来た人なら 1
static vr_sock g_extra_ls = VR_BAD_SOCK;
static const char *g_extra_env; // PC の代役の調べ用（VKR_EXTRA_LISTEN）
#ifdef _WIN32
static SRWLOCK g_serve_lk = SRWLOCK_INIT;
static void serve_lock(void) { AcquireSRWLockExclusive(&g_serve_lk); }
static void serve_unlock(void) { ReleaseSRWLockExclusive(&g_serve_lk); }
#else
static pthread_mutex_t g_serve_mx = PTHREAD_MUTEX_INITIALIZER;
static void serve_lock(void) { pthread_mutex_lock(&g_serve_mx); }
static void serve_unlock(void) { pthread_mutex_unlock(&g_serve_mx); }
#endif
void vkr_set_token(const char *hex) { g_token_ok = vr_token_parse(hex, g_token); }
static volatile int g_thermal; // iPhone の熱の状態（0 ふつう・1 やや熱い・2 熱い・3 とても熱い）。アプリが知らせる
void vkr_set_thermal(int state) { g_thermal = state; }

static double t_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
#endif
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD) ms);
#else
    usleep((useconds_t) ms * 1000);
#endif
}

// 1 つの接続の間に、どこに時間がかかったか
static struct { double wait_ms, record_ms, submit_ms, paused_ms; uint64_t waits, records, submits; uint64_t wait_by_op[64]; } g_st;
static uint32_t g_cur_op; // いま処理している命令（どの命令で GPU を待ったかを数える）
// アプリの画面のファンを回すための数。アプリが少しずつ読んで、増えた速さで回す
static volatile uint64_t g_ops_total;
static volatile int g_clients;
uint64_t vkr_ops_total(void) { return g_ops_total; }
int vkr_clients(void) { return g_clients; }

// command buffer ごとに、どの command pool の物か・記録した中でどの descriptor set を使うか
typedef struct { uint64_t cb, pool; uint64_t *sets; uint32_t n, cap; idlist *mems; } cbinfo;
static cbinfo *g_cbi;
static size_t g_ncbi, g_capcbi;

// ---- 調べ用（VKR_SYNC=1）：submit ごとに GPU を待ち、GPU を失ったら、その submit で流した命令を書き出す ----
static int g_dbg = -1;
static int dbg_on(void) {
    if (g_dbg < 0) { const char *e = getenv("VKR_SYNC"); g_dbg = e && *e == '1'; }
    return g_dbg;
}
typedef struct { uint64_t cb; vr_buf t; } dbg_trace;
static dbg_trace *g_tr;
static size_t g_ntr;
static vr_buf *trace_of(uint64_t cb) {
    for (size_t i = 0; i < g_ntr; i++) if (g_tr[i].cb == cb) return &g_tr[i].t;
    g_tr = realloc(g_tr, (g_ntr + 1) * sizeof *g_tr);
    g_tr[g_ntr] = (dbg_trace) { cb, { 0 } };
    return &g_tr[g_ntr++].t;
}
static void tracef(cbinfo *ci, const char *fmt, ...) {
    if (!ci || !dbg_on()) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int) sizeof line) n = sizeof line - 1;
    vr_buf *t = trace_of(ci->cb);
    if (t->n < (4u << 20)) { vr_put(t, line, (size_t) n); vr_put(t, "\n", 1); }
}
// descriptor set ごとに、最後に書いた中身（何番に・どのバッファの・どこから・どれだけ）
typedef struct { uint64_t set; vr_buf t; } dbg_set;
static dbg_set *g_dset;
static size_t g_ndset;
static vr_buf *dset_of(uint64_t set) {
    for (size_t i = 0; i < g_ndset; i++) if (g_dset[i].set == set) return &g_dset[i].t;
    g_dset = realloc(g_dset, (g_ndset + 1) * sizeof *g_dset);
    g_dset[g_ndset] = (dbg_set) { set, { 0 } };
    return &g_dset[g_ndset++].t;
}

static cbinfo *cbi(uint64_t cb, int create) {
    for (size_t i = 0; i < g_ncbi; i++) if (g_cbi[i].cb == cb) return &g_cbi[i];
    if (!create) return NULL;
    if (g_ncbi == g_capcbi) { g_capcbi = g_capcbi ? g_capcbi * 2 : 64; g_cbi = realloc(g_cbi, g_capcbi * sizeof *g_cbi); }
    g_cbi[g_ncbi] = (cbinfo) { cb, 0, NULL, 0, 0, NULL };
    return &g_cbi[g_ncbi++];
}

static void cbi_add_set(cbinfo *c, uint64_t set) {
    for (uint32_t i = 0; i < c->n; i++) if (c->sets[i] == set) return;
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 16; c->sets = realloc(c->sets, c->cap * sizeof *c->sets); }
    c->sets[c->n++] = set;
}
static VkFence g_fpool[64]; // 使い回す自前の fence
static int g_nfpool;

static VkFence own_fence(void) {
    if (g_nfpool) return g_fpool[--g_nfpool];
    VkFenceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f = VK_NULL_HANDLE;
    vkCreateFence(g_dev, &ci, NULL, &f);
    return f;
}

// k 番目までに出した仕事が終わるのを待って、一覧から外す
static void drain_upto(int k) {
    if (k < 0 || !g_nfl) return;
    if (!g_dev) { g_nfl = 0; return; }
    if (k >= g_nfl) k = g_nfl - 1;
    VkFence fs[256];
    for (int i = 0; i <= k; i++) fs[i] = g_fl[i].f;
    double t0 = t_ms();
    VkResult r = vkWaitForFences(g_dev, (uint32_t) (k + 1), fs, VK_TRUE, UINT64_MAX);
    g_st.wait_ms += t_ms() - t0;
    g_st.waits++;
    if (g_cur_op < 64) g_st.wait_by_op[g_cur_op]++;
    if (r != VK_SUCCESS) {
        vlog("出した仕事を待てない (%d)", r);
        if (r < 0) g_err = r;
    }
    for (int i = 0; i <= k; i++) { free(g_fl[i].snap); g_fl[i].snap = NULL; }
    for (int i = 0; i <= k; i++) {
        if (!g_fl[i].own) continue;
        vkResetFences(g_dev, 1, &g_fl[i].f);
        if (g_nfpool < 64) g_fpool[g_nfpool++] = g_fl[i].f;
        else vkDestroyFence(g_dev, g_fl[i].f, NULL);
    }
    memmove(g_fl, g_fl + k + 1, (size_t) (g_nfl - k - 1) * sizeof *g_fl);
    g_nfl -= k + 1;
}

static void drain(void) { drain_upto(g_nfl - 1); }

// その物を使う仕事のうち、いちばん後に出したものの番号（無ければ -1）
static int last_with_cb(uint64_t cb) {
    for (int i = g_nfl - 1; i >= 0; i--)
        for (uint32_t k = 0; k < g_fl[i].ncb; k++) if (g_fl[i].cbs[k] == cb) return i;
    return -1;
}

static int last_with_pool(uint64_t pool) {
    for (int i = g_nfl - 1; i >= 0; i--)
        for (uint32_t k = 0; k < g_fl[i].ncb; k++) {
            cbinfo *c = cbi(g_fl[i].cbs[k], 0);
            if (c && c->pool == pool) return i;
        }
    return -1;
}

// その descriptor pool から取った set を、まだ走っている仕事が使っているか（set の aux は pool の id）
static int last_with_dpool(uint64_t pool) {
    for (int i = g_nfl - 1; i >= 0; i--)
        for (uint32_t k = 0; k < g_fl[i].ncb; k++) {
            cbinfo *c = cbi(g_fl[i].cbs[k], 0);
            if (!c) continue;
            for (uint32_t j = 0; j < c->n; j++) {
                ent *e = find(c->sets[j]);
                if (e && e->kind == K_DSET && e->aux == pool) return i;
            }
        }
    return -1;
}

// pool を空にしたり消したりしたら、そこから取った set の表の行も消す
static void drop_sets_of_pool(uint64_t pool) {
    size_t n = 0, cap = 0;
    uint64_t *ids = NULL;
    for (size_t i = 0; i < g_cap; i++)
        if (g_tab[i].id && g_tab[i].kind == K_DSET && g_tab[i].aux == pool) {
            if (n == cap) { cap = cap ? cap * 2 : 64; ids = realloc(ids, cap * sizeof *ids); }
            ids[n++] = g_tab[i].id;
        }
    for (size_t i = 0; i < n; i++) {
        ent *e = find(ids[i]);
        if (e) idlist_free(&e->list);
        del(ids[i]);
    }
    free(ids);
}

static int last_with_set(uint64_t set) {
    for (int i = g_nfl - 1; i >= 0; i--)
        for (uint32_t k = 0; k < g_fl[i].ncb; k++) {
            cbinfo *c = cbi(g_fl[i].cbs[k], 0);
            if (!c) continue;
            for (uint32_t j = 0; j < c->n; j++) if (c->sets[j] == set) return i;
        }
    return -1;
}

static int last_with_mem(uint64_t mem) {
    for (int i = g_nfl - 1; i >= 0; i--)
        for (uint32_t k = 0; k < g_fl[i].ncb; k++) {
            cbinfo *c = cbi(g_fl[i].cbs[k], 0);
            if (!c || !c->mems) continue;
            for (uint32_t j = 0; j < c->mems->n; j++) if (c->mems->ids[j] == mem) return i;
        }
    return -1;
}

static void use_buffer_mem(cbinfo *ci, uint64_t mem) {
    if (ci && mem) idlist_add(&ci->mems, mem);
}

// バッファの番号 → それがつながっているメモリを、記録の「使うメモリ」に足す
static void use_buffer(cbinfo *ci, uint64_t buf) {
    if (!ci) return;
    ent *e = find(buf);
    if (e && e->aux) idlist_add(&ci->mems, e->aux);
}

static int last_with_fence(VkFence f) {
    for (int i = g_nfl - 1; i >= 0; i--) if (g_fl[i].f == f) return i;
    return -1;
}

// ---- submit を GPU の上で順に並べる鎖 ----
// ドライバは fence の待ちを PC で待たずに返す。アプリは「前の仕事は終わった」と思って次を出すので、
// 受け手で前の submit と次の submit が GPU の上で重なると、終わる前の答えを読んで GPU を失うことがある（whisper）。
// そこで受け手が、どの submit も「前の submit が終わってから始める」ように semaphore でつなぐ。
// 別のキューの間でも効く。VKR_CHAIN=1 で使う
static VkSemaphore g_chain[4];
static int g_chain_last = -1;
static int g_chain_on = -1;
static int chain_on(void) {
    if (g_chain_on < 0) { const char *e = getenv("VKR_CHAIN"); g_chain_on = e && *e == '1'; } // 標準では使わない（whisper の件には効かなかった）
    return g_chain_on;
}
static int chain_ready(void) {
    if (!g_dev) return 0;
    if (g_chain[0]) return 1;
    VkSemaphoreCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (int i = 0; i < 4; i++)
        if (vkCreateSemaphore(g_dev, &ci, NULL, &g_chain[i]) != VK_SUCCESS) {
            for (int k = 0; k < i; k++) vkDestroySemaphore(g_dev, g_chain[k], NULL);
            memset(g_chain, 0, sizeof g_chain);
            return 0;
        }
    g_chain_last = -1;
    return 1;
}

// ---- 1 つの接続の間に作った物を、切れたときに片づける ----
static void jp_reset(void);
static void cleanup(void) {
    drain();
    jp_reset();
    for (size_t i = 0; i < g_ncbi; i++) { free(g_cbi[i].sets); idlist_free(&g_cbi[i].mems); }
    g_ncbi = 0;
    if (g_err == VK_ERROR_DEVICE_LOST) g_lost = 1;
    g_err = VK_SUCCESS;
    if (g_dev) {
        for (int i = 0; i < g_nfpool; i++) vkDestroyFence(g_dev, g_fpool[i], NULL);
        for (int i = 0; i < 4; i++) if (g_chain[i]) vkDestroySemaphore(g_dev, g_chain[i], NULL);
    }
    memset(g_chain, 0, sizeof g_chain);
    g_chain_last = -1;
    g_nfpool = 0;
    g_dev = VK_NULL_HANDLE;
    for (int kind = K_PIPELINE; kind >= K_DEVICE; kind--) { (void) kind; }
    VkDevice dev = VK_NULL_HANDLE;
    for (size_t i = 0; i < g_cap; i++) if (g_tab[i].id && g_tab[i].kind == K_DEVICE) dev = (VkDevice) (uintptr_t) g_tab[i].h;
    if (dev) {
        vkDeviceWaitIdle(dev);
        static const int order[] = { K_PIPELINE, K_FB, K_RPASS, K_PLAYOUT, K_SHADER, K_DPOOL, K_DSL, K_SAMPLER, K_CPOOL, K_FENCE, K_SEM,
                                     K_QPOOL, K_VIEW, K_BVIEW, K_IMAGE, K_BUFFER, K_MEMORY };
        for (size_t o = 0; o < sizeof order / sizeof *order; o++)
            for (size_t i = 0; i < g_cap; i++) {
                ent *e = &g_tab[i];
                if (!e->id || e->kind != (uint32_t) order[o]) continue;
                switch (e->kind) {
                case K_PIPELINE: vkDestroyPipeline(dev, (VkPipeline) e->h, NULL); break;
                case K_PLAYOUT: vkDestroyPipelineLayout(dev, (VkPipelineLayout) e->h, NULL); break;
                case K_SHADER: vkDestroyShaderModule(dev, (VkShaderModule) e->h, NULL); break;
                case K_DPOOL: vkDestroyDescriptorPool(dev, (VkDescriptorPool) e->h, NULL); break;
                case K_DSL: vkDestroyDescriptorSetLayout(dev, (VkDescriptorSetLayout) e->h, NULL); break;
                case K_CPOOL: vkDestroyCommandPool(dev, (VkCommandPool) e->h, NULL); break;
                case K_FENCE: vkDestroyFence(dev, (VkFence) e->h, NULL); break;
                case K_SEM: vkDestroySemaphore(dev, (VkSemaphore) e->h, NULL); break;
                case K_BUFFER: vkDestroyBuffer(dev, (VkBuffer) e->h, NULL); break;
                case K_MEMORY: vkFreeMemory(dev, (VkDeviceMemory) e->h, NULL); break;
                case K_FB: vkDestroyFramebuffer(dev, (VkFramebuffer) e->h, NULL); break;
                case K_RPASS: vkDestroyRenderPass(dev, (VkRenderPass) e->h, NULL); break;
                case K_VIEW: vkDestroyImageView(dev, (VkImageView) e->h, NULL); break;
                case K_IMAGE: vkDestroyImage(dev, (VkImage) e->h, NULL); break;
                case K_SAMPLER: vkDestroySampler(dev, (VkSampler) e->h, NULL); break;
                case K_QPOOL: vkDestroyQueryPool(dev, (VkQueryPool) e->h, NULL); break;
                case K_BVIEW: vkDestroyBufferView(dev, (VkBufferView) e->h, NULL); break;
                }
            }
        if (g_dummy_vb) vkDestroyBuffer(dev, g_dummy_vb, NULL);
        if (g_dummy_mem) vkFreeMemory(dev, g_dummy_mem, NULL);
        if (g_dummy_smp) vkDestroySampler(dev, g_dummy_smp, NULL);
        vkDestroyDevice(dev, NULL);
    }
    g_dummy_vb = VK_NULL_HANDLE;
    g_dummy_mem = VK_NULL_HANDLE;
    g_dummy_smp = VK_NULL_HANDLE;
    for (size_t i = 0; i < g_cap; i++) if (g_tab[i].id) { idlist_free(&g_tab[i].list); free(g_tab[i].last); }
    free(g_tab);
    g_tab = NULL;
    g_cap = g_cnt = 0;
}

// 届いた文字列。終わりの 0 が無い物は空にする（strcmp やドライバが外を読まないように）
static const char *rd_str(vr_rd *r) {
    uint32_t l;
    const char *p = (const char *) vr_rbytes(r, &l);
    return p && l && p[l - 1] == 0 ? p : "";
}

// ---- 返事 ----
static vr_buf g_out;
static int reply(vr_sock s, int32_t result) {
    uint32_t h[2] = { (uint32_t) result, (uint32_t) g_out.n };
    int ok = vr_send_all(s, h, 8) && vr_send_all(s, g_out.p, g_out.n);
    g_out.n = 0;
    return ok;
}

// ---- 物の性質の鎖（pNext）を、届いた (sType, 大きさ) の並びから組み立てる ----
static void *build_chain(vr_rd *r, uint32_t count, void **blocks, uint32_t *sizes) {
    void *head = NULL, **link = &head;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t st = vr_ru32(r), sz = vr_ru32(r);
        if (sz < sizeof(VkBaseOutStructure)) sz = sizeof(VkBaseOutStructure);
        if (sz > (1u << 16)) sz = (1u << 16); // 申告が大きすぎるのは細工
        // ドライバは sType を見て構造体まるごと書く。申告 sz が小さくても溢れないよう、余裕をもって確保する
        uint32_t asz = sz < 4096u ? 4096u : sz;
        VkBaseOutStructure *b = calloc(1, asz);
        b->sType = (VkStructureType) st;
        blocks[i] = b;
        sizes[i] = sz;
        *link = b;
        link = (void **) &b->pNext;
    }
    return head;
}

// 画像（または見え方）の番号 → それがつながっているメモリを、記録の「使うメモリ」に足す
static void use_image(cbinfo *ci, uint64_t id) {
    if (!ci) return;
    ent *e = find(id);
    if (e) use_buffer_mem(ci, e->aux);
}

// dynamic rendering の描く先 1 つを読む。描く先と resolve 先の画像のメモリを「使う」として覚える
static void read_rendering_att(vr_rd *r, VkRenderingAttachmentInfo *a, cbinfo *ci) {
    *a = (VkRenderingAttachmentInfo) { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    uint64_t vid = vr_ru64(r);
    a->imageView = OBJ(VkImageView, vid);
    a->imageLayout = (VkImageLayout) vr_ru32(r);
    a->resolveMode = (VkResolveModeFlagBits) vr_ru32(r);
    uint64_t rid = vr_ru64(r);
    a->resolveImageView = OBJ(VkImageView, rid);
    a->resolveImageLayout = (VkImageLayout) vr_ru32(r);
    a->loadOp = (VkAttachmentLoadOp) vr_ru32(r);
    a->storeOp = (VkAttachmentStoreOp) vr_ru32(r);
    vr_rcopy(r, &a->clearValue, sizeof a->clearValue);
    use_image(ci, vid);
    use_image(ci, rid);
}

// 後から変える値（VkDynamicState の番号ごと）。小数はビットのまま届く
static void set_dynamic(VkCommandBuffer cb, uint32_t st, uint32_t n, const uint32_t *v) {
    float f[96];
    memcpy(f, v, (size_t) (n < 96 ? n : 96) * 4);
#define NEED(k) if (n < (k)) break
    switch (st) {
    case VK_DYNAMIC_STATE_LINE_WIDTH: NEED(1); vkCmdSetLineWidth(cb, f[0]); return;
    case VK_DYNAMIC_STATE_DEPTH_BIAS: NEED(3); vkCmdSetDepthBias(cb, f[0], f[1], f[2]); return;
    case VK_DYNAMIC_STATE_BLEND_CONSTANTS: NEED(4); vkCmdSetBlendConstants(cb, f); return;
    case VK_DYNAMIC_STATE_DEPTH_BOUNDS: NEED(2); vkCmdSetDepthBounds(cb, f[0], f[1]); return;
    case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: NEED(2); vkCmdSetStencilCompareMask(cb, v[0], v[1]); return;
    case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: NEED(2); vkCmdSetStencilWriteMask(cb, v[0], v[1]); return;
    case VK_DYNAMIC_STATE_STENCIL_REFERENCE: NEED(2); vkCmdSetStencilReference(cb, v[0], v[1]); return;
    default: break;
    }
    if (!(g_caps & VR_CAP_EXT_DYNAMIC_STATE)) { vlog("この値（%u）は変えられない", st); return; }
    switch (st) {
    case VK_DYNAMIC_STATE_CULL_MODE: NEED(1); vkCmdSetCullMode(cb, v[0]); return;
    case VK_DYNAMIC_STATE_FRONT_FACE: NEED(1); vkCmdSetFrontFace(cb, (VkFrontFace) v[0]); return;
    case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY: NEED(1); vkCmdSetPrimitiveTopology(cb, (VkPrimitiveTopology) v[0]); return;
    case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT: vkCmdSetViewportWithCount(cb, (n < 96 ? n : 96) / 6, (const VkViewport *) f); return;
    case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT: vkCmdSetScissorWithCount(cb, (n < 96 ? n : 96) / 4, (const VkRect2D *) v); return;
    case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE: NEED(1); vkCmdSetDepthTestEnable(cb, v[0]); return;
    case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE: NEED(1); vkCmdSetDepthWriteEnable(cb, v[0]); return;
    case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: NEED(1); vkCmdSetDepthCompareOp(cb, (VkCompareOp) v[0]); return;
    case VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE: NEED(1); vkCmdSetDepthBoundsTestEnable(cb, v[0]); return;
    case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE: NEED(1); vkCmdSetStencilTestEnable(cb, v[0]); return;
    case VK_DYNAMIC_STATE_STENCIL_OP: NEED(5); vkCmdSetStencilOp(cb, v[0], (VkStencilOp) v[1], (VkStencilOp) v[2], (VkStencilOp) v[3], (VkCompareOp) v[4]); return;
    case VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE: NEED(1); vkCmdSetRasterizerDiscardEnable(cb, v[0]); return;
    case VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE: NEED(1); vkCmdSetDepthBiasEnable(cb, v[0]); return;
    case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE: NEED(1); vkCmdSetPrimitiveRestartEnable(cb, v[0]); return;
    default: break;
    }
#undef NEED
    vlog("知らない値の変え方 %u（値 %u 個）", st, n);
}

// ---- command buffer に積まれた命令を実行する ----
static void replay(VkCommandBuffer cb, vr_rd *r, cbinfo *ci) {
    // 有効な描き方・計算の仕方がつながっていない draw と dispatch は飛ばす
    // （作れなかった pipeline のまま描くと、iPhone の MoltenVK は描く所で落ちる。10/7 に zink で起きた）
    int gfx = 0, cmp = 0;
    uint32_t skipped = 0;
    while (r->at < r->n && !r->bad) {
        uint32_t op = vr_ru32(r);
        switch (op) {
        case VR_CMD_BIND_PIPELINE: {
            uint32_t bp = vr_ru32(r);
            uint64_t pid = vr_ru64(r);
            VkPipeline pl = OBJ(VkPipeline, pid);
            tracef(ci, "  pipeline %llu%s", (unsigned long long) pid, pl ? "" : "（無い）");
            if (pl) vkCmdBindPipeline(cb, (VkPipelineBindPoint) bp, pl);
            if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) gfx = pl != VK_NULL_HANDLE;
            else if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) cmp = pl != VK_NULL_HANDLE;
        } break;
        case VR_CMD_BIND_DESCRIPTOR_SETS: {
            uint32_t bp = vr_ru32(r);
            VkPipelineLayout pl = OBJ(VkPipelineLayout, vr_ru64(r));
            uint32_t first = vr_ru32(r), n = vr_ru32(r);
            VkDescriptorSet sets[32];
            for (uint32_t i = 0; i < n && i < 32; i++) {
                uint64_t sid = vr_ru64(r);
                sets[i] = OBJ(VkDescriptorSet, sid);
                if (dbg_on() && ci) {
                    vr_buf *d = dset_of(sid);
                    tracef(ci, "  set %llu%s：%.*s", (unsigned long long) sid, sets[i] ? "" : "（無い）", (int) d->n, d->p ? (const char *) d->p : "");
                }
                if (ci) {
                    cbi_add_set(ci, sid);
                    ent *se = find(sid);
                    if (se && se->list) for (uint32_t k = 0; k < se->list->n; k++) use_buffer(ci, se->list->ids[k]);
                }
            }
            uint32_t nd = vr_ru32(r);
            uint32_t dyn[64];
            for (uint32_t i = 0; i < nd && i < 64; i++) dyn[i] = vr_ru32(r);
            vkCmdBindDescriptorSets(cb, (VkPipelineBindPoint) bp, pl, first, n < 32 ? n : 32, sets, nd < 64 ? nd : 64, dyn);
        } break;
        case VR_CMD_PUSH_CONSTANTS: {
            VkPipelineLayout pl = OBJ(VkPipelineLayout, vr_ru64(r));
            uint32_t st = vr_ru32(r), off = vr_ru32(r), sz;
            const void *d = vr_rbytes(r, &sz);
            if (dbg_on() && d) {
                char h[200] = "";
                for (uint32_t i = 0; i < sz / 4 && i < 24; i++) snprintf(h + strlen(h), sizeof h - strlen(h), " %u", ((const uint32_t *) d)[i]);
                tracef(ci, "  push %u バイト:%s", sz, h);
            }
            if (d) vkCmdPushConstants(cb, pl, st, off, sz, d);
        } break;
        case VR_CMD_DISPATCH: {
            uint32_t x = vr_ru32(r), y = vr_ru32(r), z = vr_ru32(r);
            tracef(ci, "  dispatch %u %u %u", x, y, z);
            if (!cmp) { skipped++; break; }
            vkCmdDispatch(cb, x, y, z);
        } break;
        case VR_CMD_DISPATCH_BASE: {
            uint32_t a = vr_ru32(r), b = vr_ru32(r), c = vr_ru32(r), x = vr_ru32(r), y = vr_ru32(r), z = vr_ru32(r);
            if (!cmp) { skipped++; break; }
            vkCmdDispatchBase(cb, a, b, c, x, y, z);
        } break;
        case VR_CMD_DISPATCH_INDIRECT: {
            uint64_t bid = vr_ru64(r);
            use_buffer(ci, bid);
            VkBuffer b = OBJ(VkBuffer, bid);
            uint64_t off = vr_ru64(r);
            if (!cmp || !b) { skipped++; break; }
            vkCmdDispatchIndirect(cb, b, off);
        } break;
        case VR_CMD_COPY_BUFFER: {
            uint64_t sid = vr_ru64(r), did = vr_ru64(r);
            use_buffer(ci, sid);
            use_buffer(ci, did);
            VkBuffer src = OBJ(VkBuffer, sid), dst = OBJ(VkBuffer, did);
            uint32_t n = vr_ru32(r);
            VkBufferCopy *rg = malloc(sizeof *rg * (n ? n : 1));
            for (uint32_t i = 0; i < n; i++) { rg[i].srcOffset = vr_ru64(r); rg[i].dstOffset = vr_ru64(r); rg[i].size = vr_ru64(r); }
            if (dbg_on()) {
                ent *se = find(sid), *de = find(did);
                for (uint32_t i = 0; i < n && i < 4; i++)
                    tracef(ci, "  copy buffer %llu（%llu バイト）+%llu → %llu（%llu バイト）+%llu・%llu バイト", (unsigned long long) sid,
                           (unsigned long long) (se ? se->size : 0), (unsigned long long) rg[i].srcOffset, (unsigned long long) did,
                           (unsigned long long) (de ? de->size : 0), (unsigned long long) rg[i].dstOffset, (unsigned long long) rg[i].size);
            }
            vkCmdCopyBuffer(cb, src, dst, n, rg);
            free(rg);
        } break;
        case VR_CMD_FILL_BUFFER: {
            uint64_t bid = vr_ru64(r);
            use_buffer(ci, bid);
            VkBuffer b = OBJ(VkBuffer, bid);
            uint64_t off = vr_ru64(r), sz = vr_ru64(r);
            tracef(ci, "  fill buffer %llu +%llu・%llu バイト", (unsigned long long) bid, (unsigned long long) off, (unsigned long long) sz);
            vkCmdFillBuffer(cb, b, off, sz, vr_ru32(r));
        } break;
        case VR_CMD_UPDATE_BUFFER: {
            uint64_t bid = vr_ru64(r);
            use_buffer(ci, bid);
            VkBuffer b = OBJ(VkBuffer, bid);
            uint64_t off = vr_ru64(r);
            uint32_t sz;
            const void *d = vr_rbytes(r, &sz);
            if (d) vkCmdUpdateBuffer(cb, b, off, sz, d);
        } break;
        case VR_CMD_PIPELINE_BARRIER: {
            uint32_t src = vr_ru32(r), dst = vr_ru32(r), dep = vr_ru32(r), nm = vr_ru32(r);
            VkMemoryBarrier mb[16];
            for (uint32_t i = 0; i < nm && i < 16; i++) mb[i] = (VkMemoryBarrier) { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, vr_ru32(r), vr_ru32(r) };
            uint32_t nb = vr_ru32(r);
            if (nb > r->n - r->at) { r->bad = 1; break; }
            VkBufferMemoryBarrier *bb = calloc(nb ? nb : 1, sizeof *bb);
            for (uint32_t i = 0; i < nb; i++) {
                bb[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                bb[i].srcAccessMask = vr_ru32(r);
                bb[i].dstAccessMask = vr_ru32(r);
                bb[i].srcQueueFamilyIndex = vr_ru32(r);
                bb[i].dstQueueFamilyIndex = vr_ru32(r);
                bb[i].buffer = OBJ(VkBuffer, vr_ru64(r));
                bb[i].offset = vr_ru64(r);
                bb[i].size = vr_ru64(r);
            }
            vkCmdPipelineBarrier(cb, src, dst, dep, nm < 16 ? nm : 16, mb, nb, bb, 0, NULL);
            free(bb);
        } break;
        case VR_CMD_PIPELINE_BARRIER2: {
            uint32_t src = vr_ru32(r), dst = vr_ru32(r), dep = vr_ru32(r), nm = vr_ru32(r);
            VkMemoryBarrier mb[16];
            for (uint32_t i = 0; i < nm && i < 16; i++) mb[i] = (VkMemoryBarrier) { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, vr_ru32(r), vr_ru32(r) };
            uint32_t nb = vr_ru32(r);
            if (nb > r->n - r->at) { r->bad = 1; break; }
            VkBufferMemoryBarrier *bb = calloc(nb ? nb : 1, sizeof *bb);
            for (uint32_t i = 0; i < nb; i++) {
                bb[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                bb[i].srcAccessMask = vr_ru32(r);
                bb[i].dstAccessMask = vr_ru32(r);
                bb[i].srcQueueFamilyIndex = vr_ru32(r);
                bb[i].dstQueueFamilyIndex = vr_ru32(r);
                bb[i].buffer = OBJ(VkBuffer, vr_ru64(r));
                bb[i].offset = vr_ru64(r);
                bb[i].size = vr_ru64(r);
            }
            uint32_t ni = vr_ru32(r);
            if (ni > r->n - r->at) { free(bb); r->bad = 1; break; }
            VkImageMemoryBarrier *ib = calloc(ni ? ni : 1, sizeof *ib);
            for (uint32_t i = 0; i < ni; i++) {
                ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                ib[i].srcAccessMask = vr_ru32(r);
                ib[i].dstAccessMask = vr_ru32(r);
                ib[i].oldLayout = (VkImageLayout) vr_ru32(r);
                ib[i].newLayout = (VkImageLayout) vr_ru32(r);
                ib[i].srcQueueFamilyIndex = vr_ru32(r);
                ib[i].dstQueueFamilyIndex = vr_ru32(r);
                ib[i].image = OBJ(VkImage, vr_ru64(r));
                vr_rcopy(r, &ib[i].subresourceRange, sizeof ib[i].subresourceRange);
            }
            vkCmdPipelineBarrier(cb, src, dst, dep, nm < 16 ? nm : 16, mb, nb, bb, ni, ib);
            free(bb);
            free(ib);
        } break;
        case VR_CMD_BEGIN_RENDER_PASS: {
            VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            bi.renderPass = OBJ(VkRenderPass, vr_ru64(r));
            uint64_t fid = vr_ru64(r);
            bi.framebuffer = OBJ(VkFramebuffer, fid);
            vr_rcopy(r, &bi.renderArea, sizeof bi.renderArea);
            uint32_t nc = vr_ru32(r);
            VkClearValue cv[16];
            for (uint32_t i = 0; i < nc && i < 16; i++) vr_rcopy(r, &cv[i], sizeof cv[i]);
            for (uint32_t i = 16; i < nc; i++) vr_take(r, sizeof(VkClearValue));
            bi.clearValueCount = nc < 16 ? nc : 16;
            bi.pClearValues = cv;
            uint32_t contents = vr_ru32(r);
            // 描く先の画像のメモリを「使う」として覚える
            ent *fe = find(fid);
            if (ci && fe && fe->list)
                for (uint32_t k = 0; k < fe->list->n; k++) {
                    ent *ve = find(fe->list->ids[k]);
                    if (ve) use_buffer_mem(ci, ve->aux);
                }
            if (bi.renderPass && bi.framebuffer) vkCmdBeginRenderPass(cb, &bi, (VkSubpassContents) contents);
        } break;
        case VR_CMD_END_RENDER_PASS: vkCmdEndRenderPass(cb); break;
        case VR_CMD_NEXT_SUBPASS: vkCmdNextSubpass(cb, (VkSubpassContents) vr_ru32(r)); break;
        case VR_CMD_DRAW: {
            uint32_t a = vr_ru32(r), b = vr_ru32(r), c = vr_ru32(r), d = vr_ru32(r);
            if (!gfx) { skipped++; break; }
            vkCmdDraw(cb, a, b, c, d);
        } break;
        case VR_CMD_DRAW_INDEXED: {
            uint32_t a = vr_ru32(r), b = vr_ru32(r), c = vr_ru32(r);
            int32_t d = (int32_t) vr_ru32(r);
            uint32_t e = vr_ru32(r);
            if (!gfx) { skipped++; break; }
            vkCmdDrawIndexed(cb, a, b, c, d, e);
        } break;
        case VR_CMD_DRAW_INDIRECT:
        case VR_CMD_DRAW_INDEXED_INDIRECT: {
            uint64_t bid = vr_ru64(r);
            use_buffer(ci, bid);
            VkBuffer b = OBJ(VkBuffer, bid);
            uint64_t off = vr_ru64(r);
            uint32_t n = vr_ru32(r), stride = vr_ru32(r);
            if (!gfx || !b) { skipped++; break; }
            if (op == VR_CMD_DRAW_INDIRECT) vkCmdDrawIndirect(cb, b, off, n, stride);
            else vkCmdDrawIndexedIndirect(cb, b, off, n, stride);
        } break;
        case VR_CMD_BIND_VERTEX_BUFFERS: {
            uint32_t first = vr_ru32(r), n = vr_ru32(r);
            VkBuffer bs[32];
            VkDeviceSize offs[32];
            for (uint32_t i = 0; i < n; i++) {
                uint64_t bid = vr_ru64(r);
                uint64_t off = vr_ru64(r);
                use_buffer(ci, bid);
                if (i < 32) { bs[i] = OBJ(VkBuffer, bid); offs[i] = off; }
                if (i < 32 && !bid && !g_nulldesc_gpu) { bs[i] = dummy_vb(); offs[i] = 0; }
            }
            vkCmdBindVertexBuffers(cb, first, n < 32 ? n : 32, bs, offs);
        } break;
        case VR_CMD_BIND_INDEX_BUFFER: {
            uint64_t bid = vr_ru64(r);
            use_buffer(ci, bid);
            uint64_t off = vr_ru64(r);
            vkCmdBindIndexBuffer(cb, OBJ(VkBuffer, bid), off, (VkIndexType) vr_ru32(r));
        } break;
        case VR_CMD_SET_VIEWPORT: {
            uint32_t first = vr_ru32(r), n = vr_ru32(r);
            VkViewport v[16];
            for (uint32_t i = 0; i < n; i++) { if (i < 16) vr_rcopy(r, &v[i], sizeof v[i]); else vr_take(r, sizeof v[0]); }
            vkCmdSetViewport(cb, first, n < 16 ? n : 16, v);
        } break;
        case VR_CMD_SET_SCISSOR: {
            uint32_t first = vr_ru32(r), n = vr_ru32(r);
            VkRect2D v[16];
            for (uint32_t i = 0; i < n; i++) { if (i < 16) vr_rcopy(r, &v[i], sizeof v[i]); else vr_take(r, sizeof v[0]); }
            vkCmdSetScissor(cb, first, n < 16 ? n : 16, v);
        } break;
        case VR_CMD_COPY_IMAGE_TO_BUFFER:
        case VR_CMD_COPY_BUFFER_TO_IMAGE: {
            uint64_t a = vr_ru64(r), b = vr_ru64(r);
            uint32_t layout = vr_ru32(r), n = vr_ru32(r);
            if (n > r->n - r->at) { r->bad = 1; break; }
            VkBufferImageCopy *rg = calloc(n ? n : 1, sizeof *rg);
            for (uint32_t i = 0; i < n; i++) vr_rcopy(r, &rg[i], sizeof rg[i]);
            if (op == VR_CMD_COPY_IMAGE_TO_BUFFER) {
                // a = 画像・b = バッファ
                ent *ie = find(a);
                if (ie && ci) use_buffer_mem(ci, ie->aux);
                use_buffer(ci, b);
                vkCmdCopyImageToBuffer(cb, OBJ(VkImage, a), (VkImageLayout) layout, OBJ(VkBuffer, b), n, rg);
            } else {
                // a = バッファ・b = 画像
                ent *ie = find(b);
                if (ie && ci) use_buffer_mem(ci, ie->aux);
                use_buffer(ci, a);
                vkCmdCopyBufferToImage(cb, OBJ(VkBuffer, a), OBJ(VkImage, b), (VkImageLayout) layout, n, rg);
            }
            free(rg);
        } break;
        case VR_CMD_CLEAR_COLOR_IMAGE: {
            uint64_t iid = vr_ru64(r);
            uint32_t layout = vr_ru32(r);
            VkClearColorValue c;
            vr_rcopy(r, &c, sizeof c);
            uint32_t n = vr_ru32(r);
            VkImageSubresourceRange rg[16];
            for (uint32_t i = 0; i < n; i++) { if (i < 16) vr_rcopy(r, &rg[i], sizeof rg[i]); else vr_take(r, sizeof rg[0]); }
            ent *ie = find(iid);
            if (ie && ci) use_buffer_mem(ci, ie->aux);
            vkCmdClearColorImage(cb, OBJ(VkImage, iid), (VkImageLayout) layout, &c, n < 16 ? n : 16, rg);
        } break;
        case VR_CMD_COPY_IMAGE:
        case VR_CMD_BLIT_IMAGE: {
            uint64_t sid = vr_ru64(r);
            uint32_t sl = vr_ru32(r);
            uint64_t did = vr_ru64(r);
            uint32_t dl = vr_ru32(r), n = vr_ru32(r);
            if (n > r->n - r->at) { r->bad = 1; break; }
            ent *se = find(sid), *de = find(did);
            if (se && ci) use_buffer_mem(ci, se->aux);
            if (de && ci) use_buffer_mem(ci, de->aux);
            if (op == VR_CMD_COPY_IMAGE) {
                VkImageCopy *rg = calloc(n ? n : 1, sizeof *rg);
                for (uint32_t i = 0; i < n; i++) vr_rcopy(r, &rg[i], sizeof rg[i]);
                vkCmdCopyImage(cb, OBJ(VkImage, sid), (VkImageLayout) sl, OBJ(VkImage, did), (VkImageLayout) dl, n, rg);
                free(rg);
            } else {
                VkImageBlit *rg = calloc(n ? n : 1, sizeof *rg);
                for (uint32_t i = 0; i < n; i++) vr_rcopy(r, &rg[i], sizeof rg[i]);
                uint32_t filter = vr_ru32(r);
                vkCmdBlitImage(cb, OBJ(VkImage, sid), (VkImageLayout) sl, OBJ(VkImage, did), (VkImageLayout) dl, n, rg, (VkFilter) filter);
                free(rg);
            }
        } break;
        // ---- 1.3 に向けて足した命令 ----
        case VR_CMD_BEGIN_RENDERING: {
            VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.flags = vr_ru32(r);
            vr_rcopy(r, &ri.renderArea, sizeof ri.renderArea);
            ri.layerCount = vr_ru32(r);
            ri.viewMask = vr_ru32(r);
            uint32_t nc = vr_ru32(r);
            VkRenderingAttachmentInfo ca[16], da, sa, skip;
            for (uint32_t i = 0; i < nc && !r->bad; i++) read_rendering_att(r, i < 16 ? &ca[i] : &skip, ci);
            ri.colorAttachmentCount = nc < 16 ? nc : 16;
            ri.pColorAttachments = ca;
            if (vr_ru32(r)) { read_rendering_att(r, &da, ci); ri.pDepthAttachment = &da; }
            if (vr_ru32(r)) { read_rendering_att(r, &sa, ci); ri.pStencilAttachment = &sa; }
            tracef(ci, "  begin rendering 色 %u・depth %s・stencil %s", nc, ri.pDepthAttachment ? "あり" : "なし", ri.pStencilAttachment ? "あり" : "なし");
            if (!r->bad && vkCmdBeginRendering) vkCmdBeginRendering(cb, &ri);
        } break;
        case VR_CMD_END_RENDERING:
            if (vkCmdEndRendering) vkCmdEndRendering(cb);
            break;
        case VR_CMD_BARRIER_SYNC2: {
            VkDependencyInfo di = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
            di.dependencyFlags = vr_ru32(r);
            uint32_t nm = vr_ru32(r);
            if (nm > r->n - r->at) { r->bad = 1; break; }
            VkMemoryBarrier2 *mb = calloc(nm ? nm : 1, sizeof *mb);
            for (uint32_t i = 0; i < nm; i++) {
                mb[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                mb[i].srcStageMask = vr_ru64(r);
                mb[i].srcAccessMask = vr_ru64(r);
                mb[i].dstStageMask = vr_ru64(r);
                mb[i].dstAccessMask = vr_ru64(r);
            }
            uint32_t nb = vr_ru32(r);
            if (nb > r->n - r->at) { free(mb); r->bad = 1; break; }
            VkBufferMemoryBarrier2 *bb = calloc(nb ? nb : 1, sizeof *bb);
            for (uint32_t i = 0; i < nb; i++) {
                bb[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                bb[i].srcStageMask = vr_ru64(r);
                bb[i].srcAccessMask = vr_ru64(r);
                bb[i].dstStageMask = vr_ru64(r);
                bb[i].dstAccessMask = vr_ru64(r);
                bb[i].srcQueueFamilyIndex = vr_ru32(r);
                bb[i].dstQueueFamilyIndex = vr_ru32(r);
                bb[i].buffer = OBJ(VkBuffer, vr_ru64(r));
                bb[i].offset = vr_ru64(r);
                bb[i].size = vr_ru64(r);
            }
            uint32_t ni = vr_ru32(r);
            if (ni > r->n - r->at) { free(mb); free(bb); r->bad = 1; break; }
            VkImageMemoryBarrier2 *ib = calloc(ni ? ni : 1, sizeof *ib);
            for (uint32_t i = 0; i < ni; i++) {
                ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                ib[i].srcStageMask = vr_ru64(r);
                ib[i].srcAccessMask = vr_ru64(r);
                ib[i].dstStageMask = vr_ru64(r);
                ib[i].dstAccessMask = vr_ru64(r);
                ib[i].oldLayout = (VkImageLayout) vr_ru32(r);
                ib[i].newLayout = (VkImageLayout) vr_ru32(r);
                ib[i].srcQueueFamilyIndex = vr_ru32(r);
                ib[i].dstQueueFamilyIndex = vr_ru32(r);
                ib[i].image = OBJ(VkImage, vr_ru64(r));
                vr_rcopy(r, &ib[i].subresourceRange, sizeof ib[i].subresourceRange);
            }
            di.memoryBarrierCount = nm;
            di.pMemoryBarriers = mb;
            di.bufferMemoryBarrierCount = nb;
            di.pBufferMemoryBarriers = bb;
            di.imageMemoryBarrierCount = ni;
            di.pImageMemoryBarriers = ib;
            tracef(ci, "  barrier2 memory %u・buffer %u・image %u", nm, nb, ni);
            if (!r->bad && vkCmdPipelineBarrier2) vkCmdPipelineBarrier2(cb, &di);
            free(mb);
            free(bb);
            free(ib);
        } break;
        case VR_CMD_SET_DYNAMIC: {
            uint32_t st = vr_ru32(r), n = vr_ru32(r);
            uint32_t v[96];
            for (uint32_t i = 0; i < n && !r->bad; i++) { uint32_t x = vr_ru32(r); if (i < 96) v[i] = x; }
            if (!r->bad) set_dynamic(cb, st, n < 96 ? n : 96, v);
        } break;
        case VR_CMD_BIND_VERTEX_BUFFERS2: {
            uint32_t first = vr_ru32(r), n = vr_ru32(r), has_size = vr_ru32(r), has_stride = vr_ru32(r);
            VkBuffer bs[32];
            VkDeviceSize offs[32], sizes[32], strides[32];
            for (uint32_t i = 0; i < n && !r->bad; i++) {
                uint64_t bid = vr_ru64(r), off = vr_ru64(r), sz = vr_ru64(r), sd = vr_ru64(r);
                use_buffer(ci, bid);
                if (i < 32) { bs[i] = OBJ(VkBuffer, bid); offs[i] = off; sizes[i] = sz; strides[i] = sd; }
                if (i < 32 && !bid && !g_nulldesc_gpu) { bs[i] = dummy_vb(); offs[i] = 0; sizes[i] = VK_WHOLE_SIZE; }
            }
            if (!r->bad && vkCmdBindVertexBuffers2)
                vkCmdBindVertexBuffers2(cb, first, n < 32 ? n : 32, bs, offs, has_size ? sizes : NULL, has_stride ? strides : NULL);
        } break;
        case VR_CMD_CLEAR_ATTACHMENTS: {
            uint32_t na = vr_ru32(r);
            if (na > r->n - r->at) { r->bad = 1; break; }
            VkClearAttachment *ca = calloc(na ? na : 1, sizeof *ca);
            for (uint32_t i = 0; i < na; i++) vr_rcopy(r, &ca[i], sizeof ca[i]);
            uint32_t nr = vr_ru32(r);
            if (nr > r->n - r->at) { free(ca); r->bad = 1; break; }
            VkClearRect *rc = calloc(nr ? nr : 1, sizeof *rc);
            for (uint32_t i = 0; i < nr; i++) vr_rcopy(r, &rc[i], sizeof rc[i]);
            if (!r->bad) vkCmdClearAttachments(cb, na, ca, nr, rc);
            free(ca);
            free(rc);
        } break;
        case VR_CMD_CLEAR_DEPTH_STENCIL_IMAGE: {
            uint64_t iid = vr_ru64(r);
            uint32_t layout = vr_ru32(r);
            VkClearDepthStencilValue v;
            vr_rcopy(r, &v, sizeof v);
            uint32_t n = vr_ru32(r);
            VkImageSubresourceRange rg[16];
            for (uint32_t i = 0; i < n; i++) { if (i < 16) vr_rcopy(r, &rg[i], sizeof rg[i]); else vr_take(r, sizeof rg[0]); }
            use_image(ci, iid);
            if (!r->bad) vkCmdClearDepthStencilImage(cb, OBJ(VkImage, iid), (VkImageLayout) layout, &v, n < 16 ? n : 16, rg);
        } break;
        case VR_CMD_RESOLVE_IMAGE: {
            uint64_t sid = vr_ru64(r);
            uint32_t sl = vr_ru32(r);
            uint64_t did = vr_ru64(r);
            uint32_t dl = vr_ru32(r), n = vr_ru32(r);
            if (n > r->n - r->at) { r->bad = 1; break; }
            VkImageResolve *rg = calloc(n ? n : 1, sizeof *rg);
            for (uint32_t i = 0; i < n; i++) vr_rcopy(r, &rg[i], sizeof rg[i]);
            use_image(ci, sid);
            use_image(ci, did);
            if (!r->bad) vkCmdResolveImage(cb, OBJ(VkImage, sid), (VkImageLayout) sl, OBJ(VkImage, did), (VkImageLayout) dl, n, rg);
            free(rg);
        } break;
        case VR_CMD_RESET_QUERY_POOL: {
            VkQueryPool qp = OBJ(VkQueryPool, vr_ru64(r));
            uint32_t first = vr_ru32(r), n = vr_ru32(r);
            if (!r->bad && qp) vkCmdResetQueryPool(cb, qp, first, n);
        } break;
        case VR_CMD_BEGIN_QUERY: {
            VkQueryPool qp = OBJ(VkQueryPool, vr_ru64(r));
            uint32_t q = vr_ru32(r), flags = vr_ru32(r);
            if (!r->bad && qp) vkCmdBeginQuery(cb, qp, q, flags);
        } break;
        case VR_CMD_END_QUERY: {
            VkQueryPool qp = OBJ(VkQueryPool, vr_ru64(r));
            uint32_t q = vr_ru32(r);
            if (!r->bad && qp) vkCmdEndQuery(cb, qp, q);
        } break;
        case VR_CMD_WRITE_TIMESTAMP: {
            uint32_t stage = vr_ru32(r);
            VkQueryPool qp = OBJ(VkQueryPool, vr_ru64(r));
            uint32_t q = vr_ru32(r);
            if (!r->bad && qp) vkCmdWriteTimestamp(cb, (VkPipelineStageFlagBits) stage, qp, q);
        } break;
        case VR_CMD_COPY_QUERY_RESULTS: {
            VkQueryPool qp = OBJ(VkQueryPool, vr_ru64(r));
            uint32_t first = vr_ru32(r), n = vr_ru32(r);
            uint64_t bid = vr_ru64(r), off = vr_ru64(r), stride = vr_ru64(r);
            uint32_t flags = vr_ru32(r);
            use_buffer(ci, bid);
            if (!r->bad && qp) vkCmdCopyQueryPoolResults(cb, qp, first, n, OBJ(VkBuffer, bid), off, stride, flags);
        } break;
        default:
            vlog("知らない記録の命令 %u", op);
            r->bad = 1;
        }
    }
    if (skipped) vlog("描き方か計算の仕方がつながっていないので、draw・dispatch を %u 回飛ばした", skipped);
}

// ---- 窓に出すコマを JPEG に縮める（VR_READ_JPEG） ----
// px は 1 行 stride バイトの 32 ビットの画素。bgra なら B・G・R・A、違えば R・G・B・A の並び。成功なら out に JPEG を足す
#if defined(__APPLE__)
static int jpeg_encode(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, int q, vr_buf *out) {
    int ok = 0;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo bi = bgra ? (CGBitmapInfo) (kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst)
                           : (CGBitmapInfo) (kCGBitmapByteOrder32Big | kCGImageAlphaNoneSkipLast);
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, px, (size_t) stride * h, NULL);
    CGImageRef img = CGImageCreate(w, h, 8, 32, stride, cs, bi, dp, NULL, false, kCGRenderingIntentDefault);
    CFMutableDataRef data = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(data, CFSTR("public.jpeg"), 1, NULL);
    float qf = (float) q / 100.0f;
    CFNumberRef qn = CFNumberCreate(NULL, kCFNumberFloatType, &qf);
    const void *keys[] = { kCGImageDestinationLossyCompressionQuality };
    const void *vals[] = { qn };
    CFDictionaryRef props = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (img && dst) {
        CGImageDestinationAddImage(dst, img, props);
        if (CGImageDestinationFinalize(dst)) {
            vr_put(out, CFDataGetBytePtr(data), (size_t) CFDataGetLength(data));
            ok = 1;
        }
    }
    if (props) CFRelease(props);
    if (qn) CFRelease(qn);
    if (dst) CFRelease(dst);
    if (data) CFRelease(data);
    if (img) CGImageRelease(img);
    if (dp) CGDataProviderRelease(dp);
    if (cs) CGColorSpaceRelease(cs);
    return ok;
}
#elif defined(_WIN32)
static int jpeg_encode(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, int q, vr_buf *out) {
    static IWICImagingFactory *fac;
    if (!fac) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **) &fac))) return 0;
    }
    int ok = 0;
    IStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *bag = NULL;
    uint8_t *rgb = malloc((size_t) w * 3 * h);
    if (!rgb || FAILED(CreateStreamOnHGlobal(NULL, TRUE, &st))) goto done;
    if (FAILED(IWICImagingFactory_CreateEncoder(fac, &GUID_ContainerFormatJpeg, NULL, &enc))) goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(enc, st, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &fr, &bag))) goto done;
    PROPBAG2 opt = { 0 };
    opt.pstrName = (LPOLESTR) L"ImageQuality";
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = (float) q / 100.0f;
    IPropertyBag2_Write(bag, 1, &opt, &v);
    if (FAILED(IWICBitmapFrameEncode_Initialize(fr, bag))) goto done;
    IWICBitmapFrameEncode_SetSize(fr, w, h);
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    IWICBitmapFrameEncode_SetPixelFormat(fr, &pf);
    // GPU のメモリはキャッシュが効かないことがあるので、まとめて写してから並べ替える
    uint8_t *tmp = malloc((size_t) stride * h);
    if (!tmp) goto done;
    memcpy(tmp, px, (size_t) stride * h);
    px = tmp;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t) y * stride;
        uint8_t *d = rgb + (size_t) y * w * 3;
        for (uint32_t x = 0; x < w; x++, s += 4, d += 3) {
            if (bgra) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; } else { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
        }
    }
    free(tmp);
    if (FAILED(IWICBitmapFrameEncode_WritePixels(fr, h, w * 3, w * 3 * h, rgb))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(fr)) || FAILED(IWICBitmapEncoder_Commit(enc))) goto done;
    HGLOBAL hg = NULL;
    if (FAILED(GetHGlobalFromStream(st, &hg))) goto done;
    STATSTG ss;
    if (FAILED(st->lpVtbl->Stat(st, &ss, STATFLAG_NONAME))) goto done;
    void *p = GlobalLock(hg);
    if (p) { vr_put(out, p, (size_t) ss.cbSize.QuadPart); GlobalUnlock(hg); ok = 1; }
done:
    if (bag) IPropertyBag2_Release(bag);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) st->lpVtbl->Release(st);
    free(rgb);
    return ok;
}
#endif

// ---- 空の頂点バッファの代わり ----
// 空の descriptor を持たない GPU（MoltenVK）に、空の頂点バッファを渡すと落ちる。0 で埋めた 1 MiB のバッファに差し替える
static VkBuffer dummy_vb(void) {
    if (g_dummy_vb || !g_dev) return g_dummy_vb;
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1u << 20,
                              .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT };
    VkBuffer b;
    if (vkCreateBuffer(g_dev, &bc, NULL, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, b, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t t = 0;
    while (t < mp.memoryTypeCount && !((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)) t++;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = t };
    VkDeviceMemory m;
    if (t == mp.memoryTypeCount || vkAllocateMemory(g_dev, &ai, NULL, &m) != VK_SUCCESS) { vkDestroyBuffer(g_dev, b, NULL); return VK_NULL_HANDLE; }
    void *p = NULL;
    if (vkMapMemory(g_dev, m, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS && p) { memset(p, 0, (size_t) req.size); vkUnmapMemory(g_dev, m); }
    vkBindBufferMemory(g_dev, b, m, 0);
    g_dummy_vb = b;
    g_dummy_mem = m;
    vlog("空の頂点バッファの代わりを作った");
    return b;
}

// ---- 先送りの JPEG（VR_READ_JPEG_PIPE） ----
// 今のコマを写して別の糸で縮め始め、返事にはひとつ前のコマの JPEG を入れる。
// 縮めている間に、受け手は次のコマの仕事を進められる（窓に出るのは 1 コマ遅れる）
#if defined(__APPLE__) || defined(_WIN32)
static struct {
    uint8_t *px;
    size_t cap;
    uint32_t w, h, stride;
    int bgra, q, ok, busy;
    double ms;
    vr_buf out;
#ifdef _WIN32
    HANDLE thr;
#else
    pthread_t thr;
#endif
} g_jp;

static void jp_encode(void) {
    double t0 = t_ms();
    g_jp.out.n = 0;
    g_jp.ok = jpeg_encode(g_jp.px, g_jp.w, g_jp.h, g_jp.stride, g_jp.bgra, g_jp.q, &g_jp.out);
    g_jp.ms = t_ms() - t0;
}

#ifdef _WIN32
static DWORD WINAPI jp_thread(LPVOID a) {
    (void) a;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    jp_encode();
    return 0;
}
#else
static void *jp_thread(void *a) {
    (void) a;
    jp_encode();
    return NULL;
}
#endif

// 縮めている途中なら、終わるまで待つ
static void jp_join(void) {
    if (!g_jp.busy) return;
#ifdef _WIN32
    WaitForSingleObject(g_jp.thr, INFINITE);
    CloseHandle(g_jp.thr);
#else
    pthread_join(g_jp.thr, NULL);
#endif
    g_jp.busy = 0;
}

// 接続が切れたとき：縮めている途中なら待ち、次につないだ人に前の人のコマを見せない
static void jp_reset(void) {
    jp_join();
    g_jp.ok = 0;
}

static int jp_start(const uint8_t *src) {
    size_t need = (size_t) g_jp.stride * g_jp.h;
    if (g_jp.cap < need) {
        free(g_jp.px);
        g_jp.px = malloc(need);
        g_jp.cap = g_jp.px ? need : 0;
        if (!g_jp.px) return 0;
    }
    memcpy(g_jp.px, src, need); // GPU のメモリから、まとめて写す
    g_jp.ok = 0;
#ifdef _WIN32
    g_jp.thr = CreateThread(NULL, 0, jp_thread, NULL, 0, NULL);
    g_jp.busy = g_jp.thr != NULL;
#else
    g_jp.busy = pthread_create(&g_jp.thr, NULL, jp_thread, NULL) == 0;
#endif
    if (!g_jp.busy) jp_encode(); // 糸を作れなければ、その場で縮める
    return 1;
}
#else
static void jp_reset(void) {}
#endif

// ---- 1 つの命令を実行する。返事が要るものは reply する ----
static int handle(vr_sock s, uint32_t op, vr_rd *r) {
    g_cur_op = op;
    g_ops_total++;
    // 調べ用：VKR_DRAIN=命令の番号,… なら、その命令の前は出した仕事を全部待つ
    static int drain_ops_ready;
    static unsigned char drain_ops[256];
    if (!drain_ops_ready) {
        drain_ops_ready = 1;
        const char *e = getenv("VKR_DRAIN");
        for (const char *q = e; q && *q; q++) {
            int n = atoi(q);
            if (n > 0 && n < 256) drain_ops[n] = 1;
            while (*q && *q != ',') q++;
            if (!*q) break;
        }
    }
    if (op < 256 && drain_ops[op]) drain();
    // 調べ用：VKR_SLOW_OP=命令の番号:ミリ秒 なら、その命令の前で待つ（GPU の仕事が長いときのドライバの動きを試す）
    static int slow_op = -1, slow_ms;
    if (slow_op < 0) {
        const char *e = getenv("VKR_SLOW_OP");
        slow_op = e ? atoi(e) : 0;
        slow_ms = e && strchr(e, ':') ? atoi(strchr(e, ':') + 1) : 0;
    }
    if (slow_op && (int) op == slow_op && slow_ms > 0) sleep_ms(slow_ms);
    // 出した仕事が使っているかもしれない物を変える・調べる命令の前に、出した仕事が全部終わるのを待つ
    switch (op) {
    // （command buffer の記録し直し・command pool の reset・descriptor の書き換え・fence の reset は、
    //   それを使う仕事までしか待たない。それぞれの命令の中で待つ）
    case VR_GET_FENCE_STATUS: case VR_GET_SEMAPHORE_COUNTER: case VR_FREE_COMMAND_BUFFERS:
    case VR_DESTROY_COMMAND_POOL: case VR_RESET_DESCRIPTOR_POOL: case VR_FREE_DESCRIPTOR_SETS:
    case VR_DESTROY_DESCRIPTOR_POOL: case VR_FREE_MEMORY: case VR_DESTROY_BUFFER: case VR_DESTROY_PIPELINE:
    case VR_DESTROY_PIPELINE_LAYOUT: case VR_DESTROY_SHADER_MODULE: case VR_DESTROY_DSL: case VR_DESTROY_FENCE:
    case VR_DESTROY_SEMAPHORE: case VR_DESTROY_DEVICE: case VR_QUEUE_WAIT_IDLE: case VR_DEVICE_WAIT_IDLE:
    case VR_DESTROY_IMAGE: case VR_DESTROY_IMAGE_VIEW: case VR_DESTROY_RENDER_PASS: case VR_DESTROY_FRAMEBUFFER:
    case VR_DESTROY_SAMPLER: case VR_DESTROY_BUFFER_VIEW:
    // query の結果は、出した仕事が全部終わってから読む（ドライバは fence を待たずに「終わった」と答えているので）
    case VR_DESTROY_QUERY_POOL: case VR_GET_QUERY_POOL_RESULTS: case VR_RESET_QUERY_POOL:
        drain();
        break;
    default:
        break;
    }
    // GPU を失っていたら、返事のある命令でそれを伝える（黙って間違った答えを返さない）
    if (g_err != VK_SUCCESS) {
        switch (op) {
        case VR_MEM_READ: case VR_MEM_READ_DELTA: case VR_READ_JPEG: case VR_READ_JPEG_PIPE: case VR_WAIT_FOR_FENCES: case VR_GET_FENCE_STATUS: case VR_WAIT_SEMAPHORES:
        case VR_GET_SEMAPHORE_COUNTER: case VR_QUEUE_WAIT_IDLE: case VR_DEVICE_WAIT_IDLE: case VR_GET_QUERY_POOL_RESULTS:
            return reply(s, g_err);
        default:
            break;
        }
    }
    switch (op) {
    case VR_PING: return reply(s, 0);
    case VR_HELLO: {
        uint32_t ver = vr_ru32(r);
        if (ver != VR_PROTO_VERSION) return reply(s, VK_ERROR_INCOMPATIBLE_DRIVER);
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceFeatures f;
        VkPhysicalDeviceMemoryProperties m;
        vkGetPhysicalDeviceProperties(g_pd, &p);
        vkGetPhysicalDeviceFeatures(g_pd, &f);
        like_ios_mask(&f);
        vkGetPhysicalDeviceMemoryProperties(g_pd, &m);
#if defined(__APPLE__) && TARGET_OS_IPHONE
        // iPhone では、iOS がこのアプリに許すメモリのほうが、MoltenVK の答える量よりずっと小さい（超えるとアプリが止められる）。
        // いま使ってよい量から余裕を引いた量に合わせて答える。llama.cpp などはこれを見て、載せる量や会話の長さを決める
        {
            size_t avail = os_proc_available_memory();
            uint64_t ok = avail > (768ull << 20) ? (uint64_t) avail - (768ull << 20) : 0;
            for (uint32_t i = 0; i < m.memoryHeapCount; i++)
                if (m.memoryHeaps[i].size > ok) m.memoryHeaps[i].size = ok;
            vlog("使ってよいメモリ %llu MB（iOS がいま許す %llu MB から余裕を引いた）", (unsigned long long) (ok >> 20), (unsigned long long) (avail >> 20));
        }
#endif
        vr_put(&g_out, &p, sizeof p);
        vr_put(&g_out, &f, sizeof f);
        vr_put(&g_out, &m, sizeof m);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
        vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, q);
        if (like_ios() && nq > 1) nq = 1;
        vr_u32(&g_out, nq);
        vr_put(&g_out, q, sizeof *q * nq);
        free(q);
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
        VkExtensionProperties *e = calloc(ne ? ne : 1, sizeof *e);
        vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, e);
        vr_u32(&g_out, ne);
        vr_put(&g_out, e, sizeof *e * ne);
        free(e);
        vr_u32(&g_out, g_caps);
        return reply(s, 0);
    }
    case VR_GET_FEATURES2:
    case VR_GET_PROPERTIES2: {
        uint32_t n = vr_ru32(r);
        if (n > 64) n = 64;
        void *blocks[64];
        uint32_t sizes[64];
        void *chain = build_chain(r, n, blocks, sizes);
        if (op == VR_GET_FEATURES2) {
            VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = chain };
            vkGetPhysicalDeviceFeatures2(g_pd, &f2);
            like_ios_mask(&f2.features);
            vr_put(&g_out, &f2.features, sizeof f2.features);
        } else {
            VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = chain };
            vkGetPhysicalDeviceProperties2(g_pd, &p2);
            vr_put(&g_out, &p2.properties, sizeof p2.properties);
        }
        for (uint32_t i = 0; i < n; i++) {
            vr_put(&g_out, (uint8_t *) blocks[i] + sizeof(VkBaseOutStructure), sizes[i] - sizeof(VkBaseOutStructure));
            free(blocks[i]);
        }
        return reply(s, 0);
    }
    case VR_GET_FORMAT_PROPS: {
        VkFormat fmt = (VkFormat) vr_ru32(r);
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_pd, fmt, &fp);
        if (like_ios_no_format(fmt)) memset(&fp, 0, sizeof fp);
        if (like_ios_no_vertex(fmt)) fp.bufferFeatures &= ~(VkFormatFeatureFlags) VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        vr_put(&g_out, &fp, sizeof fp);
        if (g_fmt3) {
            // 1.3 の 64 ビットの形（STORAGE_READ_WITHOUT_FORMAT など 1.0 の形に無い印がある）を後ろに付ける
            VkFormatProperties3 f3 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3 };
            VkFormatProperties2 f2 = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &f3 };
            vkGetPhysicalDeviceFormatProperties2(g_pd, fmt, &f2);
            if (like_ios_no_format(fmt)) f3.linearTilingFeatures = f3.optimalTilingFeatures = f3.bufferFeatures = 0;
            if (like_ios_no_vertex(fmt)) f3.bufferFeatures &= ~(VkFormatFeatureFlags2) VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
            vr_u64(&g_out, f3.linearTilingFeatures);
            vr_u64(&g_out, f3.optimalTilingFeatures);
            vr_u64(&g_out, f3.bufferFeatures);
        }
        return reply(s, 0);
    }
    case VR_CREATE_DEVICE: {
        uint64_t id = vr_ru64(r);
        uint32_t nq = vr_ru32(r);
        VkDeviceQueueCreateInfo qci[8];
        float prio[8][16];
        for (uint32_t i = 0; i < nq && i < 8; i++) {
            qci[i] = (VkDeviceQueueCreateInfo) { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            qci[i].queueFamilyIndex = vr_ru32(r);
            qci[i].queueCount = vr_ru32(r);
            for (uint32_t k = 0; k < qci[i].queueCount && k < 16; k++) prio[i][k] = 1.0f;
            qci[i].pQueuePriorities = prio[i];
        }
        uint32_t ne = vr_ru32(r);
        const char *names[64];
        uint32_t n_names = 0;
        for (uint32_t i = 0; i < ne && i < 63; i++) names[n_names++] = rd_str(r);
        if (g_portability) names[n_names++] = "VK_KHR_portability_subset";
        uint32_t has_f = vr_ru32(r);
        VkPhysicalDeviceFeatures feats;
        memset(&feats, 0, sizeof feats);
        if (has_f) vr_rcopy(r, &feats, sizeof feats);
        if (like_ios()) {
            VkPhysicalDeviceFeatures m = feats;
            like_ios_mask(&m);
            if (memcmp(&m, &feats, sizeof m)) {
                vlog("iPhone のふり：iPhone に無い機能を有効にしようとしたので断る");
                return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
            }
        }
        // 機能の鎖（Vulkan11Features など）は中身ごと届く
        uint32_t nc = vr_ru32(r);
        void *head = NULL, **link = &head, *blocks[32];
        for (uint32_t i = 0; i < nc && i < 32; i++) {
            uint32_t st = vr_ru32(r), sz;
            const void *d = vr_rbytes(r, &sz);
            VkBaseOutStructure *b = calloc(1, sz + sizeof(VkBaseOutStructure));
            b->sType = (VkStructureType) st;
            if (d) memcpy((uint8_t *) b + sizeof(VkBaseOutStructure), d, sz);
            blocks[i] = b;
            *link = b;
            link = (void **) &b->pNext;
        }
        VkPhysicalDeviceRobustness2FeaturesEXT r2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .nullDescriptor = VK_TRUE };
        if (g_nulldesc_gpu) {
            int has = 0;
            for (uint32_t i = 0; i < n_names; i++) if (!strcmp(names[i], "VK_EXT_robustness2")) has = 1;
            if (!has && n_names < 64) names[n_names++] = "VK_EXT_robustness2";
            *link = &r2;
        }
        VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = head, .queueCreateInfoCount = nq,
            .pQueueCreateInfos = qci, .enabledExtensionCount = n_names, .ppEnabledExtensionNames = names, .pEnabledFeatures = has_f ? &feats : NULL };
        VkDevice dev;
        VkResult res = vkCreateDevice(g_pd, &dci, NULL, &dev);
        for (uint32_t i = 0; i < nc && i < 32; i++) free(blocks[i]);
        if (res == VK_SUCCESS) { put(id, (uint64_t) (uintptr_t) dev, K_DEVICE); g_dev = dev; }
        return reply(s, res);
    }
    case VR_DESTROY_DEVICE: {
        uint64_t id = vr_ru64(r);
        (void) id;
        cleanup();
        return 1;
    }
    case VR_GET_QUEUE: {
        VkDevice d = DEV(vr_ru64(r));
        uint32_t fam = vr_ru32(r), idx = vr_ru32(r);
        VkQueue q;
        vkGetDeviceQueue(d, fam, idx, &q);
        put(vr_ru64(r), (uint64_t) (uintptr_t) q, K_QUEUE);
        return 1;
    }
    case VR_CREATE_BUFFER_NR:
    case VR_CREATE_BUFFER: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.flags = vr_ru32(r);
        bci.size = vr_ru64(r);
        bci.usage = vr_ru32(r);
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer b;
        VkResult res = vkCreateBuffer(d, &bci, NULL, &b);
        VkMemoryRequirements req;
        memset(&req, 0, sizeof req);
        if (res == VK_SUCCESS) {
            put(id, (uint64_t) b, K_BUFFER);
            vkGetBufferMemoryRequirements(d, b, &req);
        }
        if (op == VR_CREATE_BUFFER_NR) {
            if (res != VK_SUCCESS) vlog("buffer を作れない (%d)", res);
            return 1;
        }
        vr_put(&g_out, &req, sizeof req);
        return reply(s, res);
    }
    case VR_DESTROY_BUFFER: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroyBuffer(d, OBJ(VkBuffer, id), NULL);
        del(id);
        return 1;
    }
    case VR_ALLOC_MEMORY: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = vr_ru64(r);
        mai.memoryTypeIndex = vr_ru32(r);
        // 末尾の flags（VkMemoryAllocateFlagsInfo）。古いドライバは付けない
        VkMemoryAllocateFlagsInfo mfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
        if (!r->bad && r->at + 4 <= r->n) {
            mfi.flags = vr_ru32(r);
            if (mfi.flags) mai.pNext = &mfi;
        }
#if defined(__APPLE__) && TARGET_OS_IPHONE
        // iOS の上限を超えそうなら、アプリが止められる前に「足りない」と断る
        {
            size_t avail = os_proc_available_memory();
            if (mai.allocationSize + (256ull << 20) > (uint64_t) avail) {
                vlog("メモリ %llu MB は確保しない（iOS がいま許すのは %llu MB）", (unsigned long long) (mai.allocationSize >> 20), (unsigned long long) (avail >> 20));
                return reply(s, VK_ERROR_OUT_OF_DEVICE_MEMORY);
            }
        }
#endif
        VkDeviceMemory m;
        VkResult res = vkAllocateMemory(d, &mai, NULL, &m);
        if (res == VK_SUCCESS) {
            put(id, (uint64_t) m, K_MEMORY);
            find(id)->size = mai.allocationSize;
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
            if (mp.memoryTypes[mai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
                void *p = NULL;
                if (vkMapMemory(d, m, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) find(id)->map = p;
            }
        }
        return reply(s, res);
    }
    case VR_FREE_MEMORY: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkFreeMemory(d, OBJ(VkDeviceMemory, id), NULL);
        del(id);
        return 1;
    }
    case VR_BIND_BUFFER_MEMORY: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t bid = vr_ru64(r), mid = vr_ru64(r);
        VkBuffer b = OBJ(VkBuffer, bid);
        VkDeviceMemory m = OBJ(VkDeviceMemory, mid);
        ent *be = find(bid);
        if (be) be->aux = mid;
        vkBindBufferMemory(d, b, m, vr_ru64(r));
        return 1;
    }
    case VR_MEM_WRITE: {
        uint64_t mid = vr_ru64(r);
        drain_upto(last_with_mem(mid));
        ent *e = find(mid);
        uint64_t off = vr_ru64(r);
        uint32_t n;
        const void *d = vr_rbytes(r, &n);
        if (e && e->map && d && off <= e->size && n <= e->size - off) {
            memcpy((uint8_t *) e->map + off, d, n);
            if (e->last) memcpy(e->last + off, d, n);
        } else vlog("範囲の外への書き込みを捨てた（%llu + %u）", (unsigned long long) off, n);
        return 1;
    }
    case VR_MEM_READ: {
        uint64_t mid = vr_ru64(r);
        drain_upto(last_with_mem(mid));
        if (g_err != VK_SUCCESS) return reply(s, g_err);
        ent *e = find(mid);
        uint64_t off = vr_ru64(r), n = vr_ru64(r);
        if (!e || !e->map || off > e->size || n > e->size - off) return reply(s, VK_ERROR_MEMORY_MAP_FAILED);
        vr_put(&g_out, (uint8_t *) e->map + off, (size_t) n);
        return reply(s, 0);
    }
    case VR_READ_JPEG: {
        static double jt_wait, jt_enc;
        static uint64_t jn, jbytes;
        double jt0 = t_ms();
        uint64_t mid = vr_ru64(r);
        drain_upto(last_with_mem(mid));
        jt_wait += t_ms() - jt0;
        if (g_err != VK_SUCCESS) return reply(s, g_err);
        ent *e = find(mid);
        uint64_t off = vr_ru64(r);
        uint32_t w = vr_ru32(r), h = vr_ru32(r), stride = vr_ru32(r), bgra = vr_ru32(r), q = vr_ru32(r);
        if (!e || !e->map || w == 0 || h == 0 || stride < (uint64_t) w * 4 || off > e->size || (uint64_t) stride * h > e->size - off)
            return reply(s, VK_ERROR_MEMORY_MAP_FAILED);
#if defined(__APPLE__) || defined(_WIN32)
        double jt1 = t_ms();
        size_t before = g_out.n;
        if (!jpeg_encode((const uint8_t *) e->map + off, w, h, stride, (int) bgra, (int) (q ? q : 80), &g_out)) return reply(s, VK_ERROR_UNKNOWN);
        jt_enc += t_ms() - jt1;
        jbytes += g_out.n - before;
        if (++jn % 100 == 0)
            vlog("JPEG：%llu コマ・GPU を待つ %.1f ms・縮める %.1f ms・平均 %.1f KB（1 コマあたり）", (unsigned long long) jn,
                 jt_wait / 100, jt_enc / 100, jbytes / 1024.0 / 100), jt_wait = jt_enc = 0, jbytes = 0;
        return reply(s, 0);
#else
        (void) bgra; (void) q;
        return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
#endif
    }
    case VR_READ_JPEG_PIPE: {
        static double jp_wait, jp_join_ms, jp_enc;
        static uint64_t jpn, jpbytes;
        double t0 = t_ms();
        uint64_t mid = vr_ru64(r);
        drain_upto(last_with_mem(mid));
        jp_wait += t_ms() - t0;
        if (g_err != VK_SUCCESS) return reply(s, g_err);
        ent *e = find(mid);
        uint64_t off = vr_ru64(r);
        uint32_t w = vr_ru32(r), h = vr_ru32(r), stride = vr_ru32(r), bgra = vr_ru32(r), q = vr_ru32(r);
        if (!e || !e->map || w == 0 || h == 0 || stride < (uint64_t) w * 4 || off > e->size || (uint64_t) stride * h > e->size - off)
            return reply(s, VK_ERROR_MEMORY_MAP_FAILED);
#if defined(__APPLE__) || defined(_WIN32)
        // ひとつ前のコマの JPEG ができあがるのを待って、返事に入れる
        double t1 = t_ms();
        jp_join();
        jp_join_ms += t_ms() - t1;
        if (g_jp.ok) {
            vr_put(&g_out, g_jp.out.p, g_jp.out.n);
            jp_enc += g_jp.ms;
            jpbytes += g_jp.out.n;
        }
        // 今のコマを写して、後ろで縮め始める
        g_jp.w = w, g_jp.h = h, g_jp.stride = stride, g_jp.bgra = (int) bgra, g_jp.q = (int) (q ? q : 80);
        if (!jp_start((const uint8_t *) e->map + off)) return reply(s, VK_ERROR_OUT_OF_HOST_MEMORY);
        if (++jpn % 100 == 0)
            vlog("JPEG（先送り）：%llu コマ・GPU を待つ %.1f ms・前のコマを待つ %.1f ms・縮める %.1f ms（後ろで）・平均 %.1f KB（1 コマあたり）",
                 (unsigned long long) jpn, jp_wait / 100, jp_join_ms / 100, jp_enc / 100, jpbytes / 1024.0 / 100),
                jp_wait = jp_join_ms = jp_enc = 0, jpbytes = 0;
        return reply(s, 0);
#else
        (void) bgra; (void) q;
        return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
#endif
    }
    case VR_MEM_READ_DELTA: {
        // ドライバ側の写しと同じものを持っておき、今回との違い（XOR）を 4 バイトずつ見て、
        // 「同じ語が続く数・違う語の数・その XOR」の並びで送る。変わっていない所はほとんど 0 バイトになる
        uint64_t mid = vr_ru64(r);
        drain_upto(last_with_mem(mid));
        if (g_err != VK_SUCCESS) return reply(s, g_err);
        ent *e = find(mid);
        uint64_t off = vr_ru64(r), n = vr_ru64(r);
        if (!e || !e->map || off > e->size || n > e->size - off) return reply(s, VK_ERROR_MEMORY_MAP_FAILED);
        if (!e->last && e->size <= (256u << 20)) e->last = calloc(1, (size_t) e->size);
        const uint8_t *cur = (const uint8_t *) e->map + off;
        if (e->last && (off & 3) == 0 && (n & 3) == 0) {
            // GPU のメモリはキャッシュが効かないことがあり、4 バイトずつ読むと遅い。先にまとめて写してから比べる
            static uint8_t *tmp;
            static size_t tcap;
            if (tcap < n) { free(tmp); tmp = malloc((size_t) n); tcap = tmp ? (size_t) n : 0; }
            if (tmp) { memcpy(tmp, cur, (size_t) n); cur = tmp; }
            const uint32_t *c = (const uint32_t *) cur;
            uint32_t *l = (uint32_t *) (e->last + off);
            size_t words = (size_t) (n / 4);
            vr_u32(&g_out, 1);
            for (size_t i = 0; i < words;) {
                size_t z = i;
                while (z < words && c[z] == l[z]) z++;
                if (z == words) break;
                size_t k = z;
                while (k < words && c[k] != l[k]) k++;
                vr_u32(&g_out, (uint32_t) (z - i));
                vr_u32(&g_out, (uint32_t) (k - z));
                for (size_t t = z; t < k; t++) { uint32_t x = c[t] ^ l[t]; vr_put(&g_out, &x, 4); l[t] = c[t]; }
                i = k;
            }
        } else {
            vr_u32(&g_out, 0);
            vr_put(&g_out, cur, (size_t) n);
            if (e->last) memcpy(e->last + off, cur, (size_t) n);
        }
        return reply(s, 0);
    }
    case VR_CREATE_SHADER_MODULE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        uint32_t n;
        const void *code = vr_rbytes(r, &n);
        VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n, .pCode = code };
        VkShaderModule m;
        if (code && vkCreateShaderModule(d, &ci, NULL, &m) == VK_SUCCESS) put(id, (uint64_t) m, K_SHADER);
        return 1;
    }
    case VR_DESTROY_SHADER_MODULE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroyShaderModule(d, OBJ(VkShaderModule, id), NULL);
        del(id);
        return 1;
    }
    case VR_CREATE_DSL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        uint32_t flags = vr_ru32(r), n = vr_ru32(r);
        if (n > r->n - r->at) return 1; // 本文より多い binding は細工。1 つ 16 バイトなので残りより多いことはない
        VkDescriptorSetLayoutBinding *b = calloc(n ? n : 1, sizeof *b);
        if (!b) return 1;
        for (uint32_t i = 0; i < n; i++) {
            b[i].binding = vr_ru32(r);
            b[i].descriptorType = (VkDescriptorType) vr_ru32(r);
            b[i].descriptorCount = vr_ru32(r);
            b[i].stageFlags = vr_ru32(r);
        }
        VkDescriptorSetLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = flags, .bindingCount = n, .pBindings = b };
        VkDescriptorSetLayout l;
        if (vkCreateDescriptorSetLayout(d, &ci, NULL, &l) == VK_SUCCESS) put(id, (uint64_t) l, K_DSL);
        free(b);
        return 1;
    }
    case VR_DESTROY_DSL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroyDescriptorSetLayout(d, OBJ(VkDescriptorSetLayout, id), NULL);
        del(id);
        return 1;
    }
    case VR_CREATE_PIPELINE_LAYOUT: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        uint32_t ns = vr_ru32(r);
        VkDescriptorSetLayout sl[32];
        for (uint32_t i = 0; i < ns && i < 32; i++) sl[i] = OBJ(VkDescriptorSetLayout, vr_ru64(r));
        uint32_t np = vr_ru32(r);
        VkPushConstantRange pr[8];
        for (uint32_t i = 0; i < np && i < 8; i++) { pr[i].stageFlags = vr_ru32(r); pr[i].offset = vr_ru32(r); pr[i].size = vr_ru32(r); }
        VkPipelineLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = ns, .pSetLayouts = sl, .pushConstantRangeCount = np, .pPushConstantRanges = pr };
        VkPipelineLayout l;
        if (vkCreatePipelineLayout(d, &ci, NULL, &l) == VK_SUCCESS) put(id, (uint64_t) l, K_PLAYOUT);
        return 1;
    }
    case VR_DESTROY_PIPELINE_LAYOUT: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroyPipelineLayout(d, OBJ(VkPipelineLayout, id), NULL);
        del(id);
        return 1;
    }
    case VR_CREATE_COMPUTE_PIPELINE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.flags = vr_ru32(r);
        ci.layout = OBJ(VkPipelineLayout, vr_ru64(r));
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.flags = vr_ru32(r);
        ci.stage.module = OBJ(VkShaderModule, vr_ru64(r));
        ci.stage.pName = rd_str(r);
        uint32_t nm = vr_ru32(r);
        VkSpecializationMapEntry me[64];
        for (uint32_t i = 0; i < nm && i < 64; i++) { me[i].constantID = vr_ru32(r); me[i].offset = vr_ru32(r); me[i].size = (size_t) vr_ru32(r); }
        uint32_t nd;
        const void *sd = vr_rbytes(r, &nd);
        VkSpecializationInfo si = { nm, me, nd, sd };
        if (nm) ci.stage.pSpecializationInfo = &si;
        uint32_t req_subgroup = vr_ru32(r);
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = req_subgroup };
        if (req_subgroup) ci.stage.pNext = &rs;
        if (!ci.stage.module || r->bad) {
            vlog("compute pipeline の shader module が無い。作らない");
            return reply(s, VK_ERROR_INITIALIZATION_FAILED);
        }
        VkPipeline p;
        VkResult res = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, NULL, &p);
        if (res == VK_SUCCESS) put(id, (uint64_t) p, K_PIPELINE);
        else vlog("compute pipeline を作れない (%d)", res);
        return reply(s, res);
    }
    case VR_DESTROY_PIPELINE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroyPipeline(d, OBJ(VkPipeline, id), NULL);
        del(id);
        return 1;
    }
    case VR_CREATE_DESCRIPTOR_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkDescriptorPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        ci.flags = vr_ru32(r);
        ci.maxSets = vr_ru32(r);
        ci.poolSizeCount = vr_ru32(r);
        VkDescriptorPoolSize ps[16];
        for (uint32_t i = 0; i < ci.poolSizeCount && i < 16; i++) { ps[i].type = (VkDescriptorType) vr_ru32(r); ps[i].descriptorCount = vr_ru32(r); }
        if (ci.poolSizeCount > 16) ci.poolSizeCount = 16; // 詰めた分より多い数をドライバに渡さない
        ci.pPoolSizes = ps;
        VkDescriptorPool p;
        if (vkCreateDescriptorPool(d, &ci, NULL, &p) == VK_SUCCESS) put(id, (uint64_t) p, K_DPOOL);
        return 1;
    }
    case VR_DESTROY_DESCRIPTOR_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        // ドライバは仕事の終わりを待たずに「終わった」と答えるので、まだ GPU が使っている set があれば待つ
        drain_upto(last_with_dpool(id));
        vkDestroyDescriptorPool(d, OBJ(VkDescriptorPool, id), NULL);
        drop_sets_of_pool(id);
        del(id);
        return 1;
    }
    case VR_RESET_DESCRIPTOR_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        drain_upto(last_with_dpool(id));
        vkResetDescriptorPool(d, OBJ(VkDescriptorPool, id), 0);
        drop_sets_of_pool(id);
        return 1;
    }
    case VR_ALLOC_DESCRIPTOR_SETS: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t pid = vr_ru64(r);
        VkDescriptorPool pool = OBJ(VkDescriptorPool, pid);
        uint32_t n = vr_ru32(r);
        for (uint32_t i = 0; i < n; i++) {
            VkDescriptorSetLayout l = OBJ(VkDescriptorSetLayout, vr_ru64(r));
            uint64_t id = vr_ru64(r);
            VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &l };
            VkDescriptorSet ds;
            VkResult res = vkAllocateDescriptorSets(d, &ai, &ds);
            if (res == VK_SUCCESS) {
                put(id, (uint64_t) ds, K_DSET);
                ent *se = find(id);
                if (se) se->aux = pid; // どの pool から取ったか
            } else vlog("descriptor set を取れない (%d)", res);
        }
        return 1;
    }
    case VR_FREE_DESCRIPTOR_SETS: {
        VkDevice d = DEV(vr_ru64(r));
        VkDescriptorPool pool = OBJ(VkDescriptorPool, vr_ru64(r));
        uint32_t n = vr_ru32(r);
        for (uint32_t i = 0; i < n; i++) {
            uint64_t id = vr_ru64(r);
            VkDescriptorSet ds = OBJ(VkDescriptorSet, id);
            drain_upto(last_with_set(id));
            vkFreeDescriptorSets(d, pool, 1, &ds);
            del(id);
        }
        return 1;
    }
    case VR_UPDATE_DESCRIPTOR_SETS: {
        VkDevice d = DEV(vr_ru64(r));
        uint32_t nw = vr_ru32(r);
        uint64_t dbg_prev = 0;
        for (uint32_t i = 0; i < nw; i++) {
            VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            uint64_t sid = vr_ru64(r);
            if (dbg_on() && sid != dbg_prev) { dset_of(sid)->n = 0; dbg_prev = sid; }
            if (getenv("VKR_CHECK")) {
                int k1 = last_with_set(sid), k2 = -1;
                for (int q = g_nfl - 1; q >= 0 && k2 < 0; q--)
                    for (uint32_t j = 0; j < g_fl[q].nsnap; j++) if (g_fl[q].snap[j] == sid) { k2 = q; break; }
                // 同じ実物（ハンドル）を、違う id で使っている走り中の仕事があるか
                VkDescriptorSet hs = OBJ(VkDescriptorSet, sid);
                for (int q = g_nfl - 1; q > k1 && q >= 0; q--)
                    for (uint32_t j = 0; j < g_fl[q].nsnap; j++) {
                        ent *oe = find(g_fl[q].snap[j]);
                        if (g_fl[q].snap[j] != sid && oe && (VkDescriptorSet) (uintptr_t) oe->h == hs) {
                            vlog("取りこぼし（実物）：set %llu の実物は、%d 番目の仕事では set %llu として使われている（%s）", (unsigned long long) sid, q,
                                 (unsigned long long) g_fl[q].snap[j], vkGetFenceStatus(g_dev, g_fl[q].f) == VK_SUCCESS ? "もう終わっている" : "まだ走っている");
                            break;
                        }
                    }
                if (k2 > k1) {
                    VkResult st = vkGetFenceStatus(g_dev, g_fl[k2].f);
                    vlog("取りこぼし：set %llu を書き換える。今の判定は %d 番目まで待つが、submit の時点では %d 番目（%s）が使っていた。その command buffer：%llu",
                         (unsigned long long) sid, k1, k2, st == VK_SUCCESS ? "もう終わっている" : "まだ走っている",
                         (unsigned long long) (g_fl[k2].ncb ? g_fl[k2].cbs[0] : 0));
                }
            }
            drain_upto(last_with_set(sid));
            w.dstSet = OBJ(VkDescriptorSet, sid);
            w.dstBinding = vr_ru32(r);
            w.dstArrayElement = vr_ru32(r);
            w.descriptorType = (VkDescriptorType) vr_ru32(r);
            w.descriptorCount = vr_ru32(r);
            if (w.descriptorCount > r->n - r->at) return 1;
            ent *se = find(sid);
            int image = w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                        w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                        w.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            if (w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                VkBufferView *tv = calloc(w.descriptorCount ? w.descriptorCount : 1, sizeof *tv);
                for (uint32_t k = 0; k < w.descriptorCount; k++) {
                    uint64_t vid = vr_ru64(r);
                    tv[k] = OBJ(VkBufferView, vid);
                    ent *ve = find(vid);
                    if (se && ve && ve->aux) idlist_add(&se->list, ve->aux); // 見え方の aux はもとの buffer
                }
                w.pTexelBufferView = tv;
                if (!r->bad) vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
                free(tv);
                continue;
            }
            if (image) {
                VkDescriptorImageInfo *ii = calloc(w.descriptorCount ? w.descriptorCount : 1, sizeof *ii);
                for (uint32_t k = 0; k < w.descriptorCount; k++) {
                    ii[k].sampler = OBJ(VkSampler, vr_ru64(r));
                    // sampler の要る種類で空なら、代わりの sampler を入れる（空のままだと GPU のドライバの中で落ちることがある）
                    if (!ii[k].sampler && (w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
                        if (!g_dummy_smp) {
                            VkSamplerCreateInfo sc = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                                                       .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                       .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = VK_LOD_CLAMP_NONE };
                            if (vkCreateSampler(d, &sc, NULL, &g_dummy_smp) != VK_SUCCESS) g_dummy_smp = VK_NULL_HANDLE;
                            vlog("空の sampler が届いたので、代わりの sampler を入れる（ドライバの取りこぼし）");
                        }
                        ii[k].sampler = g_dummy_smp;
                    }
                    uint64_t vid = vr_ru64(r);
                    ii[k].imageView = OBJ(VkImageView, vid);
                    ii[k].imageLayout = (VkImageLayout) vr_ru32(r);
                    if (se && vid) idlist_add(&se->list, vid); // 見え方の aux はもとの画像のメモリ
                }
                w.pImageInfo = ii;
                vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
                free(ii);
                continue;
            }
            VkDescriptorBufferInfo *bi = calloc(w.descriptorCount ? w.descriptorCount : 1, sizeof *bi);
            for (uint32_t k = 0; k < w.descriptorCount; k++) {
                uint64_t bid = vr_ru64(r);
                bi[k].buffer = OBJ(VkBuffer, bid);
                bi[k].offset = vr_ru64(r);
                bi[k].range = vr_ru64(r);
                if (se) idlist_add(&se->list, bid);
                if (dbg_on()) {
                    ent *be = find(bid);
                    char t[160];
                    int tn = snprintf(t, sizeof t, " b%u=%llu（%llu）+%llu/%lld", w.dstBinding + k, (unsigned long long) bid,
                                      (unsigned long long) (be ? be->size : 0), (unsigned long long) bi[k].offset,
                                      bi[k].range == VK_WHOLE_SIZE ? -1LL : (long long) bi[k].range);
                    vr_put(dset_of(sid), t, (size_t) tn);
                }
            }
            // 空の uniform・storage buffer（OpenGL を Vulkan に直す zink が書く）は、GPU が空の descriptor を持たなければ、
            // 0 で埋めた代わりにする（iPhone は持たない。仕様の外なので、どう動くか分からない）
            for (uint32_t k = 0; k < w.descriptorCount && !g_nulldesc_gpu; k++)
                if (!bi[k].buffer && dummy_vb()) {
                    int ubo = w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                    bi[k].buffer = dummy_vb();
                    bi[k].offset = 0;
                    bi[k].range = ubo ? 16384 : VK_WHOLE_SIZE; // uniform は仕様の下限の 16 KiB に収める
                }
            w.pBufferInfo = bi;
            vkUpdateDescriptorSets(d, 1, &w, 0, NULL);
            free(bi);
        }
        return 1;
    }
    case VR_CREATE_COMMAND_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkCommandPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        ci.flags = vr_ru32(r) | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // 記録し直すたびに reset するので
        ci.queueFamilyIndex = vr_ru32(r);
        VkCommandPool p;
        if (vkCreateCommandPool(d, &ci, NULL, &p) == VK_SUCCESS) put(id, (uint64_t) p, K_CPOOL);
        return 1;
    }
    case VR_DESTROY_COMMAND_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroyCommandPool(d, OBJ(VkCommandPool, id), NULL);
        del(id);
        return 1;
    }
    case VR_RESET_COMMAND_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t pid = vr_ru64(r);
        drain_upto(last_with_pool(pid));
        vkResetCommandPool(d, OBJ(VkCommandPool, pid), 0);
        return 1;
    }
    case VR_ALLOC_COMMAND_BUFFERS: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t pid = vr_ru64(r);
        VkCommandPool pool = OBJ(VkCommandPool, pid);
        uint32_t level = vr_ru32(r), n = vr_ru32(r);
        for (uint32_t i = 0; i < n; i++) {
            VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = (VkCommandBufferLevel) level, .commandBufferCount = 1 };
            VkCommandBuffer cb;
            uint64_t id = vr_ru64(r);
            if (vkAllocateCommandBuffers(d, &ai, &cb) == VK_SUCCESS) {
                put(id, (uint64_t) (uintptr_t) cb, K_CB);
                cbinfo *c = cbi(id, 1);
                c->pool = pid;
                c->n = 0;
            }
        }
        return 1;
    }
    case VR_FREE_COMMAND_BUFFERS: {
        VkDevice d = DEV(vr_ru64(r));
        VkCommandPool pool = OBJ(VkCommandPool, vr_ru64(r));
        uint32_t n = vr_ru32(r);
        for (uint32_t i = 0; i < n; i++) {
            uint64_t id = vr_ru64(r);
            VkCommandBuffer cb = (VkCommandBuffer) (uintptr_t) H(id);
            if (cb) vkFreeCommandBuffers(d, pool, 1, &cb);
            del(id);
        }
        return 1;
    }
    case VR_RECORD_COMMAND_BUFFER: {
        uint64_t cid = vr_ru64(r);
        VkCommandBuffer cb = (VkCommandBuffer) (uintptr_t) H(cid);
        uint32_t flags = vr_ru32(r), n;
        const uint8_t *stream = vr_rbytes(r, &n);
        if (!cb) return 1;
        drain_upto(last_with_cb(cid));
        double t0 = t_ms();
        vkResetCommandBuffer(cb, 0);
        if (dbg_on()) trace_of(cid)->n = 0;
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = flags };
        vkBeginCommandBuffer(cb, &bi);
        vr_rd sr = { stream, n, 0, 0 };
        cbinfo *ci = cbi(cid, 1);
        ci->n = 0;
        if (ci->mems) ci->mems->n = 0;
        replay(cb, &sr, ci);
        vkEndCommandBuffer(cb);
        g_st.record_ms += t_ms() - t0;
        g_st.records++;
        return 1;
    }
    case VR_RESET_COMMAND_BUFFER: {
        uint64_t cid = vr_ru64(r);
        VkCommandBuffer cb = (VkCommandBuffer) (uintptr_t) H(cid);
        drain_upto(last_with_cb(cid));
        if (cb) vkResetCommandBuffer(cb, 0);
        return 1;
    }
    case VR_CREATE_FENCE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkFenceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = vr_ru32(r) };
        VkFence f;
        if (vkCreateFence(d, &ci, NULL, &f) == VK_SUCCESS) put(id, (uint64_t) f, K_FENCE);
        return 1;
    }
    case VR_DESTROY_FENCE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroyFence(d, OBJ(VkFence, id), NULL);
        del(id);
        return 1;
    }
    case VR_RESET_FENCES:
    case VR_WAIT_FOR_FENCES: {
        VkDevice d = DEV(vr_ru64(r));
        uint32_t n = vr_ru32(r);
        VkFence f[64];
        uint64_t fid[64];
        for (uint32_t i = 0; i < n && i < 64; i++) { fid[i] = vr_ru64(r); f[i] = OBJ(VkFence, fid[i]); }
        if (op == VR_RESET_FENCES) {
            // ドライバはもう「終わった」と答えているので、アプリは待ったつもりで reset してくる。
            // まだ計算中の仕事に付いている fence は待たずに、その名前を新しい fence に付け替える。
            // 古い fence は仕事が終わったら自前の物として使い回す
            for (uint32_t i = 0; i < n && i < 64; i++) {
                int k = last_with_fence(f[i]);
                if (k >= 0) {
                    for (int j = 0; j < g_nfl; j++) if (g_fl[j].f == f[i]) g_fl[j].own = 1;
                    VkFence nf = own_fence();
                    if (nf) put(fid[i], (uint64_t) nf, K_FENCE);
                    else { drain_upto(k); vkResetFences(d, 1, &f[i]); }
                } else {
                    vkResetFences(d, 1, &f[i]);
                }
            }
            return 1;
        }
        uint32_t all = vr_ru32(r);
        uint64_t timeout = vr_ru64(r);
        return reply(s, vkWaitForFences(d, n, f, all, timeout));
    }
    case VR_GET_FENCE_STATUS: {
        VkDevice d = DEV(vr_ru64(r));
        return reply(s, vkGetFenceStatus(d, OBJ(VkFence, vr_ru64(r))));
    }
    case VR_CREATE_SEMAPHORE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        // timeline semaphore（番号つきの待ち合わせ）。llama.cpp の Vulkan 版が使う
        VkSemaphoreTypeCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        ti.semaphoreType = (VkSemaphoreType) vr_ru32(r);
        ti.initialValue = vr_ru64(r);
        VkSemaphoreCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti };
        VkSemaphore sm;
        if (vkCreateSemaphore(d, &ci, NULL, &sm) == VK_SUCCESS) put(id, (uint64_t) sm, K_SEM);
        return 1;
    }
    case VR_DESTROY_SEMAPHORE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroySemaphore(d, OBJ(VkSemaphore, id), NULL);
        del(id);
        return 1;
    }
    case VR_QUEUE_SUBMIT: {
        VkQueue q = (VkQueue) (uintptr_t) H(vr_ru64(r));
        uint32_t ns = vr_ru32(r);
        if (ns > 8) ns = 8; // ドライバが 8 ずつに分けて送ってくる
        VkSubmitInfo si[8];
        VkTimelineSemaphoreSubmitInfo tl[8];
        VkSemaphore ws[8][9], ss[8][9];
        uint64_t wv[8][9], sv[8][9];
        VkPipelineStageFlags wst[8][9];
        VkCommandBuffer cbs[8][64];
        uint64_t cids[64];
        uint32_t ncid = 0;
        for (uint32_t i = 0; i < ns; i++) {
            si[i] = (VkSubmitInfo) { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tl[i] };
            tl[i] = (VkTimelineSemaphoreSubmitInfo) { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            uint32_t nw = vr_ru32(r);
            if (nw > 8) nw = 8;
            for (uint32_t k = 0; k < nw; k++) { ws[i][k] = OBJ(VkSemaphore, vr_ru64(r)); wst[i][k] = vr_ru32(r); wv[i][k] = vr_ru64(r); }
            si[i].waitSemaphoreCount = tl[i].waitSemaphoreValueCount = nw;
            si[i].pWaitSemaphores = ws[i];
            si[i].pWaitDstStageMask = wst[i];
            tl[i].pWaitSemaphoreValues = wv[i];
            uint32_t nc = vr_ru32(r);
            if (nc > 64) nc = 64;
            for (uint32_t k = 0; k < nc; k++) {
                uint64_t cid = vr_ru64(r);
                cbs[i][k] = (VkCommandBuffer) (uintptr_t) H(cid);
                if (ncid < 64) cids[ncid++] = cid;
            }
            si[i].commandBufferCount = nc;
            si[i].pCommandBuffers = cbs[i];
            uint32_t nsg = vr_ru32(r);
            if (nsg > 8) nsg = 8;
            for (uint32_t k = 0; k < nsg; k++) { ss[i][k] = OBJ(VkSemaphore, vr_ru64(r)); sv[i][k] = vr_ru64(r); }
            si[i].signalSemaphoreCount = tl[i].signalSemaphoreValueCount = nsg;
            si[i].pSignalSemaphores = ss[i];
            tl[i].pSignalSemaphoreValues = sv[i];
        }
        VkFence f = OBJ(VkFence, vr_ru64(r));
        // 同時に走らせる submit は 64 本まで（VKR_MAXFL=n で変える・0 なら上限なし）。
        // 代役の RTX で whisper を流すと、128 本では GPU を失い、64 本までなら通った。原因はまだわからない
        {
            static int maxfl = -1;
            if (maxfl < 0) { const char *e = getenv("VKR_MAXFL"); maxfl = e ? atoi(e) : 64; }
            if (maxfl > 0 && g_nfl >= maxfl) drain_upto(g_nfl - maxfl);
        }
        // 同じ command buffer を、前に出した分が終わる前にもう一度出すのは Vulkan では禁止（NVIDIA では GPU を失う）。
        // アプリは待ったつもりで出してくるので、まだ走っていたら、それが終わるまで待つ
        {
            int k = -1;
            for (uint32_t i = 0; i < ncid; i++) { int q = last_with_cb(cids[i]); if (q > k) k = q; }
            if (k >= 0) {
                if (getenv("VKR_CHECK")) vlog("submit %llu：まだ走っている command buffer をもう一度出そうとした（%d 番目まで待つ）", (unsigned long long) g_st.submits + 1, k);
                drain_upto(k);
            }
        }
        if (chain_on() && !g_err && chain_ready())
            for (uint32_t i = 0; i < ns; i++) {
                if (g_chain_last >= 0) {
                    uint32_t k = si[i].waitSemaphoreCount;
                    ws[i][k] = g_chain[g_chain_last];
                    wst[i][k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                    wv[i][k] = 0;
                    si[i].waitSemaphoreCount = tl[i].waitSemaphoreValueCount = k + 1;
                }
                int next = (g_chain_last + 1) & 3;
                uint32_t k = si[i].signalSemaphoreCount;
                ss[i][k] = g_chain[next];
                sv[i][k] = 0;
                si[i].signalSemaphoreCount = tl[i].signalSemaphoreValueCount = k + 1;
                g_chain_last = next;
            }
        // 出した仕事は必ず fence で追う（アプリが付けていなければ自前の物を付ける）
        int own = 0;
        if (!f) { f = own_fence(); own = 1; }
        if (g_nfl == 256) drain();
        // iPhone のアプリが前にいない間は GPU を使えない。出すと GPU を失うので、前に戻るまで待つ（PC は待たされるだけ）
        if (g_paused) {
            vlog("アプリが前にいないので、前に戻るまで GPU に仕事を出さずに待つ");
            double t0 = t_ms();
            while (g_paused) sleep_ms(20);
            g_st.paused_ms += t_ms() - t0;
            vlog("前に戻ったので続ける");
        }
        double t0 = t_ms();
        VkResult res = g_err ? g_err : vkQueueSubmit(q, ns, si, f);
        g_st.submit_ms += t_ms() - t0;
        g_st.submits++;
        if (dbg_on() && res == VK_SUCCESS) {
            VkResult w = vkQueueWaitIdle(q);
            if (w != VK_SUCCESS) {
                vlog("submit %llu で GPU を失った (%d)。command buffer %u 本", (unsigned long long) g_st.submits, w, ncid);
                for (uint32_t k = 0; k < ncid; k++) {
                    vr_buf *t = trace_of(cids[k]);
                    printf("-- command buffer %llu\n%.*s", (unsigned long long) cids[k], (int) t->n, t->p ? (const char *) t->p : "");
                }
                fflush(stdout);
                res = w;
                g_err = w;
            }
        }
        if (res == VK_SUCCESS) {
            inflight *e = &g_fl[g_nfl++];
            e->f = f;
            e->own = own;
            e->ncb = ncid;
            e->snap = NULL;
            e->nsnap = 0;
            if (getenv("VKR_CHECK")) {
                uint32_t tot = 0;
                for (uint32_t k = 0; k < ncid; k++) { cbinfo *c = cbi(cids[k], 0); if (c) tot += c->n; }
                e->snap = malloc((tot ? tot : 1) * sizeof *e->snap);
                for (uint32_t k = 0; k < ncid; k++) {
                    cbinfo *c = cbi(cids[k], 0);
                    if (c) for (uint32_t j = 0; j < c->n; j++) e->snap[e->nsnap++] = c->sets[j];
                }
            }
            memcpy(e->cbs, cids, ncid * sizeof *cids);
        } else {
            if (!g_err) vlog("vkQueueSubmit 失敗 (%d)。この接続ではもう GPU を使えない", res);
            if (res < 0) g_err = res;
            if (own && g_nfpool < 64) g_fpool[g_nfpool++] = f;
        }
        return 1;
    }
    case VR_WAIT_SEMAPHORES: {
        VkDevice d = DEV(vr_ru64(r));
        VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.flags = vr_ru32(r);
        uint32_t n = vr_ru32(r);
        if (n > 64) n = 64;
        VkSemaphore sm[64];
        uint64_t v[64];
        for (uint32_t i = 0; i < n; i++) { sm[i] = OBJ(VkSemaphore, vr_ru64(r)); v[i] = vr_ru64(r); }
        wi.semaphoreCount = n;
        wi.pSemaphores = sm;
        wi.pValues = v;
        return reply(s, vkWaitSemaphores(d, &wi, vr_ru64(r)));
    }
    case VR_GET_SEMAPHORE_COUNTER: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t v = 0;
        VkResult res = vkGetSemaphoreCounterValue(d, OBJ(VkSemaphore, vr_ru64(r)), &v);
        vr_u64(&g_out, v);
        return reply(s, res);
    }
    case VR_SIGNAL_SEMAPHORE: {
        VkDevice d = DEV(vr_ru64(r));
        VkSemaphoreSignalInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
        si.semaphore = OBJ(VkSemaphore, vr_ru64(r));
        si.value = vr_ru64(r);
        vkSignalSemaphore(d, &si);
        return 1;
    }
    case VR_GET_LINK: {
        // USB の口からだけ答える（インターネット共有の口から来た相手には、合言葉を教えない）
        struct in_addr ia = { 0 };
        int ok = !g_cur_remote && g_token_ok && g_extra_host[0] && inet_pton(AF_INET, g_extra_host, &ia) == 1;
        vr_u32(&g_out, ok ? 1u : 0u);
        uint8_t zero[16] = { 0 };
        vr_put(&g_out, ok ? g_token : zero, 16);
        uint32_t ip = ok ? (uint32_t) ia.s_addr : 0;
        vr_put(&g_out, &ip, 4);
        vr_u32(&g_out, ok ? (uint32_t) g_main_port : 0u);
        return reply(s, 0);
    }
    case VR_GET_STATUS: {
        // PC の代役は VKR_THERMAL_FILE に書いた数字を返す（熱くなったときのドライバの動きを試す）
        int t = g_thermal;
        const char *f = getenv("VKR_THERMAL_FILE");
        if (f) {
            FILE *fp = fopen(f, "r");
            if (fp) {
                if (fscanf(fp, "%d", &t) != 1) t = 0;
                fclose(fp);
            }
        }
        vr_u32(&g_out, (uint32_t) t);
        vr_u32(&g_out, g_paused ? 1u : 0u);
        return reply(s, 0);
    }
    case VR_CREATE_IMAGE_NR:
    case VR_CREATE_IMAGE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ci.flags = vr_ru32(r);
        ci.imageType = (VkImageType) vr_ru32(r);
        ci.format = (VkFormat) vr_ru32(r);
        ci.extent.width = vr_ru32(r);
        ci.extent.height = vr_ru32(r);
        ci.extent.depth = vr_ru32(r);
        ci.mipLevels = vr_ru32(r);
        ci.arrayLayers = vr_ru32(r);
        ci.samples = (VkSampleCountFlagBits) vr_ru32(r);
        ci.tiling = (VkImageTiling) vr_ru32(r);
        ci.usage = vr_ru32(r);
        ci.initialLayout = (VkImageLayout) vr_ru32(r);
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkImage im;
        VkMemoryRequirements req;
        memset(&req, 0, sizeof req);
        VkResult res = vkCreateImage(d, &ci, NULL, &im);
        if (res == VK_SUCCESS) {
            put(id, (uint64_t) im, K_IMAGE);
            vkGetImageMemoryRequirements(d, im, &req);
        }
        if (op == VR_CREATE_IMAGE_NR) {
            if (res != VK_SUCCESS) vlog("image を作れない (%d)", res);
            return 1;
        }
        vr_put(&g_out, &req, sizeof req);
        return reply(s, res);
    }
    case VR_DESTROY_IMAGE:
    case VR_DESTROY_IMAGE_VIEW:
    case VR_DESTROY_RENDER_PASS:
    case VR_DESTROY_FRAMEBUFFER: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        ent *e = find(id);
        if (e) {
            if (op == VR_DESTROY_IMAGE) vkDestroyImage(d, (VkImage) e->h, NULL);
            else if (op == VR_DESTROY_IMAGE_VIEW) vkDestroyImageView(d, (VkImageView) e->h, NULL);
            else if (op == VR_DESTROY_RENDER_PASS) vkDestroyRenderPass(d, (VkRenderPass) e->h, NULL);
            else vkDestroyFramebuffer(d, (VkFramebuffer) e->h, NULL);
            idlist_free(&e->list);
            del(id);
        }
        return 1;
    }
    case VR_BIND_IMAGE_MEMORY: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t iid = vr_ru64(r), mid = vr_ru64(r);
        uint64_t off = vr_ru64(r);
        ent *ie = find(iid);
        if (ie) ie->aux = mid;
        vkBindImageMemory(d, OBJ(VkImage, iid), OBJ(VkDeviceMemory, mid), off);
        return 1;
    }
    case VR_CREATE_IMAGE_VIEW: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ci.flags = vr_ru32(r);
        uint64_t iid = vr_ru64(r);
        ci.image = OBJ(VkImage, iid);
        ci.viewType = (VkImageViewType) vr_ru32(r);
        ci.format = (VkFormat) vr_ru32(r);
        vr_rcopy(r, &ci.components, sizeof ci.components);
        vr_rcopy(r, &ci.subresourceRange, sizeof ci.subresourceRange);
        VkImageView v;
        VkResult res = vkCreateImageView(d, &ci, NULL, &v);
        if (res == VK_SUCCESS) {
            put(id, (uint64_t) v, K_VIEW);
            ent *ie = find(iid), *ve = find(id);
            if (ve && ie) ve->aux = ie->aux; // 見え方からも、もとの画像のメモリが分かるように
        } else {
            vlog("image view を作れない (%d)", res);
        }
        return 1;
    }
    case VR_CREATE_RENDER_PASS: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        ci.flags = vr_ru32(r);
        ci.attachmentCount = vr_ru32(r);
        if (ci.attachmentCount > r->n - r->at) return 1;
        VkAttachmentDescription *att = calloc(ci.attachmentCount ? ci.attachmentCount : 1, sizeof *att);
        for (uint32_t i = 0; i < ci.attachmentCount; i++) vr_rcopy(r, &att[i], sizeof att[i]);
        ci.pAttachments = att;
        ci.subpassCount = vr_ru32(r);
        if (ci.subpassCount > r->n - r->at) ci.subpassCount = 0, r->bad = 1;
        VkSubpassDescription *sub = calloc(ci.subpassCount ? ci.subpassCount : 1, sizeof *sub);
        // 参照の並びは、まとめて 1 つの塊に置く
        VkAttachmentReference *refs = calloc(256, sizeof *refs);
        uint32_t *pres = calloc(256, sizeof *pres);
        uint32_t nref = 0, npres = 0;
        for (uint32_t i = 0; i < ci.subpassCount; i++) {
            sub[i].flags = vr_ru32(r);
            sub[i].pipelineBindPoint = (VkPipelineBindPoint) vr_ru32(r);
            uint32_t nin = vr_ru32(r);
            sub[i].inputAttachmentCount = nin;
            sub[i].pInputAttachments = refs + nref;
            for (uint32_t k = 0; k < nin && nref < 256; k++) vr_rcopy(r, &refs[nref++], sizeof *refs);
            uint32_t ncol = vr_ru32(r);
            sub[i].colorAttachmentCount = ncol;
            sub[i].pColorAttachments = refs + nref;
            for (uint32_t k = 0; k < ncol && nref < 256; k++) vr_rcopy(r, &refs[nref++], sizeof *refs);
            if (vr_ru32(r)) {
                sub[i].pResolveAttachments = refs + nref;
                for (uint32_t k = 0; k < ncol && nref < 256; k++) vr_rcopy(r, &refs[nref++], sizeof *refs);
            }
            if (vr_ru32(r)) {
                sub[i].pDepthStencilAttachment = refs + nref;
                if (nref < 256) vr_rcopy(r, &refs[nref++], sizeof *refs);
            }
            uint32_t np = vr_ru32(r);
            sub[i].preserveAttachmentCount = np;
            sub[i].pPreserveAttachments = pres + npres;
            for (uint32_t k = 0; k < np && npres < 256; k++) pres[npres++] = vr_ru32(r);
        }
        ci.pSubpasses = sub;
        ci.dependencyCount = vr_ru32(r);
        if (ci.dependencyCount > r->n - r->at) ci.dependencyCount = 0, r->bad = 1;
        VkSubpassDependency *dep = calloc(ci.dependencyCount ? ci.dependencyCount : 1, sizeof *dep);
        for (uint32_t i = 0; i < ci.dependencyCount; i++) vr_rcopy(r, &dep[i], sizeof dep[i]);
        ci.pDependencies = dep;
        VkRenderPass rp;
        VkResult res = r->bad ? VK_ERROR_INITIALIZATION_FAILED : vkCreateRenderPass(d, &ci, NULL, &rp);
        if (res == VK_SUCCESS) put(id, (uint64_t) rp, K_RPASS);
        else vlog("render pass を作れない (%d)", res);
        free(att); free(sub); free(refs); free(pres); free(dep);
        return 1;
    }
    case VR_CREATE_FRAMEBUFFER: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkFramebufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        ci.flags = vr_ru32(r);
        ci.renderPass = OBJ(VkRenderPass, vr_ru64(r));
        ci.attachmentCount = vr_ru32(r);
        if (ci.attachmentCount > r->n - r->at) return 1;
        VkImageView *v = calloc(ci.attachmentCount ? ci.attachmentCount : 1, sizeof *v);
        uint64_t vids[32];
        for (uint32_t i = 0; i < ci.attachmentCount; i++) {
            uint64_t vid = vr_ru64(r);
            if (i < 32) vids[i] = vid;
            v[i] = OBJ(VkImageView, vid);
        }
        ci.pAttachments = v;
        ci.width = vr_ru32(r);
        ci.height = vr_ru32(r);
        ci.layers = vr_ru32(r);
        VkFramebuffer fb;
        VkResult res = vkCreateFramebuffer(d, &ci, NULL, &fb);
        if (res == VK_SUCCESS) {
            put(id, (uint64_t) fb, K_FB);
            ent *fe = find(id);
            for (uint32_t i = 0; i < ci.attachmentCount && i < 32 && fe; i++) idlist_add(&fe->list, vids[i]);
        } else {
            vlog("framebuffer を作れない (%d)", res);
        }
        free(v);
        return 1;
    }
    case VR_CREATE_GRAPHICS_PIPELINE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        ci.flags = vr_ru32(r);
        ci.layout = OBJ(VkPipelineLayout, vr_ru64(r));
        ci.renderPass = OBJ(VkRenderPass, vr_ru64(r));
        ci.subpass = vr_ru32(r);
        // シェーダーの段
        uint32_t nst = vr_ru32(r);
        if (nst > 8) nst = 8;
        VkPipelineShaderStageCreateInfo st[8];
        VkSpecializationInfo spi[8];
        VkSpecializationMapEntry me[8][32];
        for (uint32_t i = 0; i < nst; i++) {
            st[i] = (VkPipelineShaderStageCreateInfo) { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            st[i].stage = (VkShaderStageFlagBits) vr_ru32(r);
            st[i].flags = vr_ru32(r);
            st[i].module = OBJ(VkShaderModule, vr_ru64(r));
            st[i].pName = rd_str(r);
            uint32_t nm = vr_ru32(r);
            for (uint32_t k = 0; k < nm; k++) {
                uint32_t a = vr_ru32(r), b = vr_ru32(r), c = vr_ru32(r);
                if (k < 32) me[i][k] = (VkSpecializationMapEntry) { a, b, c };
            }
            uint32_t nd;
            const void *sd = vr_rbytes(r, &nd);
            spi[i] = (VkSpecializationInfo) { nm < 32 ? nm : 32, me[i], nd, sd };
            if (nm) st[i].pSpecializationInfo = &spi[i];
        }
        for (uint32_t i = 0; i < nst; i++)
            if (!st[i].module) {
                vlog("shader module の無い段がある（段 0x%x）。pipeline を作らない", st[i].stage);
                return reply(s, VK_ERROR_INITIALIZATION_FAILED);
            }
        ci.stageCount = nst;
        ci.pStages = st;
        // 頂点の入力
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkVertexInputBindingDescription vb[32];
        VkVertexInputAttributeDescription va[32];
        if (vr_ru32(r)) {
            vi.vertexBindingDescriptionCount = vr_ru32(r);
            for (uint32_t i = 0; i < vi.vertexBindingDescriptionCount; i++) { if (i < 32) vr_rcopy(r, &vb[i], sizeof vb[i]); else vr_take(r, sizeof vb[0]); }
            if (vi.vertexBindingDescriptionCount > 32) vi.vertexBindingDescriptionCount = 32;
            vi.vertexAttributeDescriptionCount = vr_ru32(r);
            for (uint32_t i = 0; i < vi.vertexAttributeDescriptionCount; i++) { if (i < 32) vr_rcopy(r, &va[i], sizeof va[i]); else vr_take(r, sizeof va[0]); }
            if (vi.vertexAttributeDescriptionCount > 32) vi.vertexAttributeDescriptionCount = 32;
            vi.pVertexBindingDescriptions = vb;
            vi.pVertexAttributeDescriptions = va;
            ci.pVertexInputState = &vi;
        }
        // 組み立て
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        if (vr_ru32(r)) {
            ia.topology = (VkPrimitiveTopology) vr_ru32(r);
            ia.primitiveRestartEnable = vr_ru32(r);
            ci.pInputAssemblyState = &ia;
        }
        // 表示する範囲
        VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        VkViewport vpv[16];
        VkRect2D scv[16];
        if (vr_ru32(r)) {
            vps.viewportCount = vr_ru32(r);
            if (vr_ru32(r)) {
                for (uint32_t i = 0; i < vps.viewportCount; i++) { if (i < 16) vr_rcopy(r, &vpv[i], sizeof vpv[i]); else vr_take(r, sizeof vpv[0]); }
                vps.pViewports = vpv;
            }
            vps.scissorCount = vr_ru32(r);
            if (vr_ru32(r)) {
                for (uint32_t i = 0; i < vps.scissorCount; i++) { if (i < 16) vr_rcopy(r, &scv[i], sizeof scv[i]); else vr_take(r, sizeof scv[0]); }
                vps.pScissors = scv;
            }
            ci.pViewportState = &vps;
        }
        // 塗り方
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        if (vr_ru32(r)) {
            rs.depthClampEnable = vr_ru32(r);
            rs.rasterizerDiscardEnable = vr_ru32(r);
            rs.polygonMode = (VkPolygonMode) vr_ru32(r);
            rs.cullMode = vr_ru32(r);
            rs.frontFace = (VkFrontFace) vr_ru32(r);
            rs.depthBiasEnable = vr_ru32(r);
            vr_rcopy(r, &rs.depthBiasConstantFactor, 4);
            vr_rcopy(r, &rs.depthBiasClamp, 4);
            vr_rcopy(r, &rs.depthBiasSlopeFactor, 4);
            vr_rcopy(r, &rs.lineWidth, 4);
            ci.pRasterizationState = &rs;
        }
        // 標本の数
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        if (vr_ru32(r)) {
            ms.rasterizationSamples = (VkSampleCountFlagBits) vr_ru32(r);
            ms.sampleShadingEnable = vr_ru32(r);
            vr_rcopy(r, &ms.minSampleShading, 4);
            ms.alphaToCoverageEnable = vr_ru32(r);
            ms.alphaToOneEnable = vr_ru32(r);
            ci.pMultisampleState = &ms;
        }
        // 奥行き・ステンシル
        VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        if (vr_ru32(r)) {
            ds.depthTestEnable = vr_ru32(r);
            ds.depthWriteEnable = vr_ru32(r);
            ds.depthCompareOp = (VkCompareOp) vr_ru32(r);
            ds.depthBoundsTestEnable = vr_ru32(r);
            ds.stencilTestEnable = vr_ru32(r);
            vr_rcopy(r, &ds.front, sizeof ds.front);
            vr_rcopy(r, &ds.back, sizeof ds.back);
            vr_rcopy(r, &ds.minDepthBounds, 4);
            vr_rcopy(r, &ds.maxDepthBounds, 4);
            ci.pDepthStencilState = &ds;
        }
        // 色の重ね方
        VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkPipelineColorBlendAttachmentState cba[16];
        if (vr_ru32(r)) {
            cb.logicOpEnable = vr_ru32(r);
            cb.logicOp = (VkLogicOp) vr_ru32(r);
            cb.attachmentCount = vr_ru32(r);
            for (uint32_t i = 0; i < cb.attachmentCount; i++) { if (i < 16) vr_rcopy(r, &cba[i], sizeof cba[i]); else vr_take(r, sizeof cba[0]); }
            if (cb.attachmentCount > 16) cb.attachmentCount = 16;
            cb.pAttachments = cba;
            vr_rcopy(r, cb.blendConstants, sizeof cb.blendConstants);
            ci.pColorBlendState = &cb;
        }
        // 後から変える値
        VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        VkDynamicState dys[32];
        dy.dynamicStateCount = vr_ru32(r);
        for (uint32_t i = 0; i < dy.dynamicStateCount; i++) { uint32_t v = vr_ru32(r); if (i < 32) dys[i] = (VkDynamicState) v; }
        if (dy.dynamicStateCount > 32) dy.dynamicStateCount = 32;
        dy.pDynamicStates = dys;
        if (dy.dynamicStateCount) ci.pDynamicState = &dy;
        // 末尾に付いている物（[sType][中身]）。古いドライバは付けない
        VkPipelineRenderingCreateInfo pri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        VkPipelineTessellationStateCreateInfo tsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO };
        VkPipelineTessellationDomainOriginStateCreateInfo tdo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO };
        VkPipelineVertexInputDivisorStateCreateInfoEXT vdi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT };
        VkVertexInputBindingDivisorDescriptionEXT vdd[32];
        VkFormat cfmt[16];
        while (!r->bad && r->at + 8 <= r->n) {
            uint32_t st = vr_ru32(r), len;
            const uint8_t *blob = vr_rbytes(r, &len);
            if (!blob) break;
            vr_rd br = { blob, len, 0, 0 };
            if (st == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                // dynamic rendering：render pass の代わりに、描く先の形式を渡す
                pri.viewMask = vr_ru32(&br);
                uint32_t nc = vr_ru32(&br);
                for (uint32_t i = 0; i < nc; i++) { uint32_t f = vr_ru32(&br); if (i < 16) cfmt[i] = (VkFormat) f; }
                pri.colorAttachmentCount = nc < 16 ? nc : 16;
                pri.pColorAttachmentFormats = cfmt;
                pri.depthAttachmentFormat = (VkFormat) vr_ru32(&br);
                pri.stencilAttachmentFormat = (VkFormat) vr_ru32(&br);
                if (!br.bad) { pri.pNext = ci.pNext; ci.pNext = &pri; }
            } else if (st == VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
                // 頂点の step rate：インスタンスごとの頂点データを何個おきに進めるか
                uint32_t nd = vr_ru32(&br);
                for (uint32_t i = 0; i < nd; i++) {
                    uint32_t bnd = vr_ru32(&br), dvs = vr_ru32(&br);
                    if (i < 32) vdd[i] = (VkVertexInputBindingDivisorDescriptionEXT) { bnd, dvs };
                }
                vdi.vertexBindingDivisorCount = nd < 32 ? nd : 32;
                vdi.pVertexBindingDivisors = vdd;
                if (!br.bad && g_divisor && ci.pVertexInputState) { vdi.pNext = vi.pNext; vi.pNext = &vdi; }
            } else if (st == VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO) {
                // テッセレーション：1 つのパッチの点の数と、面の向きの原点
                tsi.patchControlPoints = vr_ru32(&br);
                tdo.domainOrigin = (VkTessellationDomainOrigin) vr_ru32(&br);
                if (!br.bad) {
                    if (tdo.domainOrigin != VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT) tsi.pNext = &tdo;
                    ci.pTessellationState = &tsi;
                }
            }
        }
        // iPhone のふり：MoltenVK はテッセレーションの設定が無いと何も描かない。代役でも同じに見えるよう、作らずに断る
        // iPhone のふり：iPhone が頂点に使えない形式の属性があれば、iPhone と同じく作らない
        for (uint32_t i = 0; like_ios() && ci.pVertexInputState && i < vi.vertexAttributeDescriptionCount; i++)
            if (like_ios_no_vertex(va[i].format)) {
                vlog("iPhone のふり：頂点の形式 %u は iPhone では頂点に使えないので pipeline を作らない", va[i].format);
                return reply(s, VK_ERROR_INITIALIZATION_FAILED);
            }
        if (like_ios() && !ci.pTessellationState)
            for (uint32_t i = 0; i < nst; i++)
                if (st[i].stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT || st[i].stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) {
                    vlog("iPhone のふり：テッセレーションの設定が無いので pipeline を作らない");
                    return reply(s, VK_ERROR_INITIALIZATION_FAILED);
                }
        VkPipeline pp;
        VkResult res = r->bad ? VK_ERROR_INITIALIZATION_FAILED : vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &ci, NULL, &pp);
        if (res == VK_SUCCESS) put(id, (uint64_t) pp, K_PIPELINE);
        else vlog("graphics pipeline を作れない (%d)", res);
        return reply(s, res);
    }
    case VR_CREATE_SAMPLER: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        vr_rcopy(r, (uint8_t *) &ci + offsetof(VkSamplerCreateInfo, flags), sizeof ci - offsetof(VkSamplerCreateInfo, flags));
        VkSampler sm;
        VkResult res = vkCreateSampler(d, &ci, NULL, &sm);
        if (res == VK_SUCCESS) put(id, (uint64_t) sm, K_SAMPLER);
        else vlog("sampler を作れない (%d)", res);
        return 1;
    }
    case VR_DESTROY_SAMPLER: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroySampler(d, OBJ(VkSampler, id), NULL);
        del(id);
        return 1;
    }
    case VR_GET_IMAGE_FORMAT_PROPS: {
        uint32_t fmt = vr_ru32(r), type = vr_ru32(r), tiling = vr_ru32(r), usage = vr_ru32(r), flags = vr_ru32(r);
        VkImageFormatProperties ip;
        memset(&ip, 0, sizeof ip);
        VkResult res = like_ios_no_format((VkFormat) fmt) ? VK_ERROR_FORMAT_NOT_SUPPORTED
                                                         : vkGetPhysicalDeviceImageFormatProperties(g_pd, (VkFormat) fmt, (VkImageType) type, (VkImageTiling) tiling, usage, flags, &ip);
        vr_put(&g_out, &ip, sizeof ip);
        return reply(s, res);
    }
    case VR_QUEUE_WAIT_IDLE: return reply(s, vkQueueWaitIdle((VkQueue) (uintptr_t) H(vr_ru64(r))));
    case VR_DEVICE_WAIT_IDLE: return reply(s, vkDeviceWaitIdle(DEV(vr_ru64(r))));
    // ---- 1.3 に向けて足した命令 ----
    case VR_CREATE_RENDER_PASS2: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkRenderPassCreateInfo2 ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2 };
        ci.flags = vr_ru32(r);
        ci.attachmentCount = vr_ru32(r);
        if (ci.attachmentCount > r->n - r->at) return 1;
        VkAttachmentDescription2 *att = calloc(ci.attachmentCount ? ci.attachmentCount : 1, sizeof *att);
        for (uint32_t i = 0; i < ci.attachmentCount; i++) {
            att[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            att[i].flags = vr_ru32(r);
            att[i].format = (VkFormat) vr_ru32(r);
            att[i].samples = (VkSampleCountFlagBits) vr_ru32(r);
            att[i].loadOp = (VkAttachmentLoadOp) vr_ru32(r);
            att[i].storeOp = (VkAttachmentStoreOp) vr_ru32(r);
            att[i].stencilLoadOp = (VkAttachmentLoadOp) vr_ru32(r);
            att[i].stencilStoreOp = (VkAttachmentStoreOp) vr_ru32(r);
            att[i].initialLayout = (VkImageLayout) vr_ru32(r);
            att[i].finalLayout = (VkImageLayout) vr_ru32(r);
        }
        ci.pAttachments = att;
        ci.subpassCount = vr_ru32(r);
        if (ci.subpassCount > r->n - r->at) { free(att); return 1; }
        VkSubpassDescription2 *sub = calloc(ci.subpassCount ? ci.subpassCount : 1, sizeof *sub);
        VkSubpassDescriptionDepthStencilResolve *dsr = calloc(ci.subpassCount ? ci.subpassCount : 1, sizeof *dsr);
        // 参照の並びは、まとめて 1 つの塊に置く
        VkAttachmentReference2 *refs = calloc(256, sizeof *refs);
        uint32_t *pres = calloc(256, sizeof *pres);
        uint32_t nref = 0, npres = 0;
#define REF2() do { VkAttachmentReference2 x_ = { .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 }; \
                     x_.attachment = vr_ru32(r); x_.layout = (VkImageLayout) vr_ru32(r); x_.aspectMask = vr_ru32(r); \
                     if (nref < 256) refs[nref++] = x_; } while (0)
        for (uint32_t i = 0; i < ci.subpassCount && !r->bad; i++) {
            sub[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
            sub[i].flags = vr_ru32(r);
            sub[i].pipelineBindPoint = (VkPipelineBindPoint) vr_ru32(r);
            sub[i].viewMask = vr_ru32(r);
            uint32_t nin = vr_ru32(r);
            sub[i].inputAttachmentCount = nin;
            sub[i].pInputAttachments = refs + nref;
            for (uint32_t k = 0; k < nin && !r->bad; k++) REF2();
            uint32_t ncol = vr_ru32(r);
            sub[i].colorAttachmentCount = ncol;
            sub[i].pColorAttachments = refs + nref;
            for (uint32_t k = 0; k < ncol && !r->bad; k++) REF2();
            if (vr_ru32(r)) {
                sub[i].pResolveAttachments = refs + nref;
                for (uint32_t k = 0; k < ncol && !r->bad; k++) REF2();
            }
            if (vr_ru32(r)) {
                sub[i].pDepthStencilAttachment = refs + nref;
                REF2();
            }
            uint32_t np = vr_ru32(r);
            sub[i].preserveAttachmentCount = np;
            sub[i].pPreserveAttachments = pres + npres;
            for (uint32_t k = 0; k < np && !r->bad; k++) { uint32_t x = vr_ru32(r); if (npres < 256) pres[npres++] = x; }
            // depth・stencil の resolve（VkSubpassDescriptionDepthStencilResolve）
            if (vr_ru32(r)) {
                dsr[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
                dsr[i].depthResolveMode = (VkResolveModeFlagBits) vr_ru32(r);
                dsr[i].stencilResolveMode = (VkResolveModeFlagBits) vr_ru32(r);
                if (vr_ru32(r)) {
                    dsr[i].pDepthStencilResolveAttachment = refs + nref;
                    REF2();
                }
                sub[i].pNext = &dsr[i];
            }
        }
#undef REF2
        if (nref >= 256 || npres >= 256) r->bad = 1;
        ci.pSubpasses = sub;
        ci.dependencyCount = vr_ru32(r);
        if (ci.dependencyCount > r->n - r->at) ci.dependencyCount = 0, r->bad = 1;
        VkSubpassDependency2 *dep = calloc(ci.dependencyCount ? ci.dependencyCount : 1, sizeof *dep);
        VkMemoryBarrier2 *dmb = calloc(ci.dependencyCount ? ci.dependencyCount : 1, sizeof *dmb);
        for (uint32_t i = 0; i < ci.dependencyCount && !r->bad; i++) {
            dep[i].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
            dep[i].srcSubpass = vr_ru32(r);
            dep[i].dstSubpass = vr_ru32(r);
            dep[i].srcStageMask = vr_ru32(r);
            dep[i].dstStageMask = vr_ru32(r);
            dep[i].srcAccessMask = vr_ru32(r);
            dep[i].dstAccessMask = vr_ru32(r);
            dep[i].dependencyFlags = vr_ru32(r);
            dep[i].viewOffset = (int32_t) vr_ru32(r);
            // sync2 の memory barrier が付いていれば、stage と access はそちらが使われる
            if (vr_ru32(r)) {
                dmb[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                dmb[i].srcStageMask = vr_ru64(r);
                dmb[i].srcAccessMask = vr_ru64(r);
                dmb[i].dstStageMask = vr_ru64(r);
                dmb[i].dstAccessMask = vr_ru64(r);
                dep[i].pNext = &dmb[i];
            }
        }
        ci.pDependencies = dep;
        uint32_t ncv = vr_ru32(r), cv[32];
        for (uint32_t i = 0; i < ncv && !r->bad; i++) { uint32_t x = vr_ru32(r); if (i < 32) cv[i] = x; }
        ci.correlatedViewMaskCount = ncv < 32 ? ncv : 32;
        ci.pCorrelatedViewMasks = cv;
        VkRenderPass rp;
        VkResult res = r->bad || !vkCreateRenderPass2 ? VK_ERROR_INITIALIZATION_FAILED : vkCreateRenderPass2(d, &ci, NULL, &rp);
        if (res == VK_SUCCESS) put(id, (uint64_t) rp, K_RPASS);
        else vlog("render pass 2 を作れない (%d)", res);
        free(att); free(sub); free(dsr); free(refs); free(pres); free(dep); free(dmb);
        return 1;
    }
    case VR_CREATE_QUERY_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkQueryPoolCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        ci.queryType = (VkQueryType) vr_ru32(r);
        ci.queryCount = vr_ru32(r);
        ci.pipelineStatistics = vr_ru32(r);
        VkQueryPool qp;
        VkResult res = vkCreateQueryPool(d, &ci, NULL, &qp);
        if (res == VK_SUCCESS) put(id, (uint64_t) qp, K_QPOOL);
        else vlog("query pool を作れない (%d)", res);
        return 1;
    }
    case VR_DESTROY_QUERY_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroyQueryPool(d, OBJ(VkQueryPool, id), NULL);
        del(id);
        return 1;
    }
    case VR_GET_QUERY_POOL_RESULTS: {
        VkDevice d = DEV(vr_ru64(r));
        VkQueryPool qp = OBJ(VkQueryPool, vr_ru64(r));
        uint32_t first = vr_ru32(r), n = vr_ru32(r);
        uint64_t size = vr_ru64(r), stride = vr_ru64(r);
        uint32_t flags = vr_ru32(r);
        if (!qp || !size || size > (64u << 20) || (uint64_t) n * stride > size) return reply(s, VK_ERROR_INITIALIZATION_FAILED);
        vr_reserve(&g_out, (size_t) size);
        memset(g_out.p, 0, (size_t) size);
        VkResult res = vkGetQueryPoolResults(d, qp, first, n, (size_t) size, g_out.p, stride, flags);
        g_out.n = (size_t) size;
        return reply(s, res);
    }
    case VR_RESET_QUERY_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        VkQueryPool qp = OBJ(VkQueryPool, vr_ru64(r));
        uint32_t first = vr_ru32(r), n = vr_ru32(r);
        if (qp && vkResetQueryPool) vkResetQueryPool(d, qp, first, n);
        return 1;
    }
    case VR_GET_BUFFER_ADDRESS: {
        VkDevice d = DEV(vr_ru64(r));
        VkBuffer b = OBJ(VkBuffer, vr_ru64(r));
        VkBufferDeviceAddressInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b };
        uint64_t a = b && vkGetBufferDeviceAddress && !r->bad ? vkGetBufferDeviceAddress(d, &bi) : 0;
        vr_u64(&g_out, a);
        return reply(s, 0);
    }
    case VR_CREATE_BUFFER_VIEW: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r), bid = vr_ru64(r);
        VkBufferViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = OBJ(VkBuffer, bid) };
        ci.format = (VkFormat) vr_ru32(r);
        ci.offset = vr_ru64(r);
        ci.range = vr_ru64(r);
        VkBufferView v;
        VkResult res = r->bad ? VK_ERROR_INITIALIZATION_FAILED : vkCreateBufferView(d, &ci, NULL, &v);
        if (res == VK_SUCCESS) {
            put(id, (uint64_t) v, K_BVIEW);
            ent *ve = find(id);
            if (ve) ve->aux = bid; // descriptor に入ったとき、もとの buffer を使っていると分かるように
        } else {
            vlog("buffer view を作れない (%d)", res);
        }
        return 1;
    }
    case VR_DESTROY_BUFFER_VIEW: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        ent *e = find(id);
        if (e) {
            vkDestroyBufferView(d, (VkBufferView) e->h, NULL);
            del(id);
        }
        return 1;
    }
    case VR_GET_IMAGE_SUBRESOURCE_LAYOUT: {
        VkDevice d = DEV(vr_ru64(r));
        VkImage im = OBJ(VkImage, vr_ru64(r));
        VkImageSubresource sr;
        sr.aspectMask = vr_ru32(r);
        sr.mipLevel = vr_ru32(r);
        sr.arrayLayer = vr_ru32(r);
        VkSubresourceLayout l = { 0 };
        if (!im || r->bad) return reply(s, VK_ERROR_INITIALIZATION_FAILED);
        vkGetImageSubresourceLayout(d, im, &sr, &l);
        vr_put(&g_out, &l, sizeof l);
        return reply(s, 0);
    }
    }
    vlog("知らない命令 %u", op);
    return 0;
}

static void serve_client(vr_sock c) {
    vr_nodelay(c);
    uint8_t *buf = NULL, *pk = NULL;
    size_t cap = 0, pcap = 0;
    for (;;) {
        uint32_t h[2];
        if (!vr_recv_all(c, h, 8)) break;
        if (h[1] > cap) { cap = h[1]; buf = realloc(buf, cap); }
        if (h[1] && !vr_recv_all(c, buf, h[1])) break;
        if (h[0] == VR_PACKED) {
            // いくつもの命令をまとめて縮めた物：ほどいて、中の命令を順に処理する
            uint32_t raw = 0;
            if (h[1] < 4) break;
            memcpy(&raw, buf, 4);
            if (raw > (512u << 20)) { vlog("縮めた命令が大きすぎる (%u)", raw); break; }
            if (raw > pcap) { pcap = raw; pk = realloc(pk, pcap); }
            int got = LZ4_decompress_safe((const char *) buf + 4, (char *) pk, (int) h[1] - 4, (int) raw);
            if (got != (int) raw) { vlog("縮めた命令をほどけない (%d)", got); break; }
            size_t pos = 0;
            int ok = 1;
            while (ok && pos + 8 <= raw) {
                uint32_t ih[2];
                memcpy(ih, pk + pos, 8);
                pos += 8;
                if (ih[1] > raw - pos) { ok = 0; break; }
                vr_rd r2 = { pk + pos, ih[1], 0, 0 };
                if (!handle(c, ih[0], &r2)) ok = 0;
                pos += ih[1];
            }
            if (!ok) break;
            continue;
        }
        vr_rd r = { buf, h[1], 0, 0 };
        if (!handle(c, h[0], &r)) break;
    }
    free(buf);
    free(pk);
    if (g_st.submits)
        vlog("この接続：submit %llu 回 %.0f ms・記録 %llu 回 %.0f ms・GPU を待った %llu 回 %.0f ms・後ろで止めた %.0f ms",
             (unsigned long long) g_st.submits, g_st.submit_ms, (unsigned long long) g_st.records, g_st.record_ms,
             (unsigned long long) g_st.waits, g_st.wait_ms, g_st.paused_ms);
    if (g_st.waits) {
        char line[400];
        int at = snprintf(line, sizeof line, "GPU を待った命令（命令の番号:回数）");
        for (int i = 0; i < 64 && at < (int) sizeof line - 16; i++)
            if (g_st.wait_by_op[i]) at += snprintf(line + at, sizeof line - at, " %d:%llu", i, (unsigned long long) g_st.wait_by_op[i]);
        vlog("%s", line);
    }
    memset(&g_st, 0, sizeof g_st);
    cleanup();
}

// iPhone のアプリから呼ぶ入口。戻ってこない
static void serve_one(vr_sock c, int remote) {
    serve_lock();
    g_cur_remote = remote;
    g_dbg_msgs = 0;
    vlog(remote ? "つながった（インターネット共有の口）" : "つながった");
    g_clients = 1;
    serve_client(c);
    g_clients = 0;
    g_cur_remote = 0;
    if (g_lost) {
#ifdef _WIN32
        // PC の代役（RTX）は、GPU を失うと同じプロセスでは Vulkan を作り直しても装置を作れない（-3 が続く）。
        // プロセスごと終わり、立て直しは外の台本に任せる
        vlog("GPU を失ったので終わる（代役は立て直しが要る）");
        exit(3);
#endif
        // GPU を失ったあとは、同じ Vulkan のままだと次の装置が作れないことがある
        vlog("GPU を失ったので、Vulkan を作り直す");
        dbg_stop();
        vkDestroyInstance(g_inst, NULL);
        g_inst = VK_NULL_HANDLE;
        g_lost = 0;
        if (!vk_init()) vlog("Vulkan を作り直せない");
    }
#ifdef _WIN32
    closesocket(c);
#else
    close(c);
#endif
    vlog("切れた");
    serve_unlock();
}

// インターネット共有の口で待つ糸。合言葉を確かめてから相手をする
#ifdef _WIN32
static DWORD WINAPI extra_thread(LPVOID p) {
#else
static void *extra_thread(void *p) {
#endif
    vr_sock ls = (vr_sock) (uintptr_t) p;
    for (;;) {
        vr_sock c = accept(ls, NULL, NULL);
        if (c == VR_BAD_SOCK) {
            if (ls != g_extra_ls) break; // 口を替えたので終わる
            sleep_ms(100);
            continue;
        }
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (!g_token_ok || !vr_link_auth(c, g_token)) {
            vlog("インターネット共有の口：合言葉が無い・違うので断った");
#ifdef _WIN32
            closesocket(c);
#else
            close(c);
#endif
            continue;
        }
        serve_one(c, 1);
    }
    return 0;
}

// インターネット共有の口（host）でも待ち始める。同じ口なら何もしない。違う口なら前の口を閉じて替える。
// 空の住所なら口を閉じる（インターネット共有が切れたとき）。Vulkan がまだ起きていなければ 2
int vkr_listen_extra(const char *host) {
    if (!host || !*host) {
        vr_sock old = g_extra_ls;
        g_extra_ls = VR_BAD_SOCK;
        g_extra_host[0] = 0;
        if (old != VR_BAD_SOCK) {
#ifdef _WIN32
            closesocket(old);
#else
            close(old);
#endif
            vlog("インターネット共有の口を閉じた");
        }
        return 0;
    }
    if (!strcmp(host, g_extra_host)) return 0;
    if (!g_inst) return 2; // Vulkan がまだ起きていない。あとで呼び直してもらう
    vr_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) g_main_port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 || bind(ls, (struct sockaddr *) &a, sizeof a) != 0 || listen(ls, 4) != 0) {
        vlog("インターネット共有の口 %s:%d で待てない", host, g_main_port);
#ifdef _WIN32
        closesocket(ls);
#else
        close(ls);
#endif
        return 1;
    }
    vr_sock old = g_extra_ls;
    g_extra_ls = ls;
    snprintf(g_extra_host, sizeof g_extra_host, "%s", host);
    if (old != VR_BAD_SOCK) {
#ifdef _WIN32
        closesocket(old);
#else
        close(old);
#endif
    }
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, extra_thread, (LPVOID) (uintptr_t) ls, 0, NULL);
    if (t) CloseHandle(t);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, extra_thread, (void *) (uintptr_t) ls) == 0) pthread_detach(t);
#endif
    vlog("インターネット共有の口：%s:%d でも待つ（合言葉つき）", host, g_main_port);
    return 0;
}

int vkr_serve(const char *host, int port) {
    if (!vk_init()) { vlog("Vulkan を起動できない"); return 1; }
    g_main_port = port;
    if (g_extra_env) vkr_listen_extra(g_extra_env);
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    vr_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, host, &a.sin_addr);
    if (bind(ls, (struct sockaddr *) &a, sizeof a) != 0 || listen(ls, 4) != 0) { vlog("%s:%d で待てない", host, port); return 1; }
    vlog("Vulkan の受け手：%s:%d で待っている", host, port);
    for (;;) {
        vr_sock c = accept(ls, NULL, NULL);
        if (c == VR_BAD_SOCK) continue;
#ifdef SO_NOSIGPIPE
        // PC が先に切ったときに書き込むと SIGPIPE でアプリごと落ちるので、止める
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        serve_one(c, 0);
    }
}

#ifdef VR_SERVER_MAIN
#ifdef _WIN32
// PC の代役が落ちたときに、どの関数で落ちたかを書く（横に置いた .pdb から dbghelp で名前を引く）
#include <dbghelp.h>
static void where(const char *tag, void *addr) {
    HMODULE m = NULL;
    char name[MAX_PATH] = "?";
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) addr, &m)) {
        fprintf(stderr, "%s %p\n", tag, addr);
        return;
    }
    GetModuleFileNameA(m, name, sizeof name);
    const char *b = strrchr(name, '\\');
    static int sym_ready;
    if (!sym_ready) { sym_ready = 1; SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize(GetCurrentProcess(), NULL, TRUE); }
    char sbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *) sbuf;
    memset(sbuf, 0, sizeof sbuf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0;
    int named = SymFromAddr(GetCurrentProcess(), (DWORD64) (uintptr_t) addr, &disp, si);
    fprintf(stderr, "%s %s+0x%llx %s+0x%llx\n", tag, b ? b + 1 : name, (unsigned long long) ((uintptr_t) addr - (uintptr_t) m),
            named ? si->Name : "?", (unsigned long long) disp);
}

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
    fprintf(stderr, "落ちた：例外 0x%08lx・命令 %u\n", code, g_cur_op);
    where("場所", ep->ExceptionRecord->ExceptionAddress);
    void *fr[40];
    USHORT n = RtlCaptureStackBackTrace(0, 40, fr, NULL);
    for (USHORT i = 0; i < n; i++) where("  呼び出し元", fr[i]);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    AddVectoredExceptionHandler(1, crash_handler);
    SetConsoleOutputCP(CP_UTF8);
    // PC で代役をするときは、画面取り込みなどの部品（暗黙のレイヤー）を挟まない。
    // OBS が起動している間、OBS の部品が vkCreateDevice から戻ってこなかった
    SetEnvironmentVariableA("VK_LOADER_LAYERS_DISABLE", "~implicit~");
#endif
    // 調べ用：VKR_EXTRA_LISTEN=住所 でインターネット共有の口の真似（VKR_LINK_TOKEN=16 進 32 文字の合言葉）
    const char *ex = getenv("VKR_EXTRA_LISTEN");
    if (ex && *ex) {
        g_main_port = argc > 2 ? atoi(argv[2]) : VR_DEFAULT_PORT;
        vkr_set_token(getenv("VKR_LINK_TOKEN"));
        if (!g_token_ok) vlog("VKR_LINK_TOKEN が無い・読めないので、インターネット共有の口は全部断る");
#ifdef _WIN32
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
#endif
        g_extra_env = ex; // Vulkan が起きてから開ける（vkr_serve の中）
    }
    return vkr_serve(argc > 1 ? argv[1] : "127.0.0.1", argc > 2 ? atoi(argv[2]) : VR_DEFAULT_PORT);
}
#endif
