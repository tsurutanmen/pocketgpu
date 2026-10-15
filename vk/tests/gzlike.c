// GZDoom（Vulkan）の GPU の使い方をまねた試験。窓は使わず、絵を作って検算の値を出す（tri.c と同じ流儀）。
//   gzlike [コマ数] [装置の名前の一部] [出す PPM のファイル名] [key=値 ...]
//   gzlike 100 iPhone out.ppm draws=640 tex=300 ring=4 inflight=2 readback=1 validate=1
// まねる所（調べ物：research1015/gzdoom_vk_requirements.md）：
//   - 毎コマ、descriptor pool を reset して set 0（Fixed）と set 1（HWBuffer）を作り直す
//   - set 1 は dynamic offset 付きの UBO 3 個（viewpoint・matrices・stream）と storage buffer 2 個。1 回の draw で offset を 3 個付けて bind
//   - 1 コマに数百回の draw。draw ごとに set 1 を bind し直し、set 0 と set 2（材質）も bind。3 つまとめて 1 回で bind する draw もある
//   - 数 MB の UBO の ring を、コマごとに区間を替えて回す（matrices・stream。viewpoint は小さい ring）
//   - 材質の sampled image を数百枚。1 つの set に 2 枚。pool は sampler 2000・set 1000 の単位で、起動時に 1 回作る
//   - push constants を draw ごとに 128 バイト（最低保証の上限）
//   - 頂点 buffer・index buffer・scissor を draw ごとに替える。pipeline を何度も bind し直す（independentBlend 付きの色 2 枚）
//   - samplerAnisotropy・independentBlend・fragmentStoresAndAtomics・multiDrawIndirect を有効にする（あれば）
// 絵は整数の計算だけで決まる。同じ計算を CPU でもして、毎コマ、GPU の結果と全部の画素を比べる。
// 窓の無い普通の C なので、Windows（vulkan-1.dll）と Android・Linux（libvulkan.so）の両方で組める。表示は英語（テスターに渡すため）。
// 終了コード：0 全部合った・1 食い違い・失敗・2 この装置では試せない（必須の機能が無い）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <time.h>
#endif
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "gzlike_vert_spv.h"
#include "gzlike_frag_spv.h"

// ---- 関数の表（X マクロ） ----
#define INST_FUNCS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) X(vkDestroyInstance)
#define DEV_FUNCS(X) \
    X(vkGetDeviceQueue) X(vkDestroyDevice) X(vkDeviceWaitIdle) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) \
    X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateRenderPass) X(vkDestroyRenderPass) \
    X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDrawIndexed) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdPipelineBarrier) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) X(vkQueueWaitIdle)
#define X(n) static PFN_##n n;
INST_FUNCS(X)
DEV_FUNCS(X)
#undef X
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkEnumerateInstanceLayerProperties vkEnumerateInstanceLayerProperties;
static PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties;
static PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT;
static PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT;

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, (int) r_, __LINE__); fflush(stdout); exit(1); } } while (0)

// ---- 場面の大きさ ----
enum { W = 320, H = 320, CELL = 16, GRID = 20, NCELL = GRID * GRID, TS = 16 };
// storage buffer（コマごとの区画）：lights（先頭に画素の数を数える欄）と bones
enum { LIGHT_SZ = 16 + 256 * 16, BONE_OFF = 4352, BONE_SZ = 256 * 16, REGION = 8448 };
enum { VP_BLOCK = 256, MAT_BLOCK = 192, STR_BLOCK = 256, PC_SIZE = 128, VP_EVERY = 32, NQUAD = 16 };

static VkDevice dev;
static VkPhysicalDevice pd;
static VkPhysicalDeviceMemoryProperties mp;
static VkPhysicalDeviceProperties props;
static VkQueue q;
static uint32_t qf;
static int g_invalid, g_aniso = 1, g_draws = 640, g_tex = 300, g_ring_mb = 4, g_inflight = 2, g_readback = 1, g_quiet = 0;

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) return t;
    return UINT32_MAX;
}
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// ---- 作った物の控え（最後に逆順で壊す。validation layer の「壊し忘れ」を出さないため） ----
enum { K_BUF, K_IMG, K_VIEW, K_MEM, K_SAMP, K_SHADER, K_DSL, K_PL, K_RP, K_FB, K_PIPE, K_DPOOL, K_CPOOL, K_FENCE };
typedef struct { int k; uint64_t h; } obj;
static obj *objs;
static size_t nobj, capobj;
static void keep(int k, uint64_t h) {
    if (nobj == capobj) { capobj = capobj ? capobj * 2 : 1024; objs = realloc(objs, capobj * sizeof *objs); }
    objs[nobj++] = (obj) { k, h };
}
#define KEEP(k, h) keep(k, (uint64_t) (uintptr_t) (h))
static void destroy_all(void) {
    for (size_t i = nobj; i-- > 0;) {
        uint64_t h = objs[i].h;
        switch (objs[i].k) {
        case K_BUF: vkDestroyBuffer(dev, (VkBuffer) (uintptr_t) h, NULL); break;
        case K_IMG: vkDestroyImage(dev, (VkImage) (uintptr_t) h, NULL); break;
        case K_VIEW: vkDestroyImageView(dev, (VkImageView) (uintptr_t) h, NULL); break;
        case K_MEM: vkFreeMemory(dev, (VkDeviceMemory) (uintptr_t) h, NULL); break;
        case K_SAMP: vkDestroySampler(dev, (VkSampler) (uintptr_t) h, NULL); break;
        case K_SHADER: vkDestroyShaderModule(dev, (VkShaderModule) (uintptr_t) h, NULL); break;
        case K_DSL: vkDestroyDescriptorSetLayout(dev, (VkDescriptorSetLayout) (uintptr_t) h, NULL); break;
        case K_PL: vkDestroyPipelineLayout(dev, (VkPipelineLayout) (uintptr_t) h, NULL); break;
        case K_RP: vkDestroyRenderPass(dev, (VkRenderPass) (uintptr_t) h, NULL); break;
        case K_FB: vkDestroyFramebuffer(dev, (VkFramebuffer) (uintptr_t) h, NULL); break;
        case K_PIPE: vkDestroyPipeline(dev, (VkPipeline) (uintptr_t) h, NULL); break;
        case K_DPOOL: vkDestroyDescriptorPool(dev, (VkDescriptorPool) (uintptr_t) h, NULL); break;
        case K_CPOOL: vkDestroyCommandPool(dev, (VkCommandPool) (uintptr_t) h, NULL); break;
        case K_FENCE: vkDestroyFence(dev, (VkFence) (uintptr_t) h, NULL); break;
        }
    }
    nobj = 0;
}

