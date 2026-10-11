// Linux の窓（X11）に swapchain で三角形を出す試験。窓を作り、何コマか出したあと、X から窓の中身を読んで確かめる。
//   wsitri [装置の名前の一部] [コマ数] [xcb|xlib] [見本の PPM]
// 1. 256 x 256 の窓に、tri と同じ三角形をコマ数だけ出す
// 2. 窓を 320 x 200 に変え、swapchain の作り直し（SUBOPTIMAL か OUT_OF_DATE）を待って、作り直してから 30 コマ出す
// それぞれのあとで窓の中身を X から読み、「色のついた画素の数」と「隅の色」を出す。見本の PPM（tri の 256 x 256）があれば、1 の絵と画素ごとに比べる。
// X と Vulkan のローダーは dlopen で読む（開発用のヘッダは Vulkan だけ）。$DISPLAY の X に窓を出す（試験では Xvfb の見えない画面）
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
typedef struct xcb_connection_t xcb_connection_t;
typedef uint32_t xcb_window_t;
typedef uint32_t xcb_visualid_t;
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef unsigned long VisualID;
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_xlib.h>
#include "tri_vert_spv.h"
#include "tri_frag_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("失敗 %s = %d（%d 行目）\n", #x, r_, __LINE__); return 1; } } while (0)

