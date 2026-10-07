// push descriptor（VK_KHR_push_descriptor）の試験。1 つの command buffer の中で 3 回 push して足し算をつなぐ。
//   pushdesc.exe [装置の名前の一部] [要素の数]
// 1 回目 c1 = a + b・2 回目 c2 = c1 + a（ふつうの push）・3 回目 c3 = b + c2（ひな形での push）。
// 答えは c1 = 1.5i・c2 = 2.5i・c3 = 3i。同じ記録を 2 回 submit して、2 回とも正しいかを見る
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "vecadd_spv.h"

static PFN_vkGetInstanceProcAddr gipa;
#define IFN(name) PFN_##name name = (PFN_##name) gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name) vkGetDeviceProcAddr(dev, #name)
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("失敗 %s = %d（%d 行目）\n", #x, r_, __LINE__); return 1; } } while (0)

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const char *want = argc > 1 ? argv[1] : "iPhone";
    uint32_t n = argc > 2 ? (uint32_t) atoi(argv[2]) : (1u << 16);
    VkDeviceSize bytes = (VkDeviceSize) n * sizeof(float);

    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    gipa = (PFN_vkGetInstanceProcAddr) GetProcAddress(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr); IFN(vkEnumerateDeviceExtensionProperties);

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
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne ? ne : 1, sizeof *ex);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);
    int has = 0;
    for (uint32_t i = 0; i < ne; i++) if (!strcmp(ex[i].extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) has = 1;
    if (!has) { printf("VK_KHR_push_descriptor が無い\n"); return 2; }

    uint32_t nq = 8, qf = UINT32_MAX;
    VkQueueFamilyProperties qps[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qps);
    for (uint32_t i = 0; i < nq; i++) if (qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qf = i; break; }
    float prio = 1.0f;
    const char *dext[] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = 1, .ppEnabledExtensionNames = dext };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    DFN(vkGetDeviceQueue); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory);
    DFN(vkMapMemory); DFN(vkCreateShaderModule); DFN(vkCreateDescriptorSetLayout); DFN(vkCreatePipelineLayout);
    DFN(vkCreateComputePipelines); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer);
    DFN(vkCmdBindPipeline); DFN(vkCmdPushConstants); DFN(vkCmdDispatch); DFN(vkEndCommandBuffer); DFN(vkCmdPipelineBarrier);
    DFN(vkCreateFence); DFN(vkResetFences); DFN(vkQueueSubmit); DFN(vkWaitForFences); DFN(vkDestroyDevice);
    DFN(vkCmdPushDescriptorSetKHR); DFN(vkCreateDescriptorUpdateTemplate); DFN(vkCmdPushDescriptorSetWithTemplateKHR);
    if (!vkCmdPushDescriptorSetKHR || !vkCmdPushDescriptorSetWithTemplateKHR) { printf("push の入口が無い\n"); return 2; }
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    enum { A, B, C1, C2, C3, NB };
    VkBuffer buf[NB];
    VkDeviceMemory mem[NB];
    float *ptr[NB];
    for (int i = 0; i < NB; i++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        CHECK(vkCreateBuffer(dev, &bci, NULL, &buf[i]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buf[i], &req);
        uint32_t want_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, ti = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
            if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want_flags) == want_flags) { ti = t; break; }
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = ti };
        CHECK(vkAllocateMemory(dev, &mai, NULL, &mem[i]));
        CHECK(vkBindBufferMemory(dev, buf[i], mem[i], 0));
        CHECK(vkMapMemory(dev, mem[i], 0, VK_WHOLE_SIZE, 0, (void **) &ptr[i]));
    }
    for (uint32_t i = 0; i < n; i++) { ptr[A][i] = (float) i; ptr[B][i] = 0.5f * (float) i; }

    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vecadd_spv, .pCode = vecadd_spv };
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkDescriptorSetLayoutBinding lb[3];
    for (int i = 0; i < 3; i++) lb[i] = (VkDescriptorSetLayoutBinding) { .binding = (uint32_t) i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 3, .pBindings = lb };
    VkDescriptorSetLayout dl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dl));
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = 4 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" }, .layout = pl };
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));

    // ひな形：3 つの VkDescriptorBufferInfo を並べた物から書く
    VkDescriptorUpdateTemplateEntry te[3];
    for (int i = 0; i < 3; i++)
        te[i] = (VkDescriptorUpdateTemplateEntry) { .dstBinding = (uint32_t) i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                    .offset = (size_t) i * sizeof(VkDescriptorBufferInfo), .stride = sizeof(VkDescriptorBufferInfo) };
    VkDescriptorUpdateTemplateCreateInfo tci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, .descriptorUpdateEntryCount = 3,
        .pDescriptorUpdateEntries = te, .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE, .pipelineLayout = pl, .set = 0 };
    VkDescriptorUpdateTemplate tmpl;
    CHECK(vkCreateDescriptorUpdateTemplate(dev, &tci, NULL, &tmpl));

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &n);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT };
    const int order[2][3] = { { A, B, C1 }, { C1, A, C2 } };
    for (int k = 0; k < 2; k++) {
        VkDescriptorBufferInfo bi[3];
        VkWriteDescriptorSet w[3];
        for (int i = 0; i < 3; i++) {
            bi[i] = (VkDescriptorBufferInfo) { .buffer = buf[order[k][i]], .offset = 0, .range = VK_WHOLE_SIZE };
            w[i] = (VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t) i, .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i] };
        }
        vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 3, w);
        vkCmdDispatch(cb, (n + 63) / 64, 1, 1);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    VkDescriptorBufferInfo tb[3] = { { buf[B], 0, VK_WHOLE_SIZE }, { buf[C2], 0, VK_WHOLE_SIZE }, { buf[C3], 0, VK_WHOLE_SIZE } };
    vkCmdPushDescriptorSetWithTemplateKHR(cb, tmpl, pl, 0, tb);
    vkCmdDispatch(cb, (n + 63) / 64, 1, 1);
    CHECK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    int all_ok = 1;
    for (int round = 0; round < 2; round++) {
        for (uint32_t i = 0; i < n; i++) ptr[C1][i] = ptr[C2][i] = ptr[C3][i] = -1.0f;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        CHECK(vkResetFences(dev, 1, &fence));
        CHECK(vkQueueSubmit(q, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        uint32_t bad[3] = { 0 };
        const float k[3] = { 1.5f, 2.5f, 3.0f };
        for (int j = 0; j < 3; j++)
            for (uint32_t i = 0; i < n; i++) if (ptr[C1 + j][i] != k[j] * (float) i) bad[j]++;
        printf("%d 回目：c1 %s・c2 %s・c3（ひな形）%s\n", round + 1, bad[0] ? "違う" : "正しい", bad[1] ? "違う" : "正しい", bad[2] ? "違う" : "正しい");
        if (bad[0] || bad[1] || bad[2]) all_ok = 0;
    }
    printf("push descriptor：%s\n", all_ok ? "全部正しい" : "まちがいあり");
    vkDestroyDevice(dev, NULL);
    return all_ok ? 0 : 1;
}
