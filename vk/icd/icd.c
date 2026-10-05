// iPhone を Windows の Vulkan の GPU として見せるドライバ（ICD）。
// いまは殻だけ：装置の名前と性質を答えるところまで。計算の中身は後で USB の先の iPhone に送る。
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#define EXPORT __declspec(dllexport)

struct inst { VK_LOADER_DATA loader; };
struct phys { VK_LOADER_DATA loader; };

static struct phys g_phys;
static const char *DEVICE_NAME = "Apple A19 Pro GPU (iPhone 17 Pro, USB)";

static VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void) ci; (void) a;
    struct inst *i = calloc(1, sizeof *i);
    if (!i) return VK_ERROR_OUT_OF_HOST_MEMORY;
    set_loader_magic_value(i);
    set_loader_magic_value(&g_phys);
    *out = (VkInstance) i;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_instance(VkInstance i, const VkAllocationCallbacks *a) {
    (void) a;
    free(i);
}

static VkResult VKAPI_CALL enum_instance_ext(const char *layer, uint32_t *n, VkExtensionProperties *p) {
    (void) layer; (void) p;
    *n = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL enum_instance_version(uint32_t *v) {
    *v = VK_API_VERSION_1_2;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL enum_phys(VkInstance i, uint32_t *n, VkPhysicalDevice *out) {
    (void) i;
    if (!out) { *n = 1; return VK_SUCCESS; }
    if (*n < 1) return VK_INCOMPLETE;
    out[0] = (VkPhysicalDevice) &g_phys;
    *n = 1;
    return VK_SUCCESS;
}

static void VKAPI_CALL get_props(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) {
    (void) pd;
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_API_VERSION_1_2;
    p->driverVersion = VK_MAKE_VERSION(0, 1, 0);
    p->vendorID = 0x106B; // Apple
    p->deviceID = 0xA019;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    strncpy(p->deviceName, DEVICE_NAME, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    p->limits.maxComputeWorkGroupCount[0] = p->limits.maxComputeWorkGroupCount[1] = p->limits.maxComputeWorkGroupCount[2] = 65535;
    p->limits.maxComputeWorkGroupSize[0] = p->limits.maxComputeWorkGroupSize[1] = p->limits.maxComputeWorkGroupSize[2] = 1024;
    p->limits.maxComputeWorkGroupInvocations = 1024;
    p->limits.maxComputeSharedMemorySize = 32768;
    p->limits.maxPushConstantsSize = 128;
    p->limits.maxStorageBufferRange = 1u << 30;
    p->limits.maxBoundDescriptorSets = 8;
    p->limits.minStorageBufferOffsetAlignment = 16;
    p->limits.nonCoherentAtomSize = 64;
}

static void VKAPI_CALL get_props2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) {
    get_props(pd, &p->properties);
    for (VkBaseOutStructure *s = (VkBaseOutStructure *) p->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES) {
            VkPhysicalDeviceDriverProperties *d = (VkPhysicalDeviceDriverProperties *) s;
            d->driverID = VK_DRIVER_ID_MOLTENVK;
            strncpy(d->driverName, "iphone-rpc", VK_MAX_DRIVER_NAME_SIZE - 1);
            strncpy(d->driverInfo, "Vulkan over USB to MoltenVK on iPhone", VK_MAX_DRIVER_INFO_SIZE - 1);
        }
    }
}

static void VKAPI_CALL get_features(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    (void) pd;
    memset(f, 0, sizeof *f);
    f->shaderInt16 = VK_TRUE;
}

static void VKAPI_CALL get_features2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    get_features(pd, &f->features);
}

static void VKAPI_CALL get_queue_families(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *q) {
    (void) pd;
    if (!q) { *n = 1; return; }
    if (*n < 1) return;
    memset(q, 0, sizeof *q);
    q->queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    q->queueCount = 1;
    q->timestampValidBits = 64;
    q->minImageTransferGranularity = (VkExtent3D) {1, 1, 1};
    *n = 1;
}

static void VKAPI_CALL get_queue_families2(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *q) {
    if (!q) { get_queue_families(pd, n, NULL); return; }
    uint32_t c = *n;
    if (c >= 1) get_queue_families(pd, &c, &q[0].queueFamilyProperties);
    *n = c;
}