// host=1：CPU から見えるメモリ（書き込みは flush する）。host=0：GPU のメモリ
static void mk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, int host, VkBuffer *b, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK(vkCreateBuffer(dev, &bci, NULL, b));
    KEEP(K_BUF, *b);
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, *b, &r);
    uint32_t t;
    if (host) {
        t = find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (t == UINT32_MAX) t = find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    } else {
        t = find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (t == UINT32_MAX) t = find_mem(r.memoryTypeBits, 0);
    }
    if (t == UINT32_MAX) { printf("FAIL no memory type for buffer\n"); exit(1); }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = t };
    VK(vkAllocateMemory(dev, &mai, NULL, m));
    KEEP(K_MEM, *m);
    VK(vkBindBufferMemory(dev, *b, *m, 0));
    if (map) VK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, map));
}
// 書いた範囲を flush する（coherent でも呼んでよい）。範囲は atom の大きさに揃える
static void flush_range(VkDeviceMemory m, uint64_t off, uint64_t len, uint64_t total) {
    uint64_t atom = props.limits.nonCoherentAtomSize ? props.limits.nonCoherentAtomSize : 1;
    uint64_t a = off / atom * atom, e = align_up(off + len, atom);
    VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = m, .offset = a, .size = e >= total ? VK_WHOLE_SIZE : e - a };
    VK(vkFlushMappedMemoryRanges(dev, 1, &mr));
}
static void mk_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkImage *img, VkImageView *view, VkDeviceMemory *mem) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VK(vkCreateImage(dev, &ici, NULL, img));
    KEEP(K_IMG, *img);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, *img, &r);
    uint32_t t = find_mem(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = find_mem(r.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = r.size, .memoryTypeIndex = t };
    VK(vkAllocateMemory(dev, &mai, NULL, mem));
    KEEP(K_MEM, *mem);
    VK(vkBindImageMemory(dev, *img, *mem, 0));
    VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCreateImageView(dev, &ivci, NULL, view));
    KEEP(K_VIEW, *view);
}

