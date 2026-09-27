/* A fake VK_LAYER_KHRONOS_validation for rendertest.validation-required (validation_checks.cmake):
 * its vkCreateInstance fails with FAKE_LAYER_RESULT, so the check can see that a required layer that
 * loads but fails is never read as "no Vulkan device". Built for Linux test configurations only
 * (tools/rendertest/CMakeLists.txt); never installed or shipped. */

#include <string.h>

#include <vulkan/vulkan.h>

#ifndef FAKE_LAYER_RESULT
#error "FAKE_LAYER_RESULT (the VkResult vkCreateInstance returns) must be defined"
#endif

#define FAKE_LAYER_EXPORT __attribute__((visibility("default")))

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
