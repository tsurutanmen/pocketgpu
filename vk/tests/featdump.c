// 装置の機能を全部 0・1 で出す（32 ビットと 64 ビットのドライバで読み比べる）
//   iphone-vk.cmd featdump.exe
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

static void dump(const char *name, const void *p, size_t off, size_t size) {
    const VkBool32 *b = (const VkBool32 *) ((const char *) p + off);
    printf("%s ", name);
    for (size_t i = 0; i < (size - off) / sizeof(VkBool32); i++) putchar(b[i] ? '1' : '0');
    putchar('\n');
}

int main(void) {
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst;
    if (vkCreateInstance(&ci, NULL, &inst) != VK_SUCCESS) { printf("インスタンスを作れない\n"); return 1; }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(pd, &pr);
    printf("%s api %u.%u limits maxImageDimension2D %u maxBoundDescriptorSets %u minMemoryMapAlignment %u nonCoherentAtomSize %llu\n", pr.deviceName,
           VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion), pr.limits.maxImageDimension2D, pr.limits.maxBoundDescriptorSets,
           (unsigned) pr.limits.minMemoryMapAlignment, (unsigned long long) pr.limits.nonCoherentAtomSize);
    VkPhysicalDeviceTransformFeedbackFeaturesEXT tf = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
    VkPhysicalDeviceDepthClipEnableFeaturesEXT dc = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, .pNext = &tf };
    VkPhysicalDeviceRobustness2FeaturesEXT r2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = &dc };
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cb = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .pNext = &r2 };
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vd = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .pNext = &cb };
    VkPhysicalDeviceVulkan13Features v13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &vd };
    VkPhysicalDeviceVulkan12Features v12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13 };
    VkPhysicalDeviceVulkan11Features v11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v11 };
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceFeatures f1;
    vkGetPhysicalDeviceFeatures(pd, &f1);
    dump("core1", &f1, 0, sizeof f1);
    dump("core2", &f2.features, 0, sizeof f2.features);
    dump("v11", &v11, offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess), sizeof v11);
    dump("v12", &v12, offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge), sizeof v12);
    dump("v13", &v13, offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess), sizeof v13);
    dump("div", &vd, offsetof(VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT, vertexAttributeInstanceRateDivisor), sizeof vd);
    dump("border", &cb, offsetof(VkPhysicalDeviceCustomBorderColorFeaturesEXT, customBorderColors), sizeof cb);
    dump("robust2", &r2, offsetof(VkPhysicalDeviceRobustness2FeaturesEXT, robustBufferAccess2), sizeof r2);
    dump("depthclip", &dc, offsetof(VkPhysicalDeviceDepthClipEnableFeaturesEXT, depthClipEnable), sizeof dc);
    dump("xfb", &tf, offsetof(VkPhysicalDeviceTransformFeedbackFeaturesEXT, transformFeedback), sizeof tf);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    printf("拡張 %u 個\n", ne);
    vkDestroyInstance(inst, NULL);
    return 0;
}
