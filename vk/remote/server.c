// Vulkan の命令の受け手。PC のドライバから届いた命令を、ここの本物の Vulkan で実行する。
// Windows では試験用に vulkan-1.dll（RTX 3060）で、iPhone では MoltenVK（Metal）で動く。
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "proto.h"
#include "wire.h"
#include "plane_codec.h" // 縮めた平面をほどく（VR_MEM_WRITE_CODED）
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
// 窓に出すコマを H.264 に縮める（VideoToolbox・CoreMedia・CoreVideo）
#include <VideoToolbox/VideoToolbox.h>
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
#include <ifaddrs.h>
#include <net/if.h>
#endif
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include "video.h"

// iPhone では受け手の糸を速いコアに置いてもらう。既定の糸は省電力のコアに回されることがあり、同じ Heaven が毎秒 58 と 32 に分かれた
static void qos_hi(void) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}
#ifdef __ANDROID__
#include <android/log.h>
#endif
#ifdef __linux__
// Android と Linux（lavapipe などを受け手にする試験）には ImageIO も WIC も無いので、
// JPEG は stb_image_write（public domain / MIT・https://github.com/nothings/stb）の小さな書き手で縮める
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_ASSERT(x) ((void) 0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "../third_party/stb/stb_image_write.h"
#pragma GCC diagnostic pop
#endif
// 窓に出すコマを JPEG に縮められる機械（iPhone は ImageIO、PC の代役は WIC、Android と Linux は stb）
#if defined(__APPLE__) || defined(_WIN32) || defined(__linux__)
#define VR_HAVE_JPEG 1
#endif

// 記録の言語。環境変数 PGPU_LANG=en|ja で決める（無ければ日本語）。英語のときは JE(日本語, English) が English を返す。
// iPhone は環境変数が使えないので、アプリから vkr_set_lang("en") で渡せる
static int g_lang_en = -1;
static int lang_en(void) {
    if (g_lang_en < 0) {
        const char *e = getenv("PGPU_LANG");
        g_lang_en = e && (e[0] == 'e' || e[0] == 'E') ? 1 : 0;
    }
    return g_lang_en;
}
void vkr_set_lang(const char *lang) { g_lang_en = lang && (lang[0] == 'e' || lang[0] == 'E') ? 1 : 0; }
#define JE(ja, en) (lang_en() ? (en) : (ja))

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
#ifdef __ANDROID__
    // アプリ（APK）にしたときは stdout が見えないので logcat にも出す（adb logcat -s iphone-rpc-vk）
    __android_log_print(ANDROID_LOG_INFO, "iphone-rpc-vk", "%s", line);
#endif
#endif
}

// 口（bind・listen）を開けなかった理由。errno の名前と短い説明を日英で書く（Windows は WSA の番号を同じ名前にする）
typedef struct { int code; const char *name, *ja, *en; } sock_err_ent;
#ifdef _WIN32
#define SE_C(w, p) (w)
#else
#define SE_C(w, p) (p)
#endif
static const sock_err_ent k_sock_errs[] = {
    { SE_C(WSAEADDRINUSE, EADDRINUSE), "EADDRINUSE", "この口は別のプログラムが使っている", "the port is already in use by another program" },
    { SE_C(WSAEACCES, EACCES), "EACCES", "この口を使う許可が無い", "no permission to use this port" },
    { SE_C(WSAEADDRNOTAVAIL, EADDRNOTAVAIL), "EADDRNOTAVAIL", "このアドレスはこの端末に無い", "this address does not exist on this device" },
    { SE_C(WSAEINVAL, EINVAL), "EINVAL", "アドレスか口の状態が正しくない", "invalid address or socket state" },
    { SE_C(WSAEMFILE, EMFILE), "EMFILE", "開いているファイルが多すぎる", "too many open files" },
    { SE_C(WSAENOBUFS, ENOBUFS), "ENOBUFS", "メモリが足りない", "out of buffer space" },
    { SE_C(WSAENETDOWN, ENETDOWN), "ENETDOWN", "ネットワークが使えない", "the network is down" },
    { SE_C(WSAEAFNOSUPPORT, EAFNOSUPPORT), "EAFNOSUPPORT", "この種類のアドレスは使えない", "address family not supported" },
#ifndef _WIN32
    { EPERM, "EPERM", "許されていない操作", "operation not permitted" },
    { ENOMEM, "ENOMEM", "メモリが足りない", "out of memory" },
#endif
};
#define SOCK_EINVAL SE_C(WSAEINVAL, EINVAL)
static int sock_errno(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
static void sock_err_text(int e, char *buf, size_t n) {
    for (size_t i = 0; i < sizeof k_sock_errs / sizeof k_sock_errs[0]; i++)
        if (k_sock_errs[i].code == e) {
            snprintf(buf, n, JE("%s（%s）", "%s (%s)"), k_sock_errs[i].name, JE(k_sock_errs[i].ja, k_sock_errs[i].en));
            return;
        }
    snprintf(buf, n, "errno %d", e);
}

// plane_codec.h の pc_decode が返す理由（日本語）を、英語のときは英語にする（plane_codec.h は icd とも共有なので、ここで読み替える）
static const char *pc_err_text(const char *e) {
    static const char *const k[][2] = {
        { "頭が読めない", "cannot read the header" },
        { "表が壊れている", "the table is broken" },
        { "段の並びが壊れている", "the segment layout is broken" },
        { "メモリが足りない", "out of memory" },
        { "流れが壊れている", "the data stream is broken" },
    };
    if (lang_en() && e)
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
            if (!strcmp(e, k[i][0])) return k[i][1];
    return e;
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

// レイトレーシングの土台（VR_CAP_RT）。GPU が acceleration structure と ray query を持てば detect_caps で読む
#define VKFUNCS_RT(X) \
    X(vkCreateAccelerationStructureKHR) X(vkDestroyAccelerationStructureKHR) X(vkGetAccelerationStructureBuildSizesKHR) \
    X(vkGetAccelerationStructureDeviceAddressKHR) X(vkCmdBuildAccelerationStructuresKHR) X(vkCmdCopyAccelerationStructureKHR) \
    X(vkCmdWriteAccelerationStructuresPropertiesKHR)

#define DECL(n) static PFN_##n n;
VKFUNCS(DECL)
VKFUNCS_OPT(DECL)
VKFUNCS_RT(DECL)
#undef DECL

static uint32_t g_caps = VR_CAP_DELTA; // 受け手ができること（あいさつで名乗る）
static int vid_probe(void);            // 窓に出すコマを H.264 に縮められるか（下の VR_READ_H264 のところ）
static int rec_available(void);        // 録画できるか（record.h）
// 空の descriptor（robustness2 の nullDescriptor）を GPU が持つか。持てば装置を作るときに有効にする。
// 持たなければ（MoltenVK）、空の頂点バッファだけは 0 で埋めた代わりのバッファに差し替える
static int g_nulldesc_gpu;
// 調べ用（ffhang）：VKR_HANG_NULLDESC=1 で iPhone のふりでも GPU の本物の空の descriptor を使う。VKR_HANG_ROBUST2=1 で robustBufferAccess2・robustImageAccess2 を有効にする
static int hang_env(const char *k) { const char *e = getenv(k); return e && *e == '1'; }
static int g_hang_subst[8]; // 代わりの物に差し替えた数（0 texel・1 画像・2 buffer・3 頂点）
static int g_divisor; // GPU が VK_EXT_vertex_attribute_divisor を持つ
// 試験用（Android 版）：VKR_FAKE_DIVISOR=1 なら、GPU が VK_EXT_vertex_attribute_divisor を持たなくても持つと名乗る。
// Android エミュレーターの Vulkan（gfxstream）はこの拡張を隠すが、DirectX 12（vkd3d-proton）は無いと装置を作らない。
// 名乗るだけで、装置にも pipeline にも渡さない（step rate が 1 でない描画は正しくならない）。本物の端末で使う物ではない
static int fake_divisor(void) {
    static int on = -1;
    if (on < 0) on = getenv("VKR_FAKE_DIVISOR") != NULL;
    return on && !g_divisor;
}
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

// iPhone のふりで合わせる上限。10/7 の iPhone 17 Pro の vulkaninfo（iphone_runs/v0724）の値。
// DirectX 12（vkd3d-proton）が見る物：同時につなぐ set の数・update after bind の数・texel buffer の揃え
#define IOS_MIN(x, v) do { if ((x) > (v)) (x) = (v); } while (0)
#define IOS_MAX(x, v) do { if ((x) < (v)) (x) = (v); } while (0)
// 調べ用：VKR_LIKE_IOS_VENDOR=1 で、代役が GPU の会社の番号を Apple（0x106B）と答える。
// ドライバ（iPhone 用の直しは会社の番号で決める）と、DXVK・vkd3d-proton（Apple なら root の CBV を住所で渡す、など）が本物の iPhone と同じ道を通る
// 調べ用：VKR_ADD_LOCAL_MEM=1 で、「GPU の手元だけ」のメモリの種類が無い GPU（lavapipe は CPU から見える種類 1 つだけ）に、
// 手元だけの種類を 1 つ足して見せる。中身は CPU から見える種類と同じ物を使う。
// ドライバは CPU から見える種類の DEVICE_LOCAL を外す（iPhone は手元だけの種類を別に持つ）ので、lavapipe だと手元の種類が無くなり、
// vkcube などが深さの画像を置けずに止まる
static int g_alias_mem = -1; // 足した種類の番号（足していなければ -1）
static uint32_t g_alias_of;  // その中身の種類
static void alias_mem_props(VkPhysicalDeviceMemoryProperties *m) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("VKR_ADD_LOCAL_MEM"); v = e && *e == '1'; }
    g_alias_mem = -1;
    if (!v || m->memoryTypeCount >= VK_MAX_MEMORY_TYPES) return;
    for (uint32_t i = 0; i < m->memoryTypeCount; i++)
        if ((m->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(m->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) return;
    for (uint32_t i = 0; i < m->memoryTypeCount; i++)
        if (m->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            g_alias_of = i;
            g_alias_mem = (int) m->memoryTypeCount;
            m->memoryTypes[m->memoryTypeCount].heapIndex = m->memoryTypes[i].heapIndex;
            m->memoryTypes[m->memoryTypeCount++].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            vlog(JE("試験用：メモリの種類 %u（GPU の手元だけ・中身は %u 番）を足した（VKR_ADD_LOCAL_MEM）", "Test: added memory type %u (GPU-local only, backed by type %u) (VKR_ADD_LOCAL_MEM)"), (unsigned) g_alias_mem, (unsigned) i);
            return;
        }
}
static uint32_t alias_bits(uint32_t bits) { return g_alias_mem >= 0 && (bits & (1u << g_alias_of)) ? bits | (1u << g_alias_mem) : bits; }
static uint32_t alias_type(uint32_t t) { return g_alias_mem >= 0 && t == (uint32_t) g_alias_mem ? g_alias_of : t; }

static void like_ios_vendor(VkPhysicalDeviceProperties *p) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("VKR_LIKE_IOS_VENDOR"); v = e && *e == '1'; }
    if (v) p->vendorID = 0x106b;
}

// 調べ用：VKR_LIKE_IOS_RT=1 で、iPhone のふりの代役でもレイトレーシングの土台（acceleration structure・ray query）だけは名乗る。
// 名乗る範囲は、iPhone で予定している物だけ（GPU の上で作る・作り直す・写す・大きさを数える・ray query）。
// CPU で作る・間接の build・capture replay・ray tracing pipeline は名乗らない（work/rt/DESIGN.md）
static int like_ios_rt(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("VKR_LIKE_IOS_RT"); v = e && *e == '1'; }
    return v;
}

static void like_ios_limits(VkPhysicalDeviceLimits *l) {
    if (!like_ios()) return;
    IOS_MIN(l->maxBoundDescriptorSets, 8);
    IOS_MAX(l->minTexelBufferOffsetAlignment, 64);
}
static void like_ios_props2(VkBaseOutStructure *x) {
    if (!like_ios()) return;
    if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
        VkPhysicalDeviceVulkan12Properties *p = (void *) x;
        IOS_MIN(p->maxUpdateAfterBindDescriptorsInAllPools, 1073741824u);
        IOS_MIN(p->maxPerStageDescriptorUpdateAfterBindSamplers, 500000);
        IOS_MIN(p->maxPerStageDescriptorUpdateAfterBindUniformBuffers, 1000000);
        IOS_MIN(p->maxPerStageDescriptorUpdateAfterBindStorageBuffers, 1000000);
        IOS_MIN(p->maxPerStageDescriptorUpdateAfterBindSampledImages, 1000000);
        IOS_MIN(p->maxPerStageDescriptorUpdateAfterBindStorageImages, 1000000);
        IOS_MIN(p->maxPerStageDescriptorUpdateAfterBindInputAttachments, 256);
        IOS_MIN(p->maxPerStageUpdateAfterBindResources, 1000000);
        IOS_MIN(p->maxDescriptorSetUpdateAfterBindSamplers, 500000);
        IOS_MIN(p->maxDescriptorSetUpdateAfterBindUniformBuffers, 1000000);
        IOS_MIN(p->maxDescriptorSetUpdateAfterBindStorageBuffers, 1000000);
        IOS_MIN(p->maxDescriptorSetUpdateAfterBindSampledImages, 1000000);
        IOS_MIN(p->maxDescriptorSetUpdateAfterBindStorageImages, 1000000);
        IOS_MIN(p->maxDescriptorSetUpdateAfterBindInputAttachments, 1280);
    } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES) {
        VkPhysicalDeviceVulkan13Properties *p = (void *) x;
        IOS_MAX(p->storageTexelBufferOffsetAlignmentBytes, 64);
        IOS_MAX(p->uniformTexelBufferOffsetAlignmentBytes, 64);
        p->storageTexelBufferOffsetSingleTexelAlignment = p->uniformTexelBufferOffsetSingleTexelAlignment = VK_FALSE;
    } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES) {
        VkPhysicalDeviceTexelBufferAlignmentProperties *p = (void *) x;
        IOS_MAX(p->storageTexelBufferOffsetAlignmentBytes, 64);
        IOS_MAX(p->uniformTexelBufferOffsetAlignmentBytes, 64);
        p->storageTexelBufferOffsetSingleTexelAlignment = p->uniformTexelBufferOffsetSingleTexelAlignment = VK_FALSE;
    } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR) {
        // iPhone で予定する上限（仮）。Metal の instance の番号は 24 ビット・1 つの set の数は argument buffer に収める分
        VkPhysicalDeviceAccelerationStructurePropertiesKHR *p = (void *) x;
        IOS_MIN(p->maxGeometryCount, 16777215ull);
        IOS_MIN(p->maxInstanceCount, 16777215ull);
        IOS_MIN(p->maxPrimitiveCount, 536870911ull);
        IOS_MIN(p->maxPerStageDescriptorAccelerationStructures, 64);
        IOS_MIN(p->maxPerStageDescriptorUpdateAfterBindAccelerationStructures, 64);
        IOS_MIN(p->maxDescriptorSetAccelerationStructures, 64);
        IOS_MIN(p->maxDescriptorSetUpdateAfterBindAccelerationStructures, 64);
        IOS_MAX(p->minAccelerationStructureScratchOffsetAlignment, 256);
    }
}

// iPhone のふりで、レイトレーシングの機能のうち iPhone で予定していない物を隠す
static void like_ios_rt_feats(VkBaseOutStructure *x) {
    if (!like_ios()) return;
    if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR) {
        VkPhysicalDeviceAccelerationStructureFeaturesKHR *f = (void *) x;
        if (!like_ios_rt()) f->accelerationStructure = f->descriptorBindingAccelerationStructureUpdateAfterBind = VK_FALSE;
        f->accelerationStructureCaptureReplay = f->accelerationStructureIndirectBuild = f->accelerationStructureHostCommands = VK_FALSE;
    } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR) {
        if (!like_ios_rt()) ((VkPhysicalDeviceRayQueryFeaturesKHR *) x)->rayQuery = VK_FALSE;
    } else if (x->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR) {
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR *f = (void *) x;
        f->rayTracingPipeline = f->rayTracingPipelineShaderGroupHandleCaptureReplay = f->rayTracingPipelineShaderGroupHandleCaptureReplayMixed = VK_FALSE;
        f->rayTracingPipelineTraceRaysIndirect = f->rayTraversalPrimitiveCulling = VK_FALSE;
    }
}
static VkBuffer g_dummy_vb;
static VkDeviceMemory g_dummy_mem;
static VkSampler g_dummy_smp; // 空の sampler の代わり（ドライバの取りこぼしで、アプリごと落ちないように）
// 空の画像・texel buffer の見え方の代わり（GPU が空の descriptor を持たないとき。DXVK 3・vkd3d・zink が書く）
static VkImage g_dummy_img;
static VkImageView g_dummy_iview;
static VkDeviceMemory g_dummy_img_mem;
static VkBuffer g_dummy_tb;
static VkBufferView g_dummy_bview;
static VkDeviceMemory g_dummy_tb_mem;
static VkBuffer dummy_vb(void);
static int g_fmt3;                     // VkFormatProperties3 を答えられる

// ---- PC が振った番号 → ここの本物の Vulkan の物 ----
// aux：バッファならつないだメモリの番号。list：descriptor set なら中のバッファの番号の並び
typedef struct { uint64_t *ids; uint32_t n, cap; } idlist;
typedef struct { uint64_t id, h; uint32_t kind; void *map; uint64_t size; uint64_t aux; idlist *list;
                 uint8_t *last; uint64_t dev; } ent; // last：ドライバが持っているはずの中身。VR_MEM_READ_DELTA で使う
                                                    // dev：持ち主の装置（装置ならそれ自身）

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
       K_IMAGE, K_VIEW, K_RPASS, K_FB, K_SAMPLER, K_QPOOL, K_BVIEW, K_ACCEL }; // K_ACCEL：acceleration structure（aux は buffer の番号）

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

// 持ち主の装置：作る命令はどれも最初に DEV で装置を引くので、最後に引いた装置を持ち主にする
static uint64_t g_last_dev;

static void put(uint64_t id, uint64_t h, uint32_t kind) {
    if ((g_cnt + 1) * 2 > g_cap) tab_grow();
    size_t s = slot(id);
    while (g_tab[s].id && g_tab[s].id != id) s = (s + 1) & (g_cap - 1);
    if (!g_tab[s].id) g_cnt++;
    if (g_tab[s].id == id) { idlist_free(&g_tab[s].list); free(g_tab[s].last); }
    g_tab[s] = (ent) { id, h, kind, NULL, 0, 0, NULL, NULL, kind == K_DEVICE ? h : g_last_dev };
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
    VkRenderPass: K_RPASS, VkSampler: K_SAMPLER, VkSemaphore: K_SEM, VkShaderModule: K_SHADER, VkAccelerationStructureKHR: K_ACCEL, default: 0)
static uint64_t HK(uint64_t id, uint32_t kind) {
    ent *e = find(id);
    if (e && kind == K_DEVICE && e->kind == K_DEVICE) g_last_dev = e->h;
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

// 窓に出す絵を広げる関数（iPhone のアプリが MetalFX で登録する）。無ければ広げない
typedef int (*vkr_upscale_fn)(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride, uint8_t *dst, uint32_t dw, uint32_t dh, uint32_t dstride);
static vkr_upscale_fn g_upscale;
void vkr_set_upscaler(vkr_upscale_fn fn) { g_upscale = fn; }

#ifdef __ANDROID__
// Android の Vulkan（とくにエミュレーターの gfxstream）は、1.2・1.3 の本体に取り込まれた拡張の名前を一覧から外す。
// DXVK（DirectX 11）は VK_KHR_sampler_mirror_clamp_to_edge・VK_EXT_shader_viewport_index_layer などを名前で探すので、
// 本体が持っているのに名前が無いと装置を作らない。そこで、GPU の版で本体になっている拡張の名前を一覧に足す。
// 頼まれたら（足した名前は GPU の一覧に無いので）装置を作るときに外す。機能の有る無しは足さずに、アプリが機能の構造体で調べる
static const struct { const char *name; int minor; } g_promoted[] = {
    { "VK_KHR_sampler_mirror_clamp_to_edge", 2 }, { "VK_EXT_shader_viewport_index_layer", 2 }, { "VK_KHR_timeline_semaphore", 2 },
    { "VK_KHR_create_renderpass2", 2 }, { "VK_KHR_depth_stencil_resolve", 2 }, { "VK_KHR_draw_indirect_count", 2 },
    { "VK_KHR_driver_properties", 2 }, { "VK_KHR_8bit_storage", 2 }, { "VK_KHR_16bit_storage", 1 }, { "VK_EXT_descriptor_indexing", 2 },
    { "VK_EXT_host_query_reset", 2 }, { "VK_EXT_scalar_block_layout", 2 }, { "VK_KHR_shader_atomic_int64", 2 }, { "VK_KHR_shader_float_controls", 2 },
    { "VK_KHR_spirv_1_4", 2 }, { "VK_KHR_uniform_buffer_standard_layout", 2 }, { "VK_KHR_imageless_framebuffer", 2 },
    { "VK_KHR_separate_depth_stencil_layouts", 2 }, { "VK_KHR_buffer_device_address", 2 }, { "VK_EXT_separate_stencil_usage", 2 },
    { "VK_KHR_multiview", 1 }, { "VK_KHR_shader_draw_parameters", 1 }, { "VK_KHR_variable_pointers", 1 }, { "VK_KHR_relaxed_block_layout", 1 },
    { "VK_KHR_dynamic_rendering", 3 }, { "VK_KHR_synchronization2", 3 }, { "VK_KHR_copy_commands2", 3 }, { "VK_EXT_extended_dynamic_state", 3 },
    { "VK_EXT_extended_dynamic_state2", 3 }, { "VK_KHR_shader_integer_dot_product", 3 }, { "VK_KHR_maintenance4", 3 }, { "VK_EXT_private_data", 3 },
    { "VK_EXT_subgroup_size_control", 3 }, { "VK_EXT_shader_demote_to_helper_invocation", 3 }, { "VK_KHR_shader_non_semantic_info", 3 },
    { "VK_KHR_shader_terminate_invocation", 3 }, { "VK_EXT_image_robustness", 3 }, { "VK_EXT_inline_uniform_block", 3 },
    { "VK_EXT_pipeline_creation_cache_control", 3 }, { "VK_EXT_4444_formats", 3 }, { "VK_KHR_zero_initialize_workgroup_memory", 3 },
};
static int promoted_minor(const char *name) {
    for (size_t i = 0; i < sizeof g_promoted / sizeof *g_promoted; i++) if (!strcmp(g_promoted[i].name, name)) return g_promoted[i].minor;
    return 0;
}
static int dev_has_real_ext(const char *name) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
    VkExtensionProperties *e = calloc(ne ? ne : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, e);
    int has = has_ext(e, ne, name);
    free(e);
    return has;
}
// 一覧 *pe（*pne 個・malloc）に、本体になっている拡張の名前を足す
static void add_promoted_exts(VkExtensionProperties **pe, uint32_t *pne) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_pd, &p);
    int minor = VK_API_VERSION_MAJOR(p.apiVersion) > 1 ? 99 : (int) VK_API_VERSION_MINOR(p.apiVersion);
    if (minor < 1) return;
    // 本体の機能がなければ名乗らない物（DXVK が名前だけで頼りにするもの）
    int mirror = 0, vil = 0;
    if (minor >= 2 && vkGetPhysicalDeviceFeatures2) {
        VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
        vkGetPhysicalDeviceFeatures2(g_pd, &f2);
        mirror = f12.samplerMirrorClampToEdge;
        vil = f12.shaderOutputViewportIndex && f12.shaderOutputLayer;
    }
    for (size_t i = 0; i < sizeof g_promoted / sizeof *g_promoted; i++) {
        const char *nm = g_promoted[i].name;
        if (g_promoted[i].minor > minor || has_ext(*pe, *pne, nm)) continue;
        if (!strcmp(nm, "VK_KHR_sampler_mirror_clamp_to_edge") && !mirror) continue;
        if (!strcmp(nm, "VK_EXT_shader_viewport_index_layer") && !vil) continue;
        *pe = realloc(*pe, sizeof **pe * (*pne + 1));
        memset(&(*pe)[*pne], 0, sizeof **pe);
        snprintf((*pe)[*pne].extensionName, sizeof (*pe)[*pne].extensionName, "%s", nm);
        (*pe)[(*pne)].specVersion = !strcmp(nm, "VK_KHR_sampler_mirror_clamp_to_edge") ? 3 : !strcmp(nm, "VK_KHR_shader_float_controls") ? 4
                                    : !strcmp(nm, "VK_KHR_timeline_semaphore") ? 2 : !strcmp(nm, "VK_KHR_driver_properties") ? 1 : 1;
        (*pne)++;
    }
}
#endif

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
    caps |= VR_CAP_PLANE_CODE; // 縮めた平面をほどける（CPU だけで、どの GPU でも）
    // 頂点の step rate：GPU が拡張を持てば、pipeline の末尾を読んでつなぐ（MoltenVK も持つ）
    {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
        VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex);
        vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, ex);
        for (uint32_t i = 0; i < ne; i++)
            if (!strcmp(ex[i].extensionName, "VK_EXT_vertex_attribute_divisor")) g_divisor = 1;
        free(ex);
        if (g_divisor || fake_divisor()) caps |= VR_CAP_DIVISOR;
    }
#ifdef VR_HAVE_JPEG
    caps |= VR_CAP_JPEG | VR_CAP_JPEG_PIPE; // 窓に出すコマを JPEG に縮められる（iPhone は ImageIO、PC の代役は WIC）
#endif
    if (g_upscale) caps |= VR_CAP_UPSCALE; // 縮める前に MetalFX で広げられる（アプリが関数を登録したとき）
    if (vid_probe()) caps |= VR_CAP_H264 | VR_CAP_H264_LATE; // 窓に出すコマを H.264 に縮められる（iPhone は VideoToolbox、PC の代役は Media Foundation）・遅らせて写せる
    if (rec_available()) caps |= VR_CAP_RECORD; // 窓に出すコマと PC の音を高画質で録画できる（iPhone はアプリが登録した AVAssetWriter、PC の代役は Sink Writer）
    // buffer device address（GPU の中のバッファのアドレス）
    if (fbda.bufferDeviceAddress && (v12 || e_bda)) {
        vkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress) load_opt(v12, "vkGetBufferDeviceAddress", e_bda ? "KHR" : NULL);
        if (vkGetBufferDeviceAddress) caps |= VR_CAP_BDA;
    }
    // レイトレーシングの土台：GPU が acceleration structure・ray query・deferred host operations を持ち、機能もあれば名乗る。
    // iPhone のふり（VKR_LIKE_IOS）のときは VKR_LIKE_IOS_RT=1 のときだけ。iPhone の MoltenVK は 1.4.2 では持たない
    // （PR #2771 を入れて組んだ MoltenVK なら、同じ道で名乗る。work/rt/DESIGN.md）
#define CLEAR_RT(n) n = NULL;
    VKFUNCS_RT(CLEAR_RT)
#undef CLEAR_RT
    if ((caps & VR_CAP_BDA) && has_ext(ep, ne, "VK_KHR_acceleration_structure") && has_ext(ep, ne, "VK_KHR_ray_query") &&
        has_ext(ep, ne, "VK_KHR_deferred_host_operations") && (!like_ios() || like_ios_rt()) && vkGetPhysicalDeviceFeatures2) {
        VkPhysicalDeviceAccelerationStructureFeaturesKHR fas = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
        VkPhysicalDeviceRayQueryFeaturesKHR frq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &fas };
        VkPhysicalDeviceFeatures2 rf2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &frq };
        vkGetPhysicalDeviceFeatures2(g_pd, &rf2);
        vlog(JE("レイトレーシングの機能：accelerationStructure %u・rayQuery %u", "Ray tracing features: accelerationStructure %u, rayQuery %u"), (unsigned) fas.accelerationStructure, (unsigned) frq.rayQuery);
        if (fas.accelerationStructure && frq.rayQuery) {
#define LOAD_RT(n) n = (PFN_##n) gipa(g_inst, #n);
            VKFUNCS_RT(LOAD_RT)
#undef LOAD_RT
            if (vkCreateAccelerationStructureKHR && vkDestroyAccelerationStructureKHR && vkGetAccelerationStructureBuildSizesKHR &&
                vkGetAccelerationStructureDeviceAddressKHR && vkCmdBuildAccelerationStructuresKHR && vkCmdCopyAccelerationStructureKHR &&
                vkCmdWriteAccelerationStructuresPropertiesKHR)
                caps |= VR_CAP_RT;
        }
    }
    // 空の descriptor
    g_nulldesc_gpu = 0;
    if (has_ext(ep, ne, "VK_EXT_robustness2") && vkGetPhysicalDeviceFeatures2) {
        VkPhysicalDeviceRobustness2FeaturesEXT r2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &r2 };
        vkGetPhysicalDeviceFeatures2(g_pd, &f2);
        g_nulldesc_gpu = r2.nullDescriptor == VK_TRUE && (!like_ios() || hang_env("VKR_HANG_NULLDESC"));
        if (like_ios() && g_nulldesc_gpu) vlog(JE("ffhang：iPhone のふりだが本物の空の descriptor を使う（VKR_HANG_NULLDESC）", "ffhang: pretending to be an iPhone but using a real null descriptor (VKR_HANG_NULLDESC)"));
    }
    caps |= VR_CAP_NULL_DESC;
    caps |= VR_CAP_PROGRESS; // 終わった submit の番号を、GPU を待たずに答える
    caps |= VR_CAP_REPLY_EXT | VR_CAP_PUSH_DONE; // 返事に終わった submit の番号を付ける・終わったら頼まれなくても知らせる（VR_SET_OPTIONS で頼まれたつなぎだけ）
    caps |= VR_CAP_DESC_FLAGS; // binding の flags と set ごとの数を読む（vkd3d-proton）
    caps |= VR_CAP_BATCH; // 小さな書き込みをまとめた VR_MEM_WRITE_SPANS を受ける・descriptor の取る・書くをまとめた物を受ける
    // 調べ用：VKR_CAPS=数 で名乗る印を絞る（古い受け手のふりをする）
    const char *mask = getenv("VKR_CAPS");
    if (mask && *mask) caps &= (uint32_t) strtoul(mask, NULL, 0);
    g_caps = caps;
    vlog(JE("できること（caps）0x%x：1.3 %s・dynamic rendering %s・sync2 %s・render pass 2 %s・extended dynamic state %s・query %s", "Capabilities (caps) 0x%x: 1.3 %s, dynamic rendering %s, sync2 %s, render pass 2 %s, extended dynamic state %s, query %s"),
         caps, caps & VR_CAP_VK13 ? "○" : "×", caps & VR_CAP_DYNAMIC_RENDERING ? "○" : "×", caps & VR_CAP_SYNC2 ? "○" : "×",
         caps & VR_CAP_RENDER_PASS2 ? "○" : "×", caps & VR_CAP_EXT_DYNAMIC_STATE ? "○" : "×", caps & VR_CAP_QUERY ? "○" : "×");
    // 名乗れないときに原因を追えるように、GPU の拡張のあり無しも出す。iPhone の MoltenVK は、PR #2771 で組んだ物で
    // MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1 のときだけ持つ（アプリの設定「レイトレーシングを試す」）
    {
        const char *mrt = getenv("MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING");
        vlog(JE("レイトレーシングの拡張：acceleration_structure %s・ray_query %s・deferred_host_operations %s・ray_tracing_pipeline %s・MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=%s", "Ray tracing extensions: acceleration_structure %s, ray_query %s, deferred_host_operations %s, ray_tracing_pipeline %s, MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=%s"),
             has_ext(ep, ne, "VK_KHR_acceleration_structure") ? "○" : "×", has_ext(ep, ne, "VK_KHR_ray_query") ? "○" : "×",
             has_ext(ep, ne, "VK_KHR_deferred_host_operations") ? "○" : "×", has_ext(ep, ne, "VK_KHR_ray_tracing_pipeline") ? "○" : "×",
             mrt && *mrt ? mrt : JE("なし", "none"));
    }
    vlog(JE("レイトレーシングの土台（acceleration structure・ray query）%s%s", "Ray tracing support (acceleration structure, ray query) %s%s"), caps & VR_CAP_RT ? "○" : "×",
         like_ios() ? (like_ios_rt() ? JE("（iPhone のふり・VKR_LIKE_IOS_RT で名乗る）", " (pretending to be an iPhone; advertised with VKR_LIKE_IOS_RT)") : JE("（iPhone のふり：名乗らない）", " (pretending to be an iPhone; not advertised)")) : "");
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
        vlog(JE("MoltenVK（%s）：%.440s", "MoltenVK (%s): %.440s"), sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? JE("誤り", "error") : JE("注意", "warning"), d->pMessage);
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
    if (ir != VK_SUCCESS) { vlog(JE("vkCreateInstance が失敗 (%d)", "vkCreateInstance failed (%d)"), ir); return 0; }
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
        if (g_dbg_mess) vlog(JE("MoltenVK の言い分を記録に流す", "Forwarding MoltenVK messages to the log"));
    }
