// 書き込み監視の試験と速さ比べ。CPU から見える大きなバッファの、毎回ほんの数ページだけを書き換えて、
// GPU にバッファ全体を写させる（vkCmdCopyBuffer）。ドライバは submit の前に「アプリが書いたページ」を受け手へ送る。
//   wwbench [装置の名前の一部] [MB（既定 64）] [回数（既定 100）] [1 回に書くページ（既定 16）]
// 最後に GPU 側の写し先を読み戻して、書いた値が全部届いているかを確かめる（監視が書き込みを見落とすと、ここで違う）。
// IPHONE_VK_STATS=1 を付けると、ドライバが送ったバイトと、書き込み監視にかかった時間が終わりに出る
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

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

static uint32_t find_mem(VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t t = 0; t < mp->memoryTypeCount; t++)
        if ((bits & (1u << t)) && (mp->memoryTypes[t].propertyFlags & want) == want) return t;
    for (uint32_t t = 0; t < mp->memoryTypeCount; t++) if (bits & (1u << t)) return t;
    return 0;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const char *want = argc > 1 ? argv[1] : "iPhone";
    uint32_t mb = argc > 2 ? (uint32_t) atoi(argv[2]) : 64;
    int iters = argc > 3 ? atoi(argv[3]) : 100;
    uint32_t touch = argc > 4 ? (uint32_t) atoi(argv[4]) : 16;
    VkDeviceSize bytes = (VkDeviceSize) mb << 20;
    uint32_t pages = (uint32_t) (bytes / 4096);

    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    gipa = (PFN_vkGetInstanceProcAddr) GetProcAddress(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr); IFN(vkDestroyInstance);
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
    uint32_t nq = 8, qf = 0;
    VkQueueFamilyProperties qps[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qps);
    for (uint32_t i = 0; i < nq; i++) if (qps[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { qf = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    DFN(vkGetDeviceQueue); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory);
    DFN(vkMapMemory); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdCopyBuffer);
    DFN(vkCmdPipelineBarrier); DFN(vkEndCommandBuffer); DFN(vkCreateFence); DFN(vkQueueSubmit); DFN(vkWaitForFences); DFN(vkResetFences);
    DFN(vkDestroyDevice);
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    // src：CPU から見える（アプリが書く）・dst：GPU の手元・rb：読み戻し用
    VkBuffer buf[3];
    VkDeviceMemory mem[3];
    const VkBufferUsageFlags use[3] = { VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    const VkMemoryPropertyFlags mf[3] = { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT };
    for (int i = 0; i < 3; i++) {
        VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = use[i] };
        CHECK(vkCreateBuffer(dev, &bci, NULL, &buf[i]));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buf[i], &req);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = find_mem(&mp, req.memoryTypeBits, mf[i]) };
        CHECK(vkAllocateMemory(dev, &mai, NULL, &mem[i]));
        CHECK(vkBindBufferMemory(dev, buf[i], mem[i], 0));
    }
    uint8_t *src, *rb;
    CHECK(vkMapMemory(dev, mem[0], 0, VK_WHOLE_SIZE, 0, (void **) &src));
    CHECK(vkMapMemory(dev, mem[2], 0, VK_WHOLE_SIZE, 0, (void **) &rb));
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBuffer cmd[2];
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2 };
    CHECK(vkAllocateCommandBuffers(dev, &cbai, cmd));
    VkBufferCopy whole = { 0, 0, bytes };
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    // 0：src → dst（毎回）・1：dst → rb（最後に読み戻す）
    CHECK(vkBeginCommandBuffer(cmd[0], &bi));
    vkCmdCopyBuffer(cmd[0], buf[0], buf[1], 1, &whole);
    CHECK(vkEndCommandBuffer(cmd[0]));
    CHECK(vkBeginCommandBuffer(cmd[1], &bi));
    vkCmdCopyBuffer(cmd[1], buf[1], buf[2], 1, &whole);
    VkMemoryBarrier bar = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &bar, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cmd[1]));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));

    // 最初に全部を書く（1 回目は全部送る）
    for (VkDeviceSize i = 0; i < bytes; i++) src[i] = (uint8_t) (i * 131 + 7);
    double t_first = 0, t_rest = 0, t_all = now_ms();
    clock_t c_all = clock();
    uint32_t seed = 12345;
    for (int it = 0; it < iters; it++) {
        // 毎回 touch ページの 1 か所ずつに、回の番号を書く
        for (uint32_t k = 0; k < touch && it > 0; k++) {
            seed = seed * 1103515245u + 12345u;
            uint32_t pg = (seed >> 8) % pages;
            src[(size_t) pg * 4096 + (seed & 4095)] = (uint8_t) (it & 0xff);
        }
        double t0 = now_ms();
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd[0] };
        CHECK(vkQueueSubmit(q, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(dev, 1, &fence));
        double t = now_ms() - t0;
        if (it == 0) t_first = t; else t_rest += t;
    }
    // 読み戻して確かめる
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd[1] };
    CHECK(vkQueueSubmit(q, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
    t_all = now_ms() - t_all;
    double cpu = (double) (clock() - c_all) * 1000.0 / CLOCKS_PER_SEC;
    size_t bad = 0;
    for (VkDeviceSize i = 0; i < bytes; i++) if (rb[i] != src[i]) bad++;
    // 待ちは往復せずに答えることがある（GPU の仕事は受け手にたまる）ので、最後の読み戻しまでの全体の時間と、このプロセスの CPU 時間も出す
    printf("%u MB・%d 回・1 回に %u ページを書いた：1 回目 %.1f ms・2 回目からの平均 %.2f ms・最後の読み戻しまで全体 %.0f ms・プロセスの CPU %.0f ms・"
           "読み戻した中身 %s（違うバイト %zu）\n", mb, iters, touch, t_first, iters > 1 ? t_rest / (iters - 1) : 0, t_all, cpu, bad ? "まちがい" : "全部正しい", bad);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return bad ? 1 : 0;
}