// ---- xcb（使う物だけ） ----
typedef struct { unsigned int sequence; } ck;
typedef struct {
    xcb_window_t root; uint32_t default_colormap, white_pixel, black_pixel, current_input_masks;
    uint16_t width_in_pixels, height_in_pixels, width_in_millimeters, height_in_millimeters, min_installed_maps, max_installed_maps;
    xcb_visualid_t root_visual; uint8_t backing_stores, save_unders, root_depth, allowed_depths_len;
} xscreen;
typedef struct { xscreen *data; int rem, index; } xscreen_it;
typedef struct { uint8_t response_type, depth; uint16_t sequence; uint32_t length; xcb_visualid_t visual; uint8_t pad0[20]; } ximage_reply;
static struct {
    xcb_connection_t *(*connect)(const char *, int *);
    const void *(*get_setup)(xcb_connection_t *);
    xscreen_it (*roots)(const void *);
    uint32_t (*generate_id)(xcb_connection_t *);
    ck (*create_window)(xcb_connection_t *, uint8_t, uint32_t, uint32_t, int16_t, int16_t, uint16_t, uint16_t, uint16_t, uint16_t, uint32_t, uint32_t, const void *);
    ck (*map_window)(xcb_connection_t *, uint32_t);
    ck (*configure_window)(xcb_connection_t *, uint32_t, uint16_t, const void *);
    ck (*destroy_window)(xcb_connection_t *, uint32_t);
    int (*flush)(xcb_connection_t *);
    ck (*get_image)(xcb_connection_t *, uint8_t, uint32_t, int16_t, int16_t, uint16_t, uint16_t, uint32_t);
    ximage_reply *(*get_image_reply)(xcb_connection_t *, ck, void **);
    uint8_t *(*get_image_data)(const ximage_reply *);
    int (*get_image_data_length)(const ximage_reply *);
    void (*disconnect)(xcb_connection_t *);
} X;
// Xlib（xlib の道を試すとき）
static struct {
    Display *(*open)(const char *);
    Window (*root)(Display *);
    Window (*create)(Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long);
    int (*map)(Display *, Window);
    int (*sync)(Display *, int);
    int (*destroy)(Display *, Window);
    int (*close)(Display *);
} XL;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void nap(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// 窓の中身を X から読み、R・G・B にして返す（呼んだ側が free）
static uint8_t *grab(xcb_connection_t *c, uint32_t win, uint32_t w, uint32_t h) {
    void *err = NULL;
    ximage_reply *r = X.get_image_reply(c, X.get_image(c, 2, win, 0, 0, (uint16_t) w, (uint16_t) h, 0xffffffffu), &err);
    if (!r) { printf("窓の中身を読めない\n"); free(err); return NULL; }
    const uint8_t *d = X.get_image_data(r);
    int n = X.get_image_data_length(r);
    uint8_t *rgb = NULL;
    if (n >= (int) (w * h * 4)) {
        rgb = malloc((size_t) w * h * 3);
        for (size_t i = 0; i < (size_t) w * h; i++) { rgb[i * 3] = d[i * 4 + 2]; rgb[i * 3 + 1] = d[i * 4 + 1]; rgb[i * 3 + 2] = d[i * 4]; }
    } else {
        printf("窓の中身の長さが足りない（%d バイト・深さ %u）\n", n, r->depth);
    }
    free(r);
    return rgb;
}

static uint32_t count_colored(const uint8_t *rgb, uint32_t w, uint32_t h) {
    uint32_t n = 0;
    for (size_t i = 0; i < (size_t) w * h; i++) if (rgb[i * 3] > 40 || rgb[i * 3 + 1] > 40 || rgb[i * 3 + 2] > 40) n++;
    return n;
}

static void write_ppm(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    fwrite(rgb, 1, (size_t) w * h * 3, f);
    fclose(f);
}

static uint8_t *read_ppm(const char *path, uint32_t *w, uint32_t *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    int mx;
    if (fscanf(f, "P6 %u %u %d", w, h, &mx) != 3) { fclose(f); return NULL; }
    fgetc(f);
    uint8_t *p = malloc((size_t) *w * *h * 3);
    if (p && fread(p, 1, (size_t) *w * *h * 3, f) != (size_t) *w * *h * 3) { free(p); p = NULL; }
    fclose(f);
    return p;
}

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define IFN(name) PFN_##name name = (PFN_##name) gipa(inst, #name)
#define DFN(name) name = (PFN_##name) gdpa(dev, #name)
static PFN_vkGetDeviceProcAddr gdpa;
static PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
static PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
static PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
static PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
static PFN_vkQueuePresentKHR vkQueuePresentKHR;
static PFN_vkCreateImageView vkCreateImageView;
static PFN_vkDestroyImageView vkDestroyImageView;
static PFN_vkCreateFramebuffer vkCreateFramebuffer;
static PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
static PFN_vkCreateRenderPass vkCreateRenderPass;
static PFN_vkCreateShaderModule vkCreateShaderModule;
static PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
static PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
static PFN_vkDestroyPipeline vkDestroyPipeline;
static PFN_vkCreateCommandPool vkCreateCommandPool;
static PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
static PFN_vkResetCommandBuffer vkResetCommandBuffer;
static PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer vkEndCommandBuffer;
static PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
static PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
static PFN_vkCmdBindPipeline vkCmdBindPipeline;
static PFN_vkCmdDraw vkCmdDraw;
static PFN_vkCmdSetViewport vkCmdSetViewport;
static PFN_vkCmdSetScissor vkCmdSetScissor;
static PFN_vkCreateFence vkCreateFence;
static PFN_vkWaitForFences vkWaitForFences;
static PFN_vkResetFences vkResetFences;
static PFN_vkCreateSemaphore vkCreateSemaphore;
static PFN_vkQueueSubmit vkQueueSubmit;
static PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
static PFN_vkGetDeviceQueue vkGetDeviceQueue;

// swapchain とそれに付く物
static struct {
    VkSwapchainKHR sc;
    uint32_t n, w, h;
    VkImage img[8];
    VkImageView view[8];
    VkFramebuffer fb[8];
} S;

static VkResult make_swapchain(VkPhysicalDevice pd, VkSurfaceKHR surf, VkFormat fmt, VkRenderPass rp, VkPresentModeKHR mode) {
    IFN(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
    if (r != VK_SUCCESS) return r;
    VkSwapchainKHR old = S.sc;
    for (uint32_t i = 0; i < S.n; i++) { vkDestroyFramebuffer(dev, S.fb[i], NULL); vkDestroyImageView(dev, S.view[i], NULL); }
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = 3, .imageFormat = fmt,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = caps.currentExtent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = mode, .clipped = VK_TRUE, .oldSwapchain = old };
    r = vkCreateSwapchainKHR(dev, &ci, NULL, &S.sc);
    if (old) vkDestroySwapchainKHR(dev, old, NULL);
    if (r != VK_SUCCESS) return r;
    S.w = caps.currentExtent.width, S.h = caps.currentExtent.height;
    S.n = 8;
    vkGetSwapchainImagesKHR(dev, S.sc, &S.n, S.img);
    for (uint32_t i = 0; i < S.n; i++) {
        VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = S.img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if ((r = vkCreateImageView(dev, &vc, NULL, &S.view[i])) != VK_SUCCESS) return r;
        VkFramebufferCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &S.view[i],
            .width = S.w, .height = S.h, .layers = 1 };
        if ((r = vkCreateFramebuffer(dev, &fc, NULL, &S.fb[i])) != VK_SUCCESS) return r;
    }
    printf("swapchain：%u x %u・%u 枚\n", S.w, S.h, S.n);
    return VK_SUCCESS;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *want = argc > 1 ? argv[1] : "iPhone";
    int frames = argc > 2 ? atoi(argv[2]) : 60;
    int use_xlib = argc > 3 && !strcmp(argv[3], "xlib");
    const char *ref_path = argc > 4 ? argv[4] : NULL;

    void *xh = dlopen("libxcb.so.1", RTLD_NOW);
    if (!xh) { printf("libxcb.so.1 が無い\n"); return 1; }
#define XS(f, n) (*(void **) &X.f = dlsym(xh, n))
    XS(connect, "xcb_connect"); XS(get_setup, "xcb_get_setup"); XS(roots, "xcb_setup_roots_iterator"); XS(generate_id, "xcb_generate_id");
    XS(create_window, "xcb_create_window"); XS(map_window, "xcb_map_window"); XS(configure_window, "xcb_configure_window");
    XS(destroy_window, "xcb_destroy_window"); XS(flush, "xcb_flush"); XS(get_image, "xcb_get_image"); XS(get_image_reply, "xcb_get_image_reply");
    XS(get_image_data, "xcb_get_image_data"); XS(get_image_data_length, "xcb_get_image_data_length"); XS(disconnect, "xcb_disconnect");
#undef XS
    xcb_connection_t *c = X.connect(NULL, NULL);
    xscreen *scr = X.roots(X.get_setup(c)).data;
    printf("X の画面：%u x %u・深さ %u\n", scr->width_in_pixels, scr->height_in_pixels, scr->root_depth);
    uint32_t win;
    Display *dpy = NULL;
    if (use_xlib) {
        void *xl = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
        if (!xl) { printf("libX11.so.6 が無い\n"); return 1; }
        *(void **) &XL.open = dlsym(xl, "XOpenDisplay"); *(void **) &XL.root = dlsym(xl, "XDefaultRootWindow");
        *(void **) &XL.create = dlsym(xl, "XCreateSimpleWindow"); *(void **) &XL.map = dlsym(xl, "XMapWindow");
        *(void **) &XL.sync = dlsym(xl, "XSync"); *(void **) &XL.destroy = dlsym(xl, "XDestroyWindow"); *(void **) &XL.close = dlsym(xl, "XCloseDisplay");
        dpy = XL.open(NULL);
        if (!dpy) { printf("XOpenDisplay できない\n"); return 1; }
        win = (uint32_t) XL.create(dpy, XL.root(dpy), 0, 0, 256, 256, 0, 0, 0);
        XL.map(dpy, win);
        XL.sync(dpy, 0);
        printf("窓（Xlib）0x%x\n", win);
    } else {
        win = X.generate_id(c);
        uint32_t vals[1] = { 0 }; // 背景は黒
        X.create_window(c, 0, win, scr->root, 0, 0, 256, 256, 0, 1, scr->root_visual, 2 /* BACK_PIXEL */, vals);
        X.map_window(c, win);
        X.flush(c);
        printf("窓（xcb）0x%x\n", win);
    }

    void *vk = dlopen("libvulkan.so.1", RTLD_NOW);
    gipa = (PFN_vkGetInstanceProcAddr) dlsym(vk, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    const char *iext[] = { "VK_KHR_surface", use_xlib ? "VK_KHR_xlib_surface" : "VK_KHR_xcb_surface" };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "wsitri", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkCreateDevice); IFN(vkGetPhysicalDeviceSurfaceSupportKHR); IFN(vkGetPhysicalDeviceSurfaceFormatsKHR);
    IFN(vkGetPhysicalDeviceSurfacePresentModesKHR); IFN(vkCreateXcbSurfaceKHR); IFN(vkCreateXlibSurfaceKHR); IFN(vkDestroySurfaceKHR);
    IFN(vkGetPhysicalDeviceXcbPresentationSupportKHR); IFN(vkGetPhysicalDeviceXlibPresentationSupportKHR); IFN(vkDestroyInstance);
    gdpa = (PFN_vkGetDeviceProcAddr) gipa(inst, "vkGetDeviceProcAddr");
    uint32_t nd = 16;
    VkPhysicalDevice pds[16];
    CHECK(vkEnumeratePhysicalDevices(inst, &nd, pds));
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props;
    for (uint32_t i = 0; i < nd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (strstr(props.deviceName, want)) { pd = pds[i]; break; }
    }
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("装置: %s\n", props.deviceName);
    VkSurfaceKHR surf;
    if (use_xlib) {
        VkXlibSurfaceCreateInfoKHR sc = { .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR, .dpy = dpy, .window = win };
        CHECK(vkCreateXlibSurfaceKHR(inst, &sc, NULL, &surf));
    } else {
        VkXcbSurfaceCreateInfoKHR sc = { .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, .connection = c, .window = win };
        CHECK(vkCreateXcbSurfaceKHR(inst, &sc, NULL, &surf));
    }
    uint32_t nq = 8, qf = UINT32_MAX;
    VkQueueFamilyProperties qps[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qps);
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 ok = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &ok);
        if ((qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) { qf = i; break; }
    }
    if (qf == UINT32_MAX) { printf("窓に出せるキューが無い\n"); return 1; }
    VkBool32 ps = use_xlib ? vkGetPhysicalDeviceXlibPresentationSupportKHR(pd, qf, dpy, 0) : vkGetPhysicalDeviceXcbPresentationSupportKHR(pd, qf, c, scr->root_visual);
    printf("present の支え：%s\n", ps ? "ある" : "無い");
    uint32_t nf = 8;
    VkSurfaceFormatKHR sf[8];
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, sf));
    uint32_t nm = 8;
    VkPresentModeKHR pm[8];
    CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, pm));
    printf("窓の形式 %u 個（最初 %d）・出し方 %u 個\n", nf, sf[0].format, nm);
    VkFormat fmt = sf[0].format;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    DFN(vkCreateSwapchainKHR); DFN(vkDestroySwapchainKHR); DFN(vkGetSwapchainImagesKHR); DFN(vkAcquireNextImageKHR); DFN(vkQueuePresentKHR);
    DFN(vkCreateImageView); DFN(vkDestroyImageView); DFN(vkCreateFramebuffer); DFN(vkDestroyFramebuffer); DFN(vkCreateRenderPass);
    DFN(vkCreateShaderModule); DFN(vkCreatePipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkDestroyPipeline); DFN(vkCreateCommandPool);
    DFN(vkAllocateCommandBuffers); DFN(vkResetCommandBuffer); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer); DFN(vkCmdBeginRenderPass);
    DFN(vkCmdEndRenderPass); DFN(vkCmdBindPipeline); DFN(vkCmdDraw); DFN(vkCmdSetViewport); DFN(vkCmdSetScissor); DFN(vkCreateFence);
    DFN(vkWaitForFences); DFN(vkResetFences); DFN(vkCreateSemaphore); DFN(vkQueueSubmit); DFN(vkDeviceWaitIdle); DFN(vkGetDeviceQueue);
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);

    VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkShaderModuleCreateInfo vs = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof tri_vert_spv, .pCode = tri_vert_spv };
    VkShaderModuleCreateInfo fs = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof tri_frag_spv, .pCode = tri_frag_spv };
    VkShaderModule vsm, fsm;
    CHECK(vkCreateShaderModule(dev, &vs, NULL, &vsm));
    CHECK(vkCreateShaderModule(dev, &fs, NULL, &fsm));
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = pl, .renderPass = rp, .subpass = 0 };
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf };
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBuffer cmd[2];
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2 };
    CHECK(vkAllocateCommandBuffers(dev, &cbai, cmd));
    VkFence fence[2];
    VkSemaphore got[2], done[2];
    for (int i = 0; i < 2; i++) {
        VkFenceCreateInfo fc = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        VkSemaphoreCreateInfo sc = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        CHECK(vkCreateFence(dev, &fc, NULL, &fence[i]));
        CHECK(vkCreateSemaphore(dev, &sc, NULL, &got[i]));
        CHECK(vkCreateSemaphore(dev, &sc, NULL, &done[i]));
    }
    const char *pmode = getenv("WSITRI_MODE");
    VkPresentModeKHR mode = pmode && !strcmp(pmode, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    CHECK(make_swapchain(pd, surf, fmt, rp, mode));

    int fail = 0;
    for (int stage = 0; stage < 2; stage++) {
        int want_frames = stage == 0 ? frames : 30;
        int shown = 0, remade = 0;
        double t0 = now_ms();
        for (int f = 0; shown < want_frames && f < want_frames + 200; f++) {
            int k = f & 1;
            vkWaitForFences(dev, 1, &fence[k], VK_TRUE, UINT64_MAX);
            uint32_t idx;
            VkResult ar = vkAcquireNextImageKHR(dev, S.sc, UINT64_MAX, got[k], VK_NULL_HANDLE, &idx);
            if (ar == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(dev); CHECK(make_swapchain(pd, surf, fmt, rp, mode)); remade++; continue; }
            if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { printf("acquire = %d\n", ar); return 1; }
            vkResetFences(dev, 1, &fence[k]);
            vkResetCommandBuffer(cmd[k], 0);
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            vkBeginCommandBuffer(cmd[k], &bi);
            VkClearValue clear = { .color = { .float32 = { 0.1f, 0.1f, 0.1f, 1.0f } } };
            VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = S.fb[idx],
                .renderArea = { { 0, 0 }, { S.w, S.h } }, .clearValueCount = 1, .pClearValues = &clear };
            vkCmdBeginRenderPass(cmd[k], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd[k], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            VkViewport vp = { 0, 0, (float) S.w, (float) S.h, 0, 1 };
            VkRect2D sr = { { 0, 0 }, { S.w, S.h } };
            vkCmdSetViewport(cmd[k], 0, 1, &vp);
            vkCmdSetScissor(cmd[k], 0, 1, &sr);
            vkCmdDraw(cmd[k], 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd[k]);
            vkEndCommandBuffer(cmd[k]);
            VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &got[k], .pWaitDstStageMask = &ws,
                .commandBufferCount = 1, .pCommandBuffers = &cmd[k], .signalSemaphoreCount = 1, .pSignalSemaphores = &done[k] };
            CHECK(vkQueueSubmit(q, 1, &si, fence[k]));
            VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done[k], .swapchainCount = 1,
                .pSwapchains = &S.sc, .pImageIndices = &idx };
            VkResult pr = vkQueuePresentKHR(q, &pi);
            shown++;
            if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR) {
                vkDeviceWaitIdle(dev);
                printf("present = %s → 作り直す\n", pr == VK_SUBOPTIMAL_KHR ? "SUBOPTIMAL" : "OUT_OF_DATE");
                CHECK(make_swapchain(pd, surf, fmt, rp, mode));
                remade++;
            } else if (pr != VK_SUCCESS) {
                printf("present = %d\n", pr);
                return 1;
            }
        }
        vkDeviceWaitIdle(dev);
        double t1 = now_ms();
        nap(400); // 窓に描く糸が最後のコマを描き終えるまで
        uint8_t *rgb = grab(c, win, S.w, S.h);
        if (!rgb) return 1;
        uint32_t colored = count_colored(rgb, S.w, S.h);
        const uint8_t *corner = rgb + ((size_t) 3 * S.w + 3) * 3, *mid = rgb + ((size_t) (S.h * 55 / 100) * S.w + S.w / 2) * 3;
        printf("段 %d：%d コマ出した（%.1f ms・%.1f コマ/秒・作り直し %d 回）：窓 %u x %u・色のついた画素 %u 個（%.1f%%）・隅 %u %u %u・真ん中 %u %u %u\n", stage + 1,
               shown, t1 - t0, shown * 1000.0 / (t1 - t0), remade, S.w, S.h, colored, colored * 100.0 / (S.w * S.h), corner[0], corner[1], corner[2], mid[0], mid[1], mid[2]);
        char path[64];
        snprintf(path, sizeof path, "/tmp/ipl2/wsitri_%s_%d.ppm", use_xlib ? "xlib" : "xcb", stage + 1);
        write_ppm(path, rgb, S.w, S.h);
        // 三角形は窓の面積の 32% ほど（tri の 256 x 256 で 20808 個）。隅は消した色（0.1 → 25 前後）
        double frac = colored * 1.0 / (S.w * S.h);
        int ok = frac > 0.28 && frac < 0.36 && corner[0] < 40 && corner[0] > 15 && (mid[0] > 40 || mid[1] > 40 || mid[2] > 40);
        if (stage == 1 && (S.w != 320 || S.h != 200)) ok = 0;
        if (stage == 0 && ref_path) {
            uint32_t rw, rh;
            uint8_t *ref = read_ppm(ref_path, &rw, &rh);
            if (ref && rw == S.w && rh == S.h) {
                double sum = 0;
                int maxd = 0;
                for (size_t i = 0; i < (size_t) rw * rh * 3; i++) { int d = abs((int) rgb[i] - (int) ref[i]); sum += d; if (d > maxd) maxd = d; }
                double mean = sum / ((double) rw * rh * 3);
                printf("  見本（%s）と比べた：画素の差の平均 %.2f・いちばん大きい差 %d（JPEG の質 80 の崩れ）\n", ref_path, mean, maxd);
                if (mean > 4.0) ok = 0;
            } else {
                printf("  見本 %s を読めない・大きさが違う\n", ref_path);
            }
            free(ref);
        }
        printf("  %s\n", ok ? "正しい" : "まちがい");
        if (!ok) fail = 1;
        free(rgb);
        if (stage == 0) {
            // 窓の大きさを変える（swapchain の作り直しを確かめる）
            uint32_t wh[2] = { 320, 200 };
            if (use_xlib) {
                // Xlib の窓も xcb の自分の口から変えられる（窓の番号は X の中で共通）
            }
            X.configure_window(c, win, 4 | 8 /* WIDTH | HEIGHT */, wh);
            X.flush(c);
            nap(100);
        }
    }
    vkDeviceWaitIdle(dev);
    vkDestroySwapchainKHR(dev, S.sc, NULL);
    vkDestroySurfaceKHR(inst, surf, NULL);
    PFN_vkDestroyDevice vkDestroyDevice = (PFN_vkDestroyDevice) gdpa(dev, "vkDestroyDevice");
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (use_xlib) { XL.destroy(dpy, win); XL.close(dpy); }
    else X.destroy_window(c, win);
    X.flush(c);
    X.disconnect(c);
    printf("全体：%s\n", fail ? "失敗あり" : "通った");
    return fail;
}