#define LOAD(n) n = (PFN_##n) gipa(g_inst, #n);
    VKFUNCS(LOAD)
#undef LOAD
    uint32_t n = 1;
    VkResult r = vkEnumeratePhysicalDevices(g_inst, &n, &g_pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0) { vlog(JE("GPU が見つからない (%d, %u 台)", "No GPU found (%d, %u devices)"), r, n); return 0; }
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_pd, NULL, &ne, ep);
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ep[i].extensionName, "VK_KHR_portability_subset")) g_portability = 1;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(g_pd, &p);
    vlog(JE("受け手の GPU: %s", "Receiver GPU: %s"), p.deviceName);
    detect_caps(ep, ne, p.apiVersion < want ? p.apiVersion : want);
    free(ep);
    return 1;
}

// ---- 出した仕事が終わるのを受け手の中で待つ（版 3） ----
// 待つのは、出した仕事が使っている物を変えるときだけ（その仕事まで待つ）。
// 関係の無い物を変える命令は待たないので、GPU が計算している間に次の命令を作れる
static VkDevice g_dev;
typedef struct { VkSemaphore s; uint64_t v; } tlv;
// tw・ts：timeline の値の待ちと知らせ（8 を超えたら tl_over を立て、止まっているかを調べずに前のやり方で待つ）
typedef struct { VkFence f; int own; uint64_t cbs[64]; uint32_t ncb; uint64_t *snap; uint32_t nsnap;
                 VkQueue q; tlv tw[8], ts[8]; uint8_t ntw, nts, tl_over; uint64_t msg; } inflight; // msg：届いた VR_QUEUE_SUBMIT の番号 // snap：調べ用、submit した時点の set
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

// 調べ用の VKR_CHECK。命令ごとに getenv を呼ぶと、環境変数の多い PC では 1 コマに 5,000 回・約 10 ms 食っていたので、最初の 1 回だけ読む
static int vkr_check(void) {
    static int on = -1;
    if (on < 0) on = getenv("VKR_CHECK") != NULL;
    return on;
}

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
static struct { double wait_ms, record_ms, submit_ms, paused_ms; uint64_t waits, records, submits; uint64_t wait_by_op[128]; double wait_ms_by_op[128]; } g_st; // 命令の番号は 64 を超える（READ_JPEG_PIPE 73・GET_PROGRESS 81 など）ので 128 まで数える
static uint32_t g_cur_op; // いま処理している命令（どの命令で GPU を待ったかを数える）
static struct { uint64_t n, in, out; double ms; } g_pcs; // 縮めた平面をほどいた回数・受け取ったバイト・ほどいたバイト・かかった時間
// 書き込みと descriptor の知らせの数（この接続）：1 つずつの書き込み・まとめた書き込みの通数と範囲の数・descriptor の書き込みの通数と個数・vkUpdateDescriptorSets を呼んだ回数
static struct { uint64_t memw, spans_msgs, spans, uds_msgs, uds_writes, uds_calls; } g_bst;
// 命令ごとの処理の時間（この接続・主の糸）：通数・かかった ms・そのうち GPU を待った ms。いつも数える（1 通に 2 回時計を読むだけ）
static struct { uint64_t n[128]; double ms[128], wait[128]; } g_opt;
// アプリの画面のファンを回すための数。アプリが少しずつ読んで、増えた速さで回す
static volatile uint64_t g_ops_total;
static volatile int g_clients;
uint64_t vkr_ops_total(void) { return g_ops_total; }
int vkr_clients(void) { return g_clients; }
// 窓に出すコマとして PC に返した数（JPEG・H.264）。アプリが 1 秒ごとに読んで、毎秒のコマ数を出す
static volatile uint64_t g_frames_total;
uint64_t vkr_frames_total(void) { return g_frames_total; }
// アプリの画面に出す絵：PC に返した JPEG のうち最新の 1 枚の写し。アプリが見ている間（vkr_set_preview(1)）だけ写す。
// 受け手の糸が書き、アプリが 1/30 秒ごとに読むので、ロックで守る。g_pv_serial はコマが変わるたびと消したときに 1 増える
static volatile int g_pv_on;
static uint8_t *g_pv_p;
static size_t g_pv_n, g_pv_cap;
static uint64_t g_pv_serial;
#ifdef _WIN32
static SRWLOCK g_pv_lk = SRWLOCK_INIT;
static void pv_lock(void) { AcquireSRWLockExclusive(&g_pv_lk); }
static void pv_unlock(void) { ReleaseSRWLockExclusive(&g_pv_lk); }
#else
static pthread_mutex_t g_pv_mx = PTHREAD_MUTEX_INITIALIZER;
static void pv_lock(void) { pthread_mutex_lock(&g_pv_mx); }
static void pv_unlock(void) { pthread_mutex_unlock(&g_pv_mx); }
#endif
// 返した JPEG を写す。見ていない間は何もしない
static void pv_store(const uint8_t *p, size_t n) {
    if (!g_pv_on || !p || !n || n > INT32_MAX) return;
    pv_lock();
    if (g_pv_on) {
        if (g_pv_cap < n) {
            // コマごとに大きさが少し変わるので、余裕をもって取る
            size_t c = n + n / 4;
            free(g_pv_p);
            g_pv_p = malloc(c);
            g_pv_cap = g_pv_p ? c : 0;
        }
        g_pv_n = g_pv_p ? n : 0;
        if (g_pv_p) memcpy(g_pv_p, p, n);
        g_pv_serial++;
    }
    pv_unlock();
}
// 持っている 1 枚を捨てる（見るのをやめたとき・PC が切れたとき）
static void pv_clear(void) {
    pv_lock();
    if (g_pv_p) {
        free(g_pv_p);
        g_pv_p = NULL;
        g_pv_n = g_pv_cap = 0;
        g_pv_serial++;
    }
    pv_unlock();
}
// アプリから呼ぶ：画面に絵を出している間は 1。0 にすると写すのをやめ、持っていた 1 枚も捨てる
void vkr_set_preview(int on) {
    g_pv_on = on != 0;
    if (!on) pv_clear();
}
// アプリから呼ぶ：最新の JPEG を buf に写して大きさを返す。まだ無い・cap が足りないときは 0。
// serial にはいつも今の番号を書く（buf が NULL でもよいので、番号だけ見て変わったときだけ読み直せる）
int vkr_latest_frame(uint8_t *buf, int cap, uint64_t *serial) {
    int n = 0;
    pv_lock();
    if (serial) *serial = g_pv_serial;
    if (buf && cap > 0 && g_pv_n && g_pv_n <= (size_t) cap) {
        memcpy(buf, g_pv_p, g_pv_n);
        n = (int) g_pv_n;
    }
    pv_unlock();
    return n;
}
// アプリから受け手の記録に 1 行書く（熱・電池・低電力の移り変わり）
void vkr_note(const char *line) { if (line) vlog("%s", line); }

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

// CPU からの合図（vkSignalSemaphore）をまだ待っていて始まれない仕事に印を付ける。
// 同じ queue でその後ろに出した物も、それが済むまで始まれないので印を付ける。
// 値に届く道：いまの値か、印の付かない前の仕事が知らせる値
static void mark_blocked(uint8_t *blk) {
    tlv reach[64];
    int nr = 0;
    VkQueue bq[16];
    int nbq = 0;
    // 覚えきれなかった仕事があれば、どれが止まっているか分からないので、前のやり方で全部を待つ
    for (int i = 0; i < g_nfl; i++) if (g_fl[i].tl_over) { memset(blk, 0, (size_t) g_nfl); return; }
    for (int i = 0; i < g_nfl; i++) {
        inflight *e = &g_fl[i];
        int b = 0;
        for (int j = 0; j < nbq && !b; j++) if (bq[j] == e->q) b = 1;
        for (int k = 0; k < e->ntw && !b; k++) {
            int at = -1;
            for (int j = 0; j < nr; j++) if (reach[j].s == e->tw[k].s) { at = j; break; }
            if (at < 0) {
                uint64_t now = 0;
                if (vkGetSemaphoreCounterValue(g_dev, e->tw[k].s, &now) != VK_SUCCESS) now = UINT64_MAX; // 分からなければ前のやり方で待つ
                if (nr < 64) { reach[nr] = (tlv) { e->tw[k].s, now }; at = nr++; }
                else if (now < e->tw[k].v) b = 1;
            }
            if (at >= 0 && reach[at].v < e->tw[k].v) b = 1;
        }
        blk[i] = (uint8_t) b;
        if (b) {
            if (nbq < 16) bq[nbq++] = e->q;
            continue;
        }
        for (int k = 0; k < e->nts; k++) {
            int at = -1;
            for (int j = 0; j < nr; j++) if (reach[j].s == e->ts[k].s) { at = j; break; }
            if (at >= 0) { if (e->ts[k].v > reach[at].v) reach[at].v = e->ts[k].v; }
            else if (nr < 64) reach[nr++] = e->ts[k];
        }
    }
}

static void watch_forget(VkFence f); // 見張りの糸が待っている fence を、reset する・消す前に放してもらう（下の「返事の尾と押し出し」）

// k 番目までに出した仕事が終わるのを待って、一覧から外す。CPU からの合図を待っている物は待たずに残す
static void drain_upto(int k) {
    if (k < 0 || !g_nfl) return;
    if (!g_dev) { g_nfl = 0; return; }
    if (k >= g_nfl) k = g_nfl - 1;
    uint8_t blk[256] = { 0 };
    int anytw = 0;
    for (int i = 0; i <= k && !anytw; i++) anytw = g_fl[i].ntw != 0;
    if (anytw) mark_blocked(blk);
    VkFence fs[256];
    uint32_t nf = 0;
    for (int i = 0; i <= k; i++) if (!blk[i]) fs[nf++] = g_fl[i].f;
    if (nf) {
        double t0 = t_ms();
        VkResult r = vkWaitForFences(g_dev, nf, fs, VK_TRUE, UINT64_MAX);
        double dt = t_ms() - t0;
        g_st.wait_ms += dt;
        g_st.waits++;
        if (g_cur_op < 128) { g_st.wait_by_op[g_cur_op]++; g_st.wait_ms_by_op[g_cur_op] += dt; }
        if (r != VK_SUCCESS) {
            vlog(JE("出した仕事を待てない (%d)", "Cannot wait for the submitted work (%d)"), r);
            if (r < 0) g_err = r;
        }
    }
    int w = 0;
    for (int i = 0; i < g_nfl; i++) {
        if (i > k || blk[i]) { g_fl[w++] = g_fl[i]; continue; }
        free(g_fl[i].snap);
        g_fl[i].snap = NULL;
        if (!g_fl[i].own) continue;
        watch_forget(g_fl[i].f);
        vkResetFences(g_dev, 1, &g_fl[i].f);
        if (g_nfpool < 64) g_fpool[g_nfpool++] = g_fl[i].f;
        else vkDestroyFence(g_dev, g_fl[i].f, NULL);
    }
    g_nfl = w;
}

static uint64_t g_submit_msgs; // このつなぎで届いた VR_QUEUE_SUBMIT の数

// 消すのを遅らせた物。msg：消すと言われた時点で届いていた submit の数（そこまでが全部終わったら消してよい）
typedef struct { uint64_t msg; VkDevice d; uint64_t h; uint32_t kind; } grave;
static grave *g_grave;
static size_t g_ngrave, g_cgrave;

static void grave_destroy(const grave *gv) {
    if (gv->kind == K_FB) vkDestroyFramebuffer(gv->d, (VkFramebuffer) (uintptr_t) gv->h, NULL);
    else vkDestroyImageView(gv->d, (VkImageView) (uintptr_t) gv->h, NULL);
}

static int grave_add(VkDevice d, uint64_t h, uint32_t kind) {
    if (g_ngrave == g_cgrave) {
        size_t cap = g_cgrave ? g_cgrave * 2 : 64;
        grave *q = realloc(g_grave, cap * sizeof *q);
        if (!q) return 0;
        g_grave = q;
        g_cgrave = cap;
    }
    g_grave[g_ngrave++] = (grave) { g_submit_msgs, d, h, kind };
    return 1;
}

// 終わった submit の番号（その番号までの submit は全部終わっている）
static uint64_t done_msg(void) {
    for (int i = 0; i < g_nfl; i++)
        if (vkGetFenceStatus(g_dev, g_fl[i].f) != VK_SUCCESS) return g_fl[i].msg ? g_fl[i].msg - 1 : 0;
    return g_submit_msgs;
}

// 消してよくなった物を消す。dev を渡せば、その装置の物を全部消す（装置を消す前）
static void grave_sweep(VkDevice dev) {
    if (!g_ngrave) return;
    uint64_t done = dev ? 0 : done_msg();
    size_t w = 0;
    for (size_t i = 0; i < g_ngrave; i++)
        if ((dev && g_grave[i].d == dev) || (!dev && g_grave[i].msg <= done)) grave_destroy(&g_grave[i]);
        else g_grave[w++] = g_grave[i];
    g_ngrave = w;
}

static void drain(void) { drain_upto(g_nfl - 1); grave_sweep(VK_NULL_HANDLE); }

// ---- 返事の尾と、終わった submit の押し出し（VR_CAP_REPLY_EXT・VR_CAP_PUSH_DONE） ----
// 尾：どの返事にも「終わった submit の番号」を付け、ドライバが VR_GET_PROGRESS を往復させずに fence の進みを知れるようにする。
// 押し出し：主の糸は命令を順に処理するので、VR_GET_PROGRESS の返事は前に積まれた命令（GPU を待つ MEM_WRITE・JPEG など）の後になる。
// 見張りの糸が submit の終わりを待ち、終わったらすぐ知らせる。送るのは主の糸の返事と錠で分ける（返事の途中には入れない）
static int g_rx_ext;          // このつなぎで返事に尾を付ける
static uint32_t g_reply_seq;  // このつなぎで送った返事の数
static uint32_t g_epoch;      // このつなぎで submit の番号を 0 に戻した回数
#ifdef _WIN32
static SRWLOCK g_send_lk = SRWLOCK_INIT;
static void send_lock(void) { AcquireSRWLockExclusive(&g_send_lk); }
static void send_unlock(void) { ReleaseSRWLockExclusive(&g_send_lk); }
#else
static pthread_mutex_t g_send_mx = PTHREAD_MUTEX_INITIALIZER;
static void send_lock(void) { pthread_mutex_lock(&g_send_mx); }
static void send_unlock(void) { pthread_mutex_unlock(&g_send_mx); }
#endif

// 尾に入れる「終わった submit の番号」。頭から終わった仕事を一覧から外して（VR_GET_PROGRESS と同じ。終わっているので待たない）数える
static uint64_t done_now(void) {
    if (!g_dev) return g_nfl ? 0 : g_submit_msgs;
    int k = -1;
    for (int i = 0; i < g_nfl; i++) {
        if (vkGetFenceStatus(g_dev, g_fl[i].f) != VK_SUCCESS) break;
        k = i;
    }
    if (k >= 0) drain_upto(k);
    return g_nfl ? (g_fl[0].msg ? g_fl[0].msg - 1 : 0) : g_submit_msgs;
}

// 見張り：submit に付いた fence（アプリの物か自前の物）を、別の糸が出した順に待ち、終わったらすぐ知らせる。
// 前は空の submit に見張り用の fence を付けていたが、submit が倍になる。fence はそのまま借りる。
// 主の糸がその fence を reset・消す前には watch_forget を呼ぶ（見張りが待っている最中なら待ち終わるまで待ち、一覧の印は「終わった」に替える）。
// 主の糸が reset・消すのは終わった fence だけ（まだの物は名前を付け替える：VR_RESET_FENCES）
#define WATCH_RING 512
static struct {
    int on;            // このつなぎで頼まれた
    int run, started;  // 糸が動いている
    vr_sock s;
    VkDevice dev;
    VkFence f[WATCH_RING]; // VK_NULL_HANDLE：主の糸が reset・消した（もう終わっている）
    uint64_t m[WATCH_RING];
    int head, n;
    VkFence cur;       // 見張りが錠の外で待っている fence
    uint64_t sent;     // 最後に知らせた番号
    uint32_t epoch;
    uint64_t pushes, adds;
#ifdef _WIN32
    SRWLOCK lk;
    CONDITION_VARIABLE cv;
    HANDLE thr;
#else
    pthread_mutex_t mx;
    pthread_cond_t cv;
    pthread_t thr;
#endif
} g_watch = {
#ifdef _WIN32
    .lk = SRWLOCK_INIT, .cv = CONDITION_VARIABLE_INIT,
#else
    .mx = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER,
#endif
};
#ifdef _WIN32
static void watch_lock(void) { AcquireSRWLockExclusive(&g_watch.lk); }
static void watch_unlock(void) { ReleaseSRWLockExclusive(&g_watch.lk); }
static void watch_wait(void) { SleepConditionVariableSRW(&g_watch.cv, &g_watch.lk, INFINITE, 0); }
static void watch_wake(void) { WakeAllConditionVariable(&g_watch.cv); }
#else
static void watch_lock(void) { pthread_mutex_lock(&g_watch.mx); }
static void watch_unlock(void) { pthread_mutex_unlock(&g_watch.mx); }
static void watch_wait(void) { pthread_cond_wait(&g_watch.cv, &g_watch.mx); }
static void watch_wake(void) { pthread_cond_broadcast(&g_watch.cv); }
#endif

// stop=1：これ以降は知らせない（ドライバは VR_GET_PROGRESS の問い合わせに戻る）。done はそのときまでに分かっている番号
static void watch_push(uint64_t done, int stop) {
    uint8_t m[8 + VR_EXT_BYTES];
    uint32_t h[2] = { stop ? 1u : 0u, 0 }, seq = VR_PUSH_SEQ, ep = g_watch.epoch;
    memcpy(m, h, 8);
    memcpy(m + 8, &done, 8);
    memcpy(m + 16, &seq, 4);
    memcpy(m + 20, &ep, 4);
    send_lock();
    vr_send_all(g_watch.s, m, sizeof m); // 送れなければ、主の糸が次の読みか送りで切れたことに気づく
    send_unlock();
    g_watch.pushes++;
}

static void watch_loop(void) {
    for (;;) {
        watch_lock();
        while (g_watch.run && !g_watch.n) watch_wait();
        if (!g_watch.run) { watch_unlock(); return; }
        VkFence f = g_watch.f[g_watch.head];
        VkResult w = VK_SUCCESS;
        if (f) {
            g_watch.cur = f;
            watch_unlock();
            w = vkWaitForFences(g_watch.dev, 1, &f, VK_TRUE, 50000000ull);
            watch_lock();
            g_watch.cur = VK_NULL_HANDLE;
            watch_wake(); // watch_forget で待っている主の糸を起こす
        }
        if (w == VK_TIMEOUT) { watch_unlock(); continue; }
        if (w != VK_SUCCESS) {
            // GPU を失ったなど：知らせるのをやめる（ドライバは VR_GET_PROGRESS に戻る）
            g_watch.run = 0;
            watch_wake();
            watch_unlock();
            watch_push(g_watch.sent, 1);
            return;
        }
        // 頭（いま待ち終えた物。外すのは見張りだけなので、まだ頭にある）と、その後ろの終わっている物をまとめて外す。
        // 番号は出した順なので、外した最後の物の番号までが全部終わっている。主の糸が「終わった」に替えた物（NULL）もそのまま外す
        uint64_t done = 0;
        for (int first = 1; g_watch.n; first = 0) {
            VkFence hf = g_watch.f[g_watch.head];
            if (!first && hf && vkGetFenceStatus(g_watch.dev, hf) != VK_SUCCESS) break;
            done = g_watch.m[g_watch.head];
            g_watch.head = (g_watch.head + 1) % WATCH_RING;
            g_watch.n--;
        }
        watch_wake(); // 一覧が満ちて待っている主の糸を起こす
        watch_unlock();
        if (done > g_watch.sent) {
            g_watch.sent = done;
            watch_push(done, 0);
        }
    }
}
#ifdef _WIN32
static DWORD WINAPI watch_thread(LPVOID a) { (void) a; watch_loop(); return 0; }
#else
// iPhone では主の糸と同じく速いコアに置く（省電力のコアに回ると、知らせが遅れてドライバの待ちが延びる）
static void *watch_thread(void *a) { (void) a; qos_hi(); watch_loop(); return NULL; }
#endif

// 主の糸が fence を reset する・消す前に呼ぶ
static void watch_forget(VkFence f) {
    if (!g_watch.started || !f) return;
    watch_lock();
    while (g_watch.cur == f) watch_wait();
    for (int i = 0; i < g_watch.n; i++) {
        int at = (g_watch.head + i) % WATCH_RING;
        if (g_watch.f[at] == f) g_watch.f[at] = VK_NULL_HANDLE;
    }
    watch_unlock();
}

// 見張りを止める（装置を消す前・つなぎが切れたとき・数え直す前に呼ぶ）。fence は主の糸の物なので消さない
static void watch_stop(void) {
    if (!g_watch.started) return;
    watch_lock();
    g_watch.run = 0;
    watch_wake();
    watch_unlock();
#ifdef _WIN32
    WaitForSingleObject(g_watch.thr, INFINITE);
    CloseHandle(g_watch.thr);
#else
    pthread_join(g_watch.thr, NULL);
#endif
    g_watch.started = 0;
    g_watch.n = g_watch.head = 0;
    g_watch.cur = VK_NULL_HANDLE;
    g_watch.dev = VK_NULL_HANDLE;
    vlog(JE("終わった submit の見張りを止めた（知らせた %llu 回・見張った submit %llu）", "Stopped the submit watcher (notified %llu times, watched %llu submits)"), (unsigned long long) g_watch.pushes, (unsigned long long) g_watch.adds);
}

// 糸を起こしたばかりなら 2（いまの仕事の一覧を全部積んである）・動いていれば 1・使えなければ 0
static int watch_start(vr_sock s) {
    if (g_watch.started) return g_watch.run;
    if (!g_dev) return 0;
    g_watch.dev = g_dev;
    g_watch.s = s;
    g_watch.epoch = g_epoch;
    g_watch.sent = 0;
    g_watch.pushes = g_watch.adds = 0;
    g_watch.n = g_watch.head = 0;
    g_watch.cur = VK_NULL_HANDLE;
    // まだ終わっていない仕事（drain のあとでも、CPU からの合図を待つ物は残る）を先に積む。積まずに後の submit を知らせると、
    // 残った仕事より先の番号を「終わった」と言ってしまう
    for (int i = 0; i < g_nfl && g_watch.n < WATCH_RING; i++) {
        g_watch.f[g_watch.n] = g_fl[i].f;
        g_watch.m[g_watch.n++] = g_fl[i].msg;
    }
    g_watch.run = 1;
#ifdef _WIN32
    g_watch.thr = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    g_watch.started = g_watch.thr != NULL;
#else
    g_watch.started = pthread_create(&g_watch.thr, NULL, watch_thread, NULL) == 0;
#endif
    if (!g_watch.started) {
        g_watch.run = 0;
        g_watch.on = 0;
        g_watch.n = 0;
        vlog(JE("見張りの糸を作れないので、終わった submit を知らせない", "Cannot create the watcher thread, so finished submits will not be signaled"));
        watch_push(0, 1);
        return 0;
    }
    return 2;
}

// submit の番号 msg を fence f 付きで出したあとに呼ぶ
// （fence が無い submit は無い：受け手が自前の物を付ける）
static void watch_add(vr_sock s, VkFence f, uint64_t msg) {
    if (!g_watch.on || !f) return;
    int st = watch_start(s);
    if (st != 1) return; // 起こしたばかりなら、この submit も一覧から積んである
    watch_lock();
    int full = g_watch.n == WATCH_RING;
    if (full) {
        // GPU が 512 本遅れている（CPU からの合図を待つ仕事が止まっているなど）。飛ばして後の番号を知らせると、飛ばした仕事を
        // 「終わった」と言ってしまうので、このつなぎではもう積まない。積んである分は知らせてから、やめたことを伝える。
        // 待って空きを作ると、合図の命令を読む主の糸が止まって抜けられない
        g_watch.on = 0;
    } else if (g_watch.run) {
        int at = (g_watch.head + g_watch.n) % WATCH_RING;
        g_watch.f[at] = f;
        g_watch.m[at] = msg;
        g_watch.n++;
        g_watch.adds++;
        watch_wake();
    }
    watch_unlock();
    if (full) {
        vlog(JE("見張りの一覧が満ちたので、終わった submit の押し出しをやめる", "The watcher list is full, so pushing finished submits is turned off"));
        watch_push(0, 1);
    }
}

// 待たれている timeline の値のうち、まだ届いていない物を受け手が合図する（つなぎが切れたあとだけ使う）
static int unblock_waits(void) {
    int n = 0;
    for (int i = 0; i < g_nfl; i++)
        for (int k = 0; k < g_fl[i].ntw; k++) {
            uint64_t now = 0;
            if (vkGetSemaphoreCounterValue(g_dev, g_fl[i].tw[k].s, &now) != VK_SUCCESS || now >= g_fl[i].tw[k].v) continue;
            VkSemaphoreSignalInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = g_fl[i].tw[k].s, .value = g_fl[i].tw[k].v };
            if (vkSignalSemaphore(g_dev, &si) == VK_SUCCESS) n++;
        }
    return n;
}

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
static void vid_reset(void);
// 手伝いの物を dev の上から消す（自前の fence・つなぎの semaphore・空の代わりの物）。どれも要るときにまた作る
static void helpers_free(VkDevice dev) {
    if (dev) {
        for (int i = 0; i < g_nfpool; i++) vkDestroyFence(dev, g_fpool[i], NULL);
        for (int i = 0; i < 4; i++) if (g_chain[i]) vkDestroySemaphore(dev, g_chain[i], NULL);
        if (g_dummy_vb) vkDestroyBuffer(dev, g_dummy_vb, NULL);
        if (g_dummy_mem) vkFreeMemory(dev, g_dummy_mem, NULL);
        if (g_dummy_smp) vkDestroySampler(dev, g_dummy_smp, NULL);
        if (g_dummy_iview) vkDestroyImageView(dev, g_dummy_iview, NULL);
        if (g_dummy_img) vkDestroyImage(dev, g_dummy_img, NULL);
        if (g_dummy_img_mem) vkFreeMemory(dev, g_dummy_img_mem, NULL);
        if (g_dummy_bview) vkDestroyBufferView(dev, g_dummy_bview, NULL);
        if (g_dummy_tb) vkDestroyBuffer(dev, g_dummy_tb, NULL);
        if (g_dummy_tb_mem) vkFreeMemory(dev, g_dummy_tb_mem, NULL);
    }
    memset(g_chain, 0, sizeof g_chain);
    g_chain_last = -1;
    g_nfpool = 0;
    g_dummy_img = VK_NULL_HANDLE;
    g_dummy_iview = VK_NULL_HANDLE;
    g_dummy_img_mem = VK_NULL_HANDLE;
    g_dummy_tb = VK_NULL_HANDLE;
    g_dummy_bview = VK_NULL_HANDLE;
    g_dummy_tb_mem = VK_NULL_HANDLE;
    g_dummy_vb = VK_NULL_HANDLE;
    g_dummy_mem = VK_NULL_HANDLE;
    g_dummy_smp = VK_NULL_HANDLE;
}

// 装置 dev の物を全部消してから dev を消し、表からも外す。last なら持ち主の分からない物もここで消す
static void dev_free(VkDevice dev, int last) {
    vkDeviceWaitIdle(dev);
    grave_sweep(dev);
    uint64_t me = (uint64_t) (uintptr_t) dev;
    static const int order[] = { K_PIPELINE, K_FB, K_RPASS, K_PLAYOUT, K_SHADER, K_DPOOL, K_DSL, K_SAMPLER, K_CPOOL, K_FENCE, K_SEM,
                                 K_QPOOL, K_VIEW, K_BVIEW, K_ACCEL, K_IMAGE, K_BUFFER, K_MEMORY };
    for (size_t o = 0; o < sizeof order / sizeof *order; o++)
        for (size_t i = 0; i < g_cap; i++) {
            ent *e = &g_tab[i];
            if (!e->id || !e->h || e->kind != (uint32_t) order[o] || !(e->dev == me || (last && !e->dev))) continue;
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
            case K_ACCEL: if (vkDestroyAccelerationStructureKHR) vkDestroyAccelerationStructureKHR(dev, (VkAccelerationStructureKHR) e->h, NULL); break;
            }
            e->h = 0; // 消した印（表から外すのは最後にまとめて）
        }
    vkDestroyDevice(dev, NULL);
    // 表から外す：この装置の物（消した物・キュー・command buffer・descriptor set）と装置そのもの
    size_t nk = 0, ck = 64;
    uint64_t *kill = malloc(ck * sizeof *kill);
    for (size_t i = 0; i < g_cap && kill; i++) {
        ent *e = &g_tab[i];
        if (!e->id) continue;
        if (!(e->dev == me || (last && !e->dev))) continue;
        if (nk == ck) {
            uint64_t *q = realloc(kill, ck * 2 * sizeof *kill);
            if (!q) break;
            kill = q;
            ck *= 2;
        }
        idlist_free(&e->list);
        kill[nk++] = e->id;
    }
    for (size_t i = 0; i < nk; i++) del(kill[i]);
    free(kill);
}