// ---- 場面の決まり（GPU に渡す物と CPU の計算の両方がこれを使う） ----
// 材質の画像 t の画素（x,y は 0〜15）
static void texel(int t, int x, int y, int *p) {
    p[0] = (t * 37 + x * 16 + 3) & 255;
    p[1] = (t * 91 + y * 16 + 7) & 255;
    p[2] = ((x ^ y) * 16 + t) & 255;
    p[3] = 255;
}
typedef struct { int cell, layer, cx, cy, x0, y0, w, h, t0, t1, tint[3], mode, li, bi, bp, fbits; } draw_t;
static draw_t dparams(int k, int i) {
    draw_t d;
    d.layer = i / NCELL; d.cell = i % NCELL; d.cx = d.cell % GRID; d.cy = d.cell / GRID;
    d.x0 = d.cx * CELL + (d.layer ? 4 : 0); d.y0 = d.cy * CELL + (d.layer ? 4 : 0);
    d.w = d.h = d.layer ? 8 : 16;
    d.t0 = (d.cell * 7 + k * 3 + d.layer * 11) % g_tex;
    d.t1 = (d.t0 * 5 + 1) % g_tex;
    d.tint[0] = (i * 5 + k) & 15; d.tint[1] = (i * 3 + 2 * k) & 15; d.tint[2] = (i + k * 7) & 15;
    d.mode = i & 3;
    d.li = (i * 13 + k) & 255; d.bi = (i * 7 + k) & 255; d.bp = (i + k) & 3; d.fbits = (k * 37) & 15;
    return d;
}
// 全部の draw を CPU で計算する。戻り値は画素の数（数え欄と比べる）
static uint64_t ref_frame(int k, uint8_t *o0, uint8_t *o1) {
    for (int i = 0; i < W * H; i++) {
        o0[i * 4] = o0[i * 4 + 1] = o0[i * 4 + 2] = 16; o0[i * 4 + 3] = 255;
        o1[i * 4] = 5; o1[i * 4 + 1] = 6; o1[i * 4 + 2] = 7; o1[i * 4 + 3] = 255;
    }
    uint64_t frags = 0;
    for (int i = 0; i < g_draws; i++) {
        draw_t d = dparams(k, i);
        for (int y = d.y0; y < d.y0 + d.h; y++)
            for (int x = d.x0; x < d.x0 + d.w; x++) {
                int lx = x - d.cx * CELL, ly = y - d.cy * CELL, c0[4], c1[4], lmv[4], smv[4];
                texel(d.t0, lx, ly, c0);
                texel(d.t1, lx, ly, c1);
                texel(g_tex + (k & 1), x & 15, y & 15, lmv);
                texel(g_tex + ((k + 1) & 1), x & 15, y & 15, smv);
                int b[3];
                for (int c = 0; c < 3; c++) b[c] = ((3 * c0[c] + c1[c]) >> 2) + d.tint[c];
                b[0] += ((d.li * 3) & 7);                    // lights[li].rgb = ((j*3)&7, (j*5)&7, (j*7)&7)
                b[1] += ((d.li * 5) & 7);
                b[2] += ((d.li * 7) & 7);
                b[0] += (d.bi * 3) & 15;                     // bones[bi].w
                b[2] += d.bp;                                // push constants
                b[1] += lmv[0] & 3;                          // lightmap（set 0）
                b[0] += smv[1] & 3;                          // shadowmap（set 0）
                if (d.mode == 1) { for (int c = 0; c < 3; c++) b[c] ^= d.fbits; }
                else if (d.mode == 2) { for (int c = 0; c < 3; c++) b[c] += d.fbits; }
                else if (d.mode == 3) { int t = b[0]; b[0] = b[2]; b[2] = t; }
                uint8_t *p0 = o0 + (y * W + x) * 4, *p1 = o1 + (y * W + x) * 4;
                for (int c = 0; c < 3; c++) { p0[c] = (uint8_t) (b[c] > 255 ? 255 : b[c]); p1[c] = (uint8_t) c1[c]; }
                p0[3] = p1[3] = 255;
                frags++;
            }
    }
    return frags;
}
static uint32_t fnv(uint32_t h, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// ---- validation layer の知らせ ----
static int g_vmsg, g_verr;
static VKAPI_ATTR VkBool32 VKAPI_CALL dbg_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void) type; (void) u;
    if (sev & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
        g_vmsg++;
        if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_verr++;
        if (g_vmsg <= 30) printf("VALIDATION %s: %.1500s\n", (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning", d->pMessage);
    }
    return VK_FALSE;
}

// ---- コマごとの物 ----
typedef struct {
    VkImage img[2]; VkImageView view[2]; VkDeviceMemory imem[2]; VkFramebuffer fb;
    VkBuffer rb[2]; VkDeviceMemory rbmem[2]; uint8_t *rbmap[2];
    VkDescriptorPool dpool; VkCommandPool cpool; VkCommandBuffer cb; VkFence fence;
    int busy, frame, did_rb;
} fctx;

static fctx *fc_;
static uint8_t *ref0, *ref1;
static uint8_t *ssbo_map;
static uint32_t h_gpu = 2166136261u, h_ref = 2166136261u;
static int g_bad, g_checked, g_cnt_bad;
static uint64_t g_draw_total, g_bind_total, g_push_total;
static const char *g_out;        // 絵の出し先。食い違ったコマの GPU の絵は <出し先>.gpu<コマ>.ppm にも書く

static void complete(fctx *fc) {
    VK(vkWaitForFences(dev, 1, &fc->fence, VK_TRUE, UINT64_MAX));
    VK(vkResetFences(dev, 1, &fc->fence));
    fc->busy = 0;
    int k = fc->frame, f = (int) (fc - fc_);
    uint64_t want = 0;
    if (fc->did_rb) want = ref_frame(k, ref0, ref1);
    else {
        // 絵を持ち帰らない回でも、画素の数だけは数えられる
        want = 0;
        for (int i = 0; i < g_draws; i++) { draw_t d = dparams(k, i); want += (uint64_t) d.w * d.h; }
    }
    uint32_t got = *(volatile uint32_t *) (ssbo_map + (size_t) f * REGION);
    if (got != (uint32_t) want) {
        g_cnt_bad++;
        if (g_cnt_bad <= 5) printf("frame %d: fragment counter %u, expected %u\n", k, got, (unsigned) want);
    }
    if (fc->did_rb) {
        VkMappedMemoryRange mr[2];
        for (int j = 0; j < 2; j++) mr[j] = (VkMappedMemoryRange) { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = fc->rbmem[j], .offset = 0, .size = VK_WHOLE_SIZE };
        VK(vkInvalidateMappedMemoryRanges(dev, 2, mr));
        int diff = 0;
        for (int j = 0; j < 2; j++) {
            const uint8_t *g = fc->rbmap[j], *r = j ? ref1 : ref0;
            h_gpu = fnv(h_gpu, g, W * H * 4);
            h_ref = fnv(h_ref, r, W * H * 4);
            if (memcmp(g, r, W * H * 4)) {
                int n = 0, first = -1;
                for (int i = 0; i < W * H * 4; i++) if (g[i] != r[i]) { if (first < 0) first = i; n++; }
                diff++;
                if (g_out && j == 0 && g_bad < 2) {
                    char nm[512];
                    snprintf(nm, sizeof nm, "%s.gpu%d.ppm", g_out, k);
                    FILE *fp = fopen(nm, "wb");
                    if (fp) {
                        fprintf(fp, "P6\n%d %d\n255\n", W, H);
                        for (int i = 0; i < W * H; i++) fwrite(g + i * 4, 1, 3, fp);
                        fclose(fp);
                        printf("  wrote the GPU picture of frame %d to %s\n", k, nm);
                    }
                }
                if (g_bad < 5)
                    printf("frame %d attachment %d: %d bytes differ; first at (%d,%d) channel %d: got %d, expected %d\n", k, j, n,
                           (first / 4) % W, (first / 4) / W, first & 3, g[first], r[first]);
            }
        }
        g_checked++;
        if (diff || got != (uint32_t) want) g_bad++;
    } else if (got != (uint32_t) want) g_bad++;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int frames = 50, validate = 0, pos = 0;
    const char *want_dev = "", *out = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *eq = strchr(a, '=');
        if (eq) {
            int v = atoi(eq + 1);
            if (!strncmp(a, "draws=", 6)) g_draws = v;
            else if (!strncmp(a, "tex=", 4)) g_tex = v;
            else if (!strncmp(a, "ring=", 5)) g_ring_mb = v;
            else if (!strncmp(a, "inflight=", 9)) g_inflight = v;
            else if (!strncmp(a, "readback=", 9)) g_readback = v;
            else if (!strncmp(a, "validate=", 9)) validate = v;
            else if (!strncmp(a, "quiet=", 6)) g_quiet = v;
            else if (!strncmp(a, "aniso=", 6)) g_aniso = v;
            else if (!strncmp(a, "invalid=", 8)) g_invalid = v;   // わざと間違った呼び出しを 1 回する。validation layer が効いているかの確認用
            else { printf("unknown option %s\n", a); return 1; }
        } else if (pos == 0) { frames = atoi(a); pos++; }
        else if (pos == 1) { want_dev = a; pos++; }
        else if (pos == 2) { out = a; pos++; }
    }
    g_out = out;
    if (g_draws < 1) g_draws = 1;
    if (g_draws > 2 * NCELL) g_draws = 2 * NCELL;
    if (g_tex < 2) g_tex = 2;
    if (g_tex > 1000) g_tex = 1000;     // texture の pool は set 1000 個の単位
    if (g_inflight < 1) g_inflight = 1;
    if (g_inflight > 4) g_inflight = 4;
    if (g_readback < 1) g_readback = 1;
    if (g_ring_mb < 1) g_ring_mb = 1;
    if (frames < 1) frames = 1;

    // ---- Vulkan を読み込む ----
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    gipa = lib ? (PFN_vkGetInstanceProcAddr) GetProcAddress(lib, "vkGetInstanceProcAddr") : NULL;
#else
    void *lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOW);
    gipa = lib ? (PFN_vkGetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr") : NULL;
#endif
    if (!gipa) { printf("FAIL cannot load the Vulkan loader\n"); return 1; }
    vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    vkEnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties) gipa(NULL, "vkEnumerateInstanceLayerProperties");
    vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties) gipa(NULL, "vkEnumerateInstanceExtensionProperties");

    const char *layers[1] = { "VK_LAYER_KHRONOS_validation" }, *iexts[1] = { "VK_EXT_debug_utils" };
    int use_layer = 0, use_dbg = 0;
    if (validate) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, NULL);
        VkLayerProperties *lp = calloc(n ? n : 1, sizeof *lp);
        vkEnumerateInstanceLayerProperties(&n, lp);
        for (uint32_t i = 0; i < n; i++) if (!strcmp(lp[i].layerName, layers[0])) use_layer = 1;
        free(lp);
        n = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties *ep = calloc(n ? n : 1, sizeof *ep);
        vkEnumerateInstanceExtensionProperties(NULL, &n, ep);
        for (uint32_t i = 0; i < n; i++) if (!strcmp(ep[i].extensionName, iexts[0])) use_dbg = 1;
        free(ep);
        if (!use_layer) printf("validate=1 was given but VK_LAYER_KHRONOS_validation is not available (set VK_LAYER_PATH?); running without it\n");
        else if (!use_dbg) printf("validation layer found but VK_EXT_debug_utils is not available; messages will go to the layer's own output\n");
    }
    VkInstance inst = VK_NULL_HANDLE;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "gzlike", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledLayerCount = use_layer ? 1 : 0, .ppEnabledLayerNames = layers, .enabledExtensionCount = (use_layer && use_dbg) ? 1 : 0, .ppEnabledExtensionNames = iexts };
    VkResult ir = vkCreateInstance(&ici, NULL, &inst);
    if (ir != VK_SUCCESS) {
        // ここで落ちるのはドライバより前（ローダー・layer）か、メモリ不足のことが多い。切り分けのために、layer と拡張を付けずにもう 1 回作る
        printf("FAIL vkCreateInstance(&ici, NULL, &inst) = %d (line %d)\n", (int) ir, __LINE__);
        if (ici.enabledLayerCount || ici.enabledExtensionCount) {
            ici.enabledLayerCount = 0;
            ici.enabledExtensionCount = 0;
            VkResult r2 = vkCreateInstance(&ici, NULL, &inst);
            printf("  retry without layers and extensions = %d%s\n", (int) r2, r2 == VK_SUCCESS ? " (so the layer / extension was the problem)" : "");
            if (r2 == VK_SUCCESS) { use_layer = use_dbg = 0; ir = r2; }
        }
        if (ir != VK_SUCCESS) {
            printf("  -1 is out of memory, -9 is no usable driver (check VK_DRIVER_FILES). Set VK_LOADER_DEBUG=error,warn,driver to see why the loader refused.\n");
            fflush(stdout);
            return 1;
        }
    }