static void VKAPI_CALL get_mem(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m) {
    (void) pd;
    memset(m, 0, sizeof *m);
    // iPhone はメモリが 1 種類（CPU と GPU で同じもの）
    m->memoryHeapCount = 1;
    m->memoryHeaps[0].size = 5ull << 30;
    m->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    m->memoryTypeCount = 1;
    m->memoryTypes[0].heapIndex = 0;
    m->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

static void VKAPI_CALL get_mem2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *m) {
    get_mem(pd, &m->memoryProperties);
}

static void VKAPI_CALL get_format(VkPhysicalDevice pd, VkFormat f, VkFormatProperties *p) {
    (void) pd; (void) f;
    memset(p, 0, sizeof *p);
}

static VkResult VKAPI_CALL enum_device_ext(VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    (void) pd; (void) layer; (void) p;
    *n = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void) pd; (void) ci; (void) a; (void) out;
    return VK_ERROR_INITIALIZATION_FAILED; // まだ iPhone につないでいない
}

// ローダーが必ず求める入口。画像はまだ扱わない
static VkResult VKAPI_CALL get_image_format(VkPhysicalDevice pd, VkFormat f, VkImageType t, VkImageTiling ti, VkImageUsageFlags u, VkImageCreateFlags fl, VkImageFormatProperties *p) {
    (void) pd; (void) f; (void) t; (void) ti; (void) u; (void) fl;
    memset(p, 0, sizeof *p);
    return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

static void VKAPI_CALL get_sparse_format(VkPhysicalDevice pd, VkFormat f, VkImageType t, VkSampleCountFlagBits s, VkImageUsageFlags u, VkImageTiling ti, uint32_t *n, VkSparseImageFormatProperties *p) {
    (void) pd; (void) f; (void) t; (void) s; (void) u; (void) ti; (void) p;
    *n = 0;
}

static VkResult VKAPI_CALL enum_device_layers(VkPhysicalDevice pd, uint32_t *n, VkLayerProperties *p) {
    (void) pd; (void) p;
    *n = 0;
    return VK_SUCCESS;
}

static PFN_vkVoidFunction VKAPI_CALL get_device_proc(VkDevice d, const char *pName) {
    (void) d; (void) pName;
    return NULL;
}

#define F(name, fn) if (!strcmp(pName, name)) return (PFN_vkVoidFunction) fn
static PFN_vkVoidFunction lookup(const char *pName) {
    F("vkCreateInstance", create_instance);
    F("vkDestroyInstance", destroy_instance);
    F("vkEnumerateInstanceExtensionProperties", enum_instance_ext);
    F("vkEnumerateInstanceVersion", enum_instance_version);
    F("vkEnumeratePhysicalDevices", enum_phys);
    F("vkGetPhysicalDeviceProperties", get_props);
    F("vkGetPhysicalDeviceProperties2", get_props2);
    F("vkGetPhysicalDeviceProperties2KHR", get_props2);
    F("vkGetPhysicalDeviceFeatures", get_features);
    F("vkGetPhysicalDeviceFeatures2", get_features2);
    F("vkGetPhysicalDeviceFeatures2KHR", get_features2);
    F("vkGetPhysicalDeviceQueueFamilyProperties", get_queue_families);
    F("vkGetPhysicalDeviceQueueFamilyProperties2", get_queue_families2);
    F("vkGetPhysicalDeviceQueueFamilyProperties2KHR", get_queue_families2);
    F("vkGetPhysicalDeviceMemoryProperties", get_mem);
    F("vkGetPhysicalDeviceMemoryProperties2", get_mem2);
    F("vkGetPhysicalDeviceMemoryProperties2KHR", get_mem2);
    F("vkGetPhysicalDeviceFormatProperties", get_format);
    F("vkEnumerateDeviceExtensionProperties", enum_device_ext);
    F("vkCreateDevice", create_device);
    F("vkGetPhysicalDeviceImageFormatProperties", get_image_format);
    F("vkGetPhysicalDeviceSparseImageFormatProperties", get_sparse_format);
    F("vkEnumerateDeviceLayerProperties", enum_device_layers);
    F("vkGetDeviceProcAddr", get_device_proc);
    return NULL;
}

EXPORT VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *v) {
    if (*v > 5) *v = 5;
    return VK_SUCCESS;
}

EXPORT PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance i, const char *pName) {
    (void) i;
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction) vk_icdGetInstanceProcAddr;
    return lookup(pName);
}

EXPORT PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance i, const char *pName) {
    (void) i;
    return lookup(pName);
}