static void cleanup(void) {
    drain();
    watch_stop(); // 見張りが待つ fence は装置の物なので、装置を消す前に止める。数え直す前に止めて、古い番号を新しい数え方で知らせない
    g_submit_msgs = 0;
    g_epoch++;
    // 合図を待ったまま残った仕事は、待たれている値を受け手が合図して流してから待つ
    for (int round = 0; g_nfl && g_dev && round < 8; round++) {
        int n = unblock_waits();
        if (n) vlog(JE("切れたあとに合図を待っていた仕事が残ったので、受け手が %d 個合図した", "Jobs waiting on signals were left after the disconnect, so the receiver signaled %d of them"), n);
        drain();
        if (!n) break;
    }
    if (g_nfl) {
        vlog(JE("合図を待つ仕事が %d 本残ったまま片づける", "Cleaning up with %d signal-waiting jobs still left"), g_nfl);
        g_nfl = 0;
    }
    jp_reset();
    vid_reset();
    for (size_t i = 0; i < g_ncbi; i++) { free(g_cbi[i].sets); idlist_free(&g_cbi[i].mems); }
    g_ncbi = 0;
    if (g_err == VK_ERROR_DEVICE_LOST) g_lost = 1;
    g_err = VK_SUCCESS;
    helpers_free(g_dev);
    g_dev = VK_NULL_HANDLE;
    // 装置ごとに、その装置の物を片づけてから装置を消す。持ち主の分からない物は最後の装置で片づける
    for (;;) {
        VkDevice dev = VK_NULL_HANDLE;
        int ndev = 0;
        for (size_t i = 0; i < g_cap; i++)
            if (g_tab[i].id && g_tab[i].kind == K_DEVICE) { ndev++; if (!dev) dev = (VkDevice) (uintptr_t) g_tab[i].h; }
        if (!dev) break;
        dev_free(dev, ndev == 1);
    }
    g_dummy_img = VK_NULL_HANDLE;
    g_dummy_iview = VK_NULL_HANDLE;
    g_dummy_img_mem = VK_NULL_HANDLE;
    g_dummy_tb = VK_NULL_HANDLE;
    g_dummy_bview = VK_NULL_HANDLE;
    g_dummy_tb_mem = VK_NULL_HANDLE;
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

// descriptor の書き込みを、1 つの知らせの中でためて 1 回の vkUpdateDescriptorSets にまとめる。
// 1 個ずつ渡すと 1 コマに 5,000 回呼んでいた（MoltenVK は 1 回ごとが重い）。順番はためた順のまま（仕様でも順に書かれる）
typedef struct { VkWriteDescriptorSet *w; void **info; uint32_t n, cap; } uds_batch;
static uds_batch g_uds;
static void uds_add(uds_batch *b, VkDevice d, const VkWriteDescriptorSet *w, void *info) {
    if (b->n == b->cap) {
        uint32_t c = b->cap ? b->cap * 2 : 256;
        VkWriteDescriptorSet *nw = realloc(b->w, (size_t) c * sizeof *nw);
        if (nw) b->w = nw;
        void **ni = nw ? realloc(b->info, (size_t) c * sizeof *ni) : NULL;
        if (ni) b->info = ni;
        if (!nw || !ni) { // 置き場が取れないときは、今までどおりその場で 1 個書く
            vkUpdateDescriptorSets(d, 1, w, 0, NULL);
            free(info);
            return;
        }
        b->cap = c;
    }
    b->w[b->n] = *w;
    b->info[b->n] = info;
    b->n++;
}
static void uds_flush(uds_batch *b, VkDevice d, int apply) {
    if (apply && b->n) { vkUpdateDescriptorSets(d, b->n, b->w, 0, NULL); g_bst.uds_calls++; g_bst.uds_writes += b->n; }
    for (uint32_t i = 0; i < b->n; i++) free(b->info[i]);
    b->n = 0;
}

// ---- 返事 ----
static vr_buf g_out;
// PC が切れたか（読む物があるのに 0 バイト＝相手が閉じた）。待たずに調べる
static int peer_gone(vr_sock s) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    struct timeval tv = { 0, 0 };
    if (select((int) s + 1, &rf, NULL, NULL, &tv) != 1) return 0;
    char b;
    return recv(s, &b, 1, MSG_PEEK) == 0;
}

static int reply(vr_sock s, int32_t result) {
    uint32_t h[2] = { (uint32_t) result, (uint32_t) g_out.n };
    int ok;
    if (g_rx_ext) {
        // 頭のすぐ後ろに尾（終わった submit の番号・通し番号・数え直しの回数）。小さな返事は 1 回で送る
        uint8_t m[8 + VR_EXT_BYTES + 256];
        uint64_t done = done_now();
        uint32_t seq = g_reply_seq, ep = g_epoch;
        memcpy(m, h, 8);
        memcpy(m + 8, &done, 8);
        memcpy(m + 16, &seq, 4);
        memcpy(m + 20, &ep, 4);
        size_t k = 8 + VR_EXT_BYTES;
        if (g_out.n <= 256) { if (g_out.n) memcpy(m + k, g_out.p, g_out.n); k += g_out.n; }
        send_lock();
        ok = vr_send_all(s, m, k) && (g_out.n <= 256 || vr_send_all(s, g_out.p, g_out.n));
        send_unlock();
    } else {
        ok = vr_send_all(s, h, 8) && vr_send_all(s, g_out.p, g_out.n);
    }
    g_reply_seq++;
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
    if (!(g_caps & VR_CAP_EXT_DYNAMIC_STATE)) { vlog(JE("この値（%u）は変えられない", "This value (%u) cannot be changed"), st); return; }
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
    vlog(JE("知らない値の変え方 %u（値 %u 個）", "Unknown way to change a value %u (%u values)"), st, n);
}

// pipeline layout と pipeline が持つ set の数を覚える（ent の size に「数＋1」。0 は分からない。layout と pipeline では size を使っていない）。
// replay が、受け手に無い set の番号を使う pipeline の draw・dispatch を見分けるのに使う
static void note_layout_sets(uint64_t id, uint32_t ns) {
    ent *e = find(id);
    if (e && e->kind == K_PLAYOUT) e->size = (uint64_t) ns + 1;
}
static void note_pipe_sets(uint64_t pid, uint64_t lid) {
    ent *l = find(lid), *p = find(pid);
    if (p && p->kind == K_PIPELINE) p->size = l && l->kind == K_PLAYOUT ? l->size : 0;
}
// その pipeline が使うかもしれない set の番号の印（分からなければ全部）
static uint32_t pipe_set_mask(uint64_t pid) {
    ent *p = find(pid);
    if (!p || p->kind != K_PIPELINE || !p->size) return ~0u;
    uint64_t ns = p->size - 1;
    return ns >= 32 ? ~0u : (uint32_t) ((1ull << ns) - 1);
}
static uint32_t set_range_mask(uint32_t first, uint32_t n) {
    uint32_t m = 0;
    for (uint32_t i = first; i < first + n && i < 32; i++) m |= 1u << i;
    return m;
}
static uint64_t g_missing_set_binds; // 受け手に無い set をつなごうとした回数（記録に出す数を絞る）

// ---- レイトレーシングの土台（VR_CAP_RT） ----
// vr_accel_geoms（proto.h）を読む。返した並びは呼んだ側が free する
static VkAccelerationStructureGeometryKHR *rd_geoms(vr_rd *r, uint32_t *count) {
    uint32_t n = vr_ru32(r);
    *count = 0;
    if (r->bad || n > (r->n - r->at) / 8) { r->bad = 1; return NULL; } // 1 つ 8 バイトより短いことはない
    VkAccelerationStructureGeometryKHR *g = calloc(n ? n : 1, sizeof *g);
    if (!g) { r->bad = 1; return NULL; }
    for (uint32_t i = 0; i < n && !r->bad; i++) {
        g[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g[i].geometryType = (VkGeometryTypeKHR) vr_ru32(r);
        g[i].flags = vr_ru32(r);
        if (g[i].geometryType == VK_GEOMETRY_TYPE_TRIANGLES_KHR) {
            VkAccelerationStructureGeometryTrianglesDataKHR *t = &g[i].geometry.triangles;
            t->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t->vertexFormat = (VkFormat) vr_ru32(r);
            t->vertexData.deviceAddress = vr_ru64(r);
            t->vertexStride = vr_ru64(r);
            t->maxVertex = vr_ru32(r);
            t->indexType = (VkIndexType) vr_ru32(r);
            t->indexData.deviceAddress = vr_ru64(r);
            t->transformData.deviceAddress = vr_ru64(r);
        } else if (g[i].geometryType == VK_GEOMETRY_TYPE_AABBS_KHR) {
            VkAccelerationStructureGeometryAabbsDataKHR *x = &g[i].geometry.aabbs;
            x->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
            x->data.deviceAddress = vr_ru64(r);
            x->stride = vr_ru64(r);
        } else if (g[i].geometryType == VK_GEOMETRY_TYPE_INSTANCES_KHR) {
            VkAccelerationStructureGeometryInstancesDataKHR *x = &g[i].geometry.instances;
            x->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            x->arrayOfPointers = vr_ru32(r);
            x->data.deviceAddress = vr_ru64(r);
        } else {
            vlog(JE("acceleration structure：知らない geometry の種類 %u", "acceleration structure: unknown geometry type %u"), (unsigned) g[i].geometryType);
            r->bad = 1;
        }
    }
    *count = n;
    return g;
}

// acceleration structure の番号 → その置き場の buffer のメモリを、記録の「使うメモリ」に足す
static void use_accel(cbinfo *ci, uint64_t id) {
    ent *e = find(id);
    if (e && e->kind == K_ACCEL) use_buffer(ci, e->aux);
}

// ---- command buffer に積まれた命令を実行する ----
static void replay(VkCommandBuffer cb, vr_rd *r, cbinfo *ci) {
    // 有効な描き方・計算の仕方がつながっていない draw と dispatch は飛ばす
    // （作れなかった pipeline のまま描くと、iPhone の MoltenVK は描く所で落ちる。10/7 に zink で起きた）
    int gfx = 0, cmp = 0;
    int rp_off = 0; // 始められなかった render pass の中（次の subpass・終わり・draw を飛ばす）
    uint32_t skipped = 0;
    // 受け手に無い set（取れなかった・消えた番号）は MoltenVK に渡さない。MoltenVK は set を submit のときに読むので、
    // 空の set をつなぐと submit の中で落ちる（10/10 iPhone の bindDescriptorSets の 0x3c）。つながなかった番号を miss に覚え、
    // その番号を使うかもしれない pipeline の draw・dispatch も飛ばす（描くときにも set を読んで落ちるので）。[0] 描く・[1] 計算
    int gfx_pl = 0, cmp_pl = 0;
    uint32_t miss[2] = { 0, 0 }, uses[2] = { ~0u, ~0u };
#define SET_GATE() (gfx = gfx_pl && !(miss[0] & uses[0]), cmp = cmp_pl && !(miss[1] & uses[1]))
    while (r->at < r->n && !r->bad) {
        uint32_t op = vr_ru32(r);
        switch (op) {
        case VR_CMD_BIND_PIPELINE: {
            uint32_t bp = vr_ru32(r);
            uint64_t pid = vr_ru64(r);
            VkPipeline pl = OBJ(VkPipeline, pid);
            tracef(ci, "  pipeline %llu%s", (unsigned long long) pid, pl ? "" : JE("（無い）", " (missing)"));
            if (pl) vkCmdBindPipeline(cb, (VkPipelineBindPoint) bp, pl);
            if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) { gfx_pl = pl != VK_NULL_HANDLE; uses[0] = pipe_set_mask(pid); }
            else if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) { cmp_pl = pl != VK_NULL_HANDLE; uses[1] = pipe_set_mask(pid); }
            SET_GATE();
        } break;
        case VR_CMD_BIND_DESCRIPTOR_SETS: {
            uint32_t bp = vr_ru32(r);
            VkPipelineLayout pl = OBJ(VkPipelineLayout, vr_ru64(r));
            uint32_t first = vr_ru32(r), n = vr_ru32(r);
            VkDescriptorSet sets[32];
            uint32_t nmiss = 0;
            uint64_t miss_id = 0;
            for (uint32_t i = 0; i < n && i < 32; i++) {
                uint64_t sid = vr_ru64(r);
                sets[i] = OBJ(VkDescriptorSet, sid);
                if (!sets[i] && !nmiss++) miss_id = sid;
                if (dbg_on() && ci) {
                    vr_buf *d = dset_of(sid);
                    tracef(ci, JE("  set %llu%s：%.*s", "  set %llu%s: %.*s"), (unsigned long long) sid, sets[i] ? "" : JE("（無い）", " (missing)"), (int) d->n, d->p ? (const char *) d->p : "");
                }
                if (ci) {
                    cbi_add_set(ci, sid);
                    ent *se = find(sid);
                    if (se && se->list) for (uint32_t k = 0; k < se->list->n; k++) use_buffer(ci, se->list->ids[k]);
                }
            }
            for (uint32_t i = 32; i < n && !r->bad; i++) (void) vr_ru64(r); // 上限を超えた分は読み捨てる（並びをずらさない）
            uint32_t nd = vr_ru32(r);
            uint32_t dyn[64];
            for (uint32_t i = 0; i < nd && i < 64; i++) dyn[i] = vr_ru32(r);
            for (uint32_t i = 64; i < nd && !r->bad; i++) (void) vr_ru32(r);
            int k = bp == VK_PIPELINE_BIND_POINT_GRAPHICS ? 0 : bp == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : -1;
            uint32_t range = set_range_mask(first, n < 32 ? n : 32);
            if (pl && !nmiss) {
                vkCmdBindDescriptorSets(cb, (VkPipelineBindPoint) bp, pl, first, n < 32 ? n : 32, sets, nd < 64 ? nd : 64, dyn);
                if (k >= 0) miss[k] &= ~range;
            } else {
                // つながなかった番号には前の set が残るか、何もつながっていない。どちらもその番号を使う draw は描けない
                if (k >= 0) miss[k] |= range;
                if (!pl) vlog(JE("descriptor set をつなぐ layout が無いので飛ばした", "Skipped: there is no layout to bind the descriptor set"));
                else if (++g_missing_set_binds <= 20 || g_missing_set_binds % 1000 == 0)
                    vlog(JE("受け手に無い descriptor set（%llu）をつなごうとした（%u 番から %u 個のうち %u 個が無い・command buffer %llu・%llu 回目）。つながずに、その番号を使う draw・dispatch を飛ばす", "Tried to bind a descriptor set (%llu) the receiver does not have (from set %u: %u sets, %u missing; command buffer %llu, run %llu). Not binding it, and skipping the draws and dispatches that use those numbers"),
                         (unsigned long long) miss_id, first, n, nmiss, (unsigned long long) (ci ? ci->cb : 0), (unsigned long long) g_missing_set_binds);
            }
            SET_GATE();
        } break;
        case VR_CMD_PUSH_CONSTANTS: {
            VkPipelineLayout pl = OBJ(VkPipelineLayout, vr_ru64(r));
            uint32_t st = vr_ru32(r), off = vr_ru32(r), sz;
            const void *d = vr_rbytes(r, &sz);
            if (dbg_on() && d) {
                char h[200] = "";
                for (uint32_t i = 0; i < sz / 4 && i < 24; i++) snprintf(h + strlen(h), sizeof h - strlen(h), " %u", ((const uint32_t *) d)[i]);
                tracef(ci, JE("  push %u バイト:%s", "  push %u bytes:%s"), sz, h);
            }
            if (d && pl) vkCmdPushConstants(cb, pl, st, off, sz, d);
            else if (d) vlog(JE("push constants の layout が無いので飛ばした", "Skipped: there is no layout for push constants"));
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
                    tracef(ci, JE("  copy buffer %llu（%llu バイト）+%llu → %llu（%llu バイト）+%llu・%llu バイト", "  copy buffer %llu (%llu bytes) +%llu -> %llu (%llu bytes) +%llu, %llu bytes"), (unsigned long long) sid,
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
            tracef(ci, JE("  fill buffer %llu +%llu・%llu バイト", "  fill buffer %llu +%llu, %llu bytes"), (unsigned long long) bid, (unsigned long long) off, (unsigned long long) sz);
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
            if (nc > (r->n - r->at) / sizeof(VkClearValue)) { r->bad = 1; break; }
            VkClearValue cv16[16], *cv = nc <= 16 ? cv16 : malloc(nc * sizeof *cv);
            if (!cv) { r->bad = 1; break; }
            for (uint32_t i = 0; i < nc; i++) vr_rcopy(r, &cv[i], sizeof cv[i]);
            bi.clearValueCount = nc;
            bi.pClearValues = cv;
            uint32_t contents = vr_ru32(r);
            // 描く先の画像のメモリを「使う」として覚える
            ent *fe = find(fid);
            if (ci && fe && fe->list)
                for (uint32_t k = 0; k < fe->list->n; k++) {
                    ent *ve = find(fe->list->ids[k]);
                    if (ve) use_buffer_mem(ci, ve->aux);
                }
            if (bi.renderPass && bi.framebuffer) {
                vkCmdBeginRenderPass(cb, &bi, (VkSubpassContents) contents);
                rp_off = 0;
            } else {
                vlog(JE("render pass を始めない（%s が無い）。中の命令も飛ばす", "Not starting the render pass (no %s). Skipping the commands inside too"), bi.renderPass ? "framebuffer" : "render pass");
                rp_off = 1;
            }
            if (cv != cv16) free(cv);
        } break;
        case VR_CMD_END_RENDER_PASS:
            if (!rp_off) vkCmdEndRenderPass(cb);
            rp_off = 0;
            break;
        case VR_CMD_NEXT_SUBPASS: {
            uint32_t contents = vr_ru32(r);
            if (!rp_off) vkCmdNextSubpass(cb, (VkSubpassContents) contents);
        } break;
        case VR_CMD_DRAW: {
            uint32_t a = vr_ru32(r), b = vr_ru32(r), c = vr_ru32(r), d = vr_ru32(r);
            if (!gfx || rp_off) { skipped++; break; }
            vkCmdDraw(cb, a, b, c, d);
        } break;
        case VR_CMD_DRAW_INDEXED: {
            uint32_t a = vr_ru32(r), b = vr_ru32(r), c = vr_ru32(r);
            int32_t d = (int32_t) vr_ru32(r);
            uint32_t e = vr_ru32(r);
            if (!gfx || rp_off) { skipped++; break; }
            vkCmdDrawIndexed(cb, a, b, c, d, e);
        } break;
        case VR_CMD_DRAW_INDIRECT:
        case VR_CMD_DRAW_INDEXED_INDIRECT: {
            uint64_t bid = vr_ru64(r);
            use_buffer(ci, bid);
            VkBuffer b = OBJ(VkBuffer, bid);
            uint64_t off = vr_ru64(r);
            uint32_t n = vr_ru32(r), stride = vr_ru32(r);
            if (!gfx || !b || rp_off) { skipped++; break; }
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
            tracef(ci, JE("  begin rendering 色 %u・depth %s・stencil %s", "  begin rendering color %u, depth %s, stencil %s"), nc, ri.pDepthAttachment ? JE("あり", "yes") : JE("なし", "no"), ri.pStencilAttachment ? JE("あり", "yes") : JE("なし", "no"));
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
            tracef(ci, JE("  barrier2 memory %u・buffer %u・image %u", "  barrier2 memory %u, buffer %u, image %u"), nm, nb, ni);
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
        case VR_CMD_BUILD_ACCEL: {
            uint32_t n = vr_ru32(r);
            if (n > 65536 || n > r->n - r->at) { r->bad = 1; break; }
            VkAccelerationStructureBuildGeometryInfoKHR *bi = calloc(n ? n : 1, sizeof *bi);
            VkAccelerationStructureBuildRangeInfoKHR **rg = calloc(n ? n : 1, sizeof *rg);
            if (!bi || !rg) { free(bi); free(rg); r->bad = 1; break; }
            int ok = 1; // 欠けた物があっても、並びがずれないように最後まで読む
            for (uint32_t i = 0; i < n && !r->bad; i++) {
                bi[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
                bi[i].type = (VkAccelerationStructureTypeKHR) vr_ru32(r);
                bi[i].flags = vr_ru32(r);
                bi[i].mode = (VkBuildAccelerationStructureModeKHR) vr_ru32(r);
                uint64_t sid = vr_ru64(r), did = vr_ru64(r);
                bi[i].srcAccelerationStructure = OBJ(VkAccelerationStructureKHR, sid);
                bi[i].dstAccelerationStructure = OBJ(VkAccelerationStructureKHR, did);
                bi[i].scratchData.deviceAddress = vr_ru64(r);
                uint32_t ng = 0;
                bi[i].pGeometries = rd_geoms(r, &ng);
                bi[i].geometryCount = ng;
                rg[i] = calloc(ng ? ng : 1, sizeof **rg);
                if (!rg[i]) { r->bad = 1; break; }
                for (uint32_t k = 0; k < ng && !r->bad; k++) {
                    rg[i][k].primitiveCount = vr_ru32(r);
                    rg[i][k].primitiveOffset = vr_ru32(r);
                    rg[i][k].firstVertex = vr_ru32(r);
                    rg[i][k].transformOffset = vr_ru32(r);
                }
                if (!bi[i].dstAccelerationStructure || (bi[i].mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR && !bi[i].srcAccelerationStructure)) ok = 0;
                use_accel(ci, sid);
                use_accel(ci, did);
            }
            tracef(ci, JE("  acceleration structure を作る %u 個", "  creating %u acceleration structures"), n);
            if (ok && !r->bad && n) vkCmdBuildAccelerationStructuresKHR(cb, n, bi, (const VkAccelerationStructureBuildRangeInfoKHR *const *) rg);
            else if (!r->bad) vlog(JE("acceleration structure の build：作る先（作り直すなら元も）が無いので飛ばした", "acceleration structure build: skipped because the destination (or the source for a rebuild) is missing"));
            for (uint32_t i = 0; i < n; i++) { free((void *) bi[i].pGeometries); free(rg[i]); }
            free(bi);
            free(rg);
        } break;
        case VR_CMD_COPY_ACCEL: {
            uint64_t sid = vr_ru64(r), did = vr_ru64(r);
            VkCopyAccelerationStructureInfoKHR cp = { .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
            cp.src = OBJ(VkAccelerationStructureKHR, sid);
            cp.dst = OBJ(VkAccelerationStructureKHR, did);
            cp.mode = (VkCopyAccelerationStructureModeKHR) vr_ru32(r);
            use_accel(ci, sid);
            use_accel(ci, did);
            if (!r->bad && cp.src && cp.dst) vkCmdCopyAccelerationStructureKHR(cb, &cp);
            else if (!r->bad) vlog(JE("acceleration structure の写し：元か先が無いので飛ばした", "acceleration structure copy: skipped because the source or destination is missing"));
        } break;
        case VR_CMD_WRITE_ACCEL_PROPS: {
            uint32_t n = vr_ru32(r);
            if (n > (r->n - r->at) / 8) { r->bad = 1; break; }
            VkAccelerationStructureKHR *as = calloc(n ? n : 1, sizeof *as);
            if (!as) { r->bad = 1; break; }
            int ok = 1;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t aid = vr_ru64(r);
                as[i] = OBJ(VkAccelerationStructureKHR, aid);
                if (!as[i]) ok = 0;
                use_accel(ci, aid);
            }
            VkQueryType qt = (VkQueryType) vr_ru32(r);
            VkQueryPool qp = OBJ(VkQueryPool, vr_ru64(r));
            uint32_t first = vr_ru32(r);
            if (ok && qp && !r->bad && n) vkCmdWriteAccelerationStructuresPropertiesKHR(cb, n, as, qt, qp, first);
            else if (!r->bad) vlog(JE("acceleration structure の大きさの query：acceleration structure か pool が無いので飛ばした", "acceleration structure size query: skipped because the acceleration structure or pool is missing"));
            free(as);
        } break;
        default:
            vlog(JE("知らない記録の命令 %u", "Unknown recorded command %u"), op);
            r->bad = 1;
        }
    }
    if (skipped) vlog(JE("描き方か計算の仕方がつながっていないので、draw・dispatch を %u 回飛ばした", "The pipeline is not bound, so %u draws/dispatches were skipped"), skipped);
#undef SET_GATE
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
#elif defined(__linux__)
// Android と Linux：stb_image_write の JPEG 書き手。BGRA/RGBA を RGB に並べ直してから渡す（stb は 3 バイトの RGB を受け取る）。
// 色は 4:2:0 に間引く（stb は質 90 以下で自動）。縮めるのは CPU 1 本
static void stb_to_vr(void *ctx, void *data, int size) { vr_put((vr_buf *) ctx, data, (size_t) size); }
static int jpeg_encode(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, int q, vr_buf *out) {
    uint8_t *rgb = malloc((size_t) w * 3 * h);
    if (!rgb) return 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t) y * stride;
        uint8_t *d = rgb + (size_t) y * w * 3;
        if (bgra) for (uint32_t x = 0; x < w; x++, s += 4, d += 3) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
        else for (uint32_t x = 0; x < w; x++, s += 4, d += 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
    }
    int ok = stbi_write_jpg_to_func(stb_to_vr, out, (int) w, (int) h, 3, rgb, q < 1 ? 1 : q > 100 ? 100 : q);
    free(rgb);
    return ok != 0;
}
#endif

// ---- 空の頂点バッファの代わり ----
// 空の descriptor を持たない GPU（MoltenVK）に、空の頂点バッファを渡すと落ちる。0 で埋めた 1 MiB のバッファに差し替える
// d が持つメモリの種類から、bits の中で GPU の手元の物を選ぶ（無ければ bits の最初）
static uint32_t dummy_mem_type(uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_pd, &mp);
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return t;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) if (bits & (1u << t)) return t;
    return 0;
}

// 空の画像の見え方の代わり：1 x 1 の画像を 0 で塗り、GENERAL に移して置いておく
static VkImageView dummy_iview(VkDevice d) {
    if (g_dummy_iview || !d) return g_dummy_iview;
    if (g_paused) return VK_NULL_HANDLE; // 後ろに回っている間は GPU に仕事を出せないので、今は作らない
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                             .extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (vkCreateImage(d, &ic, NULL, &g_dummy_img) != VK_SUCCESS) { g_dummy_img = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d, g_dummy_img, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = dummy_mem_type(req.memoryTypeBits) };
    if (vkAllocateMemory(d, &ai, NULL, &g_dummy_img_mem) != VK_SUCCESS || vkBindImageMemory(d, g_dummy_img, g_dummy_img_mem, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_dummy_img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView v;
    if (vkCreateImageView(d, &vc, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    // GENERAL に移して 0 で塗る。1 回だけなので、出して終わりまで待つ
    VkCommandPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool pool;
    if (vkCreateCommandPool(d, &pc, NULL, &pool) == VK_SUCCESS) {
        VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb;
        if (vkAllocateCommandBuffers(d, &ca, &cb) == VK_SUCCESS) {
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            vkBeginCommandBuffer(cb, &bi);
            VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = g_dummy_img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            VkClearColorValue zero = { 0 };
            vkCmdClearColorImage(cb, g_dummy_img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ib.subresourceRange);
            ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            vkEndCommandBuffer(cb);
            VkQueue q;
            vkGetDeviceQueue(d, 0, 0, &q);
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
            if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS) vkQueueWaitIdle(q);
        }
        vkDestroyCommandPool(d, pool, NULL);
    }
    g_dummy_iview = v;
    vlog(JE("空の画像の代わりを作った", "Created a stand-in for an empty image"));
    return v;
}

// ---- ffhang：空の画像の代わりを、同じ layout・binding に前に書かれた見え方と同じ種類（2D・配列・cube・3D）と形式で作る ----
// 前は 1 x 1 の 2D・RGBA8 の 1 種類だけで、shader が配列・cube・3D・整数の画像を読む所に入ると、GPU の不正になりうる
static int viewfix_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("VKR_HANG_VIEWFIX"); v = e && *e == '1'; } return v; }
typedef struct { uint64_t lay; uint32_t key, vt, fmt, used; } vlearn;
static vlearn g_vlearn[8192];
static vlearn *vlearn_at(uint64_t lay, uint32_t key, int create) {
    uint64_t h = (lay * 0x9E3779B97F4A7C15ull) ^ ((uint64_t) key * 0xC2B2AE3D27D4EB4Full);
    for (uint32_t i = 0; i < 64; i++) {
        vlearn *v = &g_vlearn[(h + i) & 8191];
        if (v->used && v->lay == lay && v->key == key) return v;
        if (!v->used) { if (!create) return NULL; v->used = 1; v->lay = lay; v->key = key; return v; }
    }
    return NULL;
}
typedef struct { VkDevice d; uint32_t vt, fmt, storage; VkImage img; VkDeviceMemory mem; VkImageView v; } tdummy;
static tdummy g_tdummy[64];
static int g_ntdummy;
static int fmt_is_plain_color(VkFormat f) {
    return f >= VK_FORMAT_R4G4_UNORM_PACK8 && f <= VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && !(f >= VK_FORMAT_R64_UINT && f <= VK_FORMAT_R64G64B64A64_SFLOAT);
}
static int fmt_cls(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT: case VK_FORMAT_A8B8G8R8_UINT_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_S8_UINT: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_SINT: case VK_FORMAT_A8B8G8R8_SINT_PACK32:
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT: return 2;
    default: return 0;
    }
}
static VkImageView dummy_iview_typed(VkDevice d, uint32_t vt, VkFormat want, int storage) {
    if (!d || g_paused) return VK_NULL_HANDLE;
    // 形式：ふつうの色の形式で、使い道（sampled・storage・塗る）を持てばそのまま。だめなら同じ数の種類（float・uint・sint）の RGBA
    VkFormat f = want;
    VkFormatProperties fp;
    memset(&fp, 0, sizeof fp);
    if (fmt_is_plain_color(f)) vkGetPhysicalDeviceFormatProperties(g_pd, f, &fp);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | (storage ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0);
    if (!fmt_is_plain_color(f) || (fp.optimalTilingFeatures & need) != need) {
        int c = fmt_is_plain_color(f) ? fmt_cls(f) : 0;
        f = c == 1 ? VK_FORMAT_R8G8B8A8_UINT : c == 2 ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UNORM;
    }
    for (int i = 0; i < g_ntdummy; i++)
        if (g_tdummy[i].d == d && g_tdummy[i].vt == vt && g_tdummy[i].fmt == (uint32_t) f && g_tdummy[i].storage == (uint32_t) storage) return g_tdummy[i].v;
    if (g_ntdummy >= 64) return VK_NULL_HANDLE;
    int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                             .imageType = (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY) ? VK_IMAGE_TYPE_1D : vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                             .format = f, .extent = { 1, 1, 1 }, .mipLevels = 1, .arrayLayers = cube ? 6 : 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    tdummy t = { d, vt, (uint32_t) f, (uint32_t) storage, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    if (vkCreateImage(d, &ic, NULL, &t.img) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d, t.img, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = dummy_mem_type(req.memoryTypeBits) };
    if (vkAllocateMemory(d, &ai, NULL, &t.mem) != VK_SUCCESS) { vkDestroyImage(d, t.img, NULL); return VK_NULL_HANDLE; }
    vkBindImageMemory(d, t.img, t.mem, 0);
    VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t.img, .viewType = (VkImageViewType) vt, .format = f,
                                 .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1 } };
    if (vkCreateImageView(d, &vc, NULL, &t.v) != VK_SUCCESS) { vkDestroyImage(d, t.img, NULL); vkFreeMemory(d, t.mem, NULL); return VK_NULL_HANDLE; }
    VkCommandPoolCreateInfo pc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool pool;
    if (vkCreateCommandPool(d, &pc, NULL, &pool) == VK_SUCCESS) {
        VkCommandBufferAllocateInfo ca = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb;
        if (vkAllocateCommandBuffers(d, &ca, &cb) == VK_SUCCESS) {
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            vkBeginCommandBuffer(cb, &bi);
            VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = t.img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS } };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            VkClearColorValue zero = { 0 };
            vkCmdClearColorImage(cb, t.img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &ib.subresourceRange);
            ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            vkEndCommandBuffer(cb);
            VkQueue q;
            vkGetDeviceQueue(d, 0, 0, &q);
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
            if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS) vkQueueWaitIdle(q);
        }
        vkDestroyCommandPool(d, pool, NULL);
    }
    g_tdummy[g_ntdummy++] = t;
    vlog(JE("ffhang：空の画像の代わりを作った（見え方の種類 %u・形式 %d・storage %d・頼まれた形式 %d）", "ffhang: created a stand-in for an empty image (view type %u, format %d, storage %d, requested format %d)"), vt, (int) f, storage, (int) want);
    return t.v;
}

