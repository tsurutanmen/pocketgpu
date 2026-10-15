// 熱の試験用：空の command buffer を指定の秒数だけ submit し続ける（present は無い＝AI の計算のような仕事）。
//   thermloop.exe [秒] [装置の名前の一部]
// 組み方：zig cc -target x86_64-windows-gnu -O2 -Wall -I<Vulkan-Headers>/include vk/tests/thermloop.c -o thermloop.exe（build_win.sh の tests と同じ。vulkan-1.dll は実行のときに読む）
// 3 秒ごとに submit の数を出す。VR_GET_STATUS の問い合わせは queue_submit からも出るので、present が無くても熱が記録に出るはず
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    double secs = argc > 1 ? atof(argv[1]) : 10;
    const char *want = argc > 2 ? argv[2] : "iPhone";
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    gipa = (PFN_vkGetInstanceProcAddr) GetProcAddress(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr);
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
    DFN(vkGetDeviceQueue); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer);
    DFN(vkCreateFence); DFN(vkQueueSubmit); DFN(vkWaitForFences); DFN(vkResetFences);
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qf };
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &bi));
    CHECK(vkEndCommandBuffer(cb));
    VkFence fence;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    double t0 = now_ms(), tl = t0;
    unsigned long n = 0, nl = 0;
    while (now_ms() - t0 < secs * 1000) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        CHECK(vkQueueSubmit(q, 1, &si, fence));
        CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
        CHECK(vkResetFences(dev, 1, &fence));
        n++;
        Sleep(5);
        if (now_ms() - tl >= 3000) {
            printf("submit %lu 回（この 3 秒で %lu）\n", n, n - nl);
            fflush(stdout);
            tl = now_ms();
            nl = n;
        }
    }
    printf("終了：submit %lu 回・%.1f 秒\n", n, (now_ms() - t0) / 1000);
    return 0;
}
