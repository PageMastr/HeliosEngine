// The single translation unit that compiles the Vulkan Memory Allocator implementation. Function
// pointers come from volk (vmaImportVulkanFunctionsFromVolk), never from a static libvulkan link.
// VMA_STATIC_VULKAN_FUNCTIONS=0 / VMA_DYNAMIC_VULKAN_FUNCTIONS=1 are set for the whole module in
// engine/rhi/CMakeLists.txt so every TU sees the same configuration.

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#pragma warning(disable : 4100 4127 4189 4324 4505)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

#define VMA_IMPLEMENTATION
#include "vk_common.h"

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
