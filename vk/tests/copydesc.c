// descriptor の写し（vkUpdateDescriptorSets の VkCopyDescriptorSet）の試験。
//   copydesc.exe [装置の名前の一部] [要素の数]
// set A に a・b・c を書き、set B へ写す。そのあと A を b・a・d に書き換えて、B で足し算をする。
// B が写した時点の中身を持っていれば c = a + b になり、d は手つかずのまま

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

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const char *want = argc > 1 ? argv[1] : "iPhone";
    uint32_t n = argc > 2 ? (uint32_t) atoi(argv[2]) : (1u << 20);
    VkDeviceSize bytes = (VkDeviceSize) n * sizeof(float);

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
    DFN(vkCmdBindDescriptorSets); DFN(vkCmdPushConstants); DFN(vkCmdDispatch); DFN(vkEndCommandBuffer);
    DFN(vkCreateFence); DFN(vkQueueSubmit); DFN(vkWaitForFences); DFN(vkDestroyDevice);
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBuffer buf[4];
    VkDeviceMemory mem[4];
    float *ptr[4];
    for (int i = 0; i < 4; i++) {
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
    for (uint32_t i = 0; i < n; i++) { ptr[0][i] = (float) i; ptr[1][i] = 0.5f * (float) i; ptr[2][i] = -1.0f; ptr[3][i] = -2.0f; }

    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof vecadd_spv, .pCode = vecadd_spv };
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));
    VkDescriptorSetLayoutBinding b[3];
    for (int i = 0; i < 3; i++) b[i] = (VkDescriptorSetLayoutBinding) { .binding = (uint32_t) i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b };
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

    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 6 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dl };
    VkDescriptorSet sa, ds;
    CHECK(vkAllocateDescriptorSets(dev, &dai, &sa));
    CHECK(vkAllocateDescriptorSets(dev, &dai, &ds));
    VkDescriptorBufferInfo bi[3];
    VkWriteDescriptorSet w[3];
    const int first[3] = { 0, 1, 2 }, later[3] = { 1, 0, 3 };
    for (int i = 0; i < 3; i++) {
        bi[i] = (VkDescriptorBufferInfo) { .buffer = buf[first[i]], .offset = 0, .range = VK_WHOLE_SIZE };
        w[i] = (VkWriteDescriptorSet) { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sa, .dstBinding = (uint32_t) i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i] };
    }
    vkUpdateDescriptorSets(dev, 3, w, 0, NULL);
    // A から B へ写す（binding ごとに 1 つずつ）
    VkCopyDescriptorSet cps[3];
    for (int i = 0; i < 3; i++)
        cps[i] = (VkCopyDescriptorSet) { .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, .srcSet = sa, .srcBinding = (uint32_t) i, .dstSet = ds, .dstBinding = (uint32_t) i, .descriptorCount = 1 };
    vkUpdateDescriptorSets(dev, 0, NULL, 3, cps);
    // A を書き換える。B は写した時点の中身のまま
    for (int i = 0; i < 3; i++) bi[i].buffer = buf[later[i]];
    vkUpdateDescriptorSets(dev, 3, w, 0, NULL);

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &cbbi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &n);
    vkCmdDispatch(cb, (n + 63) / 64, 1, 1);
    CHECK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    double t0 = now_ms();
    CHECK(vkQueueSubmit(q, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    double t1 = now_ms();

    (void) t0; (void) t1;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (ptr[2][i] != 1.5f * (float) i) { if (bad < 3) printf("違う: c[%u] = %g\n", i, ptr[2][i]); bad++; }
        if (ptr[3][i] != -2.0f) { if (bad < 3) printf("書き換えた先 d[%u] = %g（手つかずのはず）\n", i, ptr[3][i]); bad++; }
    }
    printf("descriptor の写し：%s\n", bad ? "まちがいあり" : "全部正しい");
    vkDestroyDevice(dev, NULL);
    return bad ? 1 : 0;
}