// 空の texel buffer の見え方の代わり
static VkBufferView dummy_bview(VkDevice d) {
    if (g_dummy_bview || !d) return g_dummy_bview;
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536,
                              .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT };
    if (vkCreateBuffer(d, &bc, NULL, &g_dummy_tb) != VK_SUCCESS) { g_dummy_tb = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, g_dummy_tb, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = dummy_mem_type(req.memoryTypeBits) };
    if (vkAllocateMemory(d, &ai, NULL, &g_dummy_tb_mem) != VK_SUCCESS || vkBindBufferMemory(d, g_dummy_tb, g_dummy_tb_mem, 0) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkBufferViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = g_dummy_tb, .format = VK_FORMAT_R32_UINT, .range = VK_WHOLE_SIZE };
    if (vkCreateBufferView(d, &vc, NULL, &g_dummy_bview) != VK_SUCCESS) { g_dummy_bview = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    vlog(JE("空の texel buffer の代わりを作った", "Created a stand-in for an empty texel buffer"));
    return g_dummy_bview;
}

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
    vlog(JE("空の頂点バッファの代わりを作った", "Created a stand-in for an empty vertex buffer"));
    return b;
}

// ---- ffhang：空の「書ける」descriptor（storage buffer・storage texel buffer・storage image）の書き込み先を、読む側の 0 の代わりと分ける ----
// 前は、空の storage buffer も空の uniform・頂点も同じ 1 MiB の「0 の buffer」に差し替えていた。shader が空の UAV に書くと
// （D3D11 では捨てられる書き込み）その 0 の buffer が汚れ、あとで空の cbuffer や頂点を読む shader が 0 でない値を読む
static int sepdummy_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("VKR_HANG_SEPDUMMY"); v = e && *e == '1'; } return v; }
static VkBuffer g_sink_buf;
static VkDeviceMemory g_sink_mem;
static VkBufferView g_sink_bview;
static VkDevice g_sink_dev;
static VkBuffer dummy_sink(VkDevice d) {
    if (g_sink_buf && g_sink_dev == d) return g_sink_buf;
    if (!d) return VK_NULL_HANDLE;
    VkBufferCreateInfo bc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1u << 20,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer b;
    if (vkCreateBuffer(d, &bc, NULL, &b) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, b, &req);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = dummy_mem_type(req.memoryTypeBits) };
    VkDeviceMemory m;
    if (vkAllocateMemory(d, &ai, NULL, &m) != VK_SUCCESS) { vkDestroyBuffer(d, b, NULL); return VK_NULL_HANDLE; }
    vkBindBufferMemory(d, b, m, 0);
    g_sink_buf = b; g_sink_mem = m; g_sink_dev = d; g_sink_bview = VK_NULL_HANDLE;
    vlog(JE("ffhang：空の storage の書き込み先を別に作った（VKR_HANG_SEPDUMMY）", "ffhang: made a separate write target for an empty storage (VKR_HANG_SEPDUMMY)"));
    return b;
}
static VkBufferView dummy_sink_bview(VkDevice d) {
    if (g_sink_bview && g_sink_dev == d) return g_sink_bview;
    VkBuffer b = dummy_sink(d);
    if (!b) return VK_NULL_HANDLE;
    VkBufferViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b, .format = VK_FORMAT_R32_UINT, .range = 65536 };
    if (vkCreateBufferView(d, &vc, NULL, &g_sink_bview) != VK_SUCCESS) g_sink_bview = VK_NULL_HANDLE;
    return g_sink_bview;
}

// ---- ffhang：0 のはずの代わりの buffer（空の頂点・cbuffer・storage の代わり）が汚れていないかを時々見る ----
static void dummy_dirty_check(const char *why) {
    static int nlog;
    static size_t last_nz;
    { static unsigned k; if ((++k % 4) == 0) vlog(JE("ffhang：空の代わりの数 texel %d・画像 %d・buffer %d・型つき画像 %d・型が分からない画像 %d（%s）", "ffhang: stand-in counts texel %d, image %d, buffer %d, typed image %d, unknown-type image %d (%s)"), g_hang_subst[0], g_hang_subst[1], g_hang_subst[2], g_hang_subst[3], g_hang_subst[4], why); }
    if (!g_dummy_vb || !g_dummy_mem || !g_dev || nlog >= 40) return;
    void *p = NULL;
    if (vkMapMemory(g_dev, g_dummy_mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS || !p) return;
    const uint32_t *w = (const uint32_t *) p;
    size_t nz = 0, first = (size_t) -1;
    uint32_t fv = 0;
    for (size_t i = 0; i < (1u << 20) / 4; i++) if (w[i]) { if (first == (size_t) -1) { first = i * 4; fv = w[i]; } nz++; }
    vkUnmapMemory(g_dev, g_dummy_mem);
    if (nz != last_nz) {
        nlog++;
        vlog(JE("ffhang：0 の代わりの buffer（1 MiB）の 0 でない 4 バイト %zu 個（最初は %zu バイト目の 0x%08x）・%s", "ffhang: %zu non-zero 4-byte values in the zero stand-in buffer (1 MiB) (first at byte %zu, 0x%08x), %s"), nz, first == (size_t) -1 ? 0 : first, fv, why);
        last_nz = nz;
    }
}

// ---- 先送りの JPEG（VR_READ_JPEG_PIPE） ----
// 今のコマを写して別の糸で縮め始め、返事にはひとつ前のコマの JPEG を入れる。
// 縮めている間に、受け手は次のコマの仕事を進められる（窓に出るのは 1 コマ遅れる）
#ifdef VR_HAVE_JPEG
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
    qos_hi();
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

// ---- 窓に出すコマを H.264 に縮める（VR_READ_H264） ----
// 遅れを小さくする設定：B フレームなし（並べ替えない）・1 コマ入れたら 1 コマ出るまで待つ・レートは低遅延の物。
// キーフレームは最初と、ドライバが頼んだとき（ほどき直し）と、決めた間隔ごと。
// 機械ごとの中身は vid_probe・vid_open・vid_encode・vid_close の 4 つ。絵は BGRA のまま渡し、中で NV12 にする
#if defined(_WIN32)
// ---- PC の代役：Media Foundation の H.264 エンコーダ ----
// GPU の物（NVIDIA などの非同期の MFT）を先に試し、だめならソフトの物（Microsoft の同期の MFT）。VKR_VIDEO_HW=0 でソフトだけ
typedef struct {
    IMFActivate *act;
    IMFTransform *t;
    ICodecAPI *api;
    IMFMediaEventGenerator *ev; // 非同期の MFT だけ
    int need, have;             // 非同期：受け取れる入力の数・受け取れる出力の数
    int provides;               // 出力の sample を MFT が用意する
    DWORD outsz;
    uint32_t ew, eh, fps;
    int hw;
    char name[96];
} mf_enc;
static int g_vid_hw_bad; // GPU のエンコーダが途中で動かなくなった。この先はソフトの物を使う

static void mf_free(mf_enc *e) {
    if (!e) return;
    if (e->t) {
        IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    if (e->ev) IMFMediaEventGenerator_Release(e->ev);
    if (e->api) ICodecAPI_Release(e->api);
    if (e->t) IMFTransform_Release(e->t);
    if (e->act) { IMFActivate_ShutdownObject(e->act); IMFActivate_Release(e->act); }
    free(e);
}

// 非同期の MFT の知らせを読む。want が 0 なら入力を受け取れるまで、1 なら出力が出るまで。1 秒で諦める
static int mf_pump(mf_enc *e, int want) {
    double t0 = t_ms();
    while (want ? e->have <= 0 : e->need <= 0) {
        IMFMediaEvent *ev = NULL;
        HRESULT hr = IMFMediaEventGenerator_GetEvent(e->ev, MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            double dt = t_ms() - t0;
            if (dt > 1000) return 0;
            if (dt > 2) Sleep(1);
            else SwitchToThread();
            continue;
        }
        if (FAILED(hr) || !ev) return 0;
        MediaEventType ty = 0;
        IMFMediaEvent_GetType(ev, &ty);
        IMFMediaEvent_Release(ev);
        if (ty == METransformNeedInput) e->need++;
        else if (ty == METransformHaveOutput) e->have++;
    }
    return 1;
}

static int mf_setup(mf_enc *e, uint32_t kbps, uint32_t fps, uint32_t gop) {
    IMFAttributes *at = NULL;
    UINT32 async = 0;
    if (SUCCEEDED(IMFTransform_GetAttributes(e->t, &at)) && at) {
        IMFAttributes_GetUINT32(at, &VG_ASYNC, &async);
        if (async) IMFAttributes_SetUINT32(at, &VG_ASYNC_UNLOCK, TRUE);
        IMFAttributes_SetUINT32(at, &VG_LOW_LATENCY, TRUE);
        IMFAttributes_Release(at);
    }
    if (async && FAILED(IMFTransform_QueryInterface(e->t, &VG_IID_EVENTGEN, (void **) &e->ev))) return 0;
    // レートの決め方：既定は CBR（0）。VKR_VIDEO_RC で変える（4 は低遅延の VBR）
    const char *rc = getenv("VKR_VIDEO_RC");
    if (SUCCEEDED(IMFTransform_QueryInterface(e->t, &VG_IID_CODECAPI, (void **) &e->api)) && e->api) {
        vid_api_bool(e->api, &VG_LOW_LATENCY, 1);
        vid_api_u32(e->api, &VG_RC_MODE, rc && *rc ? (UINT32) atoi(rc) : 0);
        vid_api_u32(e->api, &VG_MEAN_BITRATE, kbps * 1000);
        vid_api_u32(e->api, &VG_GOP, gop);
        vid_api_u32(e->api, &VG_BFRAMES, 0);
        if (!e->hw) vid_api_u32(e->api, &VG_QUALITY_SPEED, 0); // ソフトの物は速さを取る
    }
    // 出す形を先に決める（エンコーダはこの順でないと断る）。High・Main・Baseline の順に試す
    static const uint32_t prof[] = { 100, 77, 66 };
    HRESULT hr = E_FAIL;
    for (int i = 0; i < 3 && FAILED(hr); i++) {
        IMFMediaType *o = vid_type(&VG_H264, e->ew, e->eh, fps, kbps * 1000, prof[i]);
        if (!o) return 0;
        hr = IMFTransform_SetOutputType(e->t, 0, o, 0);
        IMFMediaType_Release(o);
    }
    if (FAILED(hr)) return 0;
    IMFMediaType *in = vid_type(&VG_NV12, e->ew, e->eh, fps, 0, 0);
    if (!in) return 0;
    hr = IMFTransform_SetInputType(e->t, 0, in, 0);
    IMFMediaType_Release(in);
    if (FAILED(hr)) return 0;
    MFT_OUTPUT_STREAM_INFO si;
    memset(&si, 0, sizeof si);
    if (FAILED(IMFTransform_GetOutputStreamInfo(e->t, 0, &si))) return 0;
    e->provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    e->outsz = si.cbSize > e->ew * e->eh * 3 / 2 ? si.cbSize : e->ew * e->eh * 3 / 2 + 65536;
    IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(e->t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return 1;
}

static int vid_probe(void) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const vid_mf_api *mf = vid_mf();
    if (!mf) return 0;
    MFT_REGISTER_TYPE_INFO in = { VG_VIDEO, VG_NV12 }, out = { VG_VIDEO, VG_H264 };
    IMFActivate **act = NULL;
    UINT32 n = 0;
    if (FAILED(mf->TEnumEx(VG_CAT_ENCODER, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &act, &n))) n = 0;
    for (UINT32 i = 0; i < n; i++) IMFActivate_Release(act[i]);
    if (act) CoTaskMemFree(act);
    return n > 0;
}

static void *vid_open(uint32_t ew, uint32_t eh, uint32_t kbps, uint32_t fps, uint32_t gop) {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const vid_mf_api *mf = vid_mf();
    if (!mf) return NULL;
    MFT_REGISTER_TYPE_INFO in = { VG_VIDEO, VG_NV12 }, out = { VG_VIDEO, VG_H264 };
    const char *hwe = getenv("VKR_VIDEO_HW");
    mf_enc *got = NULL;
    for (int pass = (hwe && *hwe == '0') || g_vid_hw_bad ? 1 : 0; pass < 2 && !got; pass++) {
        IMFActivate **act = NULL;
        UINT32 n = 0;
        UINT32 fl = (pass == 0 ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER;
        if (FAILED(mf->TEnumEx(VG_CAT_ENCODER, fl, &in, &out, &act, &n))) n = 0;
        for (UINT32 i = 0; i < n; i++) {
            mf_enc *e = got ? NULL : calloc(1, sizeof *e);
            if (e) {
                e->ew = ew, e->eh = eh, e->fps = fps, e->hw = pass == 0;
                WCHAR wn[96] = { 0 };
                if (SUCCEEDED(IMFActivate_GetString(act[i], &VG_FRIENDLY_NAME, wn, 96, NULL)))
                    WideCharToMultiByte(CP_UTF8, 0, wn, -1, e->name, sizeof e->name - 1, NULL, NULL);
                if (SUCCEEDED(IMFActivate_ActivateObject(act[i], &VG_IID_TRANSFORM, (void **) &e->t))) {
                    e->act = act[i];
                    IMFActivate_AddRef(act[i]);
                }
                if (e->t && mf_setup(e, kbps, fps, gop)) got = e;
                else {
                    vlog(JE("H.264 のエンコーダ %s は使えない", "H.264 encoder %s cannot be used"), e->name[0] ? e->name : "?");
                    mf_free(e);
                }
            }
            IMFActivate_Release(act[i]);
        }
        if (act) CoTaskMemFree(act);
    }
    if (got)
        vlog(JE("H.264 のエンコーダ：%s（%s）・%u x %u・%u kbps・%u コマ/秒・キーフレームは %u コマごと", "H.264 encoder: %s (%s), %u x %u, %u kbps, %u fps, keyframe every %u frames"), got->name, got->hw ? "GPU" : JE("ソフト", "software"),
             ew, eh, kbps, fps, gop);
    return got;
}

static int mf_encode(mf_enc *e, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, uint64_t idx, int key, vr_buf *out, int *iskey) {
    const DWORD n = e->ew * e->eh * 3 / 2;
    IMFSample *s = vid_sample(n);
    if (!s) return 0;
    IMFMediaBuffer *b = NULL;
    BYTE *p = NULL;
    if (FAILED(IMFSample_GetBufferByIndex(s, 0, &b)) || FAILED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) {
        if (b) IMFMediaBuffer_Release(b);
        IMFSample_Release(s);
        return 0;
    }
    vid_rgb_to_nv12(px, w, h, stride, bgra, p, e->ew, p + (size_t) e->ew * e->eh, e->ew);
    IMFMediaBuffer_Unlock(b);
    IMFMediaBuffer_SetCurrentLength(b, n);
    IMFMediaBuffer_Release(b);
    IMFSample_SetSampleTime(s, (LONGLONG) (idx * 10000000ull / e->fps));
    IMFSample_SetSampleDuration(s, (LONGLONG) (10000000ull / e->fps));
    if (key && e->api) vid_api_u32(e->api, &VG_FORCE_KEY, 1);
    HRESULT hr = e->ev && !mf_pump(e, 0) ? E_FAIL : IMFTransform_ProcessInput(e->t, 0, s, 0);
    IMFSample_Release(s);
    if (FAILED(hr)) return 0;
    if (e->ev) e->need--;
    // 出てくるまで受け取る（B フレームなしで遅れの少ない設定なので、1 つ入れれば 1 つ出る）
    int got = 0;
    for (;;) {
        if (e->ev && !mf_pump(e, 1)) break;
        MFT_OUTPUT_DATA_BUFFER ob;
        memset(&ob, 0, sizeof ob);
        if (!e->provides && !(ob.pSample = vid_sample(e->outsz))) break;
        DWORD st = 0;
        hr = IMFTransform_ProcessOutput(e->t, 0, 1, &ob, &st);
        if (e->ev) e->have--;
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // 出す形が変わった：新しい形を受け入れて続ける
            IMFMediaType *t = NULL;
            if (ob.pSample) IMFSample_Release(ob.pSample);
            if (SUCCEEDED(IMFTransform_GetOutputAvailableType(e->t, 0, 0, &t)) && t) {
                IMFTransform_SetOutputType(e->t, 0, t, 0);
                IMFMediaType_Release(t);
            }
            continue;
        }
        if (FAILED(hr)) {
            if (ob.pSample) IMFSample_Release(ob.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
            return 0;
        }
        IMFMediaBuffer *mb = NULL;
        BYTE *q = NULL;
        DWORD len = 0;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(ob.pSample, &mb)) && SUCCEEDED(IMFMediaBuffer_Lock(mb, &q, NULL, &len))) {
            vr_put(out, q, len);
            IMFMediaBuffer_Unlock(mb);
        }
        if (mb) IMFMediaBuffer_Release(mb);
        UINT32 clean = 0;
        if (SUCCEEDED(IMFSample_GetUINT32(ob.pSample, &VG_CLEAN_POINT, &clean)) && clean) *iskey = 1;
        IMFSample_Release(ob.pSample);
        got = 1;
        if (e->ev) break; // 非同期：出力の知らせ 1 つに 1 つ
    }
    return got;
}

static int vid_encode(void *p, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, uint64_t idx, int key, vr_buf *out, int *iskey) {
    mf_enc *e = (mf_enc *) p;
    static _Thread_local int com; // 作った糸と違う糸（先送りの糸など）から使うこともある
    if (!com) { CoInitializeEx(NULL, COINIT_MULTITHREADED); com = 1; }
    int ok = mf_encode(e, px, w, h, stride, bgra, idx, key, out, iskey);
    // GPU の物が出さなくなったら、この先はソフトの物にする（作り直しは呼んだ側がする）
    if (!ok && e->hw && !g_vid_hw_bad) { g_vid_hw_bad = 1; vlog(JE("GPU の H.264 エンコーダ %s が出さない。この先はソフトの物を使う", "The GPU H.264 encoder %s produced no output. Using the software one from now on"), e->name); }
    return ok;
}

static void vid_close(void *p) { mf_free((mf_enc *) p); }

#elif defined(__APPLE__)
// ---- iPhone：VideoToolbox の H.264 エンコーダ（専用回路） ----
// iPhone では未確認。Windows では組めないので、Apple の資料どおりに書いた。
// VTCompressionSession を低遅延のレート（EnableLowLatencyRateControl・iOS 14.5 から）・RealTime・並べ替えなしで作り、
// 1 コマ渡すたびに VTCompressionSessionCompleteFrames で出てくるまで待つ。出てきた AVCC（長さ＋NAL）は Annex B に直す
typedef struct {
    VTCompressionSessionRef s;
    uint32_t ew, eh, fps;
    vr_buf *out; // 縮めている間だけ、出てきた NAL をここに足す
    int key, got, force_key;
    OSStatus err;
} vt_enc;

static void vt_done(void *ref, void *frame, OSStatus st, VTEncodeInfoFlags info, CMSampleBufferRef sb) {
    (void) frame;
    vt_enc *e = (vt_enc *) ref;
    if (!e->out) return; // 閉じるときの残り
    if (st != noErr) { e->err = st; return; }
    if (!sb || (info & kVTEncodeInfo_FrameDropped)) return; // 落とされた。次をキーフレームにする
    static const uint8_t sc4[4] = { 0, 0, 0, 1 };
    int key = 1;
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (att && CFArrayGetCount(att) > 0) {
        CFDictionaryRef d = (CFDictionaryRef) CFArrayGetValueAtIndex(att, 0);
        const void *v = NULL;
        if (CFDictionaryGetValueIfPresent(d, kCMSampleAttachmentKey_NotSync, &v) && v && CFBooleanGetValue((CFBooleanRef) v)) key = 0;
    }
    CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
    size_t nps = 0;
    int nlen = 4;
    if (!fd || CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, NULL, NULL, &nps, &nlen) != noErr) { e->err = -1; return; }
    if (key)
        for (size_t i = 0; i < nps; i++) { // SPS と PPS
            const uint8_t *p = NULL;
            size_t sz = 0;
            if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, i, &p, &sz, NULL, NULL) == noErr && p) {
                vr_put(e->out, sc4, 4);
                vr_put(e->out, p, sz);
            }
        }
    CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
    size_t total = bb ? CMBlockBufferGetDataLength(bb) : 0;
    uint8_t *tmp = total ? malloc(total) : NULL;
    if (!tmp || CMBlockBufferCopyDataBytes(bb, 0, total, tmp) != kCMBlockBufferNoErr || nlen < 1 || nlen > 4) { free(tmp); e->err = -1; return; }
    for (size_t at = 0; at + (size_t) nlen <= total;) {
        uint32_t l = 0;
        for (int k = 0; k < nlen; k++) l = (l << 8) | tmp[at + (size_t) k];
        at += (size_t) nlen;
        if (l > total - at) break;
        vr_put(e->out, sc4, 4);
        vr_put(e->out, tmp + at, l);
        at += l;
    }
    free(tmp);
    e->key = key;
    e->got = 1;
}

static void vt_num(VTCompressionSessionRef s, CFStringRef k, int32_t v) {
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    if (n) { VTSessionSetProperty(s, k, n); CFRelease(n); }
}

static void *vid_open(uint32_t ew, uint32_t eh, uint32_t kbps, uint32_t fps, uint32_t gop) {
    vt_enc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->ew = ew, e->eh = eh, e->fps = fps;
    const void *sk[] = { kVTVideoEncoderSpecification_EnableLowLatencyRateControl };
    const void *sv[] = { kCFBooleanTrue };
    CFDictionaryRef spec = CFDictionaryCreate(NULL, sk, sv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    // 入れる絵：BGRA のまま。NV12 への色の直しは VideoToolbox（専用回路）に任せる。前は CPU で 1 画素ずつ直していて、
    // 1280x720 で縮める 1 コマ 10 ms のうちかなりを占め、100 コマ/秒前後で頭打ちだった。IOSurface つきにすると写さずに渡せる
    int32_t pf = (int32_t) kCVPixelFormatType_32BGRA, iw = (int32_t) ew, ih = (int32_t) eh;
    CFNumberRef npf = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf), nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &iw),
                nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &ih);
    CFDictionaryRef ios = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const void *ak[] = { kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferIOSurfacePropertiesKey };
    const void *av[] = { npf, nw, nh, ios };
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, ak, av, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus st = VTCompressionSessionCreate(NULL, iw, ih, kCMVideoCodecType_H264, spec, attrs, NULL, vt_done, e, &e->s);
    CFRelease(attrs); CFRelease(ios); CFRelease(nh); CFRelease(nw); CFRelease(npf); CFRelease(spec);
    if (st != noErr || !e->s) {
        vlog(JE("H.264 のエンコーダを作れない（%d）", "Cannot create the H.264 encoder (%d)"), (int) st);
        free(e);
        return NULL;
    }
    VTSessionSetProperty(e->s, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(e->s, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse); // B フレームなし
    VTSessionSetProperty(e->s, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue); // 質より速さ（iOS 14.5 から）
    // 低遅延の作り方は Constrained High（B フレームの無い High）を使う。断られたら High
    if (VTSessionSetProperty(e->s, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedHigh_AutoLevel) != noErr)
        VTSessionSetProperty(e->s, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);
    vt_num(e->s, kVTCompressionPropertyKey_AverageBitRate, (int32_t) (kbps > 2000000 ? 2000000000 : kbps * 1000));
    vt_num(e->s, kVTCompressionPropertyKey_ExpectedFrameRate, (int32_t) fps);
    vt_num(e->s, kVTCompressionPropertyKey_MaxKeyFrameInterval, (int32_t) gop);
    VTCompressionSessionPrepareToEncodeFrames(e->s);
    vlog(JE("H.264 のエンコーダ：VideoToolbox・%u x %u・%u kbps・%u コマ/秒・キーフレームは %u コマごと", "H.264 encoder: VideoToolbox, %u x %u, %u kbps, %u fps, keyframe every %u frames"), ew, eh, kbps, fps, gop);
    return e;
}

static void vid_close(void *p) {
    vt_enc *e = (vt_enc *) p;
    if (!e) return;
    e->out = NULL;
    if (e->s) {
        VTCompressionSessionCompleteFrames(e->s, kCMTimeInvalid);
        VTCompressionSessionInvalidate(e->s);
        CFRelease(e->s);
    }
    free(e);
}

static int vid_probe(void) {
    void *e = vid_open(64, 64, 500, 30, 30);
    if (!e) return 0;
    vid_close(e);
    return 1;
}

static int vid_encode(void *p, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, uint64_t idx, int key, vr_buf *out, int *iskey) {
    vt_enc *e = (vt_enc *) p;
    CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(e->s);
    CVPixelBufferRef pb = NULL;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess || !pb) return 0;
    CVPixelBufferLockBaseAddress(pb, 0);
    {
        // BGRA の行を写す（RGBA なら R と B を入れ替える）。色の直しは専用回路がする
        uint8_t *dst = (uint8_t *) CVPixelBufferGetBaseAddress(pb);
        size_t dstr = CVPixelBufferGetBytesPerRow(pb);
        uint32_t cw = w < e->ew ? w : e->ew, ch = h < e->eh ? h : e->eh;
        for (uint32_t y = 0; dst && y < ch; y++) {
            const uint8_t *sr = px + (size_t) y * stride;
            uint8_t *dr = dst + (size_t) y * dstr;
            if (bgra) memcpy(dr, sr, (size_t) cw * 4);
            else
                for (uint32_t x = 0; x < cw; x++) { dr[4 * x] = sr[4 * x + 2]; dr[4 * x + 1] = sr[4 * x + 1]; dr[4 * x + 2] = sr[4 * x]; dr[4 * x + 3] = 255; }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    CFDictionaryRef opt = NULL;
    if (key || e->force_key) {
        const void *k[] = { kVTEncodeFrameOptionKey_ForceKeyFrame };
        const void *v[] = { kCFBooleanTrue };
        opt = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    }
    e->out = out, e->got = 0, e->key = 0, e->err = noErr;
    CMTime pts = CMTimeMake((int64_t) idx, (int32_t) e->fps);
    VTEncodeInfoFlags fl = 0;
    OSStatus st = VTCompressionSessionEncodeFrame(e->s, pb, pts, CMTimeMake(1, (int32_t) e->fps), opt, NULL, &fl);
    // 出てくるまで待つ（後ろにためない）
    if (st == noErr) st = VTCompressionSessionCompleteFrames(e->s, pts);
    e->out = NULL;
    if (opt) CFRelease(opt);
    CVPixelBufferRelease(pb);
    if (st != noErr || e->err != noErr) {
        vlog(JE("H.264 に縮められない（%d・%d）", "Cannot compress to H.264 (%d, %d)"), (int) st, (int) e->err);
        return 0;
    }
    // 落とされたら空のまま返し、次をキーフレームにする（ドライバは空のコマを飛ばす）
    e->force_key = !e->got;
    *iskey = e->key;
    return 1;
}

#elif defined(__ANDROID__)
// ---- Android：MediaCodec（NDK の AMediaCodec）の H.264 エンコーダ ----
// 機械に GPU・SoC の専用回路があればそれを、なければ Google のソフトの物（c2.android.avc.encoder）を、システムが選ぶ。
// 入れる絵は NV12（COLOR_FormatYUV420SemiPlanar）。BGRA→NV12 は CPU（video.h）。実機の stride・slice-height は入力の形から読む。
// 出てきた NAL は Annex B（00 00 00 01）にそろえ、キーフレームには SPS と PPS を付ける（iPhone の vt_done と同じ形）。
// 1 コマ入れたら 1 コマ出るまで待つ。VKR_VIDEO_CODEC=<名前> で使うエンコーダを決められる（調べ用。adb shell dumpsys media.codec で名前が出る）
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
typedef struct {
    AMediaCodec *c;
    uint32_t ew, eh, fps;
    uint32_t stride, slice; // 入力バッファの NV12 の行の長さと、Y の面の高さ（機械ごとに 16 などにそろえてある）
    uint8_t *cfg;
    size_t cfgn;            // SPS と PPS（エンコーダが出した並びのまま）
    int force_key, cand;
    char name[96];
} mc_enc;

static unsigned g_mc_bad; // 動かなかった候補（vid_open の cand の番号の印）。この先は飛ばす（GPU の物がだめならソフトの物）

// d の NAL の種類を調べる（00 00 01 の次の 1 バイトの下 5 ビット）。7=SPS・8=PPS・5=IDR
static void mc_scan(const uint8_t *d, size_t n, int *has7, int *has5) {
    *has7 = *has5 = 0;
    for (size_t i = 0; i + 3 < n; i++)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int t = d[i + 3] & 31;
            if (t == 7) *has7 = 1;
            if (t == 5) *has5 = 1;
            i += 2;
        }
}

// Annex B（3 バイトか 4 バイトの区切り）を、4 バイトの区切りにそろえて足す。区切りが見つからなければ、全体を 1 つの NAL として足す
static void mc_put_annexb(vr_buf *o, const uint8_t *d, size_t n) {
    static const uint8_t sc4[4] = { 0, 0, 0, 1 };
    size_t start = (size_t) -1;
    for (size_t i = 0; i + 2 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != (size_t) -1) {
                size_t e = i;
                while (e > start && d[e - 1] == 0) e--; // 次の区切りの手前の 0（4 バイト区切りの頭）は NAL ではない
                vr_put(o, sc4, 4);
                vr_put(o, d + start, e - start);
            }
            start = i + 3;
            i += 2;
        }
    }
    if (start == (size_t) -1) {
        vr_put(o, sc4, 4);
        vr_put(o, d, n);
    } else if (start < n) {
        vr_put(o, sc4, 4);
        vr_put(o, d + start, n - start);
    }
}

static void mc_set_cfg(mc_enc *e, const uint8_t *d, size_t n) {
    if (n == 0 || n > (1u << 16)) return;
    uint8_t *q = realloc(e->cfg, n);
    if (!q) return;
    memcpy(q, d, n);
    e->cfg = q, e->cfgn = n;
}

// 出力の形（INFO_OUTPUT_FORMAT_CHANGED）の csd-0・csd-1（SPS・PPS）を控える
static void mc_cfg_from_format(mc_enc *e) {
    AMediaFormat *f = AMediaCodec_getOutputFormat(e->c);
    if (!f) return;
    void *b0 = NULL, *b1 = NULL;
    size_t n0 = 0, n1 = 0;
    if (!e->cfgn && AMediaFormat_getBuffer(f, "csd-0", &b0, &n0) && b0) {
        uint8_t *t = malloc(n0 + (AMediaFormat_getBuffer(f, "csd-1", &b1, &n1) && b1 ? n1 : 0));
        if (t) {
            memcpy(t, b0, n0);
            if (b1 && n1) memcpy(t + n0, b1, n1);
            mc_set_cfg(e, t, n0 + (b1 ? n1 : 0));
            free(t);
        }
    }
    AMediaFormat_delete(f);
}

static void mc_free(mc_enc *e) {
    if (!e) return;
    if (e->c) {
        AMediaCodec_stop(e->c);
        AMediaCodec_delete(e->c);
    }
    free(e->cfg);
    free(e);
}

