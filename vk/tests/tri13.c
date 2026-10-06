// tri.c と同じ三角形を、Vulkan 1.3 の機能だけで描く試験（dynamic rendering・vkCmdPipelineBarrier2・vkQueueSubmit2）。
// render pass と framebuffer は使わない。絵は tri.c と同じになるはず（RTX の検算 dca5538b）
//   tri13.exe [装置の名前の一部] [出す PPM のファイル名]
// 名前を省くと、名前に "iPhone" を含む装置、無ければ 0 番を使う。
// 描いた絵の「色のついた画素の数」と「いくつかの点の色」と「全部の画素から作った検算の値」を出す。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "tri_vert_spv.h"
#include "tri_frag_spv.h"

static PFN_vkGetInstanceProcAddr gipa;
#define IFN(name) PFN_##name name = (PFN_##name) gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name) vkGetDeviceProcAddr(dev, #name)
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("失敗 %s = %d（%d 行目）\n", #x, r_, __LINE__); return 1; } } while (0)

enum { W = 256, H = 256 };

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

static uint32_t find_mem(VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t t = 0; t < mp->memoryTypeCount; t++)
        if ((bits & (1u << t)) && (mp->memoryTypes[t].propertyFlags & want) == want) return t;
    return UINT32_MAX;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const char *want = argc > 1 ? argv[1] : "iPhone";
    const char *out = argc > 2 ? argv[2] : "tri.ppm";

    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    gipa = (PFN_vkGetInstanceProcAddr) GetProcAddress(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr);

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
    printf("装置: %s（Vulkan %u.%u）\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
    if (props.apiVersion < VK_API_VERSION_1_3) { printf("この装置は 1.3 を名乗っていない\n"); return 2; }

    uint32_t nq = 8, qf = UINT32_MAX;
    VkQueueFamilyProperties qps[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qps);
    for (uint32_t i = 0; i < nq; i++) if (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
    if (qf == UINT32_MAX) { printf("描ける（GRAPHICS の）キューが無い\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindImageMemory);
    DFN(vkCreateImageView); DFN(vkCreateShaderModule); DFN(vkCmdBeginRendering); DFN(vkCmdEndRendering);
    DFN(vkCmdPipelineBarrier2); DFN(vkQueueSubmit2);
    DFN(vkCreatePipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements);
    DFN(vkBindBufferMemory); DFN(vkMapMemory); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers);
    DFN(vkBeginCommandBuffer); DFN(vkCmdBindPipeline); DFN(vkCmdDraw);
    DFN(vkCmdCopyImageToBuffer); DFN(vkEndCommandBuffer); DFN(vkCreateFence);
    if (!vkCmdBeginRendering || !vkCmdEndRendering || !vkCmdPipelineBarrier2 || !vkQueueSubmit2) { printf("1.3 の入口が無い\n"); return 1; }
    DFN(vkWaitForFences); DFN(vkDestroyDevice);
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    // 描く先の画像（GPU のメモリ）
    VkImageCreateInfo imci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img;
    CHECK(vkCreateImage(dev, &imci, NULL, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = find_mem(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = find_mem(&mp, req.memoryTypeBits, 0);
    VkDeviceMemory imem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
    CHECK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view;
    CHECK(vkCreateImageView(dev, &ivci, NULL, &view));

    // パイプライン：頂点のバッファ無し・三角形・色はそのまま
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
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkFormat cfmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo prci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cfmt };
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &prci, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl };
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    // 持ち帰り用のバッファ（CPU から見えるメモリ）
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &req);
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = find_mem(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(dev, &bmai, NULL, &bmem));
    CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
    uint8_t *px;
    CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **) &px));
    memset(px, 0xEE, W * H * 4);

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier2 to_color = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = rng };
    VkDependencyInfo d1 = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_color };
    vkCmdPipelineBarrier2(cmd, &d1);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = { .color = { .float32 = { 0.1f, 0.1f, 0.1f, 1.0f } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier2 to_src = to_color;
    to_src.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_src.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkDependencyInfo d2 = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_src };
    vkCmdPipelineBarrier2(cmd, &d2);
    VkBufferImageCopy region = { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    VkMemoryBarrier2 mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT };
    VkDependencyInfo d3 = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb };
    vkCmdPipelineBarrier2(cmd, &d3);
    CHECK(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkCommandBufferSubmitInfo cbsi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd };
    VkSubmitInfo2 si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbsi };
    double t0 = now_ms();
    CHECK(vkQueueSubmit2(q, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    double t1 = now_ms();

    // 確かめる：背景・三角形の中・3 つの角の近く
    uint32_t colored = 0, sum = 2166136261u;
    for (int i = 0; i < W * H; i++) {
        const uint8_t *p = px + i * 4;
        if (p[0] > 40 || p[1] > 40 || p[2] > 40) colored++;
        for (int k = 0; k < 4; k++) sum = (sum ^ p[k]) * 16777619u;
    }
    const int pts[][2] = { { 5, 5 }, { 128, 140 }, { 128, 40 }, { 220, 220 }, { 36, 220 } };
    printf("描いた（%d x %d・送って描いて返るまで %.1f ms）：色のついた画素 %u 個・検算 %08x\n", W, H, t1 - t0, colored, sum);
    for (size_t i = 0; i < sizeof pts / sizeof *pts; i++) {
        const uint8_t *p = px + (pts[i][1] * W + pts[i][0]) * 4;
        printf("  (%3d,%3d) = %3u %3u %3u\n", pts[i][0], pts[i][1], p[0], p[1], p[2]);
    }
    FILE *f = fopen(out, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) fwrite(px + i * 4, 1, 3, f);
        fclose(f);
        printf("絵を %s に書いた\n", out);
    }
    vkDestroyDevice(dev, NULL);
    return colored > 1000 ? 0 : 1;
}
