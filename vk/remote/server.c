// Vulkan の命令の受け手。PC のドライバから届いた命令を、ここの本物の Vulkan で実行する。
// Windows では試験用に vulkan-1.dll（RTX 3060）で、iPhone では MoltenVK（Metal）で動く。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "proto.h"
#include "wire.h"

#ifdef _WIN32
#include <windows.h>
#endif
#ifdef __APPLE__
#include <os/log.h>
#endif
#include <stdarg.h>
#include <time.h>
#ifndef _WIN32
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
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
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
    X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkSignalSemaphore)

#define DECL(n) static PFN_##n n;
VKFUNCS(DECL)
#undef DECL

// ---- PC が振った番号 → ここの本物の Vulkan の物 ----
// aux：バッファならつないだメモリの番号。list：descriptor set なら中のバッファの番号の並び
typedef struct { uint64_t *ids; uint32_t n, cap; } idlist;
typedef struct { uint64_t id, h; uint32_t kind; void *map; uint64_t size; uint64_t aux; idlist *list; } ent;

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

enum { K_DEVICE = 1, K_QUEUE, K_BUFFER, K_MEMORY, K_SHADER, K_DSL, K_PLAYOUT, K_PIPELINE, K_DPOOL, K_DSET, K_CPOOL, K_CB, K_FENCE, K_SEM };

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
    if (g_tab[s].id == id) idlist_free(&g_tab[s].list);
    g_tab[s] = (ent) { id, h, kind, NULL, 0, 0, NULL };
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

#define DEV(id) ((VkDevice) (uintptr_t) H(id))
#define OBJ(T, id) ((T) (uintptr_t) H(id))

// ---- 起動 ----
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
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "iphone-rpc-vk", .apiVersion = VK_API_VERSION_1_2 };
    const char *ext[] = { "VK_KHR_portability_enumeration" };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
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
    for (uint32_t i = 0; i < nie; i++)
        if (!strcmp(ie[i].extensionName, "VK_KHR_portability_enumeration")) {
            ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            ci.enabledExtensionCount = 1;
            ci.ppEnabledExtensionNames = ext;
        }
    free(ie);
    VkResult ir = vkCreateInstance(&ci, NULL, &g_inst);
    if (ir != VK_SUCCESS) { vlog("vkCreateInstance が失敗 (%d)", ir); return 0; }
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
    free(ep);
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_pd, &p);
    vlog("受け手の GPU: %s", p.deviceName);
    return 1;
}

// ---- 出した仕事が終わるのを受け手の中で待つ（版 3） ----
// 待つのは、出した仕事が使っている物を変えるときだけ（その仕事まで待つ）。
// 関係の無い物を変える命令は待たないので、GPU が計算している間に次の命令を作れる
static VkDevice g_dev;
typedef struct { VkFence f; int own; uint64_t cbs[64]; uint32_t ncb; } inflight;
static inflight g_fl[256];
static int g_nfl;
static VkResult g_err;          // GPU を失ったなど。次に返事をするときに伝える
static volatile int g_paused;   // アプリが前にいない間は 1（GPU に新しい仕事を出さない）

// アプリから呼ぶ：前にいないときは 1
void vkr_set_paused(int on) { g_paused = on; }

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

// command buffer ごとに、どの command pool の物か・記録した中でどの descriptor set を使うか
typedef struct { uint64_t cb, pool; uint64_t *sets; uint32_t n, cap; idlist *mems; } cbinfo;
static cbinfo *g_cbi;
static size_t g_ncbi, g_capcbi;

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

