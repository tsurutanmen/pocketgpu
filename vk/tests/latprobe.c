// 試験用（配らない）：遅れを測るための Vulkan の窓の試験（10/15 lat1015）。
//   latprobe.exe コマ数 幅 高さ 出し方(fifo|mailbox|immediate) CPU_ms 重ね塗り 同時コマ数
// ・毎コマ、コマの番号を色にして画面全体を塗る（R = (n % 8) * 32 + 16・G = ((n / 8) % 8) * 32 + 16・B = 128）。
//   ドライバの記録（IPHONE_VK_LAT_CSV の S 行の色）から、窓に出た絵が何コマ目かを確かめられる（JPEG でも ±16 に収まる）
// ・CPU_ms：毎コマ、acquire の前に CPU を回す（ゲームの CPU の仕事の代わり）
// ・重ね塗り：画面全体を塗る回数を足す（受け手の GPU の仕事の代わり）
// ・同時コマ数：fence で待つ前に先へ進めるコマ数（ふつうのアプリは 2）
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS && r_ != VK_SUBOPTIMAL_KHR) { printf("失敗 %s = %d（%d 行目）\n", #x, r_, __LINE__); return 1; } } while (0)

static double now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    uint32_t W = argc > 2 ? (uint32_t) atoi(argv[2]) : 640, H = argc > 3 ? (uint32_t) atoi(argv[3]) : 360;
    const char *mname = argc > 4 ? argv[4] : "fifo";
    double cpu_ms = argc > 5 ? atof(argv[5]) : 0;
    int extra = argc > 6 ? atoi(argv[6]) : 0;
    int inflight = argc > 7 ? atoi(argv[7]) : 2;
    if (inflight < 1) inflight = 1;
    if (inflight > 4) inflight = 4;
    VkPresentModeKHR want = !strcmp(mname, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR : !strcmp(mname, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc = { .lpfnWndProc = wndproc, .hInstance = hi, .lpszClassName = L"latprobe", .hCursor = LoadCursor(NULL, IDC_ARROW) };
    RegisterClassW(&wc);
    RECT rc = { 0, 0, (LONG) W, (LONG) H };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hw = CreateWindowW(L"latprobe", L"latprobe", WS_OVERLAPPEDWINDOW, 0, 0, rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hi, NULL);
    ShowWindow(hw, SW_SHOWNOACTIVATE);

    const char *iext[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "latprobe", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = 2, .ppEnabledExtensionNames = iext };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t npd = 8;
    VkPhysicalDevice pds[8];
    CHECK(vkEnumeratePhysicalDevices(inst, &npd, pds));
    if (!npd) { printf("GPU が無い\n"); return 1; }
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(pd, &pp);
    printf("GPU：%s\n", pp.deviceName);
    VkWin32SurfaceCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, .hinstance = hi, .hwnd = hw };
    VkSurfaceKHR surf;
    CHECK(vkCreateWin32SurfaceKHR(inst, &sci, NULL, &surf));
    uint32_t nm = 8;
    VkPresentModeKHR pm[8];
    CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &nm, pm));
    int have = 0;
    for (uint32_t i = 0; i < nm; i++) have |= pm[i] == want;
    if (!have) { printf("出し方 %s が無いので fifo にする\n", mname); want = VK_PRESENT_MODE_FIFO_KHR; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
    VkDevice dv;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dv));
    VkQueue q;
    vkGetDeviceQueue(dv, 0, 0, &q);
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    uint32_t nimg = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && nimg > caps.maxImageCount) nimg = caps.maxImageCount;
    VkFormat fmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkSwapchainCreateInfoKHR swci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surf, .minImageCount = nimg, .imageFormat = fmt,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = { W, H }, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = want, .clipped = VK_TRUE };
    VkSwapchainKHR sw;
    CHECK(vkCreateSwapchainKHR(dv, &swci, NULL, &sw));
    uint32_t ni = 8;
    VkImage imgs[8];
    CHECK(vkGetSwapchainImagesKHR(dv, sw, &ni, imgs));
    VkAttachmentDescription ad = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dv, &rpci, NULL, &rp));
    VkImageView views[8];
    VkFramebuffer fbs[8];
    for (uint32_t i = 0; i < ni; i++) {
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = imgs[i], .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(dv, &vci, NULL, &views[i]));
        VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &views[i], .width = W, .height = H, .layers = 1 };
        CHECK(vkCreateFramebuffer(dv, &fci, NULL, &fbs[i]));
    }
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dv, &cpci, NULL, &pool));
    VkCommandBuffer cbs[4];
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 4 };
    CHECK(vkAllocateCommandBuffers(dv, &cai, cbs));
    VkFence fences[4];
    VkSemaphore acq[4], done[4];
    for (int i = 0; i < 4; i++) {
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        CHECK(vkCreateFence(dv, &fci, NULL, &fences[i]));
        VkSemaphoreCreateInfo sci2 = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        CHECK(vkCreateSemaphore(dv, &sci2, NULL, &acq[i]));
        CHECK(vkCreateSemaphore(dv, &sci2, NULL, &done[i]));
    }
    printf("出し方 %s・%u x %u・画像 %u 枚・CPU %.1f ms・重ね塗り %d・同時 %d コマ\n", mname, W, H, ni, cpu_ms, extra, inflight);
    fflush(stdout);
    double t0 = now_ms(), tw = t0;
    int f, fw = 0;
    for (f = 0; f < frames; f++) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        int k = f % inflight;
        CHECK(vkWaitForFences(dv, 1, &fences[k], VK_TRUE, UINT64_MAX));
        // ゲームの CPU の仕事の代わり（入力を読んでから絵を作るまで）
        double tc = now_ms();
        while (now_ms() - tc < cpu_ms) YieldProcessor();
        uint32_t idx;
        CHECK(vkAcquireNextImageKHR(dv, sw, UINT64_MAX, acq[k], VK_NULL_HANDLE, &idx));
        CHECK(vkResetFences(dv, 1, &fences[k]));
        VkCommandBuffer cb = cbs[k];
        CHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        CHECK(vkBeginCommandBuffer(cb, &bi));
        unsigned n = (unsigned) f + 1; // present の番号（1 から）と揃える
        VkClearValue cv = { .color = { .float32 = { ((n % 8) * 32 + 16) / 255.0f, (((n / 8) % 8) * 32 + 16) / 255.0f, 128 / 255.0f, 1 } } };
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fbs[idx], .renderArea = { { 0, 0 }, { W, H } },
            .clearValueCount = 1, .pClearValues = &cv };
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        // 重ね塗り：同じ色で画面全体を何回も塗る（GPU の仕事を足す。絵は変わらない）
        for (int e = 0; e < extra; e++) {
            VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0, .clearValue = cv };
            VkClearRect cr = { .rect = { { 0, 0 }, { W, H } }, .baseArrayLayer = 0, .layerCount = 1 };
            vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
        }
        vkCmdEndRenderPass(cb);
        CHECK(vkEndCommandBuffer(cb));
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acq[k], .pWaitDstStageMask = &ws,
            .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &done[k] };
        CHECK(vkQueueSubmit(q, 1, &si, fences[k]));
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done[k], .swapchainCount = 1, .pSwapchains = &sw, .pImageIndices = &idx };
        CHECK(vkQueuePresentKHR(q, &pi));
        double tn = now_ms();
        if (tn - tw >= 5000) { printf("直近 %.1f 秒：%.1f コマ/秒\n", (tn - tw) / 1000, (f + 1 - fw) * 1000.0 / (tn - tw)); fflush(stdout); tw = tn; fw = f + 1; }
    }
    vkDeviceWaitIdle(dv);
    double sec = (now_ms() - t0) / 1000;
    printf("present %d コマ・%.2f 秒・%.1f コマ/秒\n", f, sec, f / sec);
    vkDestroySwapchainKHR(dv, sw, NULL);
    for (uint32_t i = 0; i < ni; i++) { vkDestroyFramebuffer(dv, fbs[i], NULL); vkDestroyImageView(dv, views[i], NULL); }
    vkDestroyRenderPass(dv, rp, NULL);
    for (int i = 0; i < 4; i++) { vkDestroyFence(dv, fences[i], NULL); vkDestroySemaphore(dv, acq[i], NULL); vkDestroySemaphore(dv, done[i], NULL); }
    vkDestroyCommandPool(dv, pool, NULL);
    vkDestroyDevice(dv, NULL);
    vkDestroySurfaceKHR(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);
    DestroyWindow(hw);
    return 0;
}
