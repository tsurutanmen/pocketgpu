// 見えている GPU ごとに、名乗っている装置の拡張を全部出す。
//   extlist.exe [名前の一部]   名前の一部を付けると、その GPU だけ出す
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance in;
    if (vkCreateInstance(&ci, NULL, &in) != VK_SUCCESS) { printf("instance を作れない\n"); return 1; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(in, &n, NULL);
    VkPhysicalDevice *pd = calloc(n ? n : 1, sizeof *pd);
    vkEnumeratePhysicalDevices(in, &n, pd);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (argc > 1 && !strstr(p.deviceName, argv[1])) continue;
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd[i], NULL, &ne, NULL);
        VkExtensionProperties *e = calloc(ne ? ne : 1, sizeof *e);
        vkEnumerateDeviceExtensionProperties(pd[i], NULL, &ne, e);
        printf("%s : %u\n", p.deviceName, ne);
        for (uint32_t k = 0; k < ne; k++) printf("  %s %u\n", e[k].extensionName, e[k].specVersion);
        free(e);
    }
    vkDestroyInstance(in, NULL);
    return 0;
}