static int mc_try(mc_enc *e, const char *name, int pass, uint32_t kbps, uint32_t fps, uint32_t gop) {
    e->c = name ? AMediaCodec_createCodecByName(name) : AMediaCodec_createEncoderByType("video/avc");
    if (!e->c) return 0;
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, (int32_t) e->ew);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, (int32_t) e->eh);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); // COLOR_FormatYUV420SemiPlanar（NV12）
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, (int32_t) (kbps > 2000000 ? 2000000000u : kbps * 1000));
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_FRAME_RATE, (int32_t) fps);
    AMediaFormat_setFloat(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, (float) gop / (float) fps);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BITRATE_MODE, 2); // CBR
    // 遅れを小さくする。知らない鍵は無視される
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PRIORITY, 0);      // 実時間
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_LATENCY, 1);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_LOW_LATENCY, 1);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_OPERATING_RATE, (int32_t) fps);
    AMediaFormat_setInt32(f, "max-bframes", 0);                  // B フレームなし（API 34 の KEY_MAX_B_FRAMES。古い機械では無視）
    if (pass == 0) AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PROFILE, 8); // AVCProfileHigh。だめなら 2 回目で外す
    media_status_t st = AMediaCodec_configure(e->c, f, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(f);
    if (st == AMEDIA_OK) st = AMediaCodec_start(e->c);
    if (st != AMEDIA_OK) {
        AMediaCodec_delete(e->c);
        e->c = NULL;
        return 0;
    }
    char *nm = NULL;
    if (AMediaCodec_getName(e->c, &nm) == AMEDIA_OK && nm) {
        snprintf(e->name, sizeof e->name, "%s", nm);
        AMediaCodec_releaseName(e->c, nm);
    }
    e->stride = e->ew, e->slice = e->eh;
    AMediaFormat *inf = AMediaCodec_getInputFormat(e->c);
    if (inf) {
        int32_t v = 0;
        if (AMediaFormat_getInt32(inf, AMEDIAFORMAT_KEY_STRIDE, &v) && v >= (int32_t) e->ew && v < 16384) e->stride = (uint32_t) v;
        if (AMediaFormat_getInt32(inf, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &v) && v >= (int32_t) e->eh && v < 16384) e->slice = (uint32_t) v;
        AMediaFormat_delete(inf);
    }
    return 1;
}

static void *vid_open(uint32_t ew, uint32_t eh, uint32_t kbps, uint32_t fps, uint32_t gop) {
    mc_enc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->ew = ew, e->eh = eh, e->fps = fps;
    const char *want = getenv("VKR_VIDEO_CODEC");
    // 候補：調べ用の名前・システムの選んだ物（GPU や SoC の専用回路が先）・Google のソフトの物
    const char *cand[] = { want && *want ? want : NULL, NULL, "c2.android.avc.encoder", "OMX.google.h264.encoder" };
    int ok = 0;
    for (int i = (want && *want) ? 0 : 1; i < 4 && !ok; i++) {
        if (g_mc_bad & (1u << i)) continue; // 前に動かなかった候補は飛ばす
        for (int pass = 0; pass < 2 && !ok; pass++) ok = mc_try(e, cand[i], pass, kbps, fps, gop);
        if (ok) e->cand = i;
    }
    if (!ok) {
        vlog(JE("H.264 のエンコーダを作れない（MediaCodec）", "Cannot create the H.264 encoder (MediaCodec)"));
        free(e);
        return NULL;
    }
    vlog(JE("H.264 のエンコーダ：MediaCodec %s・%u x %u（入力の行 %u・面の高さ %u）・%u kbps・%u コマ/秒・キーフレームは %u コマごと", "H.264 encoder: MediaCodec %s, %u x %u (input stride %u, slice height %u), %u kbps, %u fps, keyframe every %u frames"), e->name, ew, eh, e->stride, e->slice,
         kbps, fps, gop);
    return e;
}

static void vid_close(void *p) { mc_free((mc_enc *) p); }

static int vid_encode(void *p, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, uint64_t idx, int key, vr_buf *out, int *iskey) {
    mc_enc *e = (mc_enc *) p;
    ssize_t ii = AMediaCodec_dequeueInputBuffer(e->c, 500000);
    if (ii < 0) { vlog(JE("H.264 の入力の席が空かない（%d）", "No free H.264 input slot (%d)"), (int) ii); g_mc_bad |= 1u << e->cand; return 0; }
    size_t cap = 0;
    uint8_t *ib = AMediaCodec_getInputBuffer(e->c, (size_t) ii, &cap);
    const size_t ysz = (size_t) e->stride * e->slice, total = ysz + (size_t) e->stride * e->eh / 2;
    if (!ib || cap < total) {
        vlog(JE("H.264 の入力バッファが小さい（%zu < %zu）", "The H.264 input buffer is too small (%zu < %zu)"), cap, total);
        AMediaCodec_queueInputBuffer(e->c, (size_t) ii, 0, 0, 0, 0);
        return 0;
    }
    vid_rgb_to_nv12(px, w, h, stride, bgra, ib, e->stride, ib + ysz, e->stride);
    if (key || e->force_key) {
        AMediaFormat *pf = AMediaFormat_new();
        AMediaFormat_setInt32(pf, "request-sync", 0); // PARAMETER_KEY_REQUEST_SYNC_FRAME
        AMediaCodec_setParameters(e->c, pf);
        AMediaFormat_delete(pf);
    }
    const int64_t pts = (int64_t) (idx * 1000000ull / e->fps);
    if (AMediaCodec_queueInputBuffer(e->c, (size_t) ii, 0, total, (uint64_t) pts, 0) != AMEDIA_OK) { g_mc_bad |= 1u << e->cand; return 0; }
    // 出てくるまで待つ（後ろにためない）。最初のコマはエンコーダの立ち上がりで遅いので長く待つ
    const double t0 = t_ms(), limit = idx == 0 ? 3000 : 500;
    int got = 0, iskey_now = 0;
    while (!got && t_ms() - t0 < limit) {
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(e->c, &info, 20000);
        if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) { mc_cfg_from_format(e); continue; }
        if (oi < 0) continue; // TRY_AGAIN・BUFFERS_CHANGED
        size_t osz = 0;
        uint8_t *ob = AMediaCodec_getOutputBuffer(e->c, (size_t) oi, &osz);
        if (ob && info.size > 0 && (size_t) info.offset + (size_t) info.size <= osz) {
            const uint8_t *d = ob + info.offset;
            const size_t n = (size_t) info.size;
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                mc_set_cfg(e, d, n);
            } else if (info.presentationTimeUs < pts) {
                e->force_key = 1; // 遅れて出てきた前のコマ。ドライバには渡していないので、つながりが切れた。次をキーフレームにする
            } else {
                int has7, has5;
                mc_scan(d, n, &has7, &has5);
                iskey_now = has5 || (info.flags & 1);
                if (iskey_now && !has7 && e->cfgn) mc_put_annexb(out, e->cfg, e->cfgn);
                static int klog; // 最初の 2 枚のキーフレームだけ、SPS・PPS をどこから付けたかを記録に出す
                if (iskey_now && klog < 2) {
                    klog++;
                    vlog(JE("H.264 のキーフレーム：SPS %s・%zu バイト", "H.264 keyframe: SPS %s, %zu bytes"), has7 ? JE("はエンコーダ自身が付けた", "added by the encoder itself") : e->cfgn ? JE("を付け足した", "added by the receiver") : JE("が無い", "missing"), n);
                }
                mc_put_annexb(out, d, n);
                got = 1;
            }
        }
        AMediaCodec_releaseOutputBuffer(e->c, (size_t) oi, false);
    }
    // 出てこなかったときは落とされたことにして、空のまま返し、次をキーフレームにする（ドライバは空のコマを飛ばす）。
    // 最初のコマが出ないなら、このエンコーダは動かない。次の候補にする
    if (!got && idx == 0) { vlog(JE("H.264 の最初のコマが %.0f ms たっても出ない（%s）", "No first H.264 frame after %.0f ms (%s)"), t_ms() - t0, e->name); g_mc_bad |= 1u << e->cand; return 0; }
    e->force_key = !got;
    *iskey = iskey_now;
    return 1;
}

static int vid_probe_once(void) {
    void *e = vid_open(320, 240, 500, 30, 30);
    if (!e) return -1;
    // 開けても動かないエンコーダがある。灰色の 1 コマを実際に縮めてみる
    uint32_t n = 320 * 240;
    uint8_t *px = malloc((size_t) n * 4);
    vr_buf o = { 0 };
    int key = 0, ok = 0;
    if (px) {
        memset(px, 128, (size_t) n * 4);
        ok = vid_encode(e, px, 320, 240, 320 * 4, 1, 0, 1, &o, &key) && o.n > 0;
    }
    vid_close(e);
    free(px);
    free(o.p);
    return ok;
}

static int vid_probe(void) {
    if (getenv("VKR_VIDEO_OFF")) return 0;
    // だめだった候補は g_mc_bad に印がつく。次の候補（ソフトの物）で 3 回まで試す
    for (int i = 0; i < 3; i++) {
        int r = vid_probe_once();
        if (r == 1) return 1;
        if (r < 0) break;
    }
    vlog(JE("H.264：試しの 1 コマが縮められなかった。H.264 は名乗らない", "H.264: the test frame could not be compressed. Not advertising H.264"));
    return 0;
}

#else
// ほかの機械（Linux での文法の確かめなど）では縮められない。名乗らない
static int vid_probe(void) { return 0; }
static void *vid_open(uint32_t ew, uint32_t eh, uint32_t kbps, uint32_t fps, uint32_t gop) {
    (void) ew; (void) eh; (void) kbps; (void) fps; (void) gop;
    return NULL;
}
static int vid_encode(void *p, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, uint64_t idx, int key, vr_buf *out, int *iskey) {
    (void) p; (void) px; (void) w; (void) h; (void) stride; (void) bgra; (void) idx; (void) key; (void) out; (void) iskey;
    return 0;
}
static void vid_close(void *p) { (void) p; }
#endif

// 流れ（ドライバの swapchain）ごとのエンコーダ。たいていは 1 つ。3 秒使われなかった物は閉じる（GPU のエンコーダは数に限りがある）
#define VENC_MAX 4
static struct venc {
    uint64_t stream, n; // n：この流れで縮めたコマの数
    uint32_t w, h, kbps, fps, gop;
    double used;
    void *e;
} g_venc[VENC_MAX];

static void venc_drop(struct venc *v) {
    if (v->e) vid_close(v->e);
    memset(v, 0, sizeof *v);
}

static struct venc *venc_get(uint64_t stream, uint32_t w, uint32_t h, uint32_t kbps, uint32_t fps, uint32_t gop) {
    double now = t_ms();
    struct venc *hit = NULL, *spare = NULL;
    for (int i = 0; i < VENC_MAX; i++) {
        struct venc *v = &g_venc[i];
        if (v->e && v->stream == stream) hit = v;
        else if (v->e && now - v->used > 3000) venc_drop(v);
    }
    // 同じ流れで大きさや設定が変わったら作り直す（最初のコマはキーフレームになる）
    if (hit && (hit->w != w || hit->h != h || hit->kbps != kbps || hit->fps != fps || hit->gop != gop)) { venc_drop(hit); spare = hit; hit = NULL; }
    if (!hit) {
        for (int i = 0; i < VENC_MAX && !spare; i++) if (!g_venc[i].e) spare = &g_venc[i];
        if (!spare) {
            spare = &g_venc[0];
            for (int i = 1; i < VENC_MAX; i++) if (g_venc[i].used < spare->used) spare = &g_venc[i];
            venc_drop(spare);
        }
        void *e = vid_open((w + 1) & ~1u, (h + 1) & ~1u, kbps, fps, gop);
        if (!e) return NULL;
        vlog(JE("H.264 のエンコーダを作るのに %.0f ms（その間、窓の絵は止まる）", "Creating the H.264 encoder took %.0f ms (the window picture is paused meanwhile)"), t_ms() - now);
        spare->stream = stream, spare->n = 0, spare->w = w, spare->h = h, spare->kbps = kbps, spare->fps = fps, spare->gop = gop, spare->e = e;
        hit = spare;
    }
    hit->used = now;
    return hit;
}

// 1 コマを縮めて、頭（VR_H264_HEAD）と NAL の並びを out に足す。縮められなければ 0
// VR_H264_UP15 なら、縮める前に登録された関数（iPhone は MetalFX の空間の拡大）で 1.5 倍に広げる。広げられなければそのまま縮める
static int h264_frame(const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride, int bgra, uint32_t flags, uint32_t kbps, uint32_t fps, uint32_t gop,
                      uint64_t stream, uint64_t tag, vr_buf *out) {
    // 調べ用：VKR_VIDEO_FAIL=n なら、n コマ目から縮められないふりをする（ドライバが JPEG に戻るかを試す）
    static long fail_at = -1;
    static uint64_t seen;
    if (fail_at < 0) { const char *e = getenv("VKR_VIDEO_FAIL"); fail_at = e ? atol(e) : 0; }
    if (fail_at > 0 && ++seen >= (uint64_t) fail_at) return 0;
    if ((flags & VR_H264_UP15) && g_upscale) {
        static uint8_t *ub;
        static size_t ucap;
        static double ums;
        static uint64_t un;
        uint32_t dw = (w * 3 / 2 + 1) & ~1u, dh = (h * 3 / 2 + 1) & ~1u;
        size_t need = (size_t) dw * dh * 4;
        if (need > ucap) { free(ub); ub = malloc(need); ucap = ub ? need : 0; }
        double tu = t_ms();
        if (ub && g_upscale(px, w, h, stride, ub, dw, dh, dw * 4)) {
            px = ub, w = dw, h = dh, stride = dw * 4;
            ums += t_ms() - tu;
            if (++un % 100 == 0) { vlog(JE("MetalFX で広げた：%u x %u・1 コマ %.1f ms（100 コマの平均）", "Upscaled with MetalFX: %u x %u, %.1f ms per frame (average of 100 frames)"), dw, dh, ums / 100); ums = 0; }
        }
    }
    const size_t at = out->n;
    uint8_t head[VR_H264_HEAD];
    memset(head, 0, sizeof head);
    int key = 0;
    for (int tries = 0; tries < 2; tries++) {
        // 1 回目が失敗したら、エンコーダを作り直してもう 1 回（iPhone で後ろに回ったあとは、作り直さないと使えない）
        struct venc *v = venc_get(stream, w, h, kbps, fps, gop);
        if (!v) break;
        out->n = at;
        vr_put(out, head, sizeof head);
        key = 0;
        if (vid_encode(v->e, px, w, h, stride, bgra, v->n, (flags & VR_H264_KEY) || v->n == 0, out, &key)) {
            v->n++;
            uint32_t f = key ? 1u : 0u;
            uint8_t *hp = out->p + at;
            memcpy(hp, &stream, 8);
            memcpy(hp + 8, &tag, 8);
            memcpy(hp + 16, &w, 4);
            memcpy(hp + 20, &h, 4);
            memcpy(hp + 24, &f, 4);
            return 1;
        }
        venc_drop(v);
    }
    out->n = at;
    return 0;
}

// 先送り：今のコマを写して別の糸で縮め始め、返事にはひとつ前のコマを入れる（JPEG の先送りと同じ形）
static struct {
    uint8_t *px;
    size_t cap;
    uint32_t w, h, stride, bgra, flags, kbps, fps, gop;
    uint64_t stream, tag;
    int has, ok, busy; // has：縮め始めたコマがある
    double ms;
    vr_buf out;
#ifdef _WIN32
    HANDLE thr;
#else
    pthread_t thr;
#endif
} g_vp;

static void vp_encode(void) {
    double t0 = t_ms();
    g_vp.out.n = 0;
    g_vp.ok = h264_frame(g_vp.px, g_vp.w, g_vp.h, g_vp.stride, (int) g_vp.bgra, g_vp.flags, g_vp.kbps, g_vp.fps, g_vp.gop, g_vp.stream, g_vp.tag, &g_vp.out);
    g_vp.ms = t_ms() - t0;
}

#ifdef _WIN32
static DWORD WINAPI vp_thread(LPVOID a) {
    (void) a;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    vp_encode();
    return 0;
}
#else
static void *vp_thread(void *a) {
    (void) a;
    qos_hi();
    vp_encode();
    return NULL;
}
#endif

static void vp_join(void) {
    if (!g_vp.busy) return;
#ifdef _WIN32
    WaitForSingleObject(g_vp.thr, INFINITE);
    CloseHandle(g_vp.thr);
#else
    pthread_join(g_vp.thr, NULL);
#endif
    g_vp.busy = 0;
}

static int vp_start(const uint8_t *src) {
    size_t need = (size_t) g_vp.stride * g_vp.h;
    if (g_vp.cap < need) {
        free(g_vp.px);
        g_vp.px = malloc(need);
        g_vp.cap = g_vp.px ? need : 0;
        if (!g_vp.px) return 0;
    }
    memcpy(g_vp.px, src, need); // GPU のメモリから、まとめて写す
    g_vp.ok = 0;
    g_vp.has = 1;
#ifdef _WIN32
    g_vp.thr = CreateThread(NULL, 0, vp_thread, NULL, 0, NULL);
    g_vp.busy = g_vp.thr != NULL;
#else
    g_vp.busy = pthread_create(&g_vp.thr, NULL, vp_thread, NULL) == 0;
#endif
    if (!g_vp.busy) vp_encode(); // 糸を作れなければ、その場で縮める
    return 1;
}

// ---- 遅らせて写す（VR_H264_LATE・VR_CAP_H264_LATE） ----
// 先送り（VR_H264_PIPE）は、頼まれたその場で「そのコマを描く GPU の仕事」が終わるまで主の糸を止めていた。
// その間、受け手は次のコマの命令（書き込み・記録・submit）を処理できず、GPU はコマの間で空く。
// 遅らせる形では、頼みを覚えるだけで GPU を待たずに次の命令へ進み、写して縮め始めるのは「次の頼み」が来たとき
// （か、そのメモリを書き換える・放す命令の前）にする。そのときには次のコマの submit がもう GPU に入っているので、待っても GPU は空かない。
// 返事には、縮め終わった物のいちばん古い物を入れる（無ければ空）。コマの遅れは先送りより 1 コマ増える（頼んだ 2 つ後の返事で届く）。
// 縮める順・中身は先送りと同じ（同じコマを同じ順に縮める）。写すのは、そのコマの GPU の仕事が終わったあと・メモリが書き換わる前
static struct {
    int on;
    uint64_t mid, off, stream, tag;
    uint64_t msg; // 頼まれた時点で、このメモリを使う submit のいちばん後の物の番号（0 なら、もう終わっている）
    uint32_t w, h, stride, bgra, flags, kbps, fps, gop;
} g_vh;
#define VQ_MAX 4
static struct { vr_buf b[VQ_MAX]; int head, n, fail; } g_vq; // 縮め終わった物の列（古い順）
static struct { uint64_t reqs, started, dropped, forced, overflow; double wait_ms, join_ms; } g_vhs; // 数え（この接続）

static void vq_push(const uint8_t *p, size_t n) {
    if (g_vq.n == VQ_MAX) { g_vq.head = (g_vq.head + 1) % VQ_MAX; g_vq.n--; g_vhs.overflow++; } // ふつうは起きない（頼み 1 つに出来上がり 1 つ）
    vr_buf *b = &g_vq.b[(g_vq.head + g_vq.n) % VQ_MAX];
    b->n = 0;
    vr_put(b, p, n);
    g_vq.n++;
}

// 縮めている物を待って、出来上がりを列に入れる
static void vh_collect(void) {
    vp_join();
    if (g_vp.has) {
        g_vp.has = 0;
        if (g_vp.ok) vq_push(g_vp.out.p, g_vp.out.n);
        else g_vq.fail = 1;
    }
}

// 覚えた頼みを写して縮め始める。その前に、そのコマの GPU の仕事（頼まれた時点でこのメモリを使っていた submit まで）を待つ
static void vh_complete(void) {
    if (!g_vh.on) return;
    g_vh.on = 0;
    double t0 = t_ms();
    int u = -1;
    for (int i = 0; i < g_nfl; i++) if (g_fl[i].msg && g_fl[i].msg <= g_vh.msg) u = i;
    drain_upto(u);
    g_vhs.wait_ms += t_ms() - t0;
    if (g_err != VK_SUCCESS) return;
    double t1 = t_ms();
    vh_collect();
    g_vhs.join_ms += t_ms() - t1;
    ent *e = find(g_vh.mid);
    if (!e || !e->map || g_vh.off > e->size || (uint64_t) g_vh.stride * g_vh.h > e->size - g_vh.off) { g_vhs.dropped++; return; }
    g_vp.w = g_vh.w, g_vp.h = g_vh.h, g_vp.stride = g_vh.stride, g_vp.bgra = g_vh.bgra, g_vp.flags = g_vh.flags & ~VR_H264_LATE;
    g_vp.kbps = g_vh.kbps, g_vp.fps = g_vh.fps, g_vp.gop = g_vh.gop, g_vp.stream = g_vh.stream, g_vp.tag = g_vh.tag;
    if (vp_start((const uint8_t *) e->map + g_vh.off)) g_vhs.started++;
    else g_vq.fail = 1;
}

// メモリ mid を書き換える・放す命令の前：覚えた頼みがそのメモリなら、先に写す（放すなら捨てる。縮めていないので並びは崩れない）
static void vh_before_mem(uint64_t mid, int freeing) {
    if (!g_vh.on || g_vh.mid != mid) return;
    if (freeing) { g_vh.on = 0; g_vhs.dropped++; return; }
    g_vhs.forced++;
    vh_complete();
}

// 遅らせる形の物を全部捨てる（接続が切れた・装置を消す・遅らせない頼みが来た）。捨てたら 1
static int vh_reset(void) {
    int any = g_vh.on || g_vq.n;
    g_vh.on = 0;
    g_vq.n = 0;
    g_vq.head = 0;
    g_vq.fail = 0;
    return any;
}

// 接続が切れたとき：縮めている途中なら待ち、エンコーダを全部閉じる（次につないだ人に前の人のコマを見せない）
static void vid_reset(void) {
    vh_reset();
    if (g_vhs.reqs)
        vlog(JE("この接続：H.264 を遅らせて写した %llu 回（写した %llu・捨てた %llu・書き換えの前に写した %llu・列があふれた %llu）・GPU を待つ %.0f ms・前のコマを待つ %.0f ms", "This connection: H.264 delayed copy %llu times (copied %llu, dropped %llu, copied before overwrite %llu, queue overflowed %llu), GPU wait %.0f ms, previous-frame wait %.0f ms"),
             (unsigned long long) g_vhs.reqs, (unsigned long long) g_vhs.started, (unsigned long long) g_vhs.dropped, (unsigned long long) g_vhs.forced,
             (unsigned long long) g_vhs.overflow, g_vhs.wait_ms, g_vhs.join_ms);
    memset(&g_vhs, 0, sizeof g_vhs);
    vp_join();
    g_vp.has = g_vp.ok = 0;
    g_vp.out.n = 0;
    for (int i = 0; i < VENC_MAX; i++) venc_drop(&g_venc[i]);
}

