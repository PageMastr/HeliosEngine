/* Fake VK_LAYER_KHRONOS_validation layers for rendertest.validation-required (validation_checks.cmake),
 * built for Linux test configurations only (tools/rendertest/CMakeLists.txt); never installed or shipped.
 *   FAKE_LAYER_RESULT=<VkResult>: vkCreateInstance fails with it, so the check can see that a required
 *     layer that loads but fails is never read as "no Vulkan device".
 *   FAKE_LAYER_PASSTHROUGH: a working layer that forwards every call down the chain but is not the
 *     validator. It is listed under the validator's name, and the loader accepts it and puts it in the
 *     chain, yet nothing validates: the RHI must not report validation as active (it asks the chain
 *     for a validation tool, vkGetPhysicalDeviceToolProperties), so --require-validation fails. */

#include <stddef.h>
#include <string.h>

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#define FAKE_LAYER_EXPORT __attribute__((visibility("default")))

FAKE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeGetInstanceProcAddr(VkInstance instance,
                                                                                     const char* name);
FAKE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeGetDeviceProcAddr(VkDevice device, const char* name);

#if defined(FAKE_LAYER_PASSTHROUGH)

/* One chain configuration per process (the tests create instances one after another), so the next
 * layer's entry points can be global. */
static PFN_vkGetInstanceProcAddr g_nextInstanceProcAddr;
static PFN_vkGetDeviceProcAddr g_nextDeviceProcAddr;

static VKAPI_ATTR VkResult VKAPI_CALL fakeCreateInstance(const VkInstanceCreateInfo* info,
                                                         const VkAllocationCallbacks* allocator,
                                                         VkInstance* instance) {
    VkLayerInstanceCreateInfo* link = (VkLayerInstanceCreateInfo*)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO)) {
        link = (VkLayerInstanceCreateInfo*)link->pNext;
    }
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext; /* the next layer reads its own link */
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)next(NULL, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    g_nextInstanceProcAddr = next;
    return create(info, allocator, instance);
}

static VKAPI_ATTR VkResult VKAPI_CALL fakeCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
                                                       const VkAllocationCallbacks* allocator, VkDevice* device) {
    VkLayerDeviceCreateInfo* link = (VkLayerDeviceCreateInfo*)info->pNext;
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO)) {
        link = (VkLayerDeviceCreateInfo*)link->pNext;
    }
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr nextInstance = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    g_nextDeviceProcAddr = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)nextInstance(NULL, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    return create(physicalDevice, info, allocator, device);
}

FAKE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeGetDeviceProcAddr(VkDevice device, const char* name) {
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)fakeGetDeviceProcAddr;
    return g_nextDeviceProcAddr ? g_nextDeviceProcAddr(device, name) : NULL;
}

FAKE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeGetInstanceProcAddr(VkInstance instance,
                                                                                     const char* name) {
    if (strcmp(name, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)fakeCreateInstance;
    if (strcmp(name, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)fakeCreateDevice;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)fakeGetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)fakeGetDeviceProcAddr;
    return g_nextInstanceProcAddr ? g_nextInstanceProcAddr(instance, name) : NULL;
}

#else

#ifndef FAKE_LAYER_RESULT
#error "FAKE_LAYER_RESULT (the VkResult vkCreateInstance returns) or FAKE_LAYER_PASSTHROUGH must be defined"
#endif

static VKAPI_ATTR VkResult VKAPI_CALL fakeCreateInstance(const VkInstanceCreateInfo* info,
                                                         const VkAllocationCallbacks* allocator,
                                                         VkInstance* instance) {
    (void)info;
    (void)allocator;
    (void)instance;
    return FAKE_LAYER_RESULT;
}

FAKE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeGetDeviceProcAddr(VkDevice device, const char* name) {
    (void)device;
    (void)name;
    return NULL;
}

FAKE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fakeGetInstanceProcAddr(VkInstance instance,
                                                                                     const char* name) {
    (void)instance;
    if (strcmp(name, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)fakeCreateInstance;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)fakeGetInstanceProcAddr;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)fakeGetDeviceProcAddr;
    return NULL;
}

#endif
