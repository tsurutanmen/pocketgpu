// 空の handle が混ざった記録を受け手に送る試験（crash34）。
// 10/14 の Android の GZDoom は、受け手で作れなかった buffer・image（返事を待たない作り方 NR のまま）や、メモリに結べなかった物を
// 含む記録を再生して、GPU のドライバの中で SIGSEGV になった。同じ形を PC の代役の受け手（lavapipe）に送る。
//   - 作れなかった buffer・image（受け手を VKR_TEST_FAIL_NR=1 で起こすと、返事を待たない作り方の物を握りつぶす）
//   - 作れたがメモリに結べていない buffer
// を、fill・update・copy・barrier・頂点 / index / indirect・clear・blit に使い、そのあと本物の物の命令と三角形が最後まで通ることを確かめる。
//   nullrec.exe          どの命令も飛ばされ、本物の命令は通れば 0 を返す
// 守りを切る（受け手を VKR_RP_GUARD=0 で起こす）と、同じ記録で受け手が落ちる（対照）
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
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); fflush(stdout); return 1; } } while (0)

enum { W = 128, H = 128 };

static uint32_t find_mem(VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t t = 0; t < mp->memoryTypeCount; t++)
        if ((bits & (1u << t)) && (mp->memoryTypes[t].propertyFlags & want) == want) return t;
    return UINT32_MAX;
}