// 録画（VR_CAP_RECORD）。窓に出すコマと PC の音を、受け手が高画質で mp4 に書く
#include "record.h"

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
    // （VR_DESTROY_IMAGE_VIEW と VR_DESTROY_FRAMEBUFFER は待たずに、消すのを遅らせる）
    case VR_DESTROY_IMAGE: case VR_DESTROY_RENDER_PASS:
    case VR_DESTROY_SAMPLER: case VR_DESTROY_BUFFER_VIEW:
    // query の結果は、出した仕事が全部終わってから読む（ドライバは fence を待たずに「終わった」と答えているので）
    // （VR_GET_QUERY_POOL_RESULTS は、その query を書いた submit までだけ待つ。中で待つ）
    case VR_DESTROY_QUERY_POOL: case VR_RESET_QUERY_POOL:
    case VR_DESTROY_ACCEL:
        drain();
        break;
    default:
        break;
    }
    // GPU を失っていたら、返事のある命令でそれを伝える（黙って間違った答えを返さない）
    if (g_err != VK_SUCCESS) {
        switch (op) {
        case VR_MEM_READ: case VR_MEM_READ_DELTA: case VR_READ_JPEG: case VR_READ_JPEG_PIPE: case VR_READ_H264: case VR_WAIT_FOR_FENCES: case VR_GET_FENCE_STATUS: case VR_WAIT_SEMAPHORES:
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
        like_ios_limits(&p.limits);
        like_ios_vendor(&p);
        vkGetPhysicalDeviceMemoryProperties(g_pd, &m);
#if defined(__APPLE__) && TARGET_OS_IPHONE
        // iPhone では、iOS がこのアプリに許すメモリのほうが、MoltenVK の答える量よりずっと小さい（超えるとアプリが止められる）。
        // いま使ってよい量から余裕を引いた量に合わせて答える。llama.cpp などはこれを見て、載せる量や会話の長さを決める
        {
            size_t avail = os_proc_available_memory();
            uint64_t ok = avail > (768ull << 20) ? (uint64_t) avail - (768ull << 20) : 0;
            for (uint32_t i = 0; i < m.memoryHeapCount; i++)
                if (m.memoryHeaps[i].size > ok) m.memoryHeaps[i].size = ok;
            vlog(JE("使ってよいメモリ %llu MB（iOS がいま許す %llu MB から余裕を引いた）", "Usable memory %llu MB (what iOS allows now, %llu MB, minus a margin)"), (unsigned long long) (ok >> 20), (unsigned long long) (avail >> 20));
        }
#endif
        alias_mem_props(&m);
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
        if (fake_divisor()) {
            e = realloc(e, sizeof *e * (ne + 1));
            memset(&e[ne], 0, sizeof *e);
            snprintf(e[ne].extensionName, sizeof e[ne].extensionName, "VK_EXT_vertex_attribute_divisor");
            e[ne++].specVersion = 3;
        }
#ifdef __ANDROID__
        add_promoted_exts(&e, &ne);
#endif
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
            for (uint32_t i = 0; i < n; i++) like_ios_rt_feats(blocks[i]);
            if (fake_divisor())
                for (uint32_t i = 0; i < n; i++) {
                    VkBaseOutStructure *b = blocks[i];
                    if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT)
                        ((VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *) b)->vertexAttributeInstanceRateDivisor = VK_TRUE;
                }
            vr_put(&g_out, &f2.features, sizeof f2.features);
        } else {
            VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = chain };
            vkGetPhysicalDeviceProperties2(g_pd, &p2);
            like_ios_limits(&p2.properties.limits);
            like_ios_vendor(&p2.properties);
            for (uint32_t i = 0; i < n; i++) like_ios_props2(blocks[i]);
            if (fake_divisor())
                for (uint32_t i = 0; i < n; i++) {
                    VkBaseOutStructure *b = blocks[i];
                    if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT)
                        ((VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *) b)->maxVertexAttribDivisor = 0x7fffffffu;
                }
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
        for (uint32_t i = 0; i < ne && i < 63; i++) {
            const char *nm = rd_str(r);
            if (fake_divisor() && nm && !strcmp(nm, "VK_EXT_vertex_attribute_divisor")) continue; // 名乗りだけ（上の fake_divisor）。装置には頼まない
#ifdef __ANDROID__
            if (nm && promoted_minor(nm) && !dev_has_real_ext(nm)) continue; // 本体になっている拡張の名前だけ足した物（add_promoted_exts）。装置には頼まない
#endif
            names[n_names++] = nm;
        }
        if (g_portability) names[n_names++] = "VK_KHR_portability_subset";
        uint32_t has_f = vr_ru32(r);
        VkPhysicalDeviceFeatures feats;
        memset(&feats, 0, sizeof feats);
        if (has_f) vr_rcopy(r, &feats, sizeof feats);
        if (like_ios()) {
            VkPhysicalDeviceFeatures m = feats;
            like_ios_mask(&m);
            if (memcmp(&m, &feats, sizeof m)) {
                vlog(JE("iPhone のふり：iPhone に無い機能を有効にしようとしたので断る", "Pretending to be an iPhone: refusing a request to enable a feature the iPhone does not have"));
                return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
            }
        }
        // 機能の鎖（Vulkan11Features など）は中身ごと届く
        uint32_t nc = vr_ru32(r);
        void *head = NULL, **link = &head, *blocks[32];
        size_t blk_sz[32] = { 0 };
        for (uint32_t i = 0; i < nc && i < 32; i++) {
            uint32_t st = vr_ru32(r), sz;
            const void *d = vr_rbytes(r, &sz);
            blk_sz[i] = sz + sizeof(VkBaseOutStructure);
            VkBaseOutStructure *b = calloc(1, sz + sizeof(VkBaseOutStructure));
            b->sType = (VkStructureType) st;
            if (d) memcpy((uint8_t *) b + sizeof(VkBaseOutStructure), d, sz);
            blocks[i] = b;
            if (fake_divisor() && (VkStructureType) st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT) continue; // 装置には渡さない
            *link = b;
            link = (void **) &b->pNext;
        }
        // 持たない機能を頼まれたら、MoltenVK に作らせずにここで断る。MoltenVK は作れない装置を片づける所で落ちる
        // （iPhone の実機で CTS の api.info の試験のあと MVKDevice::~MVKDevice の中で落ちた）。
        // 機能の鎖の写しを、ありえない値（0xFFFFFFFF）で埋めてから GPU に聞き、書き換わった構造体だけで比べる
        // （GPU が知らない構造体は書き換わらないので、比べずにそのまま渡す）
        {
            int missing = 0;
            if (has_f) {
                VkPhysicalDeviceFeatures sup;
                vkGetPhysicalDeviceFeatures(g_pd, &sup);
                const VkBool32 *q = (const VkBool32 *) &feats, *h = (const VkBool32 *) &sup;
                for (size_t k = 0; k < sizeof feats / sizeof(VkBool32); k++) if (q[k] && !h[k]) missing = 1;
            }
            if (!missing && nc && vkGetPhysicalDeviceFeatures2) {
                VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
                void *qb[32] = { 0 };
                size_t qs[32] = { 0 };
                void **ql = &f2.pNext;
                for (uint32_t i = 0; i < nc && i < 32; i++) {
                    size_t total = blk_sz[i]; // 届いた構造体の大きさ（頭の sType と pNext を含む）
                    VkBaseOutStructure *c = malloc(total);
                    if (!c) continue;
                    memset(c, 0xFF, total);
                    c->sType = ((VkBaseOutStructure *) blocks[i])->sType;
                    c->pNext = NULL;
                    qb[i] = c;
                    qs[i] = total;
                    *ql = c;
                    ql = (void **) &c->pNext;
                }
                vkGetPhysicalDeviceFeatures2(g_pd, &f2);
                for (uint32_t i = 0; i < nc && i < 32; i++) {
                    if (!qb[i]) continue;
                    const uint32_t *hw = (const uint32_t *) ((uint8_t *) qb[i] + sizeof(VkBaseOutStructure));
                    const uint32_t *rq = (const uint32_t *) ((uint8_t *) blocks[i] + sizeof(VkBaseOutStructure));
                    size_t nw = (qs[i] - sizeof(VkBaseOutStructure)) / 4;
                    int filled = 0;
                    for (size_t k = 0; k < nw; k++) if (hw[k] != 0xFFFFFFFFu) filled = 1;
                    if (filled)
                        for (size_t k = 0; k < nw; k++)
                            if (rq[k] == VK_TRUE && hw[k] == VK_FALSE) missing = 1;
                    free(qb[i]);
                }
            }
            if (missing) {
                vlog(JE("装置を作る：GPU が持たない機能を頼まれたので断る（VK_ERROR_FEATURE_NOT_PRESENT）", "Creating device: refusing a request for a feature the GPU does not have (VK_ERROR_FEATURE_NOT_PRESENT)"));
                for (uint32_t i = 0; i < nc && i < 32; i++) free(blocks[i]);
                return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
            }
        }
        VkPhysicalDeviceRobustness2FeaturesEXT r2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .nullDescriptor = g_nulldesc_gpu ? VK_TRUE : VK_FALSE };
        if (hang_env("VKR_HANG_ROBUST2")) {
            r2.robustBufferAccess2 = r2.robustImageAccess2 = VK_TRUE;
            feats.robustBufferAccess = VK_TRUE;
            if (!has_f) { has_f = 1; }
            vlog(JE("ffhang：robustBufferAccess2・robustImageAccess2 を有効にする（VKR_HANG_ROBUST2）", "ffhang: enabling robustBufferAccess2 and robustImageAccess2 (VKR_HANG_ROBUST2)"));
        }
        vlog(JE("ffhang：装置の robustBufferAccess %u（DXVK が頼んだ値）", "ffhang: device robustBufferAccess %u (the value DXVK asked for)"), (unsigned) feats.robustBufferAccess);
        // 調べ用（ffhang2・代役だけ）：VKR_LIKE_IOS_NOROBUST=1 で、名乗りはそのままに、範囲の外の読み書きの守りを GPU で有効にしない。
        // iPhone の MoltenVK は robustBufferAccess と robustImageAccess を「ある」と答えるが、Metal は buffer の範囲を確かめない。
        // 代役の GPU は本当に確かめるので、範囲の外の読みが 0 になり、iPhone で起きることが起きない。その違いを消す
        if (hang_env("VKR_LIKE_IOS_NOROBUST")) {
            feats.robustBufferAccess = VK_FALSE;
            r2.robustBufferAccess2 = r2.robustImageAccess2 = VK_FALSE;
            for (VkBaseOutStructure *b = head; b; b = b->pNext) {
                if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES) ((VkPhysicalDeviceImageRobustnessFeatures *) b)->robustImageAccess = VK_FALSE;
                else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) ((VkPhysicalDeviceVulkan13Features *) b)->robustImageAccess = VK_FALSE;
                else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
                    ((VkPhysicalDeviceRobustness2FeaturesEXT *) b)->robustBufferAccess2 = VK_FALSE;
                    ((VkPhysicalDeviceRobustness2FeaturesEXT *) b)->robustImageAccess2 = VK_FALSE;
                } else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT)
                    ((VkPhysicalDevicePipelineRobustnessFeaturesEXT *) b)->pipelineRobustness = VK_FALSE;
            }
            vlog(JE("ffhang2：iPhone のふり：範囲の外の守りを GPU で有効にしない（VKR_LIKE_IOS_NOROBUST）", "ffhang2: pretending to be an iPhone: not enabling out-of-range protection on the GPU (VKR_LIKE_IOS_NOROBUST)"));
        }
        if (g_nulldesc_gpu || r2.robustBufferAccess2) {
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
        if (res == VK_SUCCESS) {
            put(id, (uint64_t) (uintptr_t) dev, K_DEVICE);
            if (g_dev && g_dev != dev) {
                // 2 つ目の装置（CTS の試験が自分で作る物など）。手伝いの物は前の装置の上にあるので、新しい装置で作り直す
                drain();
                watch_stop(); // 次の submit のときに、新しい装置で見張り直す
                jp_reset();
                vid_reset();
                helpers_free(g_dev);
                vlog(JE("装置をもう 1 つ作った（前の装置も残す）", "Created one more device (keeping the previous one)"));
            }
            g_dev = dev;
        }
        return reply(s, res);
    }
    case VR_DESTROY_DEVICE: {
        uint64_t id = vr_ru64(r);
        if (g_vh.on) { g_vh.on = 0; g_vhs.dropped++; }
        VkDevice h = DEV(id), other = VK_NULL_HANDLE;
        int ndev = 0;
        for (size_t i = 0; i < g_cap; i++)
            if (g_tab[i].id && g_tab[i].kind == K_DEVICE) {
                ndev++;
                if ((VkDevice) (uintptr_t) g_tab[i].h != h) other = (VkDevice) (uintptr_t) g_tab[i].h;
            }
        if (!h || ndev <= 1) { cleanup(); return 1; }
        // ほかの装置が残る：この装置の物だけを片づける（前は全部を片づけ、残った装置で作ると落ちていた）
        drain();
        watch_stop(); // 消す装置の fence を見張りが待たないように。次の submit のときに、残る装置で見張り直す
        if (g_dev == h) {
            jp_reset();
            vid_reset();
            helpers_free(h);
            g_dev = other;
        }
        dev_free(h, 0);
        vlog(JE("装置を 1 つ消した（ほかの装置は残す）", "Removed one device (keeping the others)"));
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
            req.memoryTypeBits = alias_bits(req.memoryTypeBits);
        }
        if (op == VR_CREATE_BUFFER_NR) {
            if (res != VK_SUCCESS) vlog(JE("buffer を作れない (%d)", "Cannot create the buffer (%d)"), res);
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
        mai.memoryTypeIndex = alias_type(vr_ru32(r));
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
                vlog(JE("メモリ %llu MB は確保しない（iOS がいま許すのは %llu MB）", "Not allocating %llu MB of memory (iOS allows only %llu MB now)"), (unsigned long long) (mai.allocationSize >> 20), (unsigned long long) (avail >> 20));
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
        vh_before_mem(id, 1);
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
        uint64_t off = vr_ru64(r);
        uint32_t n;
        const void *d = vr_rbytes(r, &n);
        vh_before_mem(mid, 0);
        // ドライバが「書く範囲を GPU が使う submit の番号」を付けていれば、そこまでだけ待つ（付いていなければ、そのメモリを使う仕事を全部待つ）
        int k = last_with_mem(mid);
        if (r->at + 8 <= r->n) {
            uint64_t upto = vr_ru64(r);
            // いちばん上の桁：このメモリは、ここの帳簿に載らない使い方（アドレスで読む・結んだあとに書いた descriptor）をされたので、番号の方が後なら、そこまで待つ（u1）
            int wide = (int) (upto >> 63);
            upto &= ~(1ull << 63);
            int u = -1;
            for (int i = 0; i < g_nfl; i++) if (g_fl[i].msg <= upto) u = i;
            if (wide ? u > k : u < k) k = u;
        }
        drain_upto(k);
        ent *e = find(mid);
        if (e && e->map && d && off <= e->size && n <= e->size - off) {
            memcpy((uint8_t *) e->map + off, d, n);
            if (e->last) memcpy(e->last + off, d, n);
        } else vlog(JE("範囲の外への書き込みを捨てた（%llu + %u）", "Dropped a write out of range (%llu + %u)"), (unsigned long long) off, n);
        g_bst.memw++;
        return 1;
    }
    case VR_MEM_WRITE_SPANS: {
        // まとめた書き込み（VR_CAP_BATCH）：memory・番号・数・[offset・中身] の並び。
        // 待つのは 1 回だけ。番号は全部の範囲の中でいちばん後の物なので、1 つずつ送られた VR_MEM_WRITE を順に処理したときの待ちの
        // いちばん長い物と同じ所まで待つ（どれも同じメモリなので、自分の判定 last_with_mem も同じ）。そのあと届いた順に書く
        uint64_t mid = vr_ru64(r);
        uint64_t upto = vr_ru64(r);
        uint32_t n = vr_ru32(r);
        vh_before_mem(mid, 0);
        int k = last_with_mem(mid);
        {
            int wide = (int) (upto >> 63);
            upto &= ~(1ull << 63);
            int u = -1;
            for (int i = 0; i < g_nfl; i++) if (g_fl[i].msg <= upto) u = i;
            if (wide ? u > k : u < k) k = u;
        }
        drain_upto(k);
        ent *e = find(mid);
        for (uint32_t i = 0; i < n && !r->bad; i++) {
            uint64_t off = vr_ru64(r);
            uint32_t len;
            const void *d = vr_rbytes(r, &len);
            if (e && e->map && d && off <= e->size && len <= e->size - off) {
                memcpy((uint8_t *) e->map + off, d, len);
                if (e->last) memcpy(e->last + off, d, len);
            } else vlog(JE("範囲の外への書き込みを捨てた（%llu + %u）", "Dropped a write out of range (%llu + %u)"), (unsigned long long) off, len);
        }
        g_bst.spans_msgs++;
        g_bst.spans += n;
        return 1;
    }
    case VR_MEM_WRITE_CODED: {
        // 縮めた平面（VR_CAP_PLANE_CODE）：memory・offset・番号（VR_MEM_WRITE の末尾と同じ。いつも付く）・縮めた中身（plane_codec.h）
        uint64_t mid = vr_ru64(r);
        uint64_t off = vr_ru64(r);
        uint64_t upto = vr_ru64(r);
        vh_before_mem(mid, 0);
        size_t n = r->bad || r->at > r->n ? 0 : r->n - r->at;
        const uint8_t *blob = r->bad ? NULL : (const uint8_t *) vr_take(r, n);
        int k = last_with_mem(mid);
        {
            int wide = (int) (upto >> 63);
            upto &= ~(1ull << 63);
            int u = -1;
            for (int i = 0; i < g_nfl; i++) if (g_fl[i].msg <= upto) u = i;
            if (wide ? u > k : u < k) k = u;
        }
        drain_upto(k);
        ent *e = find(mid);
        uint64_t need = blob ? pc_blob_bytes(blob, n) : 0;
        if (!e || !e->map || !need || off > e->size || need > e->size - off) {
            vlog(JE("縮めた書き込みを捨てた（%llu + %llu・中身 %zu バイト）", "Dropped a compressed write (%llu + %llu, payload %zu bytes)"), (unsigned long long) off, (unsigned long long) need, n);
            return 1;
        }
        double t0 = t_ms();
        const char *err = "";
        if (pc_decode(blob, n, (uint8_t *) e->map + off, &err)) vlog(JE("縮めた書き込みをほどけない（%s）。この範囲の中身はでたらめになる", "Cannot decode a compressed write (%s). The contents of this range will be garbage"), pc_err_text(err));
        else if (e->last) memcpy(e->last + off, (uint8_t *) e->map + off, (size_t) need);
        g_pcs.n++;
        g_pcs.in += n;
        g_pcs.out += need;
        g_pcs.ms += t_ms() - t0;
        return 1;
    }
    case VR_MEM_READ: {
        uint64_t mid = vr_ru64(r);
        uint64_t off = vr_ru64(r), n = vr_ru64(r);
        int k = last_with_mem(mid);
        // 末尾の番号（いちばん上の桁つき）：アドレスで書かれうるメモリ。ここの帳簿は書き手を知らないので、番号の方が後なら、そこまで待つ（u1）
        if (r->at + 8 <= r->n) {
            uint64_t upto = vr_ru64(r);
            if (upto >> 63) {
                upto &= ~(1ull << 63);
                for (int i = 0; i < g_nfl; i++) if (g_fl[i].msg <= upto && i > k) k = i;
            }
        }
        drain_upto(k);
        if (g_err != VK_SUCCESS) return reply(s, g_err);
        ent *e = find(mid);
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
#ifdef VR_HAVE_JPEG
        double jt1 = t_ms();
        size_t before = g_out.n;
        if (!jpeg_encode((const uint8_t *) e->map + off, w, h, stride, (int) bgra, (int) (q ? q : 80), &g_out)) return reply(s, VK_ERROR_UNKNOWN);
        jt_enc += t_ms() - jt1;
        jbytes += g_out.n - before;
        pv_store(g_out.p + before, g_out.n - before); // アプリの画面に出す最新の 1 枚
        g_frames_total++;
        if (++jn % 100 == 0)
            vlog(JE("JPEG：%llu コマ・GPU を待つ %.1f ms・縮める %.1f ms・平均 %.1f KB（1 コマあたり）・熱 %d", "JPEG: %llu frames, GPU wait %.1f ms, compress %.1f ms, average %.1f KB per frame, thermal %d"), (unsigned long long) jn,
                 jt_wait / 100, jt_enc / 100, jbytes / 1024.0 / 100, g_thermal), jt_wait = jt_enc = 0, jbytes = 0;
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
#ifdef VR_HAVE_JPEG
        // ひとつ前のコマの JPEG ができあがるのを待って、返事に入れる
        double t1 = t_ms();
        jp_join();
        jp_join_ms += t_ms() - t1;
        if (g_jp.ok) {
            vr_put(&g_out, g_jp.out.p, g_jp.out.n);
            jp_enc += g_jp.ms;
            jpbytes += g_jp.out.n;
            pv_store(g_jp.out.p, g_jp.out.n); // アプリの画面に出すのも、返事に入れたひとつ前のコマ
        }
        // 今のコマを写して、後ろで縮め始める
        g_jp.w = w, g_jp.h = h, g_jp.stride = stride, g_jp.bgra = (int) bgra, g_jp.q = (int) (q ? q : 80);
        if (!jp_start((const uint8_t *) e->map + off)) return reply(s, VK_ERROR_OUT_OF_HOST_MEMORY);
        g_frames_total++;
        if (++jpn % 100 == 0)
            vlog(JE("JPEG（先送り）：%llu コマ・GPU を待つ %.1f ms・前のコマを待つ %.1f ms・縮める %.1f ms（後ろで）・平均 %.1f KB（1 コマあたり）・熱 %d", "JPEG (deferred): %llu frames, GPU wait %.1f ms, previous-frame wait %.1f ms, compress %.1f ms (in the background), average %.1f KB per frame, thermal %d"),
                 (unsigned long long) jpn, jp_wait / 100, jp_join_ms / 100, jp_enc / 100, jpbytes / 1024.0 / 100, g_thermal),
                jp_wait = jp_join_ms = jp_enc = 0, jpbytes = 0;
        return reply(s, 0);
#else
        (void) bgra; (void) q;
        return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
#endif
    }
    case VR_READ_H264: {
        static double vt_wait, vt_join_ms, vt_enc;
        static uint64_t vn, vbytes, vkeys;
        double t0 = t_ms();
        uint64_t mid = vr_ru64(r);
        uint64_t off = vr_ru64(r);
        uint32_t w = vr_ru32(r), h = vr_ru32(r), stride = vr_ru32(r), bgra = vr_ru32(r), flags = vr_ru32(r);
        uint32_t kbps = vr_ru32(r), fps = vr_ru32(r), gop = vr_ru32(r);
        uint64_t stream = vr_ru64(r), tag = vr_ru64(r);
        if (!kbps || kbps > 1000000) kbps = 8000;
        if (!fps || fps > 1000) fps = 60;
        if (!gop) gop = 600;
        if ((flags & VR_H264_LATE) && (flags & VR_H264_PIPE)) {
            // 遅らせる形（上の g_vh）：ひとつ前の頼みをいま写して縮め始め、この頼みは覚えるだけ。GPU は待たない（ひとつ前のコマの分は待つことがある）
            if (g_err != VK_SUCCESS) return reply(s, g_err);
            ent *e = find(mid);
            if (r->bad || !e || !e->map || w == 0 || h == 0 || w > 16384 || h > 16384 || stride < (uint64_t) w * 4 || off > e->size ||
                (uint64_t) stride * h > e->size - off)
                return reply(s, VK_ERROR_MEMORY_MAP_FAILED);
            vh_complete();
            if (g_err != VK_SUCCESS) return reply(s, g_err);
            int k = last_with_mem(mid);
            g_vh.on = 1;
            g_vh.mid = mid, g_vh.off = off, g_vh.w = w, g_vh.h = h, g_vh.stride = stride, g_vh.bgra = bgra, g_vh.flags = flags;
            g_vh.kbps = kbps, g_vh.fps = fps, g_vh.gop = gop, g_vh.stream = stream, g_vh.tag = tag;
            g_vh.msg = k >= 0 ? g_fl[k].msg : 0;
            g_vhs.reqs++;
            vt_wait += t_ms() - t0;
            if (g_vq.fail) { vh_reset(); return reply(s, VK_ERROR_FEATURE_NOT_PRESENT); }
            size_t before = g_out.n;
            if (g_vq.n) {
                vr_buf *b = &g_vq.b[g_vq.head];
                vr_put(&g_out, b->p, b->n);
                g_vq.head = (g_vq.head + 1) % VQ_MAX;
                g_vq.n--;
            }
            if (g_out.n > before) {
                uint32_t f = 0;
                memcpy(&f, g_out.p + before + 24, 4);
                vbytes += g_out.n - before - VR_H264_HEAD;
                vkeys += f & 1;
                g_frames_total++;
                if (++vn % 100 == 0)
                    vlog(JE("H.264（遅らせて写す）：%llu コマ・頼みで待つ %.1f ms・平均 %.1f KB・キーフレーム %llu 枚（100 コマあたり）", "H.264 (delayed copy): %llu frames, wait for requests %.1f ms, average %.1f KB, %llu keyframes (per 100 frames)"),
                         (unsigned long long) vn, vt_wait / 100, vbytes / 1024.0 / 100, (unsigned long long) vkeys),
                        vt_wait = vt_join_ms = vt_enc = 0, vbytes = vkeys = 0;
            }
            return reply(s, 0);
        }
        // 遅らせない頼み：遅らせる形で覚えた物・縮め終わった物が残っていれば捨て、並びが崩れないようにキーフレームにする
        if (vh_reset()) flags |= VR_H264_KEY;
        drain_upto(last_with_mem(mid));
        vt_wait += t_ms() - t0;
        if (g_err != VK_SUCCESS) return reply(s, g_err);
        ent *e = find(mid);
        if (r->bad || !e || !e->map || w == 0 || h == 0 || w > 16384 || h > 16384 || stride < (uint64_t) w * 4 || off > e->size ||
            (uint64_t) stride * h > e->size - off)
            return reply(s, VK_ERROR_MEMORY_MAP_FAILED);
        const uint8_t *src = (const uint8_t *) e->map + off;
        size_t before = g_out.n;
        if (flags & VR_H264_PIPE) {
            // ひとつ前のコマができあがるのを待って、返事に入れる
            double t1 = t_ms();
            vp_join();
            vt_join_ms += t_ms() - t1;
            if (g_vp.has) {
                g_vp.has = 0;
                if (!g_vp.ok) return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
                vr_put(&g_out, g_vp.out.p, g_vp.out.n);
                vt_enc += g_vp.ms;
            }
            // 今のコマを写して、後ろで縮め始める
            g_vp.w = w, g_vp.h = h, g_vp.stride = stride, g_vp.bgra = bgra, g_vp.flags = flags, g_vp.kbps = kbps, g_vp.fps = fps, g_vp.gop = gop;
            g_vp.stream = stream, g_vp.tag = tag;
            if (!vp_start(src)) return reply(s, VK_ERROR_OUT_OF_HOST_MEMORY);
        } else {
            // その場で縮める。先送りで縮めたコマが残っていれば捨て、並びが崩れないようにキーフレームにする
            vp_join();
            if (g_vp.has) { g_vp.has = 0; flags |= VR_H264_KEY; }
            double t1 = t_ms();
            if (!h264_frame(src, w, h, stride, (int) bgra, flags, kbps, fps, gop, stream, tag, &g_out)) {
                g_out.n = before;
                return reply(s, VK_ERROR_FEATURE_NOT_PRESENT);
            }
            vt_enc += t_ms() - t1;
        }
        if (g_out.n > before) {
            uint32_t f = 0;
            memcpy(&f, g_out.p + before + 24, 4);
            vbytes += g_out.n - before - VR_H264_HEAD;
            vkeys += f & 1;
            g_frames_total++;
            if (++vn % 100 == 0)
                vlog(JE("H.264：%llu コマ・GPU を待つ %.1f ms・前のコマを待つ %.1f ms・縮める %.1f ms・平均 %.1f KB・キーフレーム %llu 枚（100 コマあたり）", "H.264: %llu frames, GPU wait %.1f ms, previous-frame wait %.1f ms, compress %.1f ms, average %.1f KB, %llu keyframes (per 100 frames)"),
                     (unsigned long long) vn, vt_wait / 100, vt_join_ms / 100, vt_enc / 100, vbytes / 1024.0 / 100, (unsigned long long) vkeys),
                    vt_wait = vt_join_ms = vt_enc = 0, vbytes = vkeys = 0;
        }
        return reply(s, 0);
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
        // ffhang：cbuffer を 4096 個の vec4（64 KiB）の uniform として宣言した module（範囲の外を読みうる。DXVK が 0 を返す前提で範囲を絞る）と、
        // 同じ物を storage buffer で読む module（DXVK 1.10 の d3d11.constantBufferRangeCheck の形）を数える
        if (code && n >= 20 && (n % 4) == 0) {
            static unsigned n_mod, n_ubo64k, n_ssbo_rt;
            const uint32_t *w = (const uint32_t *) code;
            uint32_t nw = n / 4, bound = w[3] < 4000000 ? w[3] : 0;
            uint8_t *isvec4 = bound ? calloc(bound, 1) : NULL; // 1：vec4・2：4096 個の vec4 の配列・4：Block・8：BufferBlock・16：64 KiB の配列を持つ struct
            uint32_t *cval = bound ? calloc(bound, 4) : NULL;
            int arr4096 = 0, ptr_uni = 0, rt_vec4 = 0, ptr_sb = 0;
            if (isvec4 && cval) {
                for (uint32_t i = 5; i < nw;) {
                    uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
                    if (!len || i + len > nw) break;
                    if (op == 23 && len >= 4 && w[i + 1] < bound && w[i + 3] == 4) isvec4[w[i + 1]] = 1;
                    else if (op == 43 && len >= 4 && w[i + 2] < bound) cval[w[i + 2]] = w[i + 3];
                    else if (op == 71 && len >= 3 && w[i + 1] < bound && (w[i + 2] == 2 || w[i + 2] == 3)) isvec4[w[i + 1]] |= w[i + 2] == 2 ? 4 : 8;
                    else if (op == 28 && len >= 4 && w[i + 1] < bound && w[i + 2] < bound && w[i + 3] < bound && (isvec4[w[i + 2]] & 1) && cval[w[i + 3]] == 4096) isvec4[w[i + 1]] |= 2;
                    else if (op == 30 && len >= 3 && w[i + 1] < bound) {
                        for (uint32_t k = 2; k < len; k++) if (w[i + k] < bound && (isvec4[w[i + k]] & 2)) {
                            if (isvec4[w[i + 1]] & 4) arr4096 = 1;        // uniform（Block）
                            if (isvec4[w[i + 1]] & 8) rt_vec4 = 1;        // storage（BufferBlock）
                        }
                    }
                    else if (op == 29 && len >= 3 && w[i + 2] < bound && isvec4[w[i + 2]]) rt_vec4 = 1;
                    else if (op == 32 && len >= 4) { if (w[i + 2] == 2) ptr_uni = 1; if (w[i + 2] == 12) ptr_sb = 1; }
                    i += len;
                }
            }
            free(isvec4); free(cval);
            n_mod++;
            if (arr4096 && ptr_uni) n_ubo64k++;
            if (rt_vec4 || (ptr_sb && 0)) n_ssbo_rt++;
            if ((n_mod % 200) == 0 || n_mod == 50) vlog(JE("ffhang：shader module %u 個・64 KiB の cbuffer を uniform（Block）で宣言 %u 個・storage（BufferBlock）で宣言 %u 個", "ffhang: %u shader modules, %u declare a 64 KiB cbuffer as uniform (Block), %u as storage (BufferBlock)"), n_mod, n_ubo64k, n_ssbo_rt);
        }
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
        // 末尾に binding ごとの flags（VR_CAP_DESC_FLAGS・vkd3d-proton の update after bind と、数を確保のときに決める binding）
        VkDescriptorBindingFlags *bf = NULL;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
        if (!r->bad && r->at + 4 <= r->n && vr_ru32(r) == n && n && (bf = calloc(n, sizeof *bf))) {
            for (uint32_t i = 0; i < n; i++) bf[i] = vr_ru32(r);
            if (!r->bad) {
                bfi.bindingCount = n;
                bfi.pBindingFlags = bf;
                ci.pNext = &bfi;
            }
        }
        VkDescriptorSetLayout l;
        VkResult res = vkCreateDescriptorSetLayout(d, &ci, NULL, &l);
        if (res == VK_SUCCESS) put(id, (uint64_t) l, K_DSL);
        else vlog(JE("descriptor set の形を作れない (%d)", "Cannot create the descriptor set layout (%d)"), res);
        free(b);
        free(bf);
        return 1;
    }
    case VR_DESTROY_DSL: {
        // 消さずに残し、装置を消すときにまとめて消す。作る・消す・set を取る命令の届く順がアプリの順とずれると、
        // 消した layout で set を取って MoltenVK が落ちる（iPhone の実機の CTS で MVKDescriptorPool::allocateDescriptorSets）。
        // 番号は使い回されないので、残しても取り違えない
        (void) DEV(vr_ru64(r));
        (void) vr_ru64(r);
        return 1;
    }
    case VR_CREATE_PIPELINE_LAYOUT: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        uint32_t ns = vr_ru32(r);
        VkDescriptorSetLayout sl[32];
        // 上限を超えた分も読み捨てて、並びをずらさない（前は数をそのまま渡し、並びの外を読んでいた）
        for (uint32_t i = 0; i < ns && !r->bad; i++) {
            uint64_t sid = vr_ru64(r);
            if (i < 32) sl[i] = OBJ(VkDescriptorSetLayout, sid);
        }
        if (ns > 32) ns = 32;
        uint32_t np = vr_ru32(r);
        VkPushConstantRange pr[8];
        for (uint32_t i = 0; i < np && !r->bad; i++) {
            uint32_t sf = vr_ru32(r), of = vr_ru32(r), sz = vr_ru32(r);
            if (i < 8) { pr[i].stageFlags = sf; pr[i].offset = of; pr[i].size = sz; }
        }
        if (np > 8) np = 8;
        if (!d || r->bad) return 1;
        // 無い set layout（消した番号・作れなかった番号・アプリが渡した空）は、中身の無い layout で埋める。
        // MoltenVK は空のまま渡すと中で落ちる（CTS の alloc_callback_fail のあとに起きた）
        VkDescriptorSetLayout empty = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < ns; i++) {
            if (sl[i]) continue;
            if (!empty) {
                VkDescriptorSetLayoutCreateInfo ec = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                if (vkCreateDescriptorSetLayout(d, &ec, NULL, &empty) != VK_SUCCESS) { vlog(JE("pipeline layout：空の set layout を作れない。作らない", "pipeline layout: cannot create an empty set layout. Not creating it")); return 1; }
                vlog(JE("pipeline layout：無い set layout を中身の無い物で埋めた", "pipeline layout: filled the missing set layouts with empty ones"));
            }
            sl[i] = empty;
        }
        VkPipelineLayoutCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = ns, .pSetLayouts = sl, .pushConstantRangeCount = np, .pPushConstantRanges = pr };
        VkPipelineLayout l;
        if (vkCreatePipelineLayout(d, &ci, NULL, &l) == VK_SUCCESS) { put(id, (uint64_t) l, K_PLAYOUT); note_layout_sets(id, ns); }
        if (empty) vkDestroyDescriptorSetLayout(d, empty, NULL); // layout を作ったあとは消してよい
        return 1;
    }
    case VR_DESTROY_PIPELINE_LAYOUT: {
        // 消さずに残し、装置を消すときにまとめて消す。command buffer の記録は記録し終えたときにまとめて届くので、
        // アプリが記録の途中で layout を消すと、消す知らせが先に着いて、再生で layout が無くなっていた（CTS の multisample で
        // 代役の NVIDIA が落ちた）。番号は使い回されないので、残しても取り違えない。layout は小さい
        (void) DEV(vr_ru64(r));
        (void) vr_ru64(r);
        return 1;
    }
    case VR_CREATE_COMPUTE_PIPELINE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.flags = vr_ru32(r);
        uint64_t lid = vr_ru64(r);
        ci.layout = OBJ(VkPipelineLayout, lid);
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
        if (!d || !ci.stage.module || !ci.layout || r->bad) {
            vlog(JE("compute pipeline の装置・shader module・layout のどれかが無い。作らない", "compute pipeline: the device, shader module, or layout is missing. Not creating it"));
            return reply(s, VK_ERROR_INITIALIZATION_FAILED);
        }
        VkPipeline p;
        VkResult res = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, NULL, &p);
        if (res == VK_SUCCESS) { put(id, (uint64_t) p, K_PIPELINE); note_pipe_sets(id, lid); }
        else vlog(JE("compute pipeline を作れない (%d)", "Cannot create the compute pipeline (%d)"), res);
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
        // 末尾に set ごとの数（VR_CAP_DESC_FLAGS・数を確保のときに決める binding）。1 つ 16 バイトの組の後ろにある
        uint32_t *vcount = NULL;
        if (n <= (r->n - r->at) / 16 && r->at + (size_t) n * 20 <= r->n && (vcount = calloc(n ? n : 1, sizeof *vcount))) {
            vr_rd t = *r;
            t.at += (size_t) n * 16;
            for (uint32_t i = 0; i < n; i++) vcount[i] = vr_ru32(&t);
        }
        for (uint32_t i = 0; i < n; i++) {
            VkDescriptorSetLayout l = OBJ(VkDescriptorSetLayout, vr_ru64(r));
            uint64_t id = vr_ru64(r);
            VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &l };
            VkDescriptorSetVariableDescriptorCountAllocateInfo vai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
                                                                      .descriptorSetCount = 1, .pDescriptorCounts = vcount ? &vcount[i] : NULL };
            if (vcount) ai.pNext = &vai;
            VkDescriptorSet ds;
            if (!d || !pool || !l) { vlog(JE("descriptor set：装置・pool・layout のどれかが無いので取らない", "descriptor set: the device, pool, or layout is missing, so not allocating")); continue; }
            VkResult res = vkAllocateDescriptorSets(d, &ai, &ds);
            if (res == VK_SUCCESS) {
                put(id, (uint64_t) ds, K_DSET);
                ent *se = find(id);
                if (se) { se->aux = pid; se->size = (uint64_t) (uintptr_t) l; } // どの pool から取ったか・ffhang：layout
            } else vlog(JE("descriptor set を取れない (%d)", "Cannot allocate the descriptor set (%d)"), res);
        }
        free(vcount);
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
        g_bst.uds_msgs++;
        uint64_t dbg_prev = 0;
        for (uint32_t i = 0; i < nw; i++) {
            VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            uint64_t sid = vr_ru64(r);
            if (dbg_on() && sid != dbg_prev) { dset_of(sid)->n = 0; dbg_prev = sid; }
            if (vkr_check()) {
                int k1 = last_with_set(sid), k2 = -1;
                for (int q = g_nfl - 1; q >= 0 && k2 < 0; q--)
                    for (uint32_t j = 0; j < g_fl[q].nsnap; j++) if (g_fl[q].snap[j] == sid) { k2 = q; break; }
                // 同じ実物（ハンドル）を、違う id で使っている走り中の仕事があるか
                VkDescriptorSet hs = OBJ(VkDescriptorSet, sid);
                for (int q = g_nfl - 1; q > k1 && q >= 0; q--)
                    for (uint32_t j = 0; j < g_fl[q].nsnap; j++) {
                        ent *oe = find(g_fl[q].snap[j]);
                        if (g_fl[q].snap[j] != sid && oe && (VkDescriptorSet) (uintptr_t) oe->h == hs) {
                            vlog(JE("取りこぼし（実物）：set %llu の実物は、%d 番目の仕事では set %llu として使われている（%s）", "Missed sync (real): the real set %llu is used by job #%d as set %llu (%s)"), (unsigned long long) sid, q,
                                 (unsigned long long) g_fl[q].snap[j], vkGetFenceStatus(g_dev, g_fl[q].f) == VK_SUCCESS ? JE("もう終わっている", "already finished") : JE("まだ走っている", "still running"));
                            break;
                        }
                    }
                if (k2 > k1) {
                    VkResult st = vkGetFenceStatus(g_dev, g_fl[k2].f);
                    vlog(JE("取りこぼし：set %llu を書き換える。今の判定は %d 番目まで待つが、submit の時点では %d 番目（%s）が使っていた。その command buffer：%llu", "Missed sync: rewriting set %llu. The current check waits up to job #%d, but at submit time job #%d (%s) was using it. Its command buffer: %llu"),
                         (unsigned long long) sid, k1, k2, st == VK_SUCCESS ? JE("もう終わっている", "already finished") : JE("まだ走っている", "still running"),
                         (unsigned long long) (g_fl[k2].ncb ? g_fl[k2].cbs[0] : 0));
                }
            }
            drain_upto(last_with_set(sid));
            w.dstSet = OBJ(VkDescriptorSet, sid);
            w.dstBinding = vr_ru32(r);
            w.dstArrayElement = vr_ru32(r);
            w.descriptorType = (VkDescriptorType) vr_ru32(r);
            w.descriptorCount = vr_ru32(r);
            if (w.descriptorCount > r->n - r->at) { uds_flush(&g_uds, d, !r->bad); return 1; }
            ent *se = find(sid);
            int image = w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                        w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                        w.descriptorType == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            if (w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                VkBufferView *tv = calloc(w.descriptorCount ? w.descriptorCount : 1, sizeof *tv);
                for (uint32_t k = 0; k < w.descriptorCount; k++) {
                    uint64_t vid = vr_ru64(r);
                    tv[k] = OBJ(VkBufferView, vid);
                    if (!tv[k] && !g_nulldesc_gpu) { tv[k] = sepdummy_on() && w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ? dummy_sink_bview(d) : VK_NULL_HANDLE; if (!tv[k]) tv[k] = dummy_bview(d); if (g_hang_subst[0]++ < 5) vlog(JE("ffhang：空の texel buffer（種類 %d・binding %u）を代わりに", "ffhang: stand-in for an empty texel buffer (type %d, binding %u)"), (int) w.descriptorType, w.dstBinding + k); } // 空の見え方は代わりの物に
                    ent *ve = find(vid);
                    if (se && ve && ve->aux) idlist_add(&se->list, ve->aux); // 見え方の aux はもとの buffer
                }
                w.pTexelBufferView = tv;
                if (!r->bad && w.dstSet) uds_add(&g_uds, d, &w, tv); else free(tv);
                continue;
            }
            if (w.descriptorType == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
                // 1 つにつき acceleration structure の番号（VR_CAP_RT）。書き込みの鎖と並びを 1 つの塊にして、まとめ書きのあとに放す
                struct aswrite { VkWriteDescriptorSetAccelerationStructureKHR w; VkAccelerationStructureKHR h[]; } *as =
                    calloc(1, sizeof *as + (w.descriptorCount ? w.descriptorCount : 1) * sizeof(VkAccelerationStructureKHR));
                for (uint32_t k = 0; k < w.descriptorCount; k++) {
                    uint64_t aid = vr_ru64(r);
                    if (as) as->h[k] = OBJ(VkAccelerationStructureKHR, aid);
                    ent *ae = find(aid);
                    if (se && ae && ae->kind == K_ACCEL && ae->aux) idlist_add(&se->list, ae->aux); // 置き場の buffer を使う物として覚える
                }
                if (!as) continue;
                as->w = (VkWriteDescriptorSetAccelerationStructureKHR) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                                                                         .accelerationStructureCount = w.descriptorCount, .pAccelerationStructures = as->h };
                w.pNext = &as->w;
                if (!r->bad && w.dstSet && (g_caps & VR_CAP_RT)) uds_add(&g_uds, d, &w, as); else free(as);
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
                            vlog(JE("空の sampler が届いたので、代わりの sampler を入れる（ドライバの取りこぼし）", "An empty sampler arrived, so a stand-in sampler is used (driver miss)"));
                        }
                        ii[k].sampler = g_dummy_smp;
                    }
                    uint64_t vid = vr_ru64(r);
                    ii[k].imageView = OBJ(VkImageView, vid);
                    ii[k].imageLayout = (VkImageLayout) vr_ru32(r);
                    // 画像の要る種類で空なら、代わりの画像を入れる（GPU が空の descriptor を持たないとき）
                    uint32_t vkey = (w.dstBinding << 12) + w.dstArrayElement + k;
                    if (viewfix_on() && se && vid && w.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) {
                        ent *ve = find(vid);
                        vlearn *vl = ve && ve->kind == K_VIEW && ve->size ? vlearn_at(se->size, vkey, 1) : NULL;
                        if (vl) { vl->vt = (uint32_t) (ve->size >> 32); vl->fmt = (uint32_t) ve->size; }
                    }
                    if (!ii[k].imageView && !g_nulldesc_gpu && w.descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER) {
                        vlearn *vl = viewfix_on() && se ? vlearn_at(se->size, vkey, 0) : NULL;
                        ii[k].imageView = vl ? dummy_iview_typed(d, vl->vt, (VkFormat) vl->fmt, w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) : VK_NULL_HANDLE;
                        if (vl && g_hang_subst[3]++ < 20) vlog(JE("ffhang：空の画像（種類 %d・binding %u）を見え方の種類 %u・形式 %u の代わりに", "ffhang: stand-in for an empty image (type %d, binding %u) with view type %u, format %u"), (int) w.descriptorType, w.dstBinding + k, vl->vt, vl->fmt);
                        if (!vl && viewfix_on() && g_hang_subst[4]++ < 20) vlog(JE("ffhang：空の画像（種類 %d・binding %u）は前の見え方が分からない。2D RGBA8 の代わりに", "ffhang: an empty image (type %d, binding %u) has an unknown previous view. Using 2D RGBA8 as the stand-in"), (int) w.descriptorType, w.dstBinding + k);
                        if (!ii[k].imageView && sepdummy_on() && w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                            ii[k].imageView = dummy_iview_typed(d, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1); // 書ける代わりは読む代わりと別
                        if (!ii[k].imageView) ii[k].imageView = dummy_iview(d);
                        ii[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                        if (g_hang_subst[1]++ < 20) vlog(JE("ffhang：空の画像（種類 %d・binding %u・layout %d）を 2D RGBA8 の代わりに", "ffhang: stand-in 2D RGBA8 for an empty image (type %d, binding %u, layout %d)"), (int) w.descriptorType, w.dstBinding + k, (int) ii[k].imageLayout);
                    }
                    if (se && vid) idlist_add(&se->list, vid); // 見え方の aux はもとの画像のメモリ
                }
                w.pImageInfo = ii;
                if (w.dstSet) uds_add(&g_uds, d, &w, ii); else free(ii); // 取れなかった set（layout が無いなど）には書かない（NVIDIA は落ちる）
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
                    int tn = snprintf(t, sizeof t, JE(" b%u=%llu（%llu）+%llu/%lld", " b%u=%llu (%llu)+%llu/%lld"), w.dstBinding + k, (unsigned long long) bid,
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
                    bi[k].buffer = !ubo && sepdummy_on() && dummy_sink(d) ? dummy_sink(d) : dummy_vb(); // ffhang：書ける種類は別の書き込み先へ
                    bi[k].offset = 0;
                    bi[k].range = ubo ? 16384 : VK_WHOLE_SIZE; // uniform は仕様の下限の 16 KiB に収める
                    if (g_hang_subst[2]++ < 10) vlog(JE("ffhang：空の buffer（種類 %d・binding %u）を代わりに", "ffhang: stand-in for an empty buffer (type %d, binding %u)"), (int) w.descriptorType, w.dstBinding + k);
                }
            w.pBufferInfo = bi;
            if (w.dstSet) uds_add(&g_uds, d, &w, bi); else free(bi); // 取れなかった set（layout が無いなど）には書かない（NVIDIA は落ちる）
        }
        uds_flush(&g_uds, d, !r->bad);
        { static unsigned nupd; if ((++nupd % 500) == 0) dummy_dirty_check(JE("descriptor の書き込み 500 回ごと", "every 500 descriptor writes")); }
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
        watch_forget(OBJ(VkFence, id));
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
                    else { drain_upto(k); watch_forget(f[i]); vkResetFences(d, 1, &f[i]); }
                } else {
                    watch_forget(f[i]);
                    vkResetFences(d, 1, &f[i]);
                }
            }
            return 1;
        }
        uint32_t all = vr_ru32(r);
        uint64_t timeout = vr_ru64(r);
        // 100 ms ずつ待ち、PC が切れていたらやめる
        double t0 = t_ms();
        for (;;) {
            double gone = (t_ms() - t0) * 1e6;
            uint64_t left = timeout == UINT64_MAX ? UINT64_MAX : (gone >= (double) timeout ? 0 : timeout - (uint64_t) gone);
            uint64_t slice = left < 100000000ull ? left : 100000000ull;
            VkResult wr = vkWaitForFences(d, n < 64 ? n : 64, f, all, slice);
            if (wr != VK_TIMEOUT || slice == left) return reply(s, wr);
            if (peer_gone(s)) { vlog(JE("fence を待つ間に PC が切れた", "The PC disconnected while waiting for a fence")); return 0; }
        }
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
        if (vkCreateSemaphore(d, &ci, NULL, &sm) == VK_SUCCESS) {
            put(id, (uint64_t) sm, K_SEM);
            ent *e = find(id);
            if (e) e->aux = (uint64_t) ti.semaphoreType;
        }
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
        // 届いた順の番号（ドライバも送った順に数える。query の結果を読むとき「何番の submit まで待つか」に使う）
        grave_sweep(VK_NULL_HANDLE);
        uint64_t my_msg = ++g_submit_msgs;
        VkQueue q = (VkQueue) (uintptr_t) H(vr_ru64(r));
        uint32_t ns = vr_ru32(r);
        if (ns > r->n - r->at) return 1; // 1 つの submit は 12 バイト以上あるので、残りより多いことはない
        // 数どおりに確保して全部読む（上限で切って残りを読まずにいると、9 個目からの semaphore が次の項目として読まれ、
        // でたらめなハンドルで vkQueueSubmit を呼んでいた。CTS の synchronization で見つかった）。
        // semaphore の並びは、つなぎの印（chain）のために 1 つ多く取る
        typedef struct { VkSemaphore *ws, *ss; uint64_t *wv, *sv; VkPipelineStageFlags *wst; VkCommandBuffer *cbs; } subm;
        tlv ftw[8] = { 0 }, fts[8] = { 0 };
        uint8_t fntw = 0, fnts = 0, fover = 0;
        VkSubmitInfo *si = calloc(ns ? ns : 1, sizeof *si);
        VkTimelineSemaphoreSubmitInfo *tl = calloc(ns ? ns : 1, sizeof *tl);
        subm *sm = calloc(ns ? ns : 1, sizeof *sm);
        uint64_t cids[64];
        uint32_t ncid = 0, ncb_all = 0;
        for (uint32_t i = 0; i < ns && !r->bad; i++) {
            si[i] = (VkSubmitInfo) { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tl[i] };
            tl[i] = (VkTimelineSemaphoreSubmitInfo) { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            uint32_t nw = vr_ru32(r);
            if (nw > r->n - r->at) { r->bad = 1; break; }
            sm[i].ws = calloc(nw + 1, sizeof *sm[i].ws);
            sm[i].wst = calloc(nw + 1, sizeof *sm[i].wst);
            sm[i].wv = calloc(nw + 1, sizeof *sm[i].wv);
            for (uint32_t k = 0; k < nw; k++) {
                uint64_t sid = vr_ru64(r);
                sm[i].ws[k] = OBJ(VkSemaphore, sid);
                sm[i].wst[k] = vr_ru32(r);
                sm[i].wv[k] = vr_ru64(r);
                ent *se = find(sid);
                if (se && se->kind == K_SEM && se->aux == VK_SEMAPHORE_TYPE_TIMELINE && sm[i].wv[k]) {
                    if (fntw < 8) ftw[fntw++] = (tlv) { sm[i].ws[k], sm[i].wv[k] };
                    else fover = 1;
                }
            }
            si[i].waitSemaphoreCount = tl[i].waitSemaphoreValueCount = nw;
            si[i].pWaitSemaphores = sm[i].ws;
            si[i].pWaitDstStageMask = sm[i].wst;
            tl[i].pWaitSemaphoreValues = sm[i].wv;
            uint32_t nc = vr_ru32(r);
            if (nc > r->n - r->at) { r->bad = 1; break; }
            sm[i].cbs = calloc(nc ? nc : 1, sizeof *sm[i].cbs);
            for (uint32_t k = 0; k < nc; k++) {
                uint64_t cid = vr_ru64(r);
                sm[i].cbs[k] = (VkCommandBuffer) (uintptr_t) H(cid);
                if (ncid < 64) cids[ncid++] = cid;
                ncb_all++;
            }
            si[i].commandBufferCount = nc;
            si[i].pCommandBuffers = sm[i].cbs;
            uint32_t nsg = vr_ru32(r);
            if (nsg > r->n - r->at) { r->bad = 1; break; }
            sm[i].ss = calloc(nsg + 1, sizeof *sm[i].ss);
            sm[i].sv = calloc(nsg + 1, sizeof *sm[i].sv);
            for (uint32_t k = 0; k < nsg; k++) {
                uint64_t sid = vr_ru64(r);
                sm[i].ss[k] = OBJ(VkSemaphore, sid);
                sm[i].sv[k] = vr_ru64(r);
                ent *se = find(sid);
                if (se && se->kind == K_SEM && se->aux == VK_SEMAPHORE_TYPE_TIMELINE) {
                    if (fnts < 8) fts[fnts++] = (tlv) { sm[i].ss[k], sm[i].sv[k] };
                    else fover = 1;
                }
            }
            si[i].signalSemaphoreCount = tl[i].signalSemaphoreValueCount = nsg;
            si[i].pSignalSemaphores = sm[i].ss;
            tl[i].pSignalSemaphoreValues = sm[i].sv;
        }
        if (r->bad) {
            for (uint32_t i = 0; i < ns; i++) { free(sm[i].ws); free(sm[i].wst); free(sm[i].wv); free(sm[i].cbs); free(sm[i].ss); free(sm[i].sv); }
            free(si); free(tl); free(sm);
            return 1;
        }
        VkFence f = OBJ(VkFence, vr_ru64(r));
        if (g_vh.on) {
            int uses = ncb_all > ncid; // 64 本を超えて覚えきれないときは、使うものとする
            for (uint32_t k = 0; k < ncid && !uses; k++) {
                cbinfo *c = cbi(cids[k], 0);
                if (c && c->mems)
                    for (uint32_t j = 0; j < c->mems->n; j++) if (c->mems->ids[j] == g_vh.mid) { uses = 1; break; }
            }
            if (uses) vh_before_mem(g_vh.mid, 0);
        }
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
                if (vkr_check()) vlog(JE("submit %llu：まだ走っている command buffer をもう一度出そうとした（%d 番目まで待つ）", "submit %llu: tried to submit a command buffer that is still running again (waiting up to #%d)"), (unsigned long long) g_st.submits + 1, k);
                drain_upto(k);
            }
        }
        if (chain_on() && !g_err && chain_ready())
            for (uint32_t i = 0; i < ns; i++) {
                if (g_chain_last >= 0) {
                    uint32_t k = si[i].waitSemaphoreCount;
                    sm[i].ws[k] = g_chain[g_chain_last];
                    sm[i].wst[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                    sm[i].wv[k] = 0;
                    si[i].waitSemaphoreCount = tl[i].waitSemaphoreValueCount = k + 1;
                }
                int next = (g_chain_last + 1) & 3;
                uint32_t k = si[i].signalSemaphoreCount;
                sm[i].ss[k] = g_chain[next];
                sm[i].sv[k] = 0;
                si[i].signalSemaphoreCount = tl[i].signalSemaphoreValueCount = k + 1;
                g_chain_last = next;
            }
        // 出した仕事は必ず fence で追う（アプリが付けていなければ自前の物を付ける）
        int own = 0;
        if (!f) { f = own_fence(); own = 1; }
        if (g_nfl == 256) drain();
        if (g_nfl == 256) {
            // 合図を待つ仕事で一覧が埋まった。覚えきれないので、前のやり方で全部を待つ
            vlog(JE("合図を待っている仕事が 256 本たまったので、全部を待つ", "256 jobs are waiting on signals, so waiting for all of them"));
            for (int i = 0; i < g_nfl; i++) g_fl[i].tl_over = 1;
            drain();
        }
        // iPhone のアプリが前にいない間は GPU を使えない。出すと GPU を失うので、前に戻るまで待つ（PC は待たされるだけ）
        if (g_paused) {
            vlog(JE("アプリが前にいないので、前に戻るまで GPU に仕事を出さずに待つ", "The app is not in the foreground, so not submitting GPU work until it returns"));
            double t0 = t_ms();
            while (g_paused) sleep_ms(20);
            g_st.paused_ms += t_ms() - t0;
            vlog(JE("前に戻ったので続ける", "Back in the foreground, continuing"));
        }
        double t0 = t_ms();
        VkResult res = g_err ? g_err : vkQueueSubmit(q, ns, si, f);
        g_st.submit_ms += t_ms() - t0;
        g_st.submits++;
        if (dbg_on() && res == VK_SUCCESS) {
            VkResult w = vkQueueWaitIdle(q);
            if (w != VK_SUCCESS) {
                vlog(JE("submit %llu で GPU を失った (%d)。command buffer %u 本", "Lost the GPU at submit %llu (%d). %u command buffers"), (unsigned long long) g_st.submits, w, ncid);
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
            e->q = q;
            e->msg = my_msg;
            e->ntw = fntw;
            e->nts = fnts;
            e->tl_over = fover;
            memcpy(e->tw, ftw, sizeof ftw);
            memcpy(e->ts, fts, sizeof fts);
            e->snap = NULL;
            e->nsnap = 0;
            if (vkr_check()) {
                uint32_t tot = 0;
                for (uint32_t k = 0; k < ncid; k++) { cbinfo *c = cbi(cids[k], 0); if (c) tot += c->n; }
                e->snap = malloc((tot ? tot : 1) * sizeof *e->snap);
                for (uint32_t k = 0; k < ncid; k++) {
                    cbinfo *c = cbi(cids[k], 0);
                    if (c) for (uint32_t j = 0; j < c->n; j++) e->snap[e->nsnap++] = c->sets[j];
                }
            }
            memcpy(e->cbs, cids, ncid * sizeof *cids);
            watch_add(s, f, my_msg); // 頼まれていれば、終わったら見張りの糸から知らせる
        } else {
            if (!g_err) vlog(JE("vkQueueSubmit 失敗 (%d)。この接続ではもう GPU を使えない", "vkQueueSubmit failed (%d). The GPU can no longer be used on this connection"), res);
            if (res < 0) g_err = res;
            if (own && g_nfpool < 64) g_fpool[g_nfpool++] = f;
        }
        // 64 本を超える command buffer は走っているかを覚えきれないので、終わるまで待つ（同じ物を走っている間に出し直さないため）
        if (res == VK_SUCCESS && ncb_all > 64) drain();
        for (uint32_t i = 0; i < ns; i++) { free(sm[i].ws); free(sm[i].wst); free(sm[i].wv); free(sm[i].cbs); free(sm[i].ss); free(sm[i].sv); }
        free(si); free(tl); free(sm);
        return 1;
    }
    case VR_WAIT_SEMAPHORES: {
        VkDevice d = DEV(vr_ru64(r));
        VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.flags = vr_ru32(r);
        uint32_t n = vr_ru32(r);
        if (n > r->n - r->at) return reply(s, VK_ERROR_INITIALIZATION_FAILED);
        VkSemaphore *sm = calloc(n ? n : 1, sizeof *sm);
        uint64_t *v = calloc(n ? n : 1, sizeof *v);
        for (uint32_t i = 0; i < n; i++) { sm[i] = OBJ(VkSemaphore, vr_ru64(r)); v[i] = vr_ru64(r); }
        wi.semaphoreCount = n;
        wi.pSemaphores = sm;
        wi.pValues = v;
        uint64_t timeout = vr_ru64(r);
        // 100 ms ずつ待ち、PC が切れていたらやめる（合図するはずの PC のアプリがもういない）
        double t0 = t_ms();
        VkResult wr;
        for (;;) {
            double gone = (t_ms() - t0) * 1e6;
            uint64_t left = timeout == UINT64_MAX ? UINT64_MAX : (gone >= (double) timeout ? 0 : timeout - (uint64_t) gone);
            uint64_t slice = left < 100000000ull ? left : 100000000ull;
            wr = vkWaitSemaphores(d, &wi, slice);
            if (wr != VK_TIMEOUT || slice == left) break;
            if (peer_gone(s)) { vlog(JE("semaphore を待つ間に PC が切れた", "The PC disconnected while waiting for a semaphore")); free(sm); free(v); return 0; }
        }
        free(sm);
        free(v);
        return reply(s, wr);
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
    case VR_RECORD_CTL: return rec_cmd_ctl(s, r);
    case VR_RECORD_FRAME: return rec_cmd_frame(r);
    case VR_RECORD_FETCH: return rec_cmd_fetch(s, r);
    case VR_GET_PROGRESS: {
        // 終わった所までの仕事を一覧から外し（もう終わっているので待たない）、終わった submit の番号を返す
        int k = -1;
        for (int i = 0; i < g_nfl; i++) {
            if (vkGetFenceStatus(g_dev, g_fl[i].f) != VK_SUCCESS) break;
            k = i;
        }
        if (k >= 0) drain_upto(k);
        grave_sweep(VK_NULL_HANDLE);
        vr_u64(&g_out, g_nfl ? (g_fl[0].msg ? g_fl[0].msg - 1 : 0) : g_submit_msgs);
        return reply(s, g_err);
    }
    case VR_SET_OPTIONS: {
        // このつなぎの間だけ効く印。名乗っていない印は無視する
        uint32_t f = vr_ru32(r);
        int ext = (f & VR_OPT_REPLY_EXT) && (g_caps & VR_CAP_REPLY_EXT);
        int push = ext && (f & VR_OPT_PUSH_DONE) && (g_caps & VR_CAP_PUSH_DONE);
        if (!push) watch_stop();
        g_rx_ext = ext;
        g_watch.on = push;
        vlog(JE("返事の尾 %s・終わった submit の押し出し %s", "Reply tail %s, pushing finished submits %s"), ext ? JE("あり", "yes") : JE("なし", "no"), push ? JE("あり", "yes") : JE("なし", "no"));
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
            req.memoryTypeBits = alias_bits(req.memoryTypeBits);
        }
        if (op == VR_CREATE_IMAGE_NR) {
            if (res != VK_SUCCESS) vlog(JE("image を作れない (%d)", "Cannot create the image (%d)"), res);
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
        if (e && (op == VR_DESTROY_FRAMEBUFFER || op == VR_DESTROY_IMAGE_VIEW) && g_nfl && grave_add(d, e->h, op == VR_DESTROY_FRAMEBUFFER ? K_FB : K_VIEW)) {
            if (g_ngrave > 4096) drain(); // たまりすぎたら待って消す
            idlist_free(&e->list);
            del(id);
        } else if (e) {
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
            if (ve) ve->size = ((uint64_t) ci.viewType << 32) | (uint32_t) ci.format; // ffhang：空の画像の代わりを同じ形で作るため
        } else {
            vlog(JE("image view を作れない (%d)", "Cannot create the image view (%d)"), res);
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
        // 参照の並びは、まとめて 1 つの塊に置く。塊は足りなければ伸ばし、指す先は全部読んでから決める
        // （前は 256 個で打ち切り、数はそのまま渡していたので、多い render pass で塊の外を読み、並びもずれて落ちた）
        typedef struct { uint32_t in, col, res, ds, pres; } suboff; // 塊の中の始まり。res・ds は無ければ UINT32_MAX
        suboff *so = calloc(ci.subpassCount ? ci.subpassCount : 1, sizeof *so);
        uint32_t cref = 64, cpres = 64, nref = 0, npres = 0;
        VkAttachmentReference *refs = malloc(cref * sizeof *refs);
        uint32_t *pres = malloc(cpres * sizeof *pres);
        for (uint32_t i = 0; i < ci.subpassCount && !r->bad && refs && pres && so; i++) {
            sub[i].flags = vr_ru32(r);
            sub[i].pipelineBindPoint = (VkPipelineBindPoint) vr_ru32(r);
            uint32_t cnt[3] = { 0 }, *dst[3] = { &so[i].in, &so[i].col, &so[i].res };
            so[i].res = so[i].ds = UINT32_MAX;
            for (int part = 0; part < 3 && !r->bad; part++) {
                if (part == 2 && !vr_ru32(r)) break; // resolve は無いことがある（数は color と同じ）
                uint32_t n = part == 2 ? cnt[1] : vr_ru32(r);
                if (n > (r->n - r->at) / sizeof *refs) { r->bad = 1; break; }
                cnt[part] = n;
                while (nref + n + 1 > cref) {
                    VkAttachmentReference *q = realloc(refs, cref * 2 * sizeof *refs);
                    if (!q) { r->bad = 1; break; }
                    refs = q;
                    cref *= 2;
                }
                *dst[part] = nref;
                for (uint32_t k = 0; k < n && !r->bad; k++) vr_rcopy(r, &refs[nref++], sizeof *refs);
            }
            sub[i].inputAttachmentCount = cnt[0];
            sub[i].colorAttachmentCount = cnt[1];
            if (!r->bad && vr_ru32(r)) {
                if (nref + 1 > cref) {
                    VkAttachmentReference *q = realloc(refs, (cref + 1) * sizeof *refs);
                    if (!q) { r->bad = 1; break; }
                    refs = q;
                    cref++;
                }
                so[i].ds = nref;
                vr_rcopy(r, &refs[nref++], sizeof *refs);
            }
            uint32_t np = vr_ru32(r);
            if (np > (r->n - r->at) / 4) { r->bad = 1; break; }
            while (npres + np > cpres) {
                uint32_t *q = realloc(pres, cpres * 2 * sizeof *pres);
                if (!q) { r->bad = 1; break; }
                pres = q;
                cpres *= 2;
            }
            so[i].pres = npres;
            sub[i].preserveAttachmentCount = np;
            for (uint32_t k = 0; k < np && !r->bad; k++) pres[npres++] = vr_ru32(r);
        }
        if (!refs || !pres || !so) r->bad = 1;
        for (uint32_t i = 0; i < ci.subpassCount && !r->bad; i++) {
            sub[i].pInputAttachments = refs + so[i].in;
            sub[i].pColorAttachments = refs + so[i].col;
            sub[i].pResolveAttachments = so[i].res != UINT32_MAX ? refs + so[i].res : NULL;
            sub[i].pDepthStencilAttachment = so[i].ds != UINT32_MAX ? refs + so[i].ds : NULL;
            sub[i].pPreserveAttachments = pres + so[i].pres;
        }
        free(so);
        ci.pSubpasses = sub;
        ci.dependencyCount = vr_ru32(r);
        if (ci.dependencyCount > r->n - r->at) ci.dependencyCount = 0, r->bad = 1;
        VkSubpassDependency *dep = calloc(ci.dependencyCount ? ci.dependencyCount : 1, sizeof *dep);
        for (uint32_t i = 0; i < ci.dependencyCount; i++) vr_rcopy(r, &dep[i], sizeof dep[i]);
        ci.pDependencies = dep;
        VkRenderPass rp;
        VkResult res = r->bad || !d ? VK_ERROR_INITIALIZATION_FAILED : vkCreateRenderPass(d, &ci, NULL, &rp);
        if (res == VK_SUCCESS) put(id, (uint64_t) rp, K_RPASS);
        else vlog(JE("render pass を作れない (%d)", "Cannot create the render pass (%d)"), res);
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
            vlog(JE("framebuffer を作れない (%d)", "Cannot create the framebuffer (%d)"), res);
        }
        free(v);
        return 1;
    }
    case VR_CREATE_GRAPHICS_PIPELINE: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        ci.flags = vr_ru32(r);
        uint64_t lid = vr_ru64(r);
        ci.layout = OBJ(VkPipelineLayout, lid);
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
                vlog(JE("shader module の無い段がある（段 0x%x）。pipeline を作らない", "A stage has no shader module (stage 0x%x). Not creating the pipeline"), st[i].stage);
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
                vlog(JE("iPhone のふり：頂点の形式 %u は iPhone では頂点に使えないので pipeline を作らない", "Pretending to be an iPhone: vertex format %u cannot be used for vertices on an iPhone, so not creating the pipeline"), va[i].format);
                return reply(s, VK_ERROR_INITIALIZATION_FAILED);
            }
        if (like_ios() && !ci.pTessellationState)
            for (uint32_t i = 0; i < nst; i++)
                if (st[i].stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT || st[i].stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT) {
                    vlog(JE("iPhone のふり：テッセレーションの設定が無いので pipeline を作らない", "Pretending to be an iPhone: there is no tessellation state, so not creating the pipeline"));
                    return reply(s, VK_ERROR_INITIALIZATION_FAILED);
                }
        VkPipeline pp;
        VkResult res = r->bad ? VK_ERROR_INITIALIZATION_FAILED : vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &ci, NULL, &pp);
        if (res == VK_SUCCESS) { put(id, (uint64_t) pp, K_PIPELINE); note_pipe_sets(id, lid); }
        else vlog(JE("graphics pipeline を作れない (%d)", "Cannot create the graphics pipeline (%d)"), res);
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
        else vlog(JE("sampler を作れない (%d)", "Cannot create the sampler (%d)"), res);
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
        else vlog(JE("render pass 2 を作れない (%d)", "Cannot create render pass 2 (%d)"), res);
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
        else vlog(JE("query pool を作れない (%d)", "Cannot create the query pool (%d)"), res);
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
        // ドライバが付けた「その query を書いた submit の番号」までだけ待つ。付いていなければ（古いドライバ）出した仕事を全部待つ
        uint64_t upto = r->at + 8 <= r->n ? vr_ru64(r) : UINT64_MAX;
        if (upto == UINT64_MAX) drain();
        else {
            int k = -1;
            for (int i = 0; i < g_nfl; i++) if (g_fl[i].msg <= upto) k = i;
            drain_upto(k);
        }
        if (g_err != VK_SUCCESS) return reply(s, g_err);
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
    // ---- レイトレーシングの土台（VR_CAP_RT） ----
    case VR_CREATE_ACCEL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r), bid = vr_ru64(r);
        VkAccelerationStructureCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
        ci.buffer = OBJ(VkBuffer, bid);
        ci.offset = vr_ru64(r);
        ci.size = vr_ru64(r);
        ci.type = (VkAccelerationStructureTypeKHR) vr_ru32(r);
        ci.createFlags = vr_ru32(r) & ~(uint32_t) VK_ACCELERATION_STRUCTURE_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_KHR; // 名乗っていない
        if (!d || !ci.buffer || r->bad || !(g_caps & VR_CAP_RT)) {
            vlog(JE("acceleration structure：装置・buffer のどれかが無いか、レイトレーシングを名乗っていない。作らない", "acceleration structure: the device or buffer is missing, or ray tracing is not advertised. Not creating it"));
            vr_u64(&g_out, 0);
            return reply(s, VK_ERROR_INITIALIZATION_FAILED);
        }
        VkAccelerationStructureKHR as;
        VkResult res = vkCreateAccelerationStructureKHR(d, &ci, NULL, &as);
        uint64_t addr = 0;
        if (res == VK_SUCCESS) {
            put(id, (uint64_t) as, K_ACCEL);
            find(id)->aux = bid; // 置き場の buffer（使うメモリの帳簿に使う）
            VkAccelerationStructureDeviceAddressInfoKHR ai = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = as };
            addr = vkGetAccelerationStructureDeviceAddressKHR(d, &ai);
        } else vlog(JE("acceleration structure を作れない (%d)", "Cannot create the acceleration structure (%d)"), res);
        vr_u64(&g_out, addr);
        return reply(s, res);
    }
    case VR_DESTROY_ACCEL: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r);
        VkAccelerationStructureKHR as = OBJ(VkAccelerationStructureKHR, id);
        if (d && as && vkDestroyAccelerationStructureKHR) vkDestroyAccelerationStructureKHR(d, as, NULL);
        del(id);
        return 1;
    }
    case VR_GET_ACCEL_SIZES: {
        VkDevice d = DEV(vr_ru64(r));
        VkAccelerationStructureBuildTypeKHR bt = (VkAccelerationStructureBuildTypeKHR) vr_ru32(r);
        VkAccelerationStructureBuildGeometryInfoKHR bi = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bi.type = (VkAccelerationStructureTypeKHR) vr_ru32(r);
        bi.flags = vr_ru32(r);
        bi.mode = (VkBuildAccelerationStructureModeKHR) vr_ru32(r);
        uint32_t ng = 0;
        VkAccelerationStructureGeometryKHR *g = rd_geoms(r, &ng);
        uint32_t *mx = calloc(ng ? ng : 1, sizeof *mx);
        for (uint32_t i = 0; i < ng && mx && !r->bad; i++) mx[i] = vr_ru32(r);
        bi.geometryCount = ng;
        bi.pGeometries = g;
        VkAccelerationStructureBuildSizesInfoKHR sz = { .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        int ok = d && g && mx && !r->bad && (g_caps & VR_CAP_RT);
        if (ok) vkGetAccelerationStructureBuildSizesKHR(d, bt, &bi, mx, &sz);
        free(g);
        free(mx);
        vr_u64(&g_out, sz.accelerationStructureSize);
        vr_u64(&g_out, sz.updateScratchSize);
        vr_u64(&g_out, sz.buildScratchSize);
        return reply(s, ok ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED);
    }
    case VR_CREATE_BUFFER_VIEW: {
        VkDevice d = DEV(vr_ru64(r));
        uint64_t id = vr_ru64(r), bid = vr_ru64(r);
        VkBufferViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = OBJ(VkBuffer, bid) };
        ci.format = (VkFormat) vr_ru32(r);
        ci.offset = vr_ru64(r);
        ci.range = vr_ru64(r);
        // iPhone の GPU は texel buffer の見え方の始まりを 64 バイトに揃える必要がある（MoltenVK の答え）。ふりでは記録に出すだけ
        if (like_ios() && ci.offset % 64) vlog(JE("iPhone のふり：64 バイトに揃っていない texel buffer の見え方（offset %llu）。iPhone では作れないかもしれない", "Pretending to be an iPhone: a texel buffer view not aligned to 64 bytes (offset %llu). An iPhone may not be able to create it"), (unsigned long long) ci.offset);
        VkBufferView v;
        VkResult res = r->bad ? VK_ERROR_INITIALIZATION_FAILED : vkCreateBufferView(d, &ci, NULL, &v);
        if (res == VK_SUCCESS) {
            put(id, (uint64_t) v, K_BVIEW);
            ent *ve = find(id);
            if (ve) ve->aux = bid; // descriptor に入ったとき、もとの buffer を使っていると分かるように
        } else {
            vlog(JE("buffer view を作れない (%d)", "Cannot create the buffer view (%d)"), res);
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
    vlog(JE("知らない命令 %u", "Unknown command %u"), op);
    return 0;
}

static void serve_client(vr_sock c) {
    vr_nodelay(c);
    // 返事の尾と押し出しは、つなぎごとに頼まれてから
    g_rx_ext = 0;
    g_reply_seq = 0;
    g_epoch = 0;
    g_watch.on = 0;
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
            if (raw > (512u << 20)) { vlog(JE("縮めた命令が大きすぎる (%u)", "A compressed command is too large (%u)"), raw); break; }
            if (raw > pcap) { pcap = raw; pk = realloc(pk, pcap); }
            int got = LZ4_decompress_safe((const char *) buf + 4, (char *) pk, (int) h[1] - 4, (int) raw);
            if (got != (int) raw) { vlog(JE("縮めた命令をほどけない (%d)", "Cannot decode a compressed command (%d)"), got); break; }
            size_t pos = 0;
            int ok = 1;
            while (ok && pos + 8 <= raw) {
                uint32_t ih[2];
                memcpy(ih, pk + pos, 8);
                pos += 8;
                if (ih[1] > raw - pos) { ok = 0; break; }
                vr_rd r2 = { pk + pos, ih[1], 0, 0 };
                double t0 = t_ms(), w0 = g_st.wait_ms;
                if (!handle(c, ih[0], &r2)) ok = 0;
                if (ih[0] < 128) { g_opt.n[ih[0]]++; g_opt.ms[ih[0]] += t_ms() - t0; g_opt.wait[ih[0]] += g_st.wait_ms - w0; }
                pos += ih[1];
            }
            if (!ok) break;
            continue;
        }
        vr_rd r = { buf, h[1], 0, 0 };
        double t0 = t_ms(), w0 = g_st.wait_ms;
        int hk = handle(c, h[0], &r);
        if (h[0] < 128) { g_opt.n[h[0]]++; g_opt.ms[h[0]] += t_ms() - t0; g_opt.wait[h[0]] += g_st.wait_ms - w0; }
        if (!hk) break;
    }
    free(buf);
    free(pk);
    if (g_st.submits)
        vlog(JE("この接続：submit %llu 回 %.0f ms・記録 %llu 回 %.0f ms・GPU を待った %llu 回 %.0f ms・後ろで止めた %.0f ms", "This connection: submit %llu times %.0f ms, recording %llu times %.0f ms, GPU wait %llu times %.0f ms, blocked in the background %.0f ms"),
             (unsigned long long) g_st.submits, g_st.submit_ms, (unsigned long long) g_st.records, g_st.record_ms,
             (unsigned long long) g_st.waits, g_st.wait_ms, g_st.paused_ms);
    if (g_pcs.n)
        vlog(JE("この接続：縮めた平面 %llu 回・%.1f MB を %.1f MB にほどいた（%.2f 倍）・ほどくのに %.0f ms（1 回 %.2f ms）", "This connection: decoded %llu compressed planes, %.1f MB to %.1f MB (x%.2f), decoding took %.0f ms (%.2f ms each)"), (unsigned long long) g_pcs.n,
             g_pcs.in / 1e6, g_pcs.out / 1e6, g_pcs.in ? (double) g_pcs.out / (double) g_pcs.in : 0.0, g_pcs.ms, g_pcs.ms / (double) g_pcs.n);
    memset(&g_pcs, 0, sizeof g_pcs);
    if (g_bst.memw || g_bst.spans_msgs || g_bst.uds_msgs)
        vlog(JE("この接続：書き込み 1 つずつ %llu 通・まとめて %llu 通（範囲 %llu）・descriptor の書き込み %llu 通（%llu 個・vkUpdateDescriptorSets %llu 回）", "This connection: writes one by one %llu, batched %llu (ranges %llu), descriptor writes %llu (%llu items, vkUpdateDescriptorSets %llu times)"),
             (unsigned long long) g_bst.memw, (unsigned long long) g_bst.spans_msgs, (unsigned long long) g_bst.spans,
             (unsigned long long) g_bst.uds_msgs, (unsigned long long) g_bst.uds_writes, (unsigned long long) g_bst.uds_calls);
    memset(&g_bst, 0, sizeof g_bst);
    {
        // 主の糸が命令の処理にかけた時間（GPU を待った時間を除く）の上位 8 つ。命令の番号は proto.h（13 MEM_WRITE・26 ALLOC_DESCRIPTOR_SETS・28 UPDATE_DESCRIPTOR_SETS・121 MEM_WRITE_SPANS）
        char line[600];
        uint64_t tn = 0;
        double tms = 0, tw = 0;
        for (int i = 0; i < 128; i++) { tn += g_opt.n[i]; tms += g_opt.ms[i]; tw += g_opt.wait[i]; }
        int at = snprintf(line, sizeof line, JE("命令の処理（主の糸・GPU を待った時間を除く）：全部 %llu 通 %.0f ms（待ち %.0f ms）・上位（番号:通:ms）", "Command processing (main thread, excluding GPU wait): total %llu commands %.0f ms (wait %.0f ms), top (index:count:ms)"), (unsigned long long) tn, tms - tw, tw);
        int used[128] = { 0 };
        for (int k = 0; k < 8; k++) {
            int bi = -1;
            for (int i = 0; i < 128; i++)
                if (!used[i] && g_opt.n[i] && (bi < 0 || g_opt.ms[i] - g_opt.wait[i] > g_opt.ms[bi] - g_opt.wait[bi])) bi = i;
            if (bi < 0) break;
            used[bi] = 1;
            at += snprintf(line + at, sizeof line - at, " %d:%llu:%.1f", bi, (unsigned long long) g_opt.n[bi], g_opt.ms[bi] - g_opt.wait[bi]);
            if (at >= (int) sizeof line - 32) break;
        }
        if (tn) vlog("%s", line);
        memset(&g_opt, 0, sizeof g_opt);
    }
    if (g_st.waits) {
        char line[400];
        int at = snprintf(line, sizeof line, JE("GPU を待った命令（命令の番号:回数:ms）", "Commands that waited for the GPU (index:count:ms)"));
        for (int i = 0; i < 128 && at < (int) sizeof line - 24; i++)
            if (g_st.wait_by_op[i]) at += snprintf(line + at, sizeof line - at, " %d:%llu:%.0f", i, (unsigned long long) g_st.wait_by_op[i], g_st.wait_ms_by_op[i]);
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
    vlog(remote ? JE("つながった（インターネット共有の口）", "Connected (internet sharing port)") : JE("つながった", "Connected"));
    g_clients = 1;
    serve_client(c);
    g_clients = 0;
    g_cur_remote = 0;
    rec_on_disconnect(); // 録画中に切れたら書き終える（ファイルは残す）
    pv_clear(); // 次につないだときに、前の接続の絵をアプリの画面に残さない
    if (g_lost) {
#ifdef _WIN32
        // PC の代役（RTX）は、GPU を失うと同じプロセスでは Vulkan を作り直しても装置を作れない（-3 が続く）。
        // プロセスごと終わり、立て直しは外の台本に任せる
        dummy_dirty_check(JE("GPU を失った時", "when the GPU was lost"));
        vlog(JE("GPU を失ったので終わる（代役は立て直しが要る）", "Lost the GPU, so exiting (the receiver must be restarted)"));
        exit(3);
#endif
        // GPU を失ったあとは、同じ Vulkan のままだと次の装置が作れないことがある
        vlog(JE("GPU を失ったので、Vulkan を作り直す", "Lost the GPU, so recreating Vulkan"));
        dbg_stop();
        vkDestroyInstance(g_inst, NULL);
        g_inst = VK_NULL_HANDLE;
        g_lost = 0;
        if (!vk_init()) vlog(JE("Vulkan を作り直せない", "Cannot recreate Vulkan"));
    }
#ifdef _WIN32
    closesocket(c);
#else
    close(c);
#endif
    vlog(JE("切れた", "Disconnected"));
    serve_unlock();
}