// ---- 1 つの接続の間に作った物を、切れたときに片づける ----
static void cleanup(void) {
    drain();
    for (size_t i = 0; i < g_ncbi; i++) { free(g_cbi[i].sets); idlist_free(&g_cbi[i].mems); }
    g_ncbi = 0;
    g_err = VK_SUCCESS;
    if (g_dev) {
        for (int i = 0; i < g_nfpool; i++) vkDestroyFence(g_dev, g_fpool[i], NULL);
    }
    g_nfpool = 0;
    g_dev = VK_NULL_HANDLE;
    for (int kind = K_PIPELINE; kind >= K_DEVICE; kind--) { (void) kind; }
    VkDevice dev = VK_NULL_HANDLE;
    for (size_t i = 0; i < g_cap; i++) if (g_tab[i].id && g_tab[i].kind == K_DEVICE) dev = (VkDevice) (uintptr_t) g_tab[i].h;
    if (dev) {
        vkDeviceWaitIdle(dev);
        static const int order[] = { K_PIPELINE, K_PLAYOUT, K_SHADER, K_DPOOL, K_DSL, K_CPOOL, K_FENCE, K_SEM, K_BUFFER, K_MEMORY };
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
                }
            }
        vkDestroyDevice(dev, NULL);
    }
    for (size_t i = 0; i < g_cap; i++) if (g_tab[i].id) idlist_free(&g_tab[i].list);
    free(g_tab);
    g_tab = NULL;
    g_cap = g_cnt = 0;
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
        VkBaseOutStructure *b = calloc(1, sz);
        b->sType = (VkStructureType) st;
        blocks[i] = b;
        sizes[i] = sz;
        *link = b;
        link = (void **) &b->pNext;
    }
    return head;
}

