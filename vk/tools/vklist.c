// Vulkan の装置の一覧を出すだけの小さな道具（vulkaninfo の代わり）
#include <stdio.h>
#include <windows.h>
#include <vulkan/vulkan.h>

int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { printf("vulkan-1.dll が無い\n"); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr) GetProcAddress(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    VkResult r = create(&ci, NULL, &inst);
    if (r != VK_SUCCESS) { printf("vkCreateInstance %d\n", r); return 1; }
    PFN_vkEnumeratePhysicalDevices enumd = (PFN_vkEnumeratePhysicalDevices) gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties) gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties memp = (PFN_vkGetPhysicalDeviceMemoryProperties) gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    uint32_t n = 0;
    enumd(inst, &n, NULL);
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    enumd(inst, &n, devs);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceMemoryProperties m;
        props(devs[i], &p);
        memp(devs[i], &m);
        unsigned long long heap = 0;
        for (uint32_t h = 0; h < m.memoryHeapCount; h++)
            if (m.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap += m.memoryHeaps[h].size;
        printf("GPU%u: %s  (vendor 0x%04x, type %d, Vulkan %u.%u, %llu MiB)\n", i, p.deviceName, p.vendorID, p.deviceType,
               VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), heap >> 20);
    }
    return 0;
}