// インターネット共有の口で待つ糸。合言葉を確かめてから相手をする
#ifdef _WIN32
static DWORD WINAPI extra_thread(LPVOID p) {
#else
static void *extra_thread(void *p) {
#endif
    qos_hi();
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
            vlog(JE("インターネット共有の口：合言葉が無い・違うので断った", "Internet sharing port: refused because the token is missing or wrong"));
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
            vlog(JE("インターネット共有の口を閉じた", "Closed the internet sharing port"));
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
    int bad_addr = inet_pton(AF_INET, host, &a.sin_addr) != 1;
    if (bad_addr || bind(ls, (struct sockaddr *) &a, sizeof a) != 0 || listen(ls, 4) != 0) {
        char et[160];
        sock_err_text(bad_addr ? SOCK_EINVAL : sock_errno(), et, sizeof et);
        vlog(JE("インターネット共有の口 %s:%d で待てない：%s", "Cannot listen on the internet sharing port %s:%d: %s"), host, g_main_port, et);
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
    vlog(JE("インターネット共有の口：%s:%d でも待つ（合言葉つき）", "Internet sharing port: also listening on %s:%d (token required)"), host, g_main_port);
    return 0;
}

// ---- インターネット共有の網の口を選んで待つ（アプリの設定で入り切り） ----
// 待つのは、名前が bridge で始まり（iOS のインターネット共有は bridge100 など）、IPv4 が 172.20.10.0/28 の口だけ。
// 0.0.0.0 では待たない（Wi-Fi の en0 で、同じネットワークのほかの人からも届いてしまう）。
// この iPhone がほかの iPhone の共有に Wi-Fi でつないでいて en0 が 172.20.10.x でも、名前が bridge でないので待たない。
// bridge の口には、インターネット共有に Wi-Fi でつないだ人も来るので、合言葉は外さない
static uint32_t g_hs_net = 0xAC140A00u, g_hs_mask = 0xFFFFFFF0u; // 172.20.10.0/28（ホストの並び）
static int hs_match(const char *name, uint32_t ip) { return name && !strncmp(name, "bridge", 6) && (ip & g_hs_mask) == g_hs_net; }

#ifdef _WIN32
// PC の代役の試験用：VKR_HOTSPOT_FAKE_FILE に「名前 住所」を 1 行ずつ書くと、それをインターフェースの一覧として読む（呼ばれるたびに読み直す）。
// 「off」で始まる行があれば、アプリの設定を切ったのと同じ。網は VKR_HOTSPOT_NET（main で読む）で 127.0.0.0/8 などに替える
static int hs_pick(char *out, size_t cap, int *off) {
    *off = 0;
    const char *f = getenv("VKR_HOTSPOT_FAKE_FILE");
    FILE *fp = f && *f ? fopen(f, "r") : NULL;
    if (!fp) return 0;
    char line[200];
    int found = 0;
    while (!found && fgets(line, sizeof line, fp)) {
        char name[64], ip[64];
        struct in_addr ia;
        if (!strncmp(line, "off", 3)) { *off = 1; break; }
        if (sscanf(line, "%63s %63s", name, ip) != 2 || inet_pton(AF_INET, ip, &ia) != 1) continue;
        if (!hs_match(name, ntohl(ia.s_addr))) continue;
        snprintf(out, cap, "%s", ip);
        found = 1;
    }
    fclose(fp);
    return found;
}
#else
static int hs_pick(char *out, size_t cap, int *off) {
    struct ifaddrs *head = NULL;
    int found = 0;
    *off = 0;
    if (getifaddrs(&head) != 0) return 0;
    for (struct ifaddrs *p = head; p && !found; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !(p->ifa_flags & IFF_UP)) continue;
        struct in_addr ia = ((struct sockaddr_in *) (void *) p->ifa_addr)->sin_addr;
        if (!hs_match(p->ifa_name, ntohl(ia.s_addr))) continue;
        found = inet_ntop(AF_INET, &ia, out, (socklen_t) cap) != NULL;
    }
    freeifaddrs(head);
    return found;
}
#endif

// アプリから 1 秒ごとに呼ぶ。on が 1 なら共有の網の口を探して待ち（後から出てきても待ち始め、消えたら閉じる）、0 なら閉じる。
// out には、いま待っている住所を書く（待っていなければ空）。Vulkan がまだ起きていなければ 2・待とうとして待てなかったら 1・ほかは 0。
// 待てなかった住所は 5 秒おいてから試し直す（共有を入れた直後で、住所がまだ使えないことがある）
int vkr_hotspot_tick(int on, char *out, int cap) {
    static char failed[64];
    static double retry_at;
    char want[64] = "";
    int off = 0, r = 0;
    if (on && !hs_pick(want, sizeof want, &off)) want[0] = 0;
    if (off) want[0] = 0;
    if (strcmp(want, g_extra_host)) {
        if (!want[0]) {
            vkr_listen_extra("");
            failed[0] = 0;
        } else if (strcmp(want, failed) || t_ms() >= retry_at) {
            r = vkr_listen_extra(want);
            if (r == 1) {
                snprintf(failed, sizeof failed, "%s", want);
                retry_at = t_ms() + 5000;
            } else if (r == 0) {
                failed[0] = 0;
            }
        } else {
            r = 1;
        }
    }
    if (out && cap > 0) snprintf(out, (size_t) cap, "%s", g_extra_host);
    return r;
}

// ---- 待つ口（0.1.4 で足した） ----
// テスターの 1 台（Redmagic 9S Pro）で、Android のアプリの中の受け手だけが 127.0.0.1:50060 で待てず、すぐ終わっていた（端末の再起動でも同じ）。
// どの機種でも起動するように：
//   1. PGPU_FD_SOCK=抽象名：アプリ（Java）が開けた口を、その抽象名の UNIX ソケットから SCM_RIGHTS で受け取る。受け手は bind しない。
//      受け取れないときは 4 で終わる（アプリは次から 2 と 3 の形で起動し直す）
//   2. PGPU_PORT_TRY=50061,50062,…：引数の番号で待てなければ、この番号を順に試す
//   3. PGPU_UNIX=抽象名（Android だけ）：USB 用に抽象名の UNIX ソケットでも待つ。PC は adb forward tcp:50300 localabstract:抽象名。
//      つないでよいのは adb（uid 2000、エミュレーターなどは 0）と自分の uid だけ（同じ端末のほかのアプリは断る。アプリの Link.java と同じ考え）
// TCP と UNIX のどちらか 1 つでも待てれば動く。どれも無いときだけ 1 で終わる。キットの受け手（環境変数なし）は今までと同じ
#ifdef _WIN32
#define vr_close_sock(s) closesocket(s)
#else
#define vr_close_sock(s) close(s)
#endif

// host:port で待つ口を開ける。だめなら理由を記録に書いて VR_BAD_SOCK
static vr_sock tcp_listen(const char *host, int port) {
    char et[160];
    vr_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == VR_BAD_SOCK) {
        sock_err_text(sock_errno(), et, sizeof et);
        vlog(JE("%s:%d で待てない：%s（socket）", "Cannot listen on %s:%d: %s (socket)"), host, port, et);
        return VR_BAD_SOCK;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof one);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        vlog(JE("%s:%d で待てない：アドレスが読めない", "Cannot listen on %s:%d: the address cannot be read"), host, port);
        vr_close_sock(ls);
        return VR_BAD_SOCK;
    }
    if (bind(ls, (struct sockaddr *) &a, sizeof a) != 0 || listen(ls, 4) != 0) {
        sock_err_text(sock_errno(), et, sizeof et);
        vlog(JE("%s:%d で待てない：%s", "Cannot listen on %s:%d: %s"), host, port, et);
        vr_close_sock(ls);
        return VR_BAD_SOCK;
    }
    return ls;
}

