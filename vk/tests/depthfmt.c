// 深さの形式ごとに、GPU が名乗る「読める・滑らかに読める・深さとして描ける」を出す
//   iphone-vk.cmd depthfmt.exe
#include <stdio.h>
#include <vulkan/vulkan.h>

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst;
    if (vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) { printf("インスタンスを作れない\n"); return 1; }
    uint32_t n = 8;
    VkPhysicalDevice pd[8];
    vkEnumeratePhysicalDevices(inst, &n, pd);
    const struct { VkFormat f; const char *name; } fs[] = {
        { VK_FORMAT_D16_UNORM, "D16_UNORM" }, { VK_FORMAT_X8_D24_UNORM_PACK32, "X8_D24_UNORM" }, { VK_FORMAT_D32_SFLOAT, "D32_SFLOAT" },
        { VK_FORMAT_D16_UNORM_S8_UINT, "D16_UNORM_S8" }, { VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8" }, { VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8" },
    };
    for (uint32_t d = 0; d < n; d++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[d], &p);
        printf("%s\n", p.deviceName);
        for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(pd[d], fs[i].f, &fp);
            VkFormatFeatureFlags o = fp.optimalTilingFeatures;
            printf("  %-14s 読める %d・滑らかに %d・深さに描ける %d\n", fs[i].name, !!(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
                   !!(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT), !!(o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
        }
    }
    vkDestroyInstance(inst, NULL);
    return 0;
}