#define IFN(name) name = (PFN_##name) gipa(inst, #name)
#define X(n) IFN(n);
    INST_FUNCS(X)
#undef X
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (use_layer && use_dbg) {
        IFN(vkCreateDebugUtilsMessengerEXT);
        IFN(vkDestroyDebugUtilsMessengerEXT);
        VkDebugUtilsMessengerCreateInfoEXT mci = { .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = dbg_cb };
        VK(vkCreateDebugUtilsMessengerEXT(inst, &mci, NULL, &messenger));
    }

    // ---- 装置を選ぶ ----
    uint32_t nd = 16;
    VkPhysicalDevice pds[16];
    VkResult er = vkEnumeratePhysicalDevices(inst, &nd, pds);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || nd == 0) { printf("FAIL no Vulkan device (vkEnumeratePhysicalDevices = %d, %u devices). Is the receiver running and reachable (IPHONE_VK)? The Vulkan loader answers -3 when the driver has no device.\n", (int) er, (er == VK_SUCCESS || er == VK_INCOMPLETE) ? nd : 0u); return 1; }
    pd = pds[0];
    if (*want_dev) {
        int found = 0;
        for (uint32_t i = 0; i < nd; i++) {
            vkGetPhysicalDeviceProperties(pds[i], &props);
            if (strstr(props.deviceName, want_dev)) { pd = pds[i]; found = 1; break; }
        }
        if (!found) printf("no device name contains \"%s\"; using device 0\n", want_dev);
    }
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceFeatures pf;
    vkGetPhysicalDeviceFeatures(pd, &pf);
    printf("device: %s (Vulkan %u.%u.%u, vendor 0x%04x, driver 0x%x)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.vendorID, props.driverVersion);
    printf("limits: minUniformBufferOffsetAlignment %llu, maxUniformBufferRange %u, maxPushConstantsSize %u, maxDescriptorSetUniformBuffersDynamic %u, maxPerStageDescriptorSampledImages %u\n",
           (unsigned long long) props.limits.minUniformBufferOffsetAlignment, props.limits.maxUniformBufferRange, props.limits.maxPushConstantsSize,
           props.limits.maxDescriptorSetUniformBuffersDynamic, props.limits.maxPerStageDescriptorSampledImages);
    printf("features: samplerAnisotropy %d, independentBlend %d, fragmentStoresAndAtomics %d, multiDrawIndirect %d\n",
           pf.samplerAnisotropy, pf.independentBlend, pf.fragmentStoresAndAtomics, pf.multiDrawIndirect);
    if (!pf.independentBlend || !pf.fragmentStoresAndAtomics) { printf("SKIP: this device lacks independentBlend or fragmentStoresAndAtomics (GZDoom needs both)\n"); return 2; }
    if (props.limits.maxPushConstantsSize < PC_SIZE || props.limits.maxDescriptorSetUniformBuffersDynamic < 3 || props.limits.maxUniformBufferRange < MAT_BLOCK) {
        printf("SKIP: a limit is below what this test needs\n");
        return 2;
    }

    uint32_t nq = 16;
    VkQueueFamilyProperties qps[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qps);
    qf = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
    if (qf == UINT32_MAX) { printf("FAIL no graphics queue\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures ef = { .samplerAnisotropy = pf.samplerAnisotropy, .independentBlend = VK_TRUE, .fragmentStoresAndAtomics = VK_TRUE, .multiDrawIndirect = pf.multiDrawIndirect };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &ef };
    VK(vkCreateDevice(pd, &dci, NULL, &dev));
#define DFN(name) name = (PFN_##name) vkGetDeviceProcAddr(dev, #name);
#define X(n) DFN(n)
    DEV_FUNCS(X)
#undef X
    vkGetDeviceQueue(dev, qf, 0, &q);

    // ---- 大きさを決める ----
    uint64_t ubo_al = props.limits.minUniformBufferOffsetAlignment ? props.limits.minUniformBufferOffsetAlignment : 1;
    uint64_t sbo_al = props.limits.minStorageBufferOffsetAlignment ? props.limits.minStorageBufferOffsetAlignment : 1;
    if (sbo_al > 256) { printf("SKIP: minStorageBufferOffsetAlignment %llu is larger than this test supports\n", (unsigned long long) sbo_al); return 2; }
    uint64_t vp_stride = align_up(VP_BLOCK, ubo_al), mat_stride = align_up(MAT_BLOCK, ubo_al), str_stride = align_up(STR_BLOCK, ubo_al);
    uint64_t ring_bytes = (uint64_t) g_ring_mb << 20;
    int nseg = (int) (ring_bytes / mat_stride / (uint64_t) g_draws);       // ring を何区間に分けるか。コマ k は区間 k % nseg を使う
    if (nseg < g_inflight) { printf("ring=%d MB is too small for draws=%d inflight=%d\n", g_ring_mb, g_draws, g_inflight); return 1; }
    int vp_per_frame = g_draws / VP_EVERY + 1;
    uint64_t mat_size = (uint64_t) nseg * g_draws * mat_stride, str_size = (uint64_t) nseg * g_draws * str_stride, vp_size = (uint64_t) nseg * vp_per_frame * vp_stride;
    printf("setup: %d x %d, %d draws/frame, %d materials (+2 maps), in flight %d, ring: matrices %.2f MB, stream %.2f MB, viewpoint %.2f MB (%d segments)\n",
           W, H, g_draws, g_tex, g_inflight, mat_size / 1048576.0, str_size / 1048576.0, vp_size / 1048576.0, nseg);

    // ---- buffer：ring 3 本・storage・頂点と index・材質の元・持ち帰り ----
    VkBuffer vp_buf, mat_buf, str_buf, sbo_buf, vb_buf, stg_buf;
    VkDeviceMemory vp_mem, mat_mem, str_mem, sbo_mem, vb_mem, stg_mem;
    uint8_t *vp_map, *mat_map, *str_map, *vb_map, *stg_map;
    mk_buffer(vp_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1, &vp_buf, &vp_mem, (void **) &vp_map);
    mk_buffer(mat_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1, &mat_buf, &mat_mem, (void **) &mat_map);
    mk_buffer(str_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 1, &str_buf, &str_mem, (void **) &str_map);
    uint64_t sbo_size = (uint64_t) g_inflight * REGION;
    mk_buffer(sbo_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &sbo_buf, &sbo_mem, (void **) &ssbo_map);
    memset(ssbo_map, 0, sbo_size);
    for (int f = 0; f < g_inflight; f++) {
        int32_t *lights = (int32_t *) (ssbo_map + (size_t) f * REGION + 16), *bones = (int32_t *) (ssbo_map + (size_t) f * REGION + BONE_OFF);
        for (int j = 0; j < 256; j++) {
            lights[j * 4 + 0] = (j * 3) & 7; lights[j * 4 + 1] = (j * 5) & 7; lights[j * 4 + 2] = (j * 7) & 7; lights[j * 4 + 3] = 0;
            bones[j * 4 + 0] = 0; bones[j * 4 + 1] = 0; bones[j * 4 + 2] = 0; bones[j * 4 + 3] = (j * 3) & 15;
        }
    }
    flush_range(sbo_mem, 0, sbo_size, sbo_size);
    // 頂点（1 枚の四角を NQUAD 個並べる）と index
    uint64_t vb_bytes = NQUAD * 4 * 8, ib_off = vb_bytes, vb_size = vb_bytes + 16;
    mk_buffer(vb_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 1, &vb_buf, &vb_mem, (void **) &vb_map);
    {
        float *v = (float *) vb_map;
        for (int c = 0; c < NQUAD; c++) {
            const float cx[4] = { 0, 1, 1, 0 }, cy[4] = { 0, 0, 1, 1 };
            for (int j = 0; j < 4; j++) { v[(c * 4 + j) * 2] = cx[j]; v[(c * 4 + j) * 2 + 1] = cy[j]; }
        }
        const uint16_t idx[6] = { 0, 1, 2, 0, 2, 3 };
        memcpy(vb_map + ib_off, idx, sizeof idx);
        flush_range(vb_mem, 0, vb_size, vb_size);
    }

    // ---- 材質の画像（g_tex 枚＋ lightmap・shadowmap の 2 枚）。1 つの memory にまとめる ----
    int ntex = g_tex + 2;
    VkImage *timg = calloc((size_t) ntex, sizeof *timg);
    VkImageView *tview = calloc((size_t) ntex, sizeof *tview);
    uint64_t *toff = calloc((size_t) ntex, sizeof *toff);
    uint64_t tot = 0;
    uint32_t tbits = ~0u;
    for (int t = 0; t < ntex; t++) {
        VkImageCreateInfo ici2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { TS, TS, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VK(vkCreateImage(dev, &ici2, NULL, &timg[t]));
        KEEP(K_IMG, timg[t]);
        VkMemoryRequirements r;
        vkGetImageMemoryRequirements(dev, timg[t], &r);
        tot = align_up(tot, r.alignment);
        toff[t] = tot;
        tot += r.size;
        tbits &= r.memoryTypeBits;
    }
    uint32_t tt = find_mem(tbits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (tt == UINT32_MAX) tt = find_mem(tbits, 0);
    if (tt == UINT32_MAX) { printf("FAIL no common memory type for the material images\n"); return 1; }
    VkDeviceMemory tmem;
    VkMemoryAllocateInfo tmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = tot, .memoryTypeIndex = tt };
    VK(vkAllocateMemory(dev, &tmai, NULL, &tmem));
    KEEP(K_MEM, tmem);
    for (int t = 0; t < ntex; t++) {
        VK(vkBindImageMemory(dev, timg[t], tmem, toff[t]));
        VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = timg[t], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VK(vkCreateImageView(dev, &ivci, NULL, &tview[t]));
        KEEP(K_VIEW, tview[t]);
    }
    mk_buffer((uint64_t) ntex * TS * TS * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1, &stg_buf, &stg_mem, (void **) &stg_map);
    for (int t = 0; t < ntex; t++)
        for (int y = 0; y < TS; y++)
            for (int x = 0; x < TS; x++) {
                int p[4];
                texel(t, x, y, p);
                for (int c = 0; c < 4; c++) stg_map[((size_t) t * TS * TS + y * TS + x) * 4 + c] = (uint8_t) p[c];
            }
    flush_range(stg_mem, 0, (uint64_t) ntex * TS * TS * 4, (uint64_t) ntex * TS * TS * 4);

    // ---- sampler 2 つ（anisotropy を使う物と使わない物） ----
    // anisotropy を有効にした sampler で texture() すると、結果は実装ごとに違ってよい（lavapipe は nearest でも違う値を返した）。
    // 検算の値をぶらさないため、texture() で読む binding 0 は使わない sampler B、texelFetch で読む物（sampler は結果に効かない）に sampler A を結ぶ
    VkSampler sampA, sampB;
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .anisotropyEnable = (pf.samplerAnisotropy && g_aniso) ? VK_TRUE : VK_FALSE,
        .maxAnisotropy = pf.samplerAnisotropy ? (props.limits.maxSamplerAnisotropy < 8.0f ? props.limits.maxSamplerAnisotropy : 8.0f) : 1.0f, .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK };
    VK(vkCreateSampler(dev, &sci, NULL, &sampA));
    KEEP(K_SAMP, sampA);
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxAnisotropy = 1.0f;
    VK(vkCreateSampler(dev, &sci, NULL, &sampB));
    KEEP(K_SAMP, sampB);

    // ---- descriptor set layout（GZDoom と同じ並び）と pipeline layout ----
    VkDescriptorSetLayoutBinding b0[2], b1[5], b2[2];
    for (int i = 0; i < 2; i++) b0[i] = (VkDescriptorSetLayoutBinding) { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (int i = 0; i < 3; i++) b1[i] = (VkDescriptorSetLayoutBinding) { i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b1[3] = (VkDescriptorSetLayoutBinding) { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    b1[4] = (VkDescriptorSetLayoutBinding) { 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    for (int i = 0; i < 2; i++) b2[i] = (VkDescriptorSetLayoutBinding) { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayout dsl[3];
    VkDescriptorSetLayoutCreateInfo dlci[3] = {
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b0 },
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = b1 },
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b2 } };
    for (int i = 0; i < 3; i++) { VK(vkCreateDescriptorSetLayout(dev, &dlci[i], NULL, &dsl[i])); KEEP(K_DSL, dsl[i]); }
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, PC_SIZE };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3, .pSetLayouts = dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout pl;
    VK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    KEEP(K_PL, pl);

    // ---- 材質の set：pool は sampler 2000 個・set 1000 個の単位で、起動時に 1 回。材質 t の set は {t, (5t+1) % g_tex} ----
    VkDescriptorPoolSize tps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2000 };
    VkDescriptorPoolCreateInfo tdpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1000, .poolSizeCount = 1, .pPoolSizes = &tps };
    VkDescriptorPool tpool;
    VK(vkCreateDescriptorPool(dev, &tdpi, NULL, &tpool));
    KEEP(K_DPOOL, tpool);
    VkDescriptorSetLayout *tl = malloc((size_t) g_tex * sizeof *tl);
    for (int t = 0; t < g_tex; t++) tl[t] = dsl[2];
    VkDescriptorSet *tsets = calloc((size_t) g_tex, sizeof *tsets);
    VkDescriptorSetAllocateInfo tdai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = tpool, .descriptorSetCount = (uint32_t) g_tex, .pSetLayouts = tl };
    VK(vkAllocateDescriptorSets(dev, &tdai, tsets));
    {
        VkWriteDescriptorSet *w = calloc((size_t) g_tex * 2, sizeof *w);
        VkDescriptorImageInfo *ii = calloc((size_t) g_tex * 2, sizeof *ii);
        for (int t = 0; t < g_tex; t++)
            for (int l = 0; l < 2; l++) {
                int n = t * 2 + l, tx = l ? (t * 5 + 1) % g_tex : t;
                ii[n] = (VkDescriptorImageInfo) { l ? sampA : sampB, tview[tx], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                w[n] = (VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = tsets[t], .dstBinding = (uint32_t) l, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[n] };
            }
        vkUpdateDescriptorSets(dev, (uint32_t) g_tex * 2, w, 0, NULL);
        free(w); free(ii);
    }

    // ---- 描き方・pipeline（色 2 枚。独立した blend で、片方だけ blend を有効にした物と、どちらも切った物の 2 つ） ----
    VkAttachmentDescription ad[2];
    for (int i = 0; i < 2; i++)
        ad[i] = (VkAttachmentDescription) { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ar[2] = { { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 2, .pColorAttachments = ar };
    VkSubpassDependency dep[2] = {
        { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT, .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT },
        { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT } };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = ad, .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 2, .pDependencies = dep };
    VkRenderPass rp;
    VK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    KEEP(K_RP, rp);
    VkShaderModuleCreateInfo vsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof gzlike_vert_spv, .pCode = gzlike_vert_spv };
    VkShaderModuleCreateInfo fsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof gzlike_frag_spv, .pCode = gzlike_frag_spv };
    VkShaderModule vsm, fsm;
    VK(vkCreateShaderModule(dev, &vsci, NULL, &vsm));
    KEEP(K_SHADER, vsm);
    VK(vkCreateShaderModule(dev, &fsci, NULL, &fsm));
    KEEP(K_SHADER, fsm);
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
    VkVertexInputBindingDescription vib = { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription via = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vib, .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &via };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba[2][2];
    memset(cba, 0, sizeof cba);
    for (int v = 0; v < 2; v++)
        for (int a = 0; a < 2; a++) cba[v][a].colorWriteMask = 0xF;
    // pipeline 1：色 1 枚目は blend 切り・2 枚目は blend 有効（ONE / ZERO なので結果は上書きと同じ）
    cba[1][1] = (VkPipelineColorBlendAttachmentState) { .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
    VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyns };
    VkPipeline pipes[2];
    for (int v = 0; v < 2; v++) {
        VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 2, .pAttachments = cba[v] };
        VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
            .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dsi,
            .layout = pl, .renderPass = rp, .subpass = 0 };
        VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipes[v]));
        KEEP(K_PIPE, pipes[v]);
    }

    // ---- コマごとの物 ----
    fc_ = calloc((size_t) g_inflight, sizeof *fc_);
    for (int f = 0; f < g_inflight; f++) {
        fctx *fc = &fc_[f];
        for (int j = 0; j < 2; j++) {
            mk_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &fc->img[j], &fc->view[j], &fc->imem[j]);
            mk_buffer((uint64_t) W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &fc->rb[j], &fc->rbmem[j], (void **) &fc->rbmap[j]);
        }
        VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2, .pAttachments = fc->view, .width = W, .height = H, .layers = 1 };
        VK(vkCreateFramebuffer(dev, &fbci, NULL, &fc->fb));
        KEEP(K_FB, fc->fb);
        // 毎コマ reset する pool（GZDoom の Fixed set・HWBuffer set・PP set に当たる。大きさは 3 倍・2 倍の比率）
        VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * 8 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * 8 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 8 } };
        VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8, .poolSizeCount = 3, .pPoolSizes = ps };
        VK(vkCreateDescriptorPool(dev, &dpi, NULL, &fc->dpool));
        KEEP(K_DPOOL, fc->dpool);
        VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf };
        VK(vkCreateCommandPool(dev, &cpci, NULL, &fc->cpool));
        KEEP(K_CPOOL, fc->cpool);
        VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = fc->cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VK(vkAllocateCommandBuffers(dev, &cbai, &fc->cb));
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK(vkCreateFence(dev, &fci, NULL, &fc->fence));
        KEEP(K_FENCE, fc->fence);
    }
    ref0 = malloc((size_t) W * H * 4);
    ref1 = malloc((size_t) W * H * 4);

    // ---- 材質の画像を送る（1 回だけ） ----
    {
        VkCommandPool cp;
        VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
        VK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
        KEEP(K_CPOOL, cp);
        VkCommandBuffer cb;
        VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VK(vkAllocateCommandBuffers(dev, &cbai, &cb));
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VK(vkBeginCommandBuffer(cb, &bi));
        VkImageMemoryBarrier *ib = calloc((size_t) ntex, sizeof *ib);
        for (int t = 0; t < ntex; t++)
            ib[t] = (VkImageMemoryBarrier) { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = timg[t], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, (uint32_t) ntex, ib);
        for (int t = 0; t < ntex; t++) {
            VkBufferImageCopy rg = { .bufferOffset = (uint64_t) t * TS * TS * 4, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { TS, TS, 1 } };
            vkCmdCopyBufferToImage(cb, stg_buf, timg[t], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        }
        for (int t = 0; t < ntex; t++) {
            ib[t].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ib[t].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            ib[t].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            ib[t].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, (uint32_t) ntex, ib);
        free(ib);
        VK(vkEndCommandBuffer(cb));
        VkFence fn;
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK(vkCreateFence(dev, &fci, NULL, &fn));
        KEEP(K_FENCE, fn);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        VK(vkQueueSubmit(q, 1, &si, fn));
        VK(vkWaitForFences(dev, 1, &fn, VK_TRUE, UINT64_MAX));
    }

    // ---- コマの繰り返し ----
    double t_start = now_ms(), t_rec = 0;
    for (int k = 0; k < frames; k++) {
        int f = k % g_inflight, seg = k % nseg;
        fctx *fc = &fc_[f];
        if (fc->busy) complete(fc);
        double tr0 = now_ms();
        // set 0・set 1 を作り直す（pool ごと reset）
        VK(vkResetDescriptorPool(dev, fc->dpool, 0));
        VkDescriptorSet s01[2], set0, set1;
        VkDescriptorSetLayout l01[2] = { dsl[0], dsl[1] };
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = fc->dpool, .descriptorSetCount = 2, .pSetLayouts = l01 };
        VK(vkAllocateDescriptorSets(dev, &dai, s01));
        set0 = s01[0]; set1 = s01[1];
        {
            VkDescriptorImageInfo ii[2] = { { sampA, tview[g_tex + (k & 1)], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { sampB, tview[g_tex + ((k + 1) & 1)], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
            VkDescriptorBufferInfo bi[5] = { { vp_buf, 0, VP_BLOCK }, { mat_buf, 0, MAT_BLOCK }, { str_buf, 0, STR_BLOCK },
                { sbo_buf, (uint64_t) f * REGION, LIGHT_SZ }, { sbo_buf, (uint64_t) f * REGION + BONE_OFF, BONE_SZ } };
            VkWriteDescriptorSet w[7];
            memset(w, 0, sizeof w);
            for (int i = 0; i < 2; i++)
                w[i] = (VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set0, .dstBinding = (uint32_t) i, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i] };
            for (int i = 0; i < 5; i++)
                w[2 + i] = (VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set1, .dstBinding = (uint32_t) i, .descriptorCount = 1,
                    .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i] };
            vkUpdateDescriptorSets(dev, 7, w, 0, NULL);
        }
        // 画素の数を数える欄を 0 に
        *(uint32_t *) (ssbo_map + (size_t) f * REGION) = 0;
        flush_range(sbo_mem, (uint64_t) f * REGION, 16, sbo_size);

        // ring に、この区間の draw ごとの値を書く
        uint64_t mat_base = (uint64_t) seg * g_draws, vp_base = (uint64_t) seg * vp_per_frame;
        for (int s = 0; s < vp_per_frame; s++) {
            int32_t *v = (int32_t *) (vp_map + (vp_base + s) * vp_stride);
            memset(v, 0, VP_BLOCK);
            v[0] = W; v[1] = H; v[2] = (k * 37) & 15; v[3] = k;
            v[4] = s; v[5] = seg; v[6] = g_draws; v[7] = 0x5a5a;
        }
        for (int i = 0; i < g_draws; i++) {
            draw_t d = dparams(k, i);
            float *m = (float *) (mat_map + (mat_base + i) * mat_stride);
            memset(m, 0, MAT_BLOCK);
            m[0] = (float) d.w; m[5] = (float) d.h; m[10] = 1; m[12] = (float) d.x0; m[13] = (float) d.y0; m[15] = 1;   // model
            m[16] = m[21] = m[26] = m[31] = 1;                                                                           // normalModel
            if (d.layer) { m[32] = 0.5f; m[37] = 0.5f; m[44] = 0.25f; m[45] = 0.25f; } else { m[32] = 1; m[37] = 1; }  // texMat
            m[42] = 1; m[47] = 1;
            int32_t *s = (int32_t *) (str_map + (mat_base + i) * str_stride);
            memset(s, 0, STR_BLOCK);
            s[0] = d.tint[0]; s[1] = d.tint[1]; s[2] = d.tint[2]; s[4] = d.mode; s[5] = i; s[6] = k; s[7] = d.t0;
        }
        flush_range(vp_mem, vp_base * vp_stride, (uint64_t) vp_per_frame * vp_stride, vp_size);
        flush_range(mat_mem, mat_base * mat_stride, (uint64_t) g_draws * mat_stride, mat_size);
        flush_range(str_mem, mat_base * str_stride, (uint64_t) g_draws * str_stride, str_size);

        // 記録
        VkCommandBuffer cmd = fc->cb;
        VK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        VK(vkBeginCommandBuffer(cmd, &cbbi));
        VkClearValue clr[2] = { { .color = { .float32 = { 16 / 255.0f, 16 / 255.0f, 16 / 255.0f, 1.0f } } }, { .color = { .float32 = { 5 / 255.0f, 6 / 255.0f, 7 / 255.0f, 1.0f } } } };
        VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fc->fb, .renderArea = { { 0, 0 }, { W, H } },
            .clearValueCount = 2, .pClearValues = clr };
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vpt = { 0, 0, W, H, 0, 1 };
        vkCmdSetViewport(cmd, 0, 1, &vpt);
        vkCmdBindIndexBuffer(cmd, vb_buf, ib_off, VK_INDEX_TYPE_UINT16);
        int cur_pipe = -1, cur_mat = -1;
        for (int i = 0; i < g_draws; i++) {
            draw_t d = dparams(k, i);
            uint32_t offs[3] = { (uint32_t) ((vp_base + i / VP_EVERY) * vp_stride), (uint32_t) ((mat_base + i) * mat_stride), (uint32_t) ((mat_base + i) * str_stride) };
            int np = (i / 5) & 1;
            if (np != cur_pipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[np]); cur_pipe = np; }
            if ((i & 3) == 0) {
                // set 0・1・2 を 1 回で bind（dynamic offset は set 1 の 3 個だけ）
                VkDescriptorSet three[3] = { set0, set1, tsets[d.t0] };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 3, three, 3, offs);
                g_bind_total++;
                cur_mat = d.t0;
            } else {
                if ((i & 15) == 1) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set0, 0, NULL); g_bind_total++; }
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 1, 1, &set1, (g_invalid && k == 0 && i == 1) ? 2 : 3, offs);   // invalid=1：dynamic offset の数を 1 つ減らす
                g_bind_total++;
                if (d.t0 != cur_mat) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 2, 1, &tsets[d.t0], 0, NULL); g_bind_total++; cur_mat = d.t0; }
            }
            int32_t pcv[PC_SIZE / 4];
            for (int j = 0; j < PC_SIZE / 4; j++) pcv[j] = i * 31 + j;
            pcv[0] = d.bi; pcv[1] = d.li; pcv[17] = d.bp;       // pc.boneIdx・pc.lightIdx・pc.f[3].y（先頭の int 4 個の後に ivec4 が並ぶので 4 + 3 * 4 + 1）
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, PC_SIZE, pcv);
            g_push_total++;
            VkRect2D sc = { { d.x0, d.y0 }, { (uint32_t) d.w, (uint32_t) d.h } };
            vkCmdSetScissor(cmd, 0, 1, &sc);
            VkDeviceSize vo = (VkDeviceSize) (i % NQUAD) * 4 * 8;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb_buf, &vo);
            vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
            g_draw_total++;
        }
        vkCmdEndRenderPass(cmd);
        fc->did_rb = (k % g_readback == 0) || k == frames - 1;
        if (fc->did_rb) {
            for (int j = 0; j < 2; j++) {
                VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
                vkCmdCopyImageToBuffer(cmd, fc->img[j], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fc->rb[j], 1, &rg);
            }
        }
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VK(vkEndCommandBuffer(cmd));
        t_rec += now_ms() - tr0;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
        VK(vkQueueSubmit(q, 1, &si, fc->fence));
        fc->busy = 1;
        fc->frame = k;
        if (!g_quiet && (k < 3 || (k + 1) % 25 == 0)) printf("frame %d submitted\n", k);
    }
    // 残りを待って確かめる（コマの順）
    for (int k = frames > g_inflight ? frames - g_inflight : 0; k < frames; k++)
        if (fc_[k % g_inflight].busy && fc_[k % g_inflight].frame == k) complete(&fc_[k % g_inflight]);
    double t_end = now_ms();

    if (out) {
        FILE *fp = fopen(out, "wb");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", W, H);
            for (int i = 0; i < W * H; i++) fwrite(ref0 + i * 4, 1, 3, fp);
            fclose(fp);
            printf("wrote the last frame (CPU reference, identical to the GPU result if the check passed) to %s\n", out);
        }
    }
    VK(vkDeviceWaitIdle(dev));
    destroy_all();
    vkDestroyDevice(dev, NULL);
    if (messenger) vkDestroyDebugUtilsMessengerEXT(inst, messenger, NULL);
    vkDestroyInstance(inst, NULL);

    printf("done: %d frames, %llu draws, %llu descriptor-set bind calls, %llu push-constant updates, %.1f ms total (%.2f ms/frame, of which recording %.2f ms)\n",
           frames, (unsigned long long) g_draw_total, (unsigned long long) g_bind_total, (unsigned long long) g_push_total, t_end - t_start, (t_end - t_start) / frames, t_rec / frames);
    printf("checked %d frames pixel by pixel against the CPU: %d bad frames, %d fragment-counter mismatches\n", g_checked, g_bad, g_cnt_bad);
    printf("checksum %08x (CPU reference %08x)\n", h_gpu, h_ref);
    if (validate) printf("validation messages (warnings and errors): %d\n", g_vmsg);
    int ok = !g_bad && !g_cnt_bad && h_gpu == h_ref && g_verr == 0;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