int main(int argc, char **argv) {
    (void) argc; (void) argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    gipa = (PFN_vkGetInstanceProcAddr) GetProcAddress(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr);
    uint32_t nd = 16;
    VkPhysicalDevice pds[16];
    CHECK(vkEnumeratePhysicalDevices(inst, &nd, pds));
    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);
    uint32_t nq = 8, qf = UINT32_MAX;
    VkQueueFamilyProperties qps[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qps);
    for (uint32_t i = 0; i < nq; i++) if (qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
    if (qf == UINT32_MAX) { printf("no graphics queue\n"); return 1; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindImageMemory);
    DFN(vkCreateImageView); DFN(vkCreateRenderPass); DFN(vkCreateFramebuffer); DFN(vkCreateShaderModule);
    DFN(vkCreatePipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements);
    DFN(vkBindBufferMemory); DFN(vkMapMemory); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers);
    DFN(vkBeginCommandBuffer); DFN(vkCmdBeginRenderPass); DFN(vkCmdBindPipeline); DFN(vkCmdDraw); DFN(vkCmdEndRenderPass);
    DFN(vkCmdCopyImageToBuffer); DFN(vkCmdPipelineBarrier); DFN(vkEndCommandBuffer); DFN(vkCreateFence); DFN(vkQueueSubmit);
    DFN(vkWaitForFences); DFN(vkResetFences); DFN(vkResetCommandBuffer); DFN(vkDestroyBuffer); DFN(vkFreeMemory);
    DFN(vkCmdFillBuffer); DFN(vkCmdUpdateBuffer); DFN(vkCmdCopyBuffer); DFN(vkCmdBindVertexBuffers); DFN(vkCmdBindIndexBuffer);
    DFN(vkCmdDrawIndexed); DFN(vkCmdDrawIndirect); DFN(vkCmdDrawIndexedIndirect); DFN(vkCmdCopyBufferToImage);
    DFN(vkCmdClearColorImage); DFN(vkCmdBlitImage); DFN(vkCmdCopyImage); DFN(vkCmdPushConstants); DFN(vkCmdSetViewport);
    DFN(vkCmdSetScissor); DFN(vkCmdDispatchIndirect);
    DFN(vkCreateDescriptorSetLayout); DFN(vkCreateDescriptorPool); DFN(vkAllocateDescriptorSets); DFN(vkUpdateDescriptorSets); DFN(vkCmdBindDescriptorSets);
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    const VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // ---- 本物：描く先の画像・render pass・framebuffer・pipeline（tri と同じ）----
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
    VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL, .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT, .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1,
        .pAttachments = &view, .width = W, .height = H, .layers = 1 };
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));
    VkShaderModuleCreateInfo vs = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof tri_vert_spv, .pCode = tri_vert_spv };
    VkShaderModuleCreateInfo fs = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof tri_frag_spv, .pCode = tri_frag_spv };
    VkShaderModule vsm, fsm;
    CHECK(vkCreateShaderModule(dev, &vs, NULL, &vsm));
    CHECK(vkCreateShaderModule(dev, &fs, NULL, &fsm));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
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
    VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl, .renderPass = rp, .subpass = 0 };
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    // ---- buffer ----
    // 本物の buffer G（CPU から見えるメモリ。fill の結果をここで読む）。使い道 UG は、これ 1 つしか作らない（最初の 1 回は返事を待つ作り方）
    const VkBufferUsageFlags UG = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 262144, .usage = UG };
    VkBuffer good;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &good));
    vkGetBufferMemoryRequirements(dev, good, &req);
    VkMemoryAllocateInfo gmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = find_mem(&mp, req.memoryTypeBits, HOST) };
    VkDeviceMemory gmem;
    CHECK(vkAllocateMemory(dev, &gmai, NULL, &gmem));
    CHECK(vkBindBufferMemory(dev, good, gmem, 0));
    volatile uint32_t *gp;
    CHECK(vkMapMemory(dev, gmem, 0, VK_WHOLE_SIZE, 0, (void **) &gp));
    memset((void *) gp, 0xEE, 262144);

    // 空の buffer（受け手で作れなかった物）：同じ使い道の buffer を 1 つ作って（返事を待つ）受け手に使い道の性質を覚えさせ、次のを返事を待たずに作る
    const VkBufferUsageFlags UB = UG | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBufferCreateInfo pbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 1000, .usage = UB };
    VkBuffer probe;
    CHECK(vkCreateBuffer(dev, &pbci, NULL, &probe));
    VkBufferCreateInfo gbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096 + 8, .usage = UB };
    VkBuffer ghost;
    CHECK(vkCreateBuffer(dev, &gbci, NULL, &ghost));
    vkGetBufferMemoryRequirements(dev, ghost, &req);
    VkMemoryAllocateInfo hmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = find_mem(&mp, req.memoryTypeBits, HOST) };
    VkDeviceMemory hmem;
    CHECK(vkAllocateMemory(dev, &hmai, NULL, &hmem));
    CHECK(vkBindBufferMemory(dev, ghost, hmem, 0)); // 受け手では buffer が無いので結ばれない

    // メモリに結ばない buffer（作れたが結べていない）
    VkBufferCreateInfo ubci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = UB | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT };
    VkBuffer unbound;
    CHECK(vkCreateBuffer(dev, &ubci, NULL, &unbound));

    // ---- dynamic offset の試験：uniform buffer（動的）の descriptor set。上限を超える range・範囲の外・揃わない offset ----
    VkBufferCreateInfo ucbi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 8192, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer ubuf;
    CHECK(vkCreateBuffer(dev, &ucbi, NULL, &ubuf));
    vkGetBufferMemoryRequirements(dev, ubuf, &req);
    VkMemoryAllocateInfo umai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = find_mem(&mp, req.memoryTypeBits, HOST) };
    VkDeviceMemory umem;
    CHECK(vkAllocateMemory(dev, &umai, NULL, &umem));
    CHECK(vkBindBufferMemory(dev, ubuf, umem, 0));
    VkDescriptorSetLayoutBinding dlb = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dlci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &dlb };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dlci, NULL, &dsl));
    VkPipelineLayoutCreateInfo pl2ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl2;
    CHECK(vkCreatePipelineLayout(dev, &pl2ci, NULL, &pl2));
    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps };
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet dset;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &dset));
    VkDescriptorBufferInfo dbi = { ubuf, 0, 0x7fffffffull }; // 上限を超える range（受け手が切り詰める）
    VkWriteDescriptorSet dw = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pBufferInfo = &dbi };
    vkUpdateDescriptorSets(dev, 1, &dw, 0, NULL);
    dbi.range = 256;
    vkUpdateDescriptorSets(dev, 1, &dw, 0, NULL);

    // ---- 空の image（受け手で作れなかった物）----
    VkImageCreateInfo xci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 16, 16, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage ximg_probe, ximg_ghost;
    CHECK(vkCreateImage(dev, &xci, NULL, &ximg_probe));  // 返事を待つ（受け手が作る）
    CHECK(vkCreateImage(dev, &xci, NULL, &ximg_ghost));  // 同じ条件：返事を待たずに作る（VKR_TEST_FAIL_NR=1 の受け手は作らない）
    vkGetImageMemoryRequirements(dev, ximg_ghost, &req);
    VkMemoryAllocateInfo xmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = find_mem(&mp, req.memoryTypeBits, 0) };
    VkDeviceMemory xmem;
    CHECK(vkAllocateMemory(dev, &xmai, NULL, &xmem));
    CHECK(vkBindImageMemory(dev, ximg_ghost, xmem, 0));

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));

    // ---- 記録 ----
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    // (1) buffer の命令：空の物と本物を混ぜる。本物の fill は通らなければならない
    vkCmdFillBuffer(cmd, ghost, 0, 256, 0xAAAAAAAAu);
    vkCmdFillBuffer(cmd, unbound, 0, 256, 0xBBBBBBBBu);
    vkCmdFillBuffer(cmd, good, 0, 256, 0x11111111u);
    uint32_t data[4] = { 1, 2, 3, 4 };
    vkCmdUpdateBuffer(cmd, ghost, 0, sizeof data, data);
    vkCmdUpdateBuffer(cmd, good, 256, sizeof data, data);
    VkBufferCopy bc = { 0, 512, 256 };
    vkCmdCopyBuffer(cmd, ghost, good, 1, &bc);
    vkCmdCopyBuffer(cmd, good, ghost, 1, &bc);
    vkCmdCopyBuffer(cmd, unbound, good, 1, &bc);
    VkBufferCopy bc2 = { 0, 1024, 256 };
    vkCmdCopyBuffer(cmd, good, good, 1, &bc2); // 本物（通る）
    // (2) barrier：空の buffer・image の barrier だけが外れる
    VkBufferMemoryBarrier bmb[3] = {
        { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = ghost, .size = VK_WHOLE_SIZE },
        { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = good, .size = VK_WHOLE_SIZE },
        { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .buffer = unbound, .size = VK_WHOLE_SIZE } };
    VkImageMemoryBarrier imb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = ximg_ghost,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 3, bmb, 1, &imb);
    // (3) image の命令
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 16, 16, 1 } };
    vkCmdCopyBufferToImage(cmd, good, ximg_ghost, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    vkCmdCopyBufferToImage(cmd, ghost, ximg_ghost, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    vkCmdCopyImageToBuffer(cmd, ximg_ghost, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, good, 1, &bic);
    VkClearColorValue cc = { .float32 = { 1, 0, 0, 1 } };
    VkImageSubresourceRange srr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cmd, ximg_ghost, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &srr);
    VkImageBlit blit = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { 16, 16, 1 } },
                         .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { 0, 0, 0 }, { 16, 16, 1 } } };
    vkCmdBlitImage(cmd, ximg_ghost, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ximg_ghost, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .extent = { 16, 16, 1 } };
    vkCmdCopyImage(cmd, ximg_ghost, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ximg_probe, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    // (4) 外れた値：push constants が上限の外・viewport の番号が上限の外
    uint8_t pcd[64] = { 0 };
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 8192, sizeof pcd, pcd);
    VkViewport nvp = { 0, 0, W, H, 0, 1 };
    vkCmdSetViewport(cmd, 40, 1, &nvp);
    // (4b) dynamic offset：範囲の外・揃わない offset はつながれず、本物はつながる
    uint32_t dyn_bad = 1u << 20, dyn_align = 4, dyn_ok = 256;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2, 0, 1, &dset, 1, &dyn_bad);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2, 0, 1, &dset, 1, &dyn_align);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl2, 0, 1, &dset, 1, &dyn_ok);
    // (5) 描く：空の頂点 / index / indirect を結んだ draw は飛ばされ、本物を結び直したあとの draw は描ける
    VkClearValue clear = { .color = { .float32 = { 0.1f, 0.1f, 0.1f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
        .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &ghost, &zero);
    vkCmdDraw(cmd, 3, 1, 0, 0);               // 空の頂点 buffer：飛ばされる
    vkCmdBindVertexBuffers(cmd, 0, 1, &good, &zero);
    vkCmdBindIndexBuffer(cmd, ghost, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, 3, 1, 0, 0, 0);     // 空の index buffer：飛ばされる
    vkCmdDrawIndirect(cmd, ghost, 0, 1, 16);  // 空の indirect buffer：飛ばされる
    vkCmdDrawIndexedIndirect(cmd, unbound, 0, 1, 20);
    vkCmdDraw(cmd, 3, 1, 0, 0);               // 本物：三角形が描ける
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 }, .bufferOffset = 4096 };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, good, 1, &region);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd));

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VkResult sr = vkQueueSubmit(q, 1, &si, fence);
    VkResult wr = sr == VK_SUCCESS ? vkWaitForFences(dev, 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000) : sr;
    printf("RESULT round1 submit=%d wait=%d\n", sr, wr);
    if (sr || wr) return 2;

    // 確かめる：本物の fill・update・copy が通った・三角形が描けた
    int fill_ok = gp[0] == 0x11111111u && gp[63] == 0x11111111u && gp[64] == 1 && gp[65] == 2 && gp[256] == 0x11111111u /* 本物の copy: 0 → 1024 の位置 */ ;
    uint32_t colored = 0;
    const uint8_t *px = (const uint8_t *) gp + 4096;
    for (int i = 0; i < W * H; i++) if (px[i * 4] > 40 || px[i * 4 + 1] > 40 || px[i * 4 + 2] > 40) colored++;
    printf("RESULT round1 fill_ok=%d (g0=%08x g64=%u g256=%08x) triangle_pixels=%u\n", fill_ok, gp[0], gp[64], gp[256], colored);

    // 2 回目：受け手が生きていて、同じ装置でまた記録を通せる
    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdFillBuffer(cmd, ghost, 0, 256, 0xAAAAAAAAu);
    vkCmdFillBuffer(cmd, good, 8192, 256, 0x22222222u);
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkResetFences(dev, 1, &fence));
    sr = vkQueueSubmit(q, 1, &si, fence);
    wr = sr == VK_SUCCESS ? vkWaitForFences(dev, 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000) : sr;
    int fill2 = gp[2048] == 0x22222222u;
    printf("RESULT round2 submit=%d wait=%d fill_ok=%d\n", sr, wr, fill2);
    int ok = !sr && !wr && fill_ok && colored > 500 && fill2;
    printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 3;
}
