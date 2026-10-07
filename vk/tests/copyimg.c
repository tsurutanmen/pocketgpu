// 画像の descriptor の写しの試験（storage image とサンプラーつきの画像）。
//   copyimg.exe [装置の名前の一部]
// 16x16 の画像を 3 枚（赤が 1・10・100）作る。set A に 1 と 10 の画像を書いて set B へ写し、A を 100 の画像に書き換える。
// B で「storage image + サンプラーつきの画像」を足すと、写した時点の中身なら 11 になる
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "copyimg_spv.h"

static PFN_vkGetInstanceProcAddr gipa;
#define IFN(name) PFN_##name name = (PFN_##name) gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name) vkGetDeviceProcAddr(dev, #name)
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("失敗 %s = %d（%d 行目）\n", #x, r_, __LINE__); return 1; } } while (0)

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const char *want = argc > 1 ? argv[1] : "iPhone";
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
    for (uint32_t i = 0; i < nd; i++) {
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (strstr(props.deviceName, want)) { pd = pds[i]; break; }
    }
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("装置: %s\n", props.deviceName);
    uint32_t nq = 8, qf = UINT32_MAX;
    VkQueueFamilyProperties qps[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qps);
    for (uint32_t i = 0; i < nq; i++) if (qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    DFN(vkGetDeviceQueue); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory);
    DFN(vkMapMemory); DFN(vkCreateShaderModule); DFN(vkCreateDescriptorSetLayout); DFN(vkCreatePipelineLayout);
    DFN(vkCreateComputePipelines); DFN(vkCreateDescriptorPool); DFN(vkAllocateDescriptorSets); DFN(vkUpdateDescriptorSets);
    DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdBindPipeline);
    DFN(vkCmdBindDescriptorSets); DFN(vkCmdDispatch); DFN(vkEndCommandBuffer); DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyBufferToImage);
    DFN(vkCreateFence); DFN(vkQueueSubmit); DFN(vkWaitForFences); DFN(vkDestroyDevice);
    DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkBindImageMemory); DFN(vkCreateImageView); DFN(vkCreateSampler);
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
#define PICK(bits, want_flags, out) do { out = UINT32_MAX; for (uint32_t t = 0; t < mp.memoryTypeCount; t++) \
    if (((bits) & (1u << t)) && (mp.memoryTypes[t].propertyFlags & (want_flags)) == (want_flags)) { out = t; break; } } while (0)

    // 写す元（CPU から見える）と、答えを書く先
    const uint32_t W = 16, N = W * W;
    VkBuffer stg[3], out;
    VkDeviceMemory stgm[3], outm;
    float *sp[3], *op;
    for (int i = 0; i < 4; i++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize) N * 16,
                                   .usage = i < 3 ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        VkBuffer b;
        CHECK(vkCreateBuffer(dev, &bci, NULL, &b));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, b, &req);
        uint32_t ti;
        PICK(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ti);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = ti };
        VkDeviceMemory m;
        CHECK(vkAllocateMemory(dev, &mai, NULL, &m));
        CHECK(vkBindBufferMemory(dev, b, m, 0));
        void *p;
        CHECK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p));
        if (i < 3) { stg[i] = b; stgm[i] = m; sp[i] = p; } else { out = b; outm = m; op = p; }
    }
    (void) stgm; (void) outm;
    const float val[3] = { 1.0f, 10.0f, 100.0f };
    for (int i = 0; i < 3; i++)
        for (uint32_t k = 0; k < N; k++) { sp[i][k * 4] = val[i]; sp[i][k * 4 + 1] = sp[i][k * 4 + 2] = 0; sp[i][k * 4 + 3] = 1; }
    for (uint32_t k = 0; k < N; k++) op[k] = -1.0f;

    // 画像 3 枚
    VkImage img[3];
    VkImageView view[3];
    for (int i = 0; i < 3; i++) {
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                                 .extent = { W, W, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
        CHECK(vkCreateImage(dev, &ic, NULL, &img[i]));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, img[i], &req);
        uint32_t ti;
        PICK(req.memoryTypeBits, 0, ti);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = ti };
        VkDeviceMemory m;
        CHECK(vkAllocateMemory(dev, &mai, NULL, &m));
        CHECK(vkBindImageMemory(dev, img[i], m, 0));
        VkImageViewCreateInfo vc = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CHECK(vkCreateImageView(dev, &vc, NULL, &view[i]));
    }
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST };
    VkSampler smp;
    CHECK(vkCreateSampler(dev, &sci, NULL, &smp));

    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof copyimg_spv, .pCode = copyimg_spv };
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkDescriptorSetLayoutBinding lb[3] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = lb };
    VkDescriptorSetLayout dl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dl));
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" }, .layout = pl };
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));

    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 3, .pPoolSizes = ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetLayout two[2] = { dl, dl };
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 2, .pSetLayouts = two };
    VkDescriptorSet sets[2];
    CHECK(vkAllocateDescriptorSets(dev, &dai, sets));
    VkDescriptorSet sa = sets[0], sb = sets[1];
    VkDescriptorImageInfo ii0 = { VK_NULL_HANDLE, view[0], VK_IMAGE_LAYOUT_GENERAL }, ii2 = { smp, view[1], VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorBufferInfo bi = { out, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sa, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii0 },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sa, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sa, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii2 },
    };
    vkUpdateDescriptorSets(dev, 3, w, 0, NULL);
    VkCopyDescriptorSet cps[3];
    for (int i = 0; i < 3; i++)
        cps[i] = (VkCopyDescriptorSet) { .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = sa, .srcBinding = (uint32_t) i, .dstSet = sb, .dstBinding = (uint32_t) i, .descriptorCount = 1 };
    vkUpdateDescriptorSets(dev, 0, NULL, 3, cps);
    // A を 100 の画像に書き換える。B は写した時点のまま
    ii0.imageView = view[2];
    ii2.imageView = view[2];
    VkWriteDescriptorSet w2[2] = { w[0], w[2] };
    vkUpdateDescriptorSets(dev, 2, w2, 0, NULL);

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cpool));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &cbbi));
    VkImageMemoryBarrier ib[3];
    for (int i = 0; i < 3; i++)
        ib[i] = (VkImageMemoryBarrier) { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 3, ib);
    for (int i = 0; i < 3; i++) {
        VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, W, 1 } };
        vkCmdCopyBufferToImage(cb, stg[i], img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    }
    for (int i = 0; i < 3; i++) {
        ib[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ib[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 3, ib);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sb, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    CHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(vkQueueSubmit(q, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    uint32_t bad = 0;
    for (uint32_t k = 0; k < N; k++) if (op[k] != 11.0f) { if (bad < 3) printf("違う: out[%u] = %g（11 のはず）\n", k, op[k]); bad++; }
    printf("画像の descriptor の写し：%s\n", bad ? "まちがいあり" : "全部正しい");
    vkDestroyDevice(dev, NULL);
    return bad ? 1 : 0;
}