#ifdef __ANDROID__
#include <sys/un.h>
static int g_given_tcp = -1, g_given_unix = -1; // アプリから受け取った口（-1 は無し）
static int g_given_port;                        // 受け取った TCP の口の番号
static char g_unix_name[100];                   // UNIX の口の抽象名（記録用）

// 抽象名の住所。先頭の 0 が抽象名のしるし。長さは Java の LocalServerSocket と adb の localabstract に合わせる
static int unix_addr(const char *name, struct sockaddr_un *u, socklen_t *len) {
    size_t n = strlen(name);
    if (!n || n + 1 > sizeof u->sun_path) return 0;
    memset(u, 0, sizeof *u);
    u->sun_family = AF_UNIX;
    memcpy(u->sun_path + 1, name, n);
    *len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + n);
    return 1;
}

static int unix_listen(const char *name) {
    struct sockaddr_un u;
    socklen_t ul;
    char et[160];
    if (!unix_addr(name, &u, &ul)) {
        vlog(JE("USB の口 localabstract:%s で待てない：名前が長すぎる", "Cannot listen on the USB socket localabstract:%s: the name is too long"), name);
        return -1;
    }
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0 || bind(s, (struct sockaddr *) &u, ul) != 0 || listen(s, 4) != 0) {
        sock_err_text(errno, et, sizeof et);
        vlog(JE("USB の口 localabstract:%s で待てない：%s", "Cannot listen on the USB socket localabstract:%s: %s"), name, et);
        if (s >= 0) close(s);
        return -1;
    }
    snprintf(g_unix_name, sizeof g_unix_name, "%s", name);
    return s;
}

// fd が待っている口（listen 済み）で、種類が dom なら 1
static int is_listening_fd(int fd, int dom) {
    int v = 0;
    socklen_t l = sizeof v;
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &v, &l) != 0 || !v) return 0;
    v = -1;
    l = sizeof v;
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &v, &l) != 0) return 0;
    return v == dom;
}

// アプリから口を受け取る。アプリは「PGPU-FDS tcp=番号 unix=名前\n」（無い方は tcp=0・unix=-）と、
// 同じ順の fd（TCP・UNIX）を 1 回の書き込みで送ってくる。受け取ったら「OK\n」を返す。1 つでも受け取れたら 1
static int recv_given(const char *name) {
    struct sockaddr_un u;
    socklen_t ul;
    char et[160];
    if (!unix_addr(name, &u, &ul)) return 0;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        sock_err_text(errno, et, sizeof et);
        vlog(JE("アプリから口を受け取れない：%s（socket）", "Cannot receive the sockets from the app: %s (socket)"), et);
        return 0;
    }
    struct timeval to = { 5, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    if (connect(s, (struct sockaddr *) &u, ul) != 0) {
        sock_err_text(errno, et, sizeof et);
        vlog(JE("アプリから口を受け取れない：%s（connect）", "Cannot receive the sockets from the app: %s (connect)"), et);
        close(s);
        return 0;
    }
    char msg[256];
    size_t got = 0;
    int fds[4], nfd = 0;
    while (got < sizeof msg - 1 && !memchr(msg, '\n', got)) {
        union { char b[CMSG_SPACE(sizeof(int) * 4)]; struct cmsghdr align; } cb;
        struct iovec iov = { msg + got, sizeof msg - 1 - got };
        struct msghdr mh;
        memset(&mh, 0, sizeof mh);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cb.b;
        mh.msg_controllen = sizeof cb.b;
        ssize_t k = recvmsg(s, &mh, MSG_CMSG_CLOEXEC);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            int n = (int) ((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < n; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof fd);
                if (nfd < 4) fds[nfd++] = fd;
                else close(fd);
            }
        }
        got += (size_t) k;
    }
    msg[got] = 0;
    int tport = 0, at = 0;
    char uname[100] = "-";
    const char *p = strstr(msg, "tcp=");
    if (p) tport = atoi(p + 4);
    p = strstr(msg, "unix=");
    if (p) sscanf(p + 5, "%99[^ \r\n]", uname);
    if (strncmp(msg, "PGPU-FDS ", 9) != 0) {
        vlog(JE("アプリから口を受け取れない：知らない形（%.40s）", "Cannot receive the sockets from the app: unknown message (%.40s)"), msg);
        for (int i = 0; i < nfd; i++) close(fds[i]);
        close(s);
        return 0;
    }
    if (tport > 0 && at < nfd) {
        int fd = fds[at++];
        if (is_listening_fd(fd, AF_INET) || is_listening_fd(fd, AF_INET6)) {
            g_given_tcp = fd;
            g_given_port = tport;
        } else {
            close(fd);
        }
    }
    if (strcmp(uname, "-") && at < nfd) {
        int fd = fds[at++];
        if (is_listening_fd(fd, AF_UNIX)) {
            g_given_unix = fd;
            snprintf(g_unix_name, sizeof g_unix_name, "%s", uname);
        } else {
            close(fd);
        }
    }
    while (at < nfd) close(fds[at++]);
    int ok = g_given_tcp >= 0 || g_given_unix >= 0;
    if (ok) {
        static const char reply[] = "OK\n";
        send(s, reply, sizeof reply - 1, MSG_NOSIGNAL);
    } else {
        vlog(JE("アプリから口を受け取れない：届いた fd が待っている口ではない（%d 個）", "Cannot receive the sockets from the app: the received fds are not listening sockets (%d)"), nfd);
    }
    close(s);
    return ok;
}

// UNIX の口で待つ。adb（2000・0）と自分の uid だけ相手をする
static void unix_accept_loop(int ls) {
    for (;;) {
        int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
        if (c < 0) {
            if (errno != EINTR) sleep_ms(100);
            continue;
        }
        struct ucred cr;
        socklen_t l = sizeof cr;
        int uid = getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &l) == 0 ? (int) cr.uid : -1;
        if (uid != 2000 && uid != 0 && uid != (int) getuid()) {
            vlog(JE("USB の口：adb ではない接続（uid %d）を断った。この端末のほかのアプリが接続しようとした", "USB socket: refused a connection that is not from adb (uid %d). Another app on this phone tried to connect"), uid);
            close(c);
            continue;
        }
        serve_one(c, 0);
    }
}

static void *unix_thread(void *p) {
    qos_hi();
    unix_accept_loop((int) (intptr_t) p);
    return 0;
}
#endif

int vkr_serve(const char *host, int port) {
    qos_hi();
#ifdef __ANDROID__
    if (g_given_tcp >= 0) port = g_given_port;
#endif
    g_main_port = port; // 共有の網の口も同じ番号で待つ。Vulkan が起きるとすぐ vkr_hotspot_tick が開けるので、先に決める
    if (!vk_init()) { vlog(JE("Vulkan を起動できない", "Cannot start Vulkan")); return 1; }
    if (g_extra_env) vkr_listen_extra(g_extra_env);
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    int one = 1;
    vr_sock ls = VR_BAD_SOCK;
    int uls = -1, given = 0;
#ifdef __ANDROID__
    if (g_given_tcp >= 0 || g_given_unix >= 0) {
        // アプリが開けた口を使う
        given = 1;
        ls = g_given_tcp >= 0 ? g_given_tcp : VR_BAD_SOCK;
        uls = g_given_unix;
    }
#endif
    if (!given) {
        ls = tcp_listen(host, port);
        const char *tr = getenv("PGPU_PORT_TRY");
        for (const char *p = tr; ls == VR_BAD_SOCK && p && *p;) {
            char *e;
            long v = strtol(p, &e, 10);
            if (e == p) { p++; continue; }
            p = e;
            if (v <= 0 || v > 65535 || v == port) continue;
            ls = tcp_listen(host, (int) v);
            if (ls != VR_BAD_SOCK) {
                vlog(JE("%d で待てないので、%ld で待つ", "Cannot listen on %d, so using %ld instead"), port, v);
                port = (int) v;
                g_main_port = port;
            }
        }
#ifdef __ANDROID__
        const char *un = getenv("PGPU_UNIX");
        if (un && *un) uls = unix_listen(un);
#endif
    }
    if (ls == VR_BAD_SOCK && uls < 0) return 1;
    if (ls != VR_BAD_SOCK)
        vlog(given ? JE("Vulkan の受け手：%s:%d で待っている（口はアプリが開けた）", "Vulkan receiver: listening on %s:%d (socket opened by the app)")
                   : JE("Vulkan の受け手：%s:%d で待っている", "Vulkan receiver: listening on %s:%d"),
             host, port);
#ifdef __ANDROID__
    if (uls >= 0) {
        if (ls == VR_BAD_SOCK) vlog(JE("Vulkan の受け手：USB（localabstract:%s）だけで待っている", "Vulkan receiver: listening on USB only (localabstract:%s)"), g_unix_name);
        else vlog(JE("Vulkan の受け手：USB（localabstract:%s）でも待っている", "Vulkan receiver: also listening on USB (localabstract:%s)"), g_unix_name);
        if (ls == VR_BAD_SOCK) {
            unix_accept_loop(uls); // 戻ってこない
            return 1;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, unix_thread, (void *) (intptr_t) uls) == 0) pthread_detach(t);
        else vlog(JE("USB の口の糸を作れない", "Cannot start the USB socket thread"));
    }
#endif
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
    fprintf(stderr, JE("落ちた：例外 0x%08lx・命令 %u\n", "Crashed: exception 0x%08lx, command %u\n"), code, g_cur_op);
    where(JE("場所", "location"), ep->ExceptionRecord->ExceptionAddress);
    void *fr[40];
    USHORT n = RtlCaptureStackBackTrace(0, 40, fr, NULL);
    for (USHORT i = 0; i < n; i++) where(JE("  呼び出し元", "  caller"), fr[i]);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// 試験用：VKR_FAKE_UPSCALE=1 で、PC の代役でも「広げる」を名乗る（CPU で最近傍に広げるだけ。iPhone の MetalFX の代わり）。
// ドライバの IPHONE_VK_UPSCALE と、返事の大きさの約束を、iPhone の版を作る前に確かめるため
static int fake_upscale(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride, uint8_t *dst, uint32_t dw, uint32_t dh, uint32_t dstride) {
    for (uint32_t y = 0; y < dh; y++) {
        const uint32_t *sr = (const uint32_t *) (src + (size_t) (y * h / dh) * stride);
        uint32_t *dr = (uint32_t *) (dst + (size_t) y * dstride);
        for (uint32_t x = 0; x < dw; x++) dr[x] = sr[x * w / dw];
    }
    return 1;
}

// 試験用（VKR_HOTSPOT=1）：アプリの 1 秒ごとの見直しの真似。待つ口が変わったら記録に出す
#ifdef _WIN32
static DWORD WINAPI hs_ticker(LPVOID p) {
#else
static void *hs_ticker(void *p) {
#endif
    (void) p;
    char now[64] = "", last[64] = "";
    for (;;) {
        int r = vkr_hotspot_tick(1, now, sizeof now);
        if (r != 2 && strcmp(now, last)) {
            vlog(JE("試験用：共有の網の口 %s", "Test: internet sharing network port %s"), now[0] ? now : JE("なし", "none"));
            snprintf(last, sizeof last, "%s", now);
        }
        sleep_ms(1000);
    }
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    AddVectoredExceptionHandler(1, crash_handler);
    { const char *fu = getenv("VKR_FAKE_UPSCALE"); if (fu && *fu == '1') { vkr_set_upscaler(fake_upscale); vlog(JE("試験用：CPU で広げる（VKR_FAKE_UPSCALE）", "Test: upscaling on the CPU (VKR_FAKE_UPSCALE)")); } }
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
        if (!g_token_ok) vlog(JE("VKR_LINK_TOKEN が無い・読めないので、インターネット共有の口は全部断る", "VKR_LINK_TOKEN is missing or unreadable, so all internet sharing connections are refused"));
#ifdef _WIN32
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
#endif
        g_extra_env = ex; // Vulkan が起きてから開ける（vkr_serve の中）
    }
    // 調べ用：VKR_HOTSPOT=1 でアプリと同じく 1 秒ごとに vkr_hotspot_tick を呼ぶ（インターフェースは VKR_HOTSPOT_FAKE_FILE・網は VKR_HOTSPOT_NET=127.0.0.0/8 など）
    const char *hs = getenv("VKR_HOTSPOT");
    if (hs && *hs == '1') {
        const char *nt = getenv("VKR_HOTSPOT_NET");
        if (nt && *nt) {
            char b[64];
            snprintf(b, sizeof b, "%s", nt);
            char *sl = strchr(b, '/');
            int bits = sl ? atoi(sl + 1) : 32;
            if (sl) *sl = 0;
            struct in_addr ia;
            if (inet_pton(AF_INET, b, &ia) == 1 && bits >= 1 && bits <= 32) {
                g_hs_mask = bits == 32 ? 0xffffffffu : ~(0xffffffffu >> bits);
                g_hs_net = ntohl(ia.s_addr) & g_hs_mask;
            }
        }
        vkr_set_token(getenv("VKR_LINK_TOKEN"));
        if (!g_token_ok) vlog(JE("VKR_LINK_TOKEN が無い・読めないので、インターネット共有の口は全部断る", "VKR_LINK_TOKEN is missing or unreadable, so all internet sharing connections are refused"));
        g_main_port = argc > 2 ? atoi(argv[2]) : VR_DEFAULT_PORT;
#ifdef _WIN32
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        HANDLE t = CreateThread(NULL, 0, hs_ticker, NULL, 0, NULL);
        if (t) CloseHandle(t);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, hs_ticker, NULL) == 0) pthread_detach(t);
#endif
        vlog(JE("試験用：1 秒ごとに共有の網の口を探す（VKR_HOTSPOT）", "Test: looking for the sharing network port every second (VKR_HOTSPOT)"));
    }
#ifdef __ANDROID__
    // アプリが開けた口を受け取る（0.1.4）。Vulkan より先に（アプリは数秒しか待たない）
    const char *fds = getenv("PGPU_FD_SOCK");
    if (fds && *fds && !recv_given(fds)) return 4;
#endif
    return vkr_serve(argc > 1 ? argv[1] : "127.0.0.1", argc > 2 ? atoi(argv[2]) : VR_DEFAULT_PORT);
}
#endif