// ---- command buffer に積まれた命令を実行する ----
static void replay(VkCommandBuffer cb, vr_rd *r, cbinfo *ci) {
    while (r->at < r->n && !r->bad) {
        uint32_t op = vr_ru32(r);
        switch (op) {
        case VR_CMD_BIND_PIPELINE: {
            uint32_t bp = vr_ru32(r);
            VkPipeline pl = OBJ(VkPipeline, vr_ru64(r));
            if (pl) vkCmdBindPipeline(cb, (VkPipelineBindPoint) bp, pl);
        } break;
        case VR_CMD_BIND_DESCRIPTOR_SETS: {
            uint32_t bp = vr_ru32(r);
            VkPipelineLayout pl = OBJ(VkPipelineLayout, vr_ru64(r));
            uint32_t first = vr_ru32(r), n = vr_ru32(r);
            VkDescriptorSet sets[32];
            for (uint32_t i = 0; i < n && i < 32; i++) {
                uint64_t sid = vr_ru64(r);
                sets[i] = OBJ(VkDescriptorSet, sid);
                if (ci) {
                    cbi_add_set(ci, sid);
                    ent *se = find(sid);
                    if (se && se->list) for (uint32_t k = 0; k < se->list->n; k++) use_buffer(ci, se->list->ids[k]);
                }
            }
            uint32_t nd = vr_ru32(r);
            uint32_t dyn[64];
            for (uint32_t i = 0; i < nd && i < 64; i++) dyn[i] = vr_ru32(r);
            vkCmdBindDescriptorSets(cb, (VkPipelineBindPoint) bp, pl, first, n, sets, nd, dyn);
        } break;
        case VR_CMD_PUSH_CONSTANTS: {
            VkPipelineLayout pl = OBJ(VkPipelineLayout, vr_ru64(r));
            uint32_t st = vr_ru32(r), off = vr_ru32(r), sz;
            const void *d = vr_rbytes(r, &sz);
            if (d) vkCmdPushConstants(cb, pl, st, off, sz, d);
        } break;
        case VR_CMD_DISPATCH: {
            uint32_t x = vr_ru32(r), y = vr_ru32(r), z = vr_ru32(r);
            vkCmdDispatch(cb, x, y, z);
        } break;
        case VR_CMD_DISPATCH_BASE: {
            uint32_t a = vr_ru32(r), b = vr_ru32(r), c = vr_ru32(r), x = vr_ru32(r), y = vr_ru32(r), z = vr_ru32(r);
            vkCmdDispatchBase(cb, a, b, c, x, y, z);
        } break;
        case VR_CMD_DISPATCH_INDIRECT: {
            uint64_t bid = vr_ru64(r);
            use_buffer(ci, bid);
            VkBuffer b = OBJ(VkBuffer, bid);
            vkCmdDispatchIndirect(cb, b, vr_ru64(r));
        } break;
        case VR_CMD_COPY_BUFFER: {
            uint64_t sid = vr_ru64(r), did = vr_ru64(r);
            use_buffer(ci, sid);
            use_buffer(ci, did);
            VkBuffer src = OBJ(VkBuffer, sid), dst = OBJ(VkBuffer, did);
            uint32_t n = vr_ru32(r);
            VkBufferCopy *rg = malloc(sizeof *rg * (n ? n : 1));
            for (uint32_t i = 0; i < n; i++) { rg[i].srcOffset = vr_ru64(r); rg[i].dstOffset = vr_ru64(r); rg[i].size = vr_ru64(r); }
            vkCmdCopyBuffer(cb, src, dst, n, rg);
            free(rg);
        } break;
        case VR_CMD_FILL_BUFFER: {
            uint64_t bid = vr_ru64(r);
            use_buffer(ci, bid);
            VkBuffer b = OBJ(VkBuffer, bid);
            uint64_t off = vr_ru64(r), sz = vr_ru64(r);
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
        default:
            vlog("知らない記録の命令 %u", op);
            r->bad = 1;
        }
    }
}

// ---- 1 つの命令を実行する。返事が要るものは reply する ----
static int handle(vr_sock s, uint32_t op, vr_rd *r) {
    g_cur_op = op;
    // 出した仕事が使っているかもしれない物を変える・調べる命令の前に、出した仕事が全部終わるのを待つ
    switch (op) {
    // （command buffer の記録し直し・command pool の reset・descriptor の書き換え・fence の reset は、
    //   それを使う仕事までしか待たない。それぞれの命令の中で待つ）
    case VR_GET_FENCE_STATUS: case VR_GET_SEMAPHORE_COUNTER: case VR_FREE_COMMAND_BUFFERS:
    case VR_DESTROY_COMMAND_POOL: case VR_RESET_DESCRIPTOR_POOL: case VR_FREE_DESCRIPTOR_SETS:
    case VR_DESTROY_DESCRIPTOR_POOL: case VR_FREE_MEMORY: case VR_DESTROY_BUFFER: case VR_DESTROY_PIPELINE:
    case VR_DESTROY_PIPELINE_LAYOUT: case VR_DESTROY_SHADER_MODULE: case VR_DESTROY_DSL: case VR_DESTROY_FENCE:
    case VR_DESTROY_SEMAPHORE: case VR_DESTROY_DEVICE: case VR_QUEUE_WAIT_IDLE: case VR_DEVICE_WAIT_IDLE:
        drain();
        break;
    default:
        break;
    }
    // GPU を失っていたら、返事のある命令でそれを伝える（黙って間違った答えを返さない）
    if (g_err != VK_SUCCESS) {
        switch (op) {
        case VR_MEM_READ: case VR_WAIT_FOR_FENCES: case VR_GET_FENCE_STATUS: case VR_WAIT_SEMAPHORES:
        case VR_GET_SEMAPHORE_COUNTER: case VR_QUEUE_WAIT_IDLE: case VR_DEVICE_WAIT_IDLE:
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
        vkGetPhysicalDeviceMemoryProperties(g_pd, &m);
        vr_put(&g_out, &p, sizeof p);
        vr_put(&g_out, &f, sizeof f);
        vr_put(&g_out, &m, sizeof m);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
        vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &nq, q);
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
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_pd, (VkFormat) vr_ru32(r), &fp);
        vr_put(&g_out, &fp, sizeof fp);
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
        for (uint32_t i = 0; i < ne && i < 63; i++) { uint32_t l; names[n_names++] = (const char *) vr_rbytes(r, &l); }
        if (g_portability) names[n_names++] = "VK_KHR_portability_subset";
        uint32_t has_f = vr_ru32(r);
        VkPhysicalDeviceFeatures feats;
        memset(&feats, 0, sizeof feats);
        if (has_f) vr_rcopy(r, &feats, sizeof feats);
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
        if (e && e->map && d && off <= e->size && n <= e->size - off) memcpy((uint8_t *) e->map + off, d, n);
        else vlog("範囲の外への書き込みを捨てた（%llu + %u）", (unsigned long long) off, n);
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
        VkDescriptorSetLayoutBinding *b = calloc(n ? n : 1, sizeof *b);
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
        uint32_t l;
        ci.stage.pName = (const char *) vr_rbytes(r, &l);
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
        ci.pPoolSizes = ps;
        VkDescriptorPool p;
        if (vkCreateDescriptorPool(d, &ci, NULL, &p) == VK_SUCCESS) put(id, (uint64_t) p, K_DPOOL);
        return 1;
    }
    case VR_DESTROY_DESCRIPTOR_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        vkDestroyDescriptorPool(d, OBJ(VkDescriptorPool, id), NULL);
        del(id);
        return 1;
    }
    case VR_RESET_DESCRIPTOR_POOL: {
        VkDevice d = DEV(vr_ru64(r));
        vkResetDescriptorPool(d, OBJ(VkDescriptorPool, vr_ru64(r)), 0);
        return 1;
    }
    case VR_ALLOC_DESCRIPTOR_SETS: {
        VkDevice d = DEV(vr_ru64(r));
        VkDescriptorPool pool = OBJ(VkDescriptorPool, vr_ru64(r));
        uint32_t n = vr_ru32(r);
        for (uint32_t i = 0; i < n; i++) {
            VkDescriptorSetLayout l = OBJ(VkDescriptorSetLayout, vr_ru64(r));
            uint64_t id = vr_ru64(r);
            VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &l };
            VkDescriptorSet ds;
            VkResult res = vkAllocateDescriptorSets(d, &ai, &ds);
            if (res == VK_SUCCESS) put(id, (uint64_t) ds, K_DSET);
            else vlog("descriptor set を取れない (%d)", res);
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
            vkFreeDescriptorSets(d, pool, 1, &ds);
            del(id);
        }
        return 1;
    }
    case VR_UPDATE_DESCRIPTOR_SETS: {
        VkDevice d = DEV(vr_ru64(r));
        uint32_t nw = vr_ru32(r);
        for (uint32_t i = 0; i < nw; i++) {
            VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            uint64_t sid = vr_ru64(r);
            drain_upto(last_with_set(sid));
            w.dstSet = OBJ(VkDescriptorSet, sid);
            w.dstBinding = vr_ru32(r);
            w.dstArrayElement = vr_ru32(r);
            w.descriptorType = (VkDescriptorType) vr_ru32(r);
            w.descriptorCount = vr_ru32(r);
            VkDescriptorBufferInfo *bi = calloc(w.descriptorCount ? w.descriptorCount : 1, sizeof *bi);
            ent *se = find(sid);
            for (uint32_t k = 0; k < w.descriptorCount; k++) {
                uint64_t bid = vr_ru64(r);
                bi[k].buffer = OBJ(VkBuffer, bid);
                bi[k].offset = vr_ru64(r);
                bi[k].range = vr_ru64(r);
                if (se) idlist_add(&se->list, bid);
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
        VkSemaphore ws[8][8], ss[8][8];
        uint64_t wv[8][8], sv[8][8];
        VkPipelineStageFlags wst[8][8];
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
        if (res == VK_SUCCESS) {
            inflight *e = &g_fl[g_nfl++];
            e->f = f;
            e->own = own;
            e->ncb = ncid;
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
    case VR_QUEUE_WAIT_IDLE: return reply(s, vkQueueWaitIdle((VkQueue) (uintptr_t) H(vr_ru64(r))));
    case VR_DEVICE_WAIT_IDLE: return reply(s, vkDeviceWaitIdle(DEV(vr_ru64(r))));
    }
    vlog("知らない命令 %u", op);
    return 0;
}

static void serve_client(vr_sock c) {
    vr_nodelay(c);
    uint8_t *buf = NULL;
    size_t cap = 0;
    for (;;) {
        uint32_t h[2];
        if (!vr_recv_all(c, h, 8)) break;
        if (h[1] > cap) { cap = h[1]; buf = realloc(buf, cap); }
        if (h[1] && !vr_recv_all(c, buf, h[1])) break;
        vr_rd r = { buf, h[1], 0, 0 };
        if (!handle(c, h[0], &r)) break;
    }
    free(buf);
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
int vkr_serve(const char *host, int port) {
    if (!vk_init()) { vlog("Vulkan を起動できない"); return 1; }
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
        vlog("つながった");
        serve_client(c);
#ifdef _WIN32
        closesocket(c);
#else
        close(c);
#endif
        vlog("切れた");
    }
}

#ifdef VR_SERVER_MAIN
int main(int argc, char **argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    // PC で代役をするときは、画面取り込みなどの部品（暗黙のレイヤー）を挟まない。
    // OBS が起動している間、OBS の部品が vkCreateDevice から戻ってこなかった
    SetEnvironmentVariableA("VK_LOADER_LAYERS_DISABLE", "~implicit~");
#endif
    return vkr_serve(argc > 1 ? argv[1] : "127.0.0.1", argc > 2 ? atoi(argv[2]) : VR_DEFAULT_PORT);
}
#endif
