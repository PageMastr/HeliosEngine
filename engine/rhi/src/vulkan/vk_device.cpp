// Vulkan 1.3 device: instance/adapter/device creation, resources, bindless heap, pipelines,
// submission, frames, deferred deletion and diagnostics. See vk_device.h for the structure.

#include "vk_device.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>

#include "helios/core/jobs.h"

namespace helios::rhi::vk {
namespace {

constexpr u32 kTargetSampledImages = 131072;  // 03 §1.1
constexpr u32 kTargetStorageImages = 16384;
constexpr u32 kTargetStorageBuffers = 65536;
constexpr u32 kTargetSamplers = 128;
constexpr u64 kBreadcrumbBytes = kQueueCount * 2 * sizeof(u32);

// volkLoadInstanceTable() writes volk's global vkGetDeviceProcAddr, which volkLoadDeviceTable()
// then reads: serialize table loading across devices created on different threads.
std::mutex g_volkMutex;

Result<void> ensureVolk() {
    static std::once_flag once;
    static VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    std::call_once(once, [] { result = volkInitialize(); });
    if (result != VK_SUCCESS) {
        return Error{ErrorCode::Unsupported, "Vulkan loader not found (volkInitialize failed)"};
    }
    return {};
}

std::string versionString(u32 v) {
    return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(),
                       [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

std::vector<VkExtensionProperties> deviceExtensions(const VolkInstanceTable& vki, VkPhysicalDevice pd) {
    u32 count = 0;
    vki.vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vki.vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data());
    exts.resize(count);
    return exts;
}

struct FeatureChain {
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};

    FeatureChain() = default;
    FeatureChain(const FeatureChain&) = delete;
    FeatureChain& operator=(const FeatureChain&) = delete;

    void link(bool withFault) {
        core.pNext = &v11;
        v11.pNext = &v12;
        v12.pNext = &v13;
        v13.pNext = withFault ? static_cast<void*>(&fault) : nullptr;
        fault.pNext = nullptr;
    }
};

struct PropertyChain {
    VkPhysicalDeviceProperties2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan11Properties v11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};

    PropertyChain(const VolkInstanceTable& vki, VkPhysicalDevice pd) {
        core.pNext = &v11;
        v11.pNext = &v12;
        vki.vkGetPhysicalDeviceProperties2(pd, &core);
    }
    PropertyChain(const PropertyChain&) = delete;
    PropertyChain& operator=(const PropertyChain&) = delete;
};

/// Empty string when the adapter meets every Helios requirement, else the first missing one.
std::string checkRequirements(const VolkInstanceTable& vki, VkPhysicalDevice pd) {
    VkPhysicalDeviceProperties props{};
    vki.vkGetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) return "Vulkan 1.3 (device reports " + versionString(props.apiVersion) + ")";
    FeatureChain f;
    f.link(false);
    vki.vkGetPhysicalDeviceFeatures2(pd, &f.core);
    struct Req {
        VkBool32 value;
        const char* name;
    };
    const Req reqs[] = {
        {f.v13.dynamicRendering, "dynamicRendering"},
        {f.v13.synchronization2, "synchronization2"},
        {f.v13.maintenance4, "maintenance4"},
        {f.v12.timelineSemaphore, "timelineSemaphore"},
        {f.v12.bufferDeviceAddress, "bufferDeviceAddress"},
        {f.v12.descriptorIndexing, "descriptorIndexing"},
        {f.v12.runtimeDescriptorArray, "runtimeDescriptorArray"},
        {f.v12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound"},
        {f.v12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind"},
        {f.v12.descriptorBindingStorageImageUpdateAfterBind, "descriptorBindingStorageImageUpdateAfterBind"},
        {f.v12.descriptorBindingStorageBufferUpdateAfterBind, "descriptorBindingStorageBufferUpdateAfterBind"},
        {f.v12.descriptorBindingUpdateUnusedWhilePending, "descriptorBindingUpdateUnusedWhilePending"},
        {f.v12.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing"},
        {f.v12.shaderStorageBufferArrayNonUniformIndexing, "shaderStorageBufferArrayNonUniformIndexing"},
        {f.v12.shaderStorageImageArrayNonUniformIndexing, "shaderStorageImageArrayNonUniformIndexing"},
        {f.v12.drawIndirectCount, "drawIndirectCount"},
        {f.v12.scalarBlockLayout, "scalarBlockLayout"},
        {f.v12.samplerFilterMinmax, "samplerFilterMinmax"},
        {f.v11.shaderDrawParameters, "shaderDrawParameters"},
        {f.core.features.multiDrawIndirect, "multiDrawIndirect"},
        {f.core.features.drawIndirectFirstInstance, "drawIndirectFirstInstance"},
        {f.core.features.independentBlend, "independentBlend"},
        {f.core.features.imageCubeArray, "imageCubeArray"},
        {f.core.features.fragmentStoresAndAtomics, "fragmentStoresAndAtomics"},
        {f.core.features.textureCompressionBC, "textureCompressionBC"},
    };
    for (const Req& r : reqs) {
        if (!r.value) return r.name;
    }
    u32 familyCount = 0;
    vki.vkGetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vki.vkGetPhysicalDeviceQueueFamilyProperties(pd, &familyCount, families.data());
    const bool hasGraphics = std::any_of(families.begin(), families.end(), [](const VkQueueFamilyProperties& q) {
        return (q.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
               (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
    });
    if (!hasGraphics) return "a graphics+compute queue family";
    PropertyChain p(vki, pd);
    const VkSubgroupFeatureFlags needed = VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    if ((p.v11.subgroupSupportedOperations & needed) != needed) return "subgroup ballot + arithmetic";
    return {};
}

AdapterType adapterType(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return AdapterType::Integrated;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return AdapterType::Discrete;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return AdapterType::Virtual;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return AdapterType::Cpu;
    default: return AdapterType::Other;
    }
}

AdapterInfo describeAdapter(const VolkInstanceTable& vki, VkPhysicalDevice pd, u32 index) {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    driver.pNext = &ids;
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &driver;
    vki.vkGetPhysicalDeviceProperties2(pd, &props);
    VkPhysicalDeviceMemoryProperties mem{};
    vki.vkGetPhysicalDeviceMemoryProperties(pd, &mem);

    AdapterInfo info;
    info.index = index;
    info.name = props.properties.deviceName;
    info.type = adapterType(props.properties.deviceType);
    info.vendorId = props.properties.vendorID;
    info.deviceId = props.properties.deviceID;
    info.driverName = driver.driverName;
    info.driverInfo = driver.driverInfo;
    info.driverVersion = props.properties.driverVersion;
    info.apiVersion = props.properties.apiVersion;
    std::memcpy(info.deviceUuid.data(), ids.deviceUUID, info.deviceUuid.size());
    for (u32 i = 0; i < mem.memoryHeapCount; ++i) {
        if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) info.deviceLocalBytes += mem.memoryHeaps[i].size;
    }
    info.missingRequirement = checkRequirements(vki, pd);
    info.meetsRequirements = info.missingRequirement.empty();
    return info;
}

int adapterScore(AdapterType type, AdapterPreference pref) {
    switch (pref) {
    case AdapterPreference::HighPerformance:
        return type == AdapterType::Discrete ? 4 : type == AdapterType::Integrated ? 3 : type == AdapterType::Virtual ? 2
                                               : type == AdapterType::Cpu          ? 1 : 0;
    case AdapterPreference::LowPower:
        return type == AdapterType::Integrated ? 4 : type == AdapterType::Discrete ? 3 : type == AdapterType::Virtual ? 2
                                                 : type == AdapterType::Cpu        ? 1 : 0;
    case AdapterPreference::Software:
        return type == AdapterType::Cpu ? 4 : type == AdapterType::Virtual ? 3 : type == AdapterType::Integrated ? 2
                                          : type == AdapterType::Discrete ? 1 : 0;
    }
    return 0;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// Minimal instance for adapter enumeration; `vki` receives its function table.
Result<VkInstance> createEnumerationInstance(VolkInstanceTable& vki) {
    HELIOS_TRY(ensureVolk());
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        return Error{ErrorCode::Unsupported, "Vulkan 1.3 loader required, found " + versionString(volkGetInstanceVersion())};
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Helios adapter enumeration";
    app.pEngineName = "Helios";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult r = vkCreateInstance(&ci, nullptr, &instance);
    if (r != VK_SUCCESS) return makeError(ErrorCode::Unsupported, "vkCreateInstance failed: {}", resultName(r));
    std::lock_guard lock(g_volkMutex);
    volkLoadInstanceTable(&vki, instance);
    return instance;
}

} // namespace

// =================================================================================================
// Creation
// =================================================================================================
VulkanDevice::VulkanDevice(const DeviceDesc& desc) : m_desc(desc) {
    m_caps.backend = Backend::Vulkan;
    m_frames.resize(std::max<u32>(desc.framesInFlight, 1));
}

Result<void> VulkanDevice::init() {
    HELIOS_TRY(createInstance());
    HELIOS_TRY(selectPhysicalDevice());
    HELIOS_TRY(createLogicalDevice());
    HELIOS_TRY(createAllocator());
    HELIOS_TRY(createBindlessHeap());
    fillCaps();
    HELIOS_TRY(createDefaultResources());
    HELIOS_LOG_INFO(LogRhi, "Vulkan device: {} ({}, driver {} {}, API {}), queues: compute {}, transfer {}",
                    m_caps.adapter.name, adapterTypeName(m_caps.adapter.type), m_caps.adapter.driverName,
                    m_caps.adapter.driverInfo, versionString(m_caps.adapter.apiVersion),
                    m_caps.has(CapBit::AsyncComputeQueue) ? "dedicated" : "aliased",
                    m_caps.has(CapBit::TransferQueue) ? "dedicated" : "aliased");
    return {};
}

Result<void> VulkanDevice::createInstance() {
    HELIOS_TRY(ensureVolk());
    const u32 loaderVersion = volkGetInstanceVersion();
    if (loaderVersion < VK_API_VERSION_1_3) {
        return Error{ErrorCode::Unsupported, "Vulkan 1.3 loader required, found " + versionString(loaderVersion)};
    }

    u32 layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    u32 extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());

    std::vector<const char*> enabledLayers;
    std::vector<const char*> enabledExts;
    if (m_desc.validation || m_desc.requireValidation) {
        const bool available = std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
            return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (available) {
            enabledLayers.push_back("VK_LAYER_KHRONOS_validation");
            m_validationLayer = true;
        } else if (m_desc.requireValidation) {
            return Error{ErrorCode::Unsupported, "VK_LAYER_KHRONOS_validation is required but not installed"};
        } else {
            HELIOS_LOG_INFO(LogRhi, "Vulkan validation requested but VK_LAYER_KHRONOS_validation is not installed");
        }
    }
    if ((m_desc.debugNames || m_validationLayer) && hasExtension(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
        hasFlag(m_desc.capsMask, CapBit::DebugUtils)) {
        enabledExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        m_debugUtils = true;
    }
    if (m_desc.enableSwapchain && hasExtension(exts, VK_KHR_SURFACE_EXTENSION_NAME) &&
        hasFlag(m_desc.capsMask, CapBit::Swapchain)) {
        enabledExts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        // Whatever SDL may need on this platform (it creates the surface itself).
        // VK_EXT_headless_surface backs SDL's "offscreen" video driver (windowless swapchain tests).
        for (const char* name : {"VK_KHR_win32_surface", "VK_KHR_xlib_surface", "VK_KHR_xcb_surface",
                                 "VK_KHR_wayland_surface", "VK_EXT_headless_surface"}) {
            if (hasExtension(exts, name)) enabledExts.push_back(name);
        }
        m_surfaceExtensions = true;
    }

    const std::string appName(m_desc.appName);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = appName.c_str();
    app.pEngineName = "Helios";
    app.engineVersion = VK_MAKE_API_VERSION(0, HELIOS_VERSION_MAJOR, HELIOS_VERSION_MINOR, HELIOS_VERSION_PATCH);
    app.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messengerInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messengerInfo.pfnUserCallback = &VulkanDevice::debugCallback;
    messengerInfo.pUserData = this;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = static_cast<u32>(enabledLayers.size());
    ci.ppEnabledLayerNames = enabledLayers.data();
    ci.enabledExtensionCount = static_cast<u32>(enabledExts.size());
    ci.ppEnabledExtensionNames = enabledExts.data();
    if (m_debugUtils) ci.pNext = &messengerInfo;  // also reports instance creation/destruction issues
    const VkResult r = vkCreateInstance(&ci, nullptr, &m_instance);
    if (r != VK_SUCCESS) return makeError(ErrorCode::Unsupported, "vkCreateInstance failed: {}", resultName(r));
    {
        std::lock_guard lock(g_volkMutex);
        volkLoadInstanceTable(&m_vki, m_instance);
    }
    if (m_debugUtils && m_vki.vkCreateDebugUtilsMessengerEXT) {
        m_vki.vkCreateDebugUtilsMessengerEXT(m_instance, &messengerInfo, nullptr, &m_messenger);
    }
    return {};
}

Result<void> VulkanDevice::selectPhysicalDevice() {
    u32 count = 0;
    m_vki.vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) return Error{ErrorCode::Unsupported, "no Vulkan physical devices"};
    std::vector<VkPhysicalDevice> devices(count);
    m_vki.vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
    devices.resize(count);
    std::vector<AdapterInfo> infos;
    for (u32 i = 0; i < count; ++i) infos.push_back(describeAdapter(m_vki, devices[i], i));

    i32 chosen = -1;
    if (auto env = detail::envVar("HELIOS_RHI_ADAPTER")) {
        const bool numeric = std::all_of(env->begin(), env->end(), [](char c) { return c >= '0' && c <= '9'; });
        if (numeric) {
            chosen = std::atoi(env->c_str());
        } else {
            const std::string needle = toLower(*env);
            for (const AdapterInfo& a : infos) {
                if (toLower(a.name).find(needle) != std::string::npos) {
                    chosen = static_cast<i32>(a.index);
                    break;
                }
            }
        }
        if (chosen < 0 || chosen >= static_cast<i32>(count)) {
            return makeError(ErrorCode::NotFound, "HELIOS_RHI_ADAPTER='{}' matches no adapter", *env);
        }
    } else if (m_desc.adapterIndex >= 0) {
        if (m_desc.adapterIndex >= static_cast<i32>(count)) {
            return makeError(ErrorCode::NotFound, "adapter index {} out of range ({} adapters)", m_desc.adapterIndex,
                             count);
        }
        chosen = m_desc.adapterIndex;
    } else {
        int best = -1;
        for (const AdapterInfo& a : infos) {
            if (!a.meetsRequirements) continue;
            const int score = adapterScore(a.type, m_desc.adapterPreference);
            if (score > best) {
                best = score;
                chosen = static_cast<i32>(a.index);
            }
        }
        if (chosen < 0) {
            std::string reasons;
            for (const AdapterInfo& a : infos) reasons += std::format(" [{}: missing {}]", a.name, a.missingRequirement);
            return Error{ErrorCode::Unsupported, "no Vulkan adapter meets Helios' requirements:" + reasons};
        }
    }
    const AdapterInfo& info = infos[static_cast<usize>(chosen)];
    if (!info.meetsRequirements) {
        return makeError(ErrorCode::Unsupported, "adapter '{}' lacks {}", info.name, info.missingRequirement);
    }
    m_physical = devices[static_cast<usize>(chosen)];
    m_caps.adapter = info;
    m_vki.vkGetPhysicalDeviceProperties(m_physical, &m_properties);
    u32 familyCount = 0;
    m_vki.vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &familyCount, nullptr);
    m_queueFamilies.resize(familyCount);
    m_vki.vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &familyCount, m_queueFamilies.data());
    return {};
}

Result<void> VulkanDevice::createLogicalDevice() {
    // -- Queues: graphics, then dedicated async compute / transfer if the adapter has them.
    constexpr VkQueueFlags kGC = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    u32 graphics = ~0u;
    for (u32 i = 0; i < m_queueFamilies.size(); ++i) {
        if ((m_queueFamilies[i].queueFlags & kGC) == kGC) {
            graphics = i;
            break;
        }
    }
    std::vector<u32> used(m_queueFamilies.size(), 0);
    auto take = [&](u32 family) -> std::pair<u32, u32> {
        const u32 index = used[family]++;
        return {family, index};
    };
    std::array<std::pair<u32, u32>, kQueueCount> assignment{};
    assignment[0] = take(graphics);
    bool asyncDedicated = false;
    bool transferDedicated = false;
    // Async compute: a compute family without graphics, else a second graphics-family queue.
    for (u32 i = 0; i < m_queueFamilies.size() && !asyncDedicated; ++i) {
        const VkQueueFlags f = m_queueFamilies[i].queueFlags;
        if ((f & VK_QUEUE_COMPUTE_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) && used[i] < m_queueFamilies[i].queueCount) {
            assignment[1] = take(i);
            asyncDedicated = true;
        }
    }
    if (!asyncDedicated && used[graphics] < m_queueFamilies[graphics].queueCount) {
        assignment[1] = take(graphics);
        asyncDedicated = true;
    }
    // Transfer: a transfer-only family (copy engine), else nothing dedicated.
    for (u32 i = 0; i < m_queueFamilies.size() && !transferDedicated; ++i) {
        const VkQueueFlags f = m_queueFamilies[i].queueFlags;
        if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) &&
            used[i] < m_queueFamilies[i].queueCount) {
            assignment[2] = take(i);
            transferDedicated = true;
        }
    }
    if (!hasFlag(m_desc.capsMask, CapBit::AsyncComputeQueue) && asyncDedicated) {
        used[assignment[1].first]--;
        asyncDedicated = false;
    }
    if (!hasFlag(m_desc.capsMask, CapBit::TransferQueue) && transferDedicated) {
        used[assignment[2].first]--;
        transferDedicated = false;
    }
    if (!asyncDedicated) assignment[1] = assignment[0];
    if (!transferDedicated) assignment[2] = assignment[0];

    std::vector<VkDeviceQueueCreateInfo> queueInfos;
    std::vector<std::vector<f32>> priorities(m_queueFamilies.size());
    for (u32 i = 0; i < m_queueFamilies.size(); ++i) {
        if (used[i] == 0) continue;
        priorities[i].assign(used[i], 1.0f);
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = i;
        qi.queueCount = used[i];
        qi.pQueuePriorities = priorities[i].data();
        queueInfos.push_back(qi);
    }

    // -- Extensions
    const std::vector<VkExtensionProperties> exts = deviceExtensions(m_vki, m_physical);
    std::vector<const char*> enabled;
    auto want = [&](const char* name, CapBit bit) {
        if (hasExtension(exts, name) && hasFlag(m_desc.capsMask, bit)) {
            enabled.push_back(name);
            m_enabledDeviceExtensions.emplace_back(name);
            return true;
        }
        return false;
    };
    m_swapchainExtension = m_surfaceExtensions && want(VK_KHR_SWAPCHAIN_EXTENSION_NAME, CapBit::Swapchain);
    m_memoryBudget = want(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, CapBit::MemoryBudget);

    // -- Features: all required ones plus the optional ones we use.
    FeatureChain supported;
    const bool faultAvailable = hasExtension(exts, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    supported.link(faultAvailable);
    m_vki.vkGetPhysicalDeviceFeatures2(m_physical, &supported.core);
    if (faultAvailable && supported.fault.deviceFault && hasFlag(m_desc.capsMask, CapBit::DeviceFault)) {
        m_deviceFault = want(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, CapBit::DeviceFault);
        m_deviceFaultVendorBinary = m_deviceFault && supported.fault.deviceFaultVendorBinary;
    }

    FeatureChain f;
    f.link(m_deviceFault);
    VkPhysicalDeviceFeatures& c = f.core.features;
    const VkPhysicalDeviceFeatures& s = supported.core.features;
    c.multiDrawIndirect = VK_TRUE;
    c.drawIndirectFirstInstance = VK_TRUE;
    c.independentBlend = VK_TRUE;
    c.imageCubeArray = VK_TRUE;
    c.fragmentStoresAndAtomics = VK_TRUE;
    c.textureCompressionBC = VK_TRUE;
    c.samplerAnisotropy = s.samplerAnisotropy && hasFlag(m_desc.capsMask, CapBit::SamplerAnisotropy);
    c.shaderInt64 = s.shaderInt64 && hasFlag(m_desc.capsMask, CapBit::ShaderInt64);
    c.fillModeNonSolid = s.fillModeNonSolid && hasFlag(m_desc.capsMask, CapBit::FillModeNonSolid);
    c.depthClamp = s.depthClamp;
    c.depthBiasClamp = s.depthBiasClamp;
    c.shaderInt16 = s.shaderInt16;
    c.vertexPipelineStoresAndAtomics = s.vertexPipelineStoresAndAtomics;
    c.shaderStorageImageReadWithoutFormat =
        s.shaderStorageImageReadWithoutFormat && hasFlag(m_desc.capsMask, CapBit::StorageImageWithoutFormat);
    c.shaderStorageImageWriteWithoutFormat =
        s.shaderStorageImageWriteWithoutFormat && hasFlag(m_desc.capsMask, CapBit::StorageImageWithoutFormat);
    f.v11.shaderDrawParameters = VK_TRUE;
    f.v11.storageBuffer16BitAccess = supported.v11.storageBuffer16BitAccess;
    f.v12.timelineSemaphore = VK_TRUE;
    f.v12.bufferDeviceAddress = VK_TRUE;
    f.v12.descriptorIndexing = VK_TRUE;
    f.v12.runtimeDescriptorArray = VK_TRUE;
    f.v12.descriptorBindingPartiallyBound = VK_TRUE;
    f.v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f.v12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    f.v12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    f.v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f.v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f.v12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    f.v12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    f.v12.drawIndirectCount = VK_TRUE;
    f.v12.scalarBlockLayout = VK_TRUE;
    f.v12.samplerFilterMinmax = VK_TRUE;
    f.v12.hostQueryReset = supported.v12.hostQueryReset;
    f.v12.shaderFloat16 = supported.v12.shaderFloat16 && hasFlag(m_desc.capsMask, CapBit::ShaderFloat16);
    f.v12.vulkanMemoryModel = supported.v12.vulkanMemoryModel;
    f.v12.vulkanMemoryModelDeviceScope = supported.v12.vulkanMemoryModelDeviceScope;
    f.v12.storageBuffer8BitAccess = supported.v12.storageBuffer8BitAccess;
    f.v12.shaderInt8 = supported.v12.shaderInt8;
    f.v13.dynamicRendering = VK_TRUE;
    f.v13.synchronization2 = VK_TRUE;
    f.v13.maintenance4 = VK_TRUE;
    if (m_deviceFault) {
        f.fault.deviceFault = VK_TRUE;
        f.fault.deviceFaultVendorBinary = m_deviceFaultVendorBinary ? VK_TRUE : VK_FALSE;
    }

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f.core;
    ci.queueCreateInfoCount = static_cast<u32>(queueInfos.size());
    ci.pQueueCreateInfos = queueInfos.data();
    ci.enabledExtensionCount = static_cast<u32>(enabled.size());
    ci.ppEnabledExtensionNames = enabled.data();
    const VkResult r = m_vki.vkCreateDevice(m_physical, &ci, nullptr, &m_device);
    if (r != VK_SUCCESS) return makeError(ErrorCode::Unsupported, "vkCreateDevice failed: {}", resultName(r));
    {
        std::lock_guard lock(g_volkMutex);
        volkLoadDeviceTable(&m_vk, m_device);
    }

    // -- Logical queues and their timelines
    for (u32 q = 0; q < kQueueCount; ++q) {
        QueueSlot& slot = m_queues[q];
        slot.family = assignment[q].first;
        slot.indexInFamily = assignment[q].second;
        slot.flags = m_queueFamilies[slot.family].queueFlags;
        m_vk.vkGetDeviceQueue(m_device, slot.family, slot.indexInFamily, &slot.queue);
        slot.submitMutex = &m_queueMutexes[q];
        for (u32 p = 0; p < q; ++p) {
            if (m_queues[p].queue == slot.queue) {
                slot.submitMutex = m_queues[p].submitMutex;  // aliased: VkQueue is externally synchronized
                break;
            }
        }
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        si.pNext = &type;
        const VkResult sr = m_vk.vkCreateSemaphore(m_device, &si, nullptr, &slot.timeline);
        if (sr != VK_SUCCESS) return vkError(sr, "vkCreateSemaphore(timeline)");
        const std::string name = std::format("{} timeline", queueName(static_cast<Queue>(q)));
        setObjectName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<u64>(slot.timeline), name);
    }
    if (asyncDedicated) m_caps.bits |= CapBit::AsyncComputeQueue;
    if (transferDedicated) m_caps.bits |= CapBit::TransferQueue;

    VkPipelineCacheCreateInfo pc{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pc.initialDataSize = m_desc.pipelineCacheData.size();
    pc.pInitialData = m_desc.pipelineCacheData.empty() ? nullptr : m_desc.pipelineCacheData.data();
    if (m_vk.vkCreatePipelineCache(m_device, &pc, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        pc.initialDataSize = 0;  // incompatible data: start empty
        pc.pInitialData = nullptr;
        m_vk.vkCreatePipelineCache(m_device, &pc, nullptr, &m_pipelineCache);
    }
    return {};
}

Result<void> VulkanDevice::createAllocator() {
    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (m_memoryBudget) ci.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    ci.physicalDevice = m_physical;
    ci.device = m_device;
    ci.instance = m_instance;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    // VMA fetches everything else through these for *this* instance/device
    // (VMA_DYNAMIC_VULKAN_FUNCTIONS=1). vmaImportVulkanFunctionsFromVolk() would copy volk's
    // process-global instance pointers, which this backend never loads (see vk_device.h).
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;  // loader entry point, instance-agnostic
    functions.vkGetDeviceProcAddr = m_vki.vkGetDeviceProcAddr;
    ci.pVulkanFunctions = &functions;
    VkResult r = vmaCreateAllocator(&ci, &m_allocator);
    if (r != VK_SUCCESS) return vkError(r, "vmaCreateAllocator");

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kBreadcrumbBytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    // Every queue family writes its own markers: share the buffer instead of transferring ownership.
    std::array<u32, kQueueCount> families{};
    u32 familyCount = 0;
    for (const QueueSlot& q : m_queues) {
        if (std::find(families.begin(), families.begin() + familyCount, q.family) == families.begin() + familyCount) {
            families[familyCount++] = q.family;
        }
    }
    if (familyCount > 1) {
        bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bi.queueFamilyIndexCount = familyCount;
        bi.pQueueFamilyIndices = families.data();
    }
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    r = vmaCreateBuffer(m_allocator, &bi, &ai, &m_breadcrumbBuffer, &m_breadcrumbAllocation, &info);
    if (r != VK_SUCCESS) return vkError(r, "vmaCreateBuffer(breadcrumbs)");
    std::memset(info.pMappedData, 0, kBreadcrumbBytes);
    vmaFlushAllocation(m_allocator, m_breadcrumbAllocation, 0, VK_WHOLE_SIZE);
    m_breadcrumbData = static_cast<const volatile u32*>(info.pMappedData);
    setObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(m_breadcrumbBuffer), "RHI breadcrumbs");
    return {};
}

Result<void> VulkanDevice::createBindlessHeap() {
    PropertyChain p(m_vki, m_physical);
    const VkPhysicalDeviceVulkan12Properties& v12 = p.v12;
    u32 sampled = std::min({kTargetSampledImages, v12.maxDescriptorSetUpdateAfterBindSampledImages,
                            v12.maxPerStageDescriptorUpdateAfterBindSampledImages});
    u32 storageImages = std::min({kTargetStorageImages, v12.maxDescriptorSetUpdateAfterBindStorageImages,
                                  v12.maxPerStageDescriptorUpdateAfterBindStorageImages});
    u32 storageBuffers = std::min({kTargetStorageBuffers, v12.maxDescriptorSetUpdateAfterBindStorageBuffers,
                                   v12.maxPerStageDescriptorUpdateAfterBindStorageBuffers});
    const u32 samplers = std::min({kTargetSamplers, v12.maxDescriptorSetUpdateAfterBindSamplers,
                                   v12.maxPerStageDescriptorUpdateAfterBindSamplers});
    // Stay within the per-stage resource total by shrinking the largest array first.
    const u64 budget = v12.maxPerStageUpdateAfterBindResources;
    while (static_cast<u64>(sampled) + storageImages + storageBuffers > budget && sampled > 1024) sampled /= 2;
    while (static_cast<u64>(sampled) + storageImages + storageBuffers > budget && storageBuffers > 1024) storageBuffers /= 2;
    while (static_cast<u64>(sampled) + storageImages + storageBuffers > budget && storageImages > 256) storageImages /= 2;
    if (sampled < 16 || storageImages < 4 || storageBuffers < 16 || samplers < 4) {
        return Error{ErrorCode::Unsupported, "device descriptor-indexing limits are too small for the bindless heap"};
    }

    const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER};
    const u32 counts[4] = {sampled, storageImages, storageBuffers, samplers};
    VkDescriptorSetLayoutBinding bindings[4]{};
    VkDescriptorBindingFlags flags[4]{};
    VkDescriptorPoolSize poolSizes[4]{};
    for (u32 i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = counts[i];
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
        flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                   VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        poolSizes[i] = {types[i], counts[i]};
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagInfo.bindingCount = 4;
    flagInfo.pBindingFlags = flags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &flagInfo;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 4;
    li.pBindings = bindings;
    VkResult r = m_vk.vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_bindlessLayout);
    if (r != VK_SUCCESS) return vkError(r, "vkCreateDescriptorSetLayout(bindless)");

    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = poolSizes;
    r = m_vk.vkCreateDescriptorPool(m_device, &pi, nullptr, &m_bindlessPool);
    if (r != VK_SUCCESS) return vkError(r, "vkCreateDescriptorPool(bindless)");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_bindlessPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_bindlessLayout;
    r = m_vk.vkAllocateDescriptorSets(m_device, &ai, &m_bindlessSet);
    if (r != VK_SUCCESS) return vkError(r, "vkAllocateDescriptorSets(bindless)");
    setObjectName(VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<u64>(m_bindlessSet), "Bindless heap");

    // One pipeline layout for every pipeline (03 §1.1): the heap + 128 bytes of push constants.
    VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, kMaxPushConstantBytes};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_bindlessLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    r = m_vk.vkCreatePipelineLayout(m_device, &pl, nullptr, &m_pipelineLayout);
    if (r != VK_SUCCESS) return vkError(r, "vkCreatePipelineLayout");

    m_sampledSlots.reset(sampled);
    m_storageImageSlots.reset(storageImages);
    m_storageBufferSlots.reset(storageBuffers);
    m_caps.limits.maxBindlessSampledImages = sampled;
    m_caps.limits.maxBindlessStorageImages = storageImages;
    m_caps.limits.maxBindlessStorageBuffers = storageBuffers;
    m_caps.limits.maxBindlessSamplers = samplers;
    return {};
}

void VulkanDevice::fillCaps() {
    const VkPhysicalDeviceLimits& l = m_properties.limits;
    Limits& out = m_caps.limits;
    out.maxPushConstantBytes = kMaxPushConstantBytes;
    out.maxTextureDimension2D = l.maxImageDimension2D;
    out.maxTextureDimension3D = l.maxImageDimension3D;
    out.maxTextureArrayLayers = l.maxImageArrayLayers;
    for (u32 i = 0; i < 3; ++i) {
        out.maxComputeWorkGroupCount[i] = l.maxComputeWorkGroupCount[i];
        out.maxComputeWorkGroupSize[i] = l.maxComputeWorkGroupSize[i];
    }
    out.maxComputeWorkGroupInvocations = l.maxComputeWorkGroupInvocations;
    out.minStorageBufferOffsetAlignment = l.minStorageBufferOffsetAlignment;
    out.minUniformBufferOffsetAlignment = l.minUniformBufferOffsetAlignment;
    out.optimalBufferCopyRowPitchAlignment = static_cast<u32>(l.optimalBufferCopyRowPitchAlignment);
    out.timestampPeriodNs = l.timestampPeriod;
    out.maxSamplerAnisotropy = l.maxSamplerAnisotropy;
    m_maxStorageBufferRange = l.maxStorageBufferRange;
    PropertyChain p(m_vki, m_physical);
    out.subgroupSize = p.v11.subgroupSize;

    FeatureChain f;
    f.link(false);
    m_vki.vkGetPhysicalDeviceFeatures2(m_physical, &f.core);
    const std::vector<VkExtensionProperties> exts = deviceExtensions(m_vki, m_physical);
    CapBit bits = m_caps.bits;
    auto set = [&](bool condition, CapBit bit) {
        if (condition) bits |= bit;
    };
    set(hasExtension(exts, VK_EXT_MESH_SHADER_EXTENSION_NAME), CapBit::MeshShader);
    set(hasExtension(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME), CapBit::RayQuery);
    set(hasExtension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME), CapBit::AccelerationStructure);
    set(hasExtension(exts, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME), CapBit::DescriptorBuffer);
    set(hasExtension(exts, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME), CapBit::GraphicsPipelineLibrary);
    set(hasExtension(exts, "VK_KHR_pipeline_binary"), CapBit::PipelineBinary);
    set(m_memoryBudget, CapBit::MemoryBudget);
    set(hasExtension(exts, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME), CapBit::MemoryPriority);
    set(m_deviceFault, CapBit::DeviceFault);
    set(m_debugUtils, CapBit::DebugUtils);
    set(l.timestampComputeAndGraphics == VK_TRUE, CapBit::TimestampQueries);
    set(hasExtension(exts, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME), CapBit::ImageInt64Atomics);
    set(f.core.features.shaderStorageImageReadWithoutFormat && f.core.features.shaderStorageImageWriteWithoutFormat,
        CapBit::StorageImageWithoutFormat);
    set(hasExtension(exts, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) && hasExtension(exts, VK_KHR_PRESENT_ID_EXTENSION_NAME),
        CapBit::PresentWait);
    set(hasExtension(exts, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ||
            hasExtension(exts, "VK_KHR_calibrated_timestamps"),
        CapBit::CalibratedTimestamps);
    set(hasExtension(exts, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME), CapBit::FragmentShadingRate);
    set(m_validationLayer, CapBit::ValidationLayer);
    set(m_swapchainExtension, CapBit::Swapchain);
    set(f.core.features.samplerAnisotropy == VK_TRUE, CapBit::SamplerAnisotropy);
    set(f.core.features.shaderInt64 == VK_TRUE, CapBit::ShaderInt64);
    set(f.v12.shaderFloat16 == VK_TRUE, CapBit::ShaderFloat16);
    set(f.core.features.fillModeNonSolid == VK_TRUE, CapBit::FillModeNonSolid);
    m_caps.bits = bits & m_desc.capsMask;
}

Result<void> VulkanDevice::createDefaultResources() {
    // Slot 0 of every bindless array holds a harmless default (see shaders/core/bindless.slang).
    SamplerDesc defaultSampler;
    if (sampler(defaultSampler) != 0) return Error{ErrorCode::InvalidState, "default sampler did not get slot 0"};

    TextureDesc td = TextureDesc::tex2D(Format::RGBA8Unorm, 1, 1, TextureUsage::Sampled | TextureUsage::TransferDst,
                                        "RHI default texture (magenta)");
    HELIOS_TRY_ASSIGN(m_defaultTexture, createTexture(td));
    TextureDesc sd = TextureDesc::tex2D(Format::RGBA8Unorm, 1, 1, TextureUsage::Storage, "RHI default storage image");
    HELIOS_TRY_ASSIGN(m_defaultStorageImage, createTexture(sd));
    BufferDesc bd;
    bd.size = 256;
    bd.usage = BufferUsage::Storage | BufferUsage::TransferDst;
    bd.name = "RHI default buffer";
    HELIOS_TRY_ASSIGN(m_defaultBuffer, createBuffer(bd));

    TextureRes* tex = findTexture(m_defaultTexture);
    TextureRes* storage = findTexture(m_defaultStorageImage);
    BufferRes* buf = findBuffer(m_defaultBuffer);
    if (uav(m_defaultStorageImage, 0) == kInvalidBindless) {
        return Error{ErrorCode::InvalidState, "default storage image view failed"};
    }
    m_defaultSampledView = tex->srvView;
    m_defaultStorageView = storage->uavs[0].first;
    m_defaultVkBuffer = buf->buffer;
    writeSampledImage(0, m_defaultSampledView);
    writeStorageImage(0, m_defaultStorageView);
    writeStorageBuffer(0, m_defaultVkBuffer, bd.size);

    return immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toDst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toDst.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = tex->image;
        toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier2 toGeneral = toDst;
        toGeneral.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.image = storage->image;
        VkImageMemoryBarrier2 first[2] = {toDst, toGeneral};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = first;
        m_vk.vkCmdPipelineBarrier2(cmd, &dep);

        const VkClearColorValue magenta{{1.0f, 0.0f, 1.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        m_vk.vkCmdClearColorImage(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &range);
        m_vk.vkCmdFillBuffer(cmd, buf->buffer, 0, VK_WHOLE_SIZE, 0);

        VkImageMemoryBarrier2 toRead = toDst;
        toRead.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        toRead.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toRead.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        toRead.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        VkMemoryBarrier2 bufferDone{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        bufferDone.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        bufferDone.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        bufferDone.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        bufferDone.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep2.memoryBarrierCount = 1;
        dep2.pMemoryBarriers = &bufferDone;
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &toRead;
        m_vk.vkCmdPipelineBarrier2(cmd, &dep2);
    });
}

Result<void> VulkanDevice::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
    auto* list = static_cast<VulkanCommandList*>(acquireCommandList(Queue::Graphics, "RHI immediate"));
    if (!list) return Error{ErrorCode::InvalidState, "immediateSubmit: no command list"};
    record(list->handle());
    list->end();
    CommandList* lists[] = {list};
    HELIOS_TRY_ASSIGN(TimelinePoint done, submit(Queue::Graphics, lists, {}));
    return wait(done, ~0ull);
}

// =================================================================================================
// Destruction
// =================================================================================================
VulkanDevice::~VulkanDevice() {
    while (m_pendingCompiles.load(std::memory_order_acquire) != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    m_tearingDown = true;  // deferred frees stop re-pointing bindless slots at the (dying) defaults
    if (m_device) {
        m_vk.vkDeviceWaitIdle(m_device);
        // Swapchains first (they own texture entries and surfaces).
        std::vector<SwapchainH> swapchains;
        {
            std::lock_guard lock(m_swapchainMutex);
            m_swapchains.forEach([&](SwapchainH h, SwapchainRes&) { swapchains.push_back(h); });
        }
        for (SwapchainH h : swapchains) destroy(h);

        u32 leakedBuffers = 0;
        u32 leakedTextures = 0;
        u32 leakedPipelines = 0;
        std::vector<BufferH> buffers;
        std::vector<TextureH> textures;
        std::vector<PipelineH> pipelines;
        m_buffers.forEach([&](BufferH h, BufferRes&) { buffers.push_back(h); });
        m_textures.forEach([&](TextureH h, TextureRes&) { textures.push_back(h); });
        m_pipelines.forEach([&](PipelineH h, PipelineRes&) { pipelines.push_back(h); });
        for (BufferH h : buffers) {
            if (h != m_defaultBuffer) ++leakedBuffers;
            destroy(h);
        }
        for (TextureH h : textures) {
            if (h != m_defaultTexture && h != m_defaultStorageImage) ++leakedTextures;
            destroy(h);
        }
        for (PipelineH h : pipelines) {
            ++leakedPipelines;
            destroy(h);
        }
        if (leakedBuffers + leakedTextures + leakedPipelines > 0) {
            HELIOS_LOG_WARN(LogRhi, "Vulkan device destroyed with {} buffer(s), {} texture(s), {} pipeline(s) alive",
                            leakedBuffers, leakedTextures, leakedPipelines);
        }
        sealGarbage();
        collectGarbage(true);

        for (auto& [desc, s] : m_samplers) m_vk.vkDestroySampler(m_device, s, nullptr);
        for (FrameSlot& frame : m_frames) {
            for (auto& thread : frame.threads) {
                for (CommandPoolSet& set : thread->perQueue) {
                    set.lists.clear();
                    if (set.pool) m_vk.vkDestroyCommandPool(m_device, set.pool, nullptr);
                }
            }
        }
        if (m_breadcrumbBuffer) vmaDestroyBuffer(m_allocator, m_breadcrumbBuffer, m_breadcrumbAllocation);
        if (m_pipelineLayout) m_vk.vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_bindlessPool) m_vk.vkDestroyDescriptorPool(m_device, m_bindlessPool, nullptr);
        if (m_bindlessLayout) m_vk.vkDestroyDescriptorSetLayout(m_device, m_bindlessLayout, nullptr);
        if (m_pipelineCache) m_vk.vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
        for (QueueSlot& q : m_queues) {
            if (q.timeline) m_vk.vkDestroySemaphore(m_device, q.timeline, nullptr);
        }
        if (m_allocator) vmaDestroyAllocator(m_allocator);
        m_vk.vkDestroyDevice(m_device, nullptr);
    }
    if (m_messenger && m_vki.vkDestroyDebugUtilsMessengerEXT) {
        m_vki.vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
    }
    if (m_instance) m_vki.vkDestroyInstance(m_instance, nullptr);
}

// =================================================================================================
// Diagnostics helpers
// =================================================================================================
void VulkanDevice::setObjectName(VkObjectType type, u64 handle, std::string_view name) const {
    if (!m_debugUtils || !m_vki.vkSetDebugUtilsObjectNameEXT || name.empty() || handle == 0) return;
    const std::string copy(name);
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = copy.c_str();
    m_vki.vkSetDebugUtilsObjectNameEXT(m_device, &info);
}

void VulkanDevice::reportError(std::string message) {
    m_validationErrors.fetch_add(1, std::memory_order_relaxed);
    HELIOS_LOG_ERROR(LogRhi, "{}", message);
    if (m_desc.onMessage) m_desc.onMessage(ValidationMessage{ValidationMessage::Severity::Error, std::move(message)});
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                           VkDebugUtilsMessageTypeFlagsEXT,
                                                           const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                           void* userData) {
    auto* self = static_cast<VulkanDevice*>(userData);
    const char* text = data && data->pMessage ? data->pMessage : "(no message)";
    ValidationMessage msg;
    msg.text = text;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        msg.severity = ValidationMessage::Severity::Error;
        self->m_validationErrors.fetch_add(1, std::memory_order_relaxed);
        HELIOS_LOG_ERROR(LogRhi, "[Vulkan] {}", text);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        msg.severity = ValidationMessage::Severity::Warning;
        HELIOS_LOG_WARN(LogRhi, "[Vulkan] {}", text);
    } else {
        msg.severity = ValidationMessage::Severity::Info;
        HELIOS_LOG_DEBUG(LogRhi, "[Vulkan] {}", text);
    }
    if (self->m_desc.onMessage) self->m_desc.onMessage(msg);
    return VK_FALSE;
}

Error VulkanDevice::vkError(VkResult result, std::string_view where) {
    if (result == VK_ERROR_DEVICE_LOST) return deviceLost(where);
    const ErrorCode code = (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
                               ? ErrorCode::OutOfMemory
                               : ErrorCode::Unknown;
    return makeError(code, "{} failed: {}", where, resultName(result));
}

Error VulkanDevice::deviceLost(std::string_view where) {
    bool expected = false;
    if (m_deviceLost.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        DeviceLostInfo info;
        info.reason = std::format("VK_ERROR_DEVICE_LOST in {}", where);
        if (m_deviceFault && m_vk.vkGetDeviceFaultInfoEXT) {
            VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
            if (m_vk.vkGetDeviceFaultInfoEXT(m_device, &counts, nullptr) == VK_SUCCESS) {
                std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
                std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
                counts.vendorBinarySize = 0;  // vendor binaries are for vendor tools; not captured here
                VkDeviceFaultInfoEXT fault{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
                fault.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
                fault.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
                const VkResult fr = m_vk.vkGetDeviceFaultInfoEXT(m_device, &counts, &fault);
                if (fr == VK_SUCCESS || fr == VK_INCOMPLETE) {  // INCOMPLETE still fills what fits
                    info.faultDescription = fault.description;
                    for (u32 i = 0; i < counts.addressInfoCount && i < addresses.size(); ++i) {
                        info.faultDetails.push_back(std::format("address type {} at 0x{:016x} (+/- 0x{:x})",
                                                                static_cast<int>(addresses[i].addressType),
                                                                addresses[i].reportedAddress,
                                                                addresses[i].addressPrecision));
                    }
                    for (u32 i = 0; i < counts.vendorInfoCount && i < vendors.size(); ++i) {
                        info.faultDetails.push_back(std::format("vendor: {} (code 0x{:x}, data 0x{:x})",
                                                                vendors[i].description, vendors[i].vendorFaultCode,
                                                                vendors[i].vendorFaultData));
                    }
                }
            }
        }
        info.breadcrumbs = breadcrumbs();
        info.submittedValues = submittedValues();
        info.completedValues = completedValues();
        HELIOS_LOG_ERROR(LogRhi, "GPU device lost ({}). Fault: '{}'", info.reason, info.faultDescription);
        for (const std::string& d : info.faultDetails) HELIOS_LOG_ERROR(LogRhi, "  {}", d);
        for (u32 q = 0; q < kQueueCount; ++q) {
            HELIOS_LOG_ERROR(LogRhi, "  {} queue: submitted {}, completed {}, last pass begin 0x{:08x} end 0x{:08x}",
                             queueName(static_cast<Queue>(q)), info.submittedValues[q], info.completedValues[q],
                             info.breadcrumbs[q].lastBegin, info.breadcrumbs[q].lastEnd);
        }
        if (m_desc.onDeviceLost) m_desc.onDeviceLost(info);
    }
    return makeError(ErrorCode::InvalidState, "GPU device lost ({})", where);
}

std::array<BreadcrumbState, kQueueCount> VulkanDevice::breadcrumbs() const {
    std::array<BreadcrumbState, kQueueCount> out{};
    if (!m_breadcrumbData) return out;
    vmaInvalidateAllocation(m_allocator, m_breadcrumbAllocation, 0, VK_WHOLE_SIZE);
    for (u32 q = 0; q < kQueueCount; ++q) {
        out[q].lastBegin = m_breadcrumbData[q * 2 + 0];
        out[q].lastEnd = m_breadcrumbData[q * 2 + 1];
    }
    return out;
}

MemoryStats VulkanDevice::memoryStats() const {
    MemoryStats s;
    VkPhysicalDeviceMemoryProperties mem{};
    m_vki.vkGetPhysicalDeviceMemoryProperties(m_physical, &mem);
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
    vmaGetHeapBudgets(m_allocator, budgets.data());
    for (u32 i = 0; i < mem.memoryHeapCount; ++i) {
        MemoryHeapStats h;
        h.budgetBytes = budgets[i].budget;
        h.usageBytes = budgets[i].usage;
        h.deviceLocal = (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        s.heaps.push_back(h);
    }
    s.bufferBytes = m_bufferBytes.load(std::memory_order_relaxed);
    s.textureBytes = m_textureBytes.load(std::memory_order_relaxed);
    s.bufferCount = m_buffers.size();
    s.textureCount = m_textures.size();
    s.pipelineCount = m_pipelineCount.load(std::memory_order_relaxed);
    s.psoMisses = m_psoMisses.load(std::memory_order_relaxed);
    return s;
}

// =================================================================================================
// Buffers
// =================================================================================================
Result<BufferH> VulkanDevice::createBuffer(const BufferDesc& desc) {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "device lost"};
    HELIOS_TRY(detail::validateBufferDesc(desc));
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = desc.size;
    bi.usage = toVkBufferUsage(desc.usage);
    std::array<u32, kQueueCount> families{};
    u32 familyCount = 0;
    for (const QueueSlot& q : m_queues) {
        if (std::find(families.begin(), families.begin() + familyCount, q.family) == families.begin() + familyCount) {
            families[familyCount++] = q.family;
        }
    }
    if (familyCount > 1) {  // no ownership transfers for buffers (03 §2.2)
        bi.sharingMode = VK_SHARING_MODE_CONCURRENT;
        bi.queueFamilyIndexCount = familyCount;
        bi.pQueueFamilyIndices = families.data();
    }
    VmaAllocationCreateInfo ai{};
    switch (desc.memory) {
    case MemoryUsage::GpuOnly: ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE; break;
    case MemoryUsage::Upload:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case MemoryUsage::Readback:
        ai.usage = VMA_MEMORY_USAGE_AUTO;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }
    BufferRes res;
    VmaAllocationInfo info{};
    const VkResult r = vmaCreateBuffer(m_allocator, &bi, &ai, &res.buffer, &res.allocation, &info);
    if (r != VK_SUCCESS) return vkError(r, std::format("vmaCreateBuffer('{}', {} bytes)", desc.name, desc.size));
    res.mapped = desc.memory == MemoryUsage::GpuOnly ? nullptr : info.pMappedData;
    res.allocationSize = info.size;
    VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    addressInfo.buffer = res.buffer;
    res.address = m_vk.vkGetBufferDeviceAddress(m_device, &addressInfo);
    res.desc = desc;
    res.desc.tag = desc.tag == MemoryTag::Unknown ? detail::defaultBufferTag() : desc.tag;
    res.name = std::string(desc.name);
    if (hasFlag(desc.usage, BufferUsage::Storage)) {
        res.storageIndex = m_storageBufferSlots.allocate();
        if (res.storageIndex == kInvalidBindless) {
            vmaDestroyBuffer(m_allocator, res.buffer, res.allocation);
            return Error{ErrorCode::LimitExceeded, "bindless storage-buffer slots exhausted"};
        }
        writeStorageBuffer(res.storageIndex, res.buffer, desc.size);
    }
    setObjectName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<u64>(res.buffer), res.name);
    trackAllocation(res.desc.tag, static_cast<usize>(res.allocationSize));
    m_bufferBytes.fetch_add(res.allocationSize, std::memory_order_relaxed);
    const BufferH h = m_buffers.create(std::move(res));
    BufferRes* stored = m_buffers.get(h);
    stored->desc.name = stored->name;
    return h;
}

void VulkanDevice::destroy(BufferH h) {
    BufferRes* b = m_buffers.get(h);
    if (!b) return;
    const VkBuffer buffer = b->buffer;
    const VmaAllocation allocation = b->allocation;
    const BindlessIndex index = b->storageIndex;
    const MemoryTag tag = b->desc.tag;
    const u64 bytes = b->allocationSize;
    m_buffers.destroy(h);
    defer([this, buffer, allocation, index, tag, bytes] {
        if (index != kInvalidBindless) {
            if (!m_tearingDown) writeStorageBuffer(index, m_defaultVkBuffer, 256);
            m_storageBufferSlots.release(index);
        }
        vmaDestroyBuffer(m_allocator, buffer, allocation);
        trackDeallocation(tag, static_cast<usize>(bytes));
        m_bufferBytes.fetch_sub(bytes, std::memory_order_relaxed);
    });
}

BufferDesc VulkanDevice::bufferDesc(BufferH h) const {
    const BufferRes* b = m_buffers.get(h);
    return b ? b->desc : BufferDesc{};
}

void* VulkanDevice::map(BufferH h) {
    BufferRes* b = m_buffers.get(h);
    return b ? b->mapped : nullptr;
}

void VulkanDevice::flushMapped(BufferH h, u64 offset, u64 size) {
    BufferRes* b = m_buffers.get(h);
    if (b && b->mapped) vmaFlushAllocation(m_allocator, b->allocation, offset, size == kWholeSize ? VK_WHOLE_SIZE : size);
}

void VulkanDevice::invalidateMapped(BufferH h, u64 offset, u64 size) {
    BufferRes* b = m_buffers.get(h);
    if (b && b->mapped) {
        vmaInvalidateAllocation(m_allocator, b->allocation, offset, size == kWholeSize ? VK_WHOLE_SIZE : size);
    }
}

BindlessIndex VulkanDevice::srv(BufferH h) {
    BufferRes* b = m_buffers.get(h);
    if (!b) return kInvalidBindless;
    if (b->storageIndex == kInvalidBindless) {
        reportError(std::format("srv: buffer '{}' lacks BufferUsage::Storage", b->name));
    }
    return b->storageIndex;
}

u64 VulkanDevice::deviceAddress(BufferH h) {
    const BufferRes* b = m_buffers.get(h);
    return b ? b->address : 0;
}

// =================================================================================================
// Textures and views
// =================================================================================================
Result<TextureH> VulkanDevice::createTexture(const TextureDesc& desc) {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "device lost"};
    HELIOS_TRY(detail::validateTextureDesc(desc, m_caps.limits));
    if (hasFlag(desc.usage, TextureUsage::Storage) && isSrgbFormat(desc.format)) {
        return makeError(ErrorCode::InvalidArgument,
                         "texture '{}': sRGB formats cannot be storage images; use the UNORM twin", desc.name);
    }
    const VkFormat format = toVkFormat(desc.format);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = desc.type == TextureType::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {desc.width, desc.height, desc.depth};
    ci.mipLevels = desc.mipLevels;
    ci.arrayLayers = desc.arrayLayers;
    ci.samples = toVkSamples(desc.sampleCount);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = toVkImageUsage(desc.usage);
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.type == TextureType::Cube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    std::array<u32, kQueueCount> families{};
    u32 familyCount = 0;
    for (const QueueSlot& q : m_queues) {
        if (std::find(families.begin(), families.begin() + familyCount, q.family) == families.begin() + familyCount) {
            families[familyCount++] = q.family;
        }
    }
    if (familyCount > 1) {  // Phase 0: concurrent sharing instead of ownership transfers
        ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = familyCount;
        ci.pQueueFamilyIndices = families.data();
    }
    VkImageFormatProperties formatProps{};
    const VkResult fr = m_vki.vkGetPhysicalDeviceImageFormatProperties(m_physical, format, ci.imageType, ci.tiling,
                                                                       ci.usage, ci.flags, &formatProps);
    if (fr != VK_SUCCESS) {
        return makeError(ErrorCode::Unsupported, "texture '{}': format {} with these usages is not supported ({})",
                         desc.name, formatName(desc.format), resultName(fr));
    }
    if (!(formatProps.sampleCounts & static_cast<VkSampleCountFlags>(ci.samples)) ||
        desc.mipLevels > formatProps.maxMipLevels ||
        desc.arrayLayers > formatProps.maxArrayLayers || desc.width > formatProps.maxExtent.width ||
        desc.height > formatProps.maxExtent.height || desc.depth > formatProps.maxExtent.depth) {
        return makeError(ErrorCode::Unsupported,
                         "texture '{}': {}x{}x{}, {} mips, {} layers, {} samples exceed what {} supports here",
                         desc.name, desc.width, desc.height, desc.depth, desc.mipLevels, desc.arrayLayers,
                         desc.sampleCount, formatName(desc.format));
    }

    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (hasAnyFlag(desc.usage, TextureUsage::ColorAttachment | TextureUsage::DepthStencil) &&
        static_cast<u64>(desc.width) * desc.height >= 1024ull * 1024ull) {
        ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // big render targets
    }
    TextureRes res;
    VmaAllocationInfo info{};
    const VkResult r = vmaCreateImage(m_allocator, &ci, &ai, &res.image, &res.allocation, &info);
    if (r != VK_SUCCESS) return vkError(r, std::format("vmaCreateImage('{}')", desc.name));
    res.format = format;
    res.aspect = aspectOf(desc.format);
    res.allocationSize = info.size;
    res.desc = desc;
    res.desc.tag = desc.tag == MemoryTag::Unknown ? detail::defaultTextureTag() : desc.tag;
    res.name = std::string(desc.name);
    res.uavs.assign(desc.mipLevels, {VK_NULL_HANDLE, kInvalidBindless});
    res.attachmentViews.assign(static_cast<usize>(desc.mipLevels) * desc.arrayLayers, VK_NULL_HANDLE);
    setObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(res.image), res.name);

    if (hasFlag(desc.usage, TextureUsage::Sampled)) {
        auto view = detail::resolveView(desc, {});
        if (!view) {
            vmaDestroyImage(m_allocator, res.image, res.allocation);
            return view.error();
        }
        auto vk = createView(res, toVkViewType(view->type), 0, view->mipCount, 0, view->layerCount, false);
        if (!vk) {
            vmaDestroyImage(m_allocator, res.image, res.allocation);
            return vk.error();
        }
        res.srvView = *vk;
        res.srvIndex = m_sampledSlots.allocate();
        if (res.srvIndex == kInvalidBindless) {
            m_vk.vkDestroyImageView(m_device, res.srvView, nullptr);
            vmaDestroyImage(m_allocator, res.image, res.allocation);
            return Error{ErrorCode::LimitExceeded, "bindless sampled-image slots exhausted"};
        }
        writeSampledImage(res.srvIndex, res.srvView);
    }
    trackAllocation(res.desc.tag, static_cast<usize>(res.allocationSize));
    m_textureBytes.fetch_add(res.allocationSize, std::memory_order_relaxed);
    const TextureH h = m_textures.create(std::move(res));
    TextureRes* stored = m_textures.get(h);
    stored->desc.name = stored->name;
    return h;
}

Result<VkImageView> VulkanDevice::createView(const TextureRes& texture, VkImageViewType type, u32 baseMip,
                                             u32 mipCount, u32 baseLayer, u32 layerCount, bool storage) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = texture.image;
    vi.viewType = type;
    vi.format = texture.format;
    // Sampled views of depth-stencil images read depth; attachments need every aspect.
    VkImageAspectFlags aspect = texture.aspect;
    if (!storage && (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange = {aspect, baseMip, mipCount, baseLayer, layerCount};
    VkImageView view = VK_NULL_HANDLE;
    const VkResult r = m_vk.vkCreateImageView(m_device, &vi, nullptr, &view);
    if (r != VK_SUCCESS) return vkError(r, std::format("vkCreateImageView('{}')", texture.name));
    return view;
}

VkImageView VulkanDevice::attachmentView(TextureRes& texture, u32 mip, u32 layer) {
    const usize slot = static_cast<usize>(mip) * texture.desc.arrayLayers + layer;
    std::lock_guard lock(m_viewMutex);
    if (slot >= texture.attachmentViews.size()) return VK_NULL_HANDLE;
    VkImageView& view = texture.attachmentViews[slot];
    if (!view) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = texture.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = texture.format;
        vi.subresourceRange = {texture.aspect, mip, 1, layer, 1};
        if (m_vk.vkCreateImageView(m_device, &vi, nullptr, &view) != VK_SUCCESS) view = VK_NULL_HANDLE;
    }
    return view;
}

BindlessIndex VulkanDevice::srv(TextureH h, const ViewDesc& viewDesc) {
    TextureRes* t = m_textures.get(h);
    if (!t) return kInvalidBindless;
    if (!hasFlag(t->desc.usage, TextureUsage::Sampled)) {
        reportError(std::format("srv: texture '{}' lacks TextureUsage::Sampled", t->name));
        return kInvalidBindless;
    }
    if (viewDesc == ViewDesc{}) return t->srvIndex;
    auto resolved = detail::resolveView(t->desc, viewDesc);
    if (!resolved) {
        reportError(resolved.error().message);
        return kInvalidBindless;
    }
    std::lock_guard lock(m_viewMutex);
    for (const auto& [v, entry] : t->views) {
        if (v == *resolved) return entry.second;
    }
    auto view = createView(*t, toVkViewType(resolved->type), resolved->baseMip, resolved->mipCount,
                           resolved->baseLayer, resolved->layerCount, false);
    if (!view) {
        reportError(view.error().message);
        return kInvalidBindless;
    }
    const BindlessIndex index = m_sampledSlots.allocate();
    if (index == kInvalidBindless) {
        m_vk.vkDestroyImageView(m_device, *view, nullptr);
        reportError("srv: bindless sampled-image slots exhausted");
        return kInvalidBindless;
    }
    writeSampledImage(index, *view);
    t->views.push_back({*resolved, {*view, index}});
    return index;
}

BindlessIndex VulkanDevice::uav(TextureH h, u32 mip) {
    TextureRes* t = m_textures.get(h);
    if (!t) return kInvalidBindless;
    if (!hasFlag(t->desc.usage, TextureUsage::Storage) || mip >= t->desc.mipLevels) {
        reportError(std::format("uav: texture '{}' lacks TextureUsage::Storage or mip {} is out of range", t->name, mip));
        return kInvalidBindless;
    }
    std::lock_guard lock(m_viewMutex);
    auto& entry = t->uavs[mip];
    if (entry.second != kInvalidBindless) return entry.second;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    if (t->desc.type == TextureType::Tex3D) {
        type = VK_IMAGE_VIEW_TYPE_3D;
    } else if (t->desc.arrayLayers > 1) {
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    auto view = createView(*t, type, mip, 1, 0, t->desc.arrayLayers, true);
    if (!view) {
        reportError(view.error().message);
        return kInvalidBindless;
    }
    const BindlessIndex index = m_storageImageSlots.allocate();
    if (index == kInvalidBindless) {
        m_vk.vkDestroyImageView(m_device, *view, nullptr);
        reportError("uav: bindless storage-image slots exhausted");
        return kInvalidBindless;
    }
    writeStorageImage(index, *view);
    entry = {*view, index};
    return index;
}

void VulkanDevice::destroy(TextureH h) {
    TextureRes* t = m_textures.get(h);
    if (!t) return;
    if (t->swapchainImage) {
        reportError(std::format("destroy: texture '{}' is a swapchain image; destroy the swapchain instead", t->name));
        return;
    }
    // Collect everything under the view lock, then free it once the GPU is done with the frame.
    std::vector<VkImageView> views;
    std::vector<BindlessIndex> sampledSlots;
    std::vector<BindlessIndex> storageSlots;
    {
        std::lock_guard lock(m_viewMutex);
        if (t->srvView) views.push_back(t->srvView);
        sampledSlots.push_back(t->srvIndex);
        for (auto& [v, entry] : t->views) {
            views.push_back(entry.first);
            sampledSlots.push_back(entry.second);
        }
        for (auto& [view, index] : t->uavs) {
            if (view) views.push_back(view);
            storageSlots.push_back(index);
        }
        for (VkImageView v : t->attachmentViews) {
            if (v) views.push_back(v);
        }
    }
    const VkImage image = t->image;
    const VmaAllocation allocation = t->allocation;
    const MemoryTag tag = t->desc.tag;
    const u64 bytes = t->allocationSize;
    m_textures.destroy(h);
    defer([this, views = std::move(views), sampledSlots = std::move(sampledSlots),
           storageSlots = std::move(storageSlots), image, allocation, tag, bytes] {
        for (BindlessIndex i : sampledSlots) {
            if (i == kInvalidBindless) continue;
            if (!m_tearingDown) writeSampledImage(i, m_defaultSampledView);
            m_sampledSlots.release(i);
        }
        for (BindlessIndex i : storageSlots) {
            if (i == kInvalidBindless) continue;
            if (!m_tearingDown) writeStorageImage(i, m_defaultStorageView);
            m_storageImageSlots.release(i);
        }
        for (VkImageView v : views) m_vk.vkDestroyImageView(m_device, v, nullptr);
        vmaDestroyImage(m_allocator, image, allocation);
        trackDeallocation(tag, static_cast<usize>(bytes));
        m_textureBytes.fetch_sub(bytes, std::memory_order_relaxed);
    });
}

void VulkanDevice::destroyTextureNow(TextureRes& t) {
    std::lock_guard lock(m_viewMutex);
    if (t.srvView) m_vk.vkDestroyImageView(m_device, t.srvView, nullptr);
    m_sampledSlots.release(t.srvIndex);
    for (auto& [v, entry] : t.views) {
        m_vk.vkDestroyImageView(m_device, entry.first, nullptr);
        m_sampledSlots.release(entry.second);
    }
    for (auto& [view, index] : t.uavs) {
        if (view) m_vk.vkDestroyImageView(m_device, view, nullptr);
        m_storageImageSlots.release(index);
    }
    for (VkImageView v : t.attachmentViews) {
        if (v) m_vk.vkDestroyImageView(m_device, v, nullptr);
    }
    t.views.clear();
    t.uavs.clear();
    t.attachmentViews.clear();
    t.srvView = VK_NULL_HANDLE;
}

TextureDesc VulkanDevice::textureDesc(TextureH h) const {
    const TextureRes* t = m_textures.get(h);
    if (!t) {
        TextureDesc d;
        d.width = 0;
        return d;
    }
    return t->desc;
}

// =================================================================================================
// Bindless writes and samplers
// =================================================================================================
void VulkanDevice::writeSampledImage(BindlessIndex index, VkImageView view) {
    VkDescriptorImageInfo info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_bindlessSet;
    w.dstBinding = static_cast<u32>(BindlessBinding::SampledImages);
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &info;
    std::lock_guard lock(m_descriptorMutex);
    m_vk.vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
}

void VulkanDevice::writeStorageImage(BindlessIndex index, VkImageView view) {
    VkDescriptorImageInfo info{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_bindlessSet;
    w.dstBinding = static_cast<u32>(BindlessBinding::StorageImages);
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &info;
    std::lock_guard lock(m_descriptorMutex);
    m_vk.vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
}

void VulkanDevice::writeStorageBuffer(BindlessIndex index, VkBuffer buffer, u64 size) {
    if (!buffer) return;
    // A storage-buffer descriptor may cover at most maxStorageBufferRange bytes (2^27 on some
    // drivers); larger buffers expose their first part through the heap and the rest through BDA.
    const u64 range = m_maxStorageBufferRange != 0 ? std::min<u64>(size, m_maxStorageBufferRange) : size;
    VkDescriptorBufferInfo info{buffer, 0, range};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_bindlessSet;
    w.dstBinding = static_cast<u32>(BindlessBinding::StorageBuffers);
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &info;
    std::lock_guard lock(m_descriptorMutex);
    m_vk.vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
}

void VulkanDevice::writeSampler(BindlessIndex index, VkSampler s) {
    VkDescriptorImageInfo info{s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_bindlessSet;
    w.dstBinding = static_cast<u32>(BindlessBinding::Samplers);
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &info;
    std::lock_guard lock(m_descriptorMutex);
    m_vk.vkUpdateDescriptorSets(m_device, 1, &w, 0, nullptr);
}

BindlessIndex VulkanDevice::sampler(const SamplerDesc& desc) {
    std::lock_guard lock(m_samplerMutex);
    for (usize i = 0; i < m_samplers.size(); ++i) {
        if (m_samplers[i].first == desc) return static_cast<BindlessIndex>(i);
    }
    if (m_samplers.size() >= m_caps.limits.maxBindlessSamplers) {
        reportError("sampler: bindless sampler slots exhausted");
        return kInvalidBindless;
    }
    VkSamplerReductionModeCreateInfo reduction{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    reduction.reductionMode = toVkReduction(desc.reduction);
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    if (desc.reduction != SamplerReduction::WeightedAverage) ci.pNext = &reduction;
    ci.magFilter = toVkFilter(desc.magFilter);
    ci.minFilter = toVkFilter(desc.minFilter);
    ci.mipmapMode = toVkMipmapMode(desc.mipFilter);
    ci.addressModeU = toVkAddressMode(desc.addressU);
    ci.addressModeV = toVkAddressMode(desc.addressV);
    ci.addressModeW = toVkAddressMode(desc.addressW);
    ci.mipLodBias = desc.mipLodBias;
    ci.anisotropyEnable = desc.maxAnisotropy > 1.0f && m_caps.has(CapBit::SamplerAnisotropy);
    ci.maxAnisotropy = std::min(desc.maxAnisotropy, m_caps.limits.maxSamplerAnisotropy);
    ci.compareEnable = desc.compareEnable;
    ci.compareOp = toVkCompareOp(desc.compareOp);
    ci.minLod = desc.minLod;
    ci.maxLod = desc.maxLod;
    ci.borderColor = toVkBorderColor(desc.borderColor);
    VkSampler s = VK_NULL_HANDLE;
    const VkResult r = m_vk.vkCreateSampler(m_device, &ci, nullptr, &s);
    if (r != VK_SUCCESS) {
        reportError(std::format("vkCreateSampler failed: {}", resultName(r)));
        return kInvalidBindless;
    }
    const auto index = static_cast<BindlessIndex>(m_samplers.size());
    m_samplers.emplace_back(desc, s);
    writeSampler(index, s);
    return index;
}

// =================================================================================================
// Pipelines
// =================================================================================================
Result<VkShaderModule> VulkanDevice::createShaderModule(std::span<const u32> spirv, std::string_view name) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size_bytes();
    ci.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult r = m_vk.vkCreateShaderModule(m_device, &ci, nullptr, &module);
    if (r != VK_SUCCESS) return vkError(r, std::format("vkCreateShaderModule('{}')", name));
    return module;
}

Result<VkPipeline> VulkanDevice::buildGraphicsPipeline(const GraphicsPipelineDesc& d) {
    HELIOS_TRY_ASSIGN(VkShaderModule vs, createShaderModule(d.vertex.spirv, d.name));
    VkShaderModule fs = VK_NULL_HANDLE;
    if (!d.fragment.spirv.empty()) {
        auto m = createShaderModule(d.fragment.spirv, d.name);
        if (!m) {
            m_vk.vkDestroyShaderModule(m_device, vs, nullptr);
            return m.error();
        }
        fs = *m;
    }
    const std::string vsEntry(d.vertex.entryPoint);
    const std::string fsEntry(d.fragment.entryPoint);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = vsEntry.c_str();
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = fsEntry.c_str();

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = toVkTopology(d.topology);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = d.raster.depthClamp;
    rs.polygonMode = toVkPolygonMode(d.raster.polygonMode);
    rs.cullMode = toVkCullMode(d.raster.cullMode);
    rs.frontFace = toVkFrontFace(d.raster.frontFace);
    rs.depthBiasEnable = d.raster.depthBiasConstant != 0.0f || d.raster.depthBiasSlope != 0.0f;
    rs.depthBiasConstantFactor = d.raster.depthBiasConstant;
    rs.depthBiasSlopeFactor = d.raster.depthBiasSlope;
    rs.depthBiasClamp = d.raster.depthBiasClamp;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = toVkSamples(d.sampleCount);
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth.testEnable;
    ds.depthWriteEnable = d.depth.writeEnable;
    ds.depthCompareOp = toVkCompareOp(d.depth.compareOp);
    std::array<VkPipelineColorBlendAttachmentState, kMaxColorAttachments> blends{};
    std::array<VkFormat, kMaxColorAttachments> colorFormats{};
    for (u32 i = 0; i < d.colorCount; ++i) {
        const BlendState& b = d.blend[i];
        blends[i].blendEnable = b.enable;
        blends[i].srcColorBlendFactor = toVkBlendFactor(b.srcColor);
        blends[i].dstColorBlendFactor = toVkBlendFactor(b.dstColor);
        blends[i].colorBlendOp = toVkBlendOp(b.colorOp);
        blends[i].srcAlphaBlendFactor = toVkBlendFactor(b.srcAlpha);
        blends[i].dstAlphaBlendFactor = toVkBlendFactor(b.dstAlpha);
        blends[i].alphaBlendOp = toVkBlendOp(b.alphaOp);
        blends[i].colorWriteMask = static_cast<VkColorComponentFlags>(toUnderlying(b.writeMask));
        colorFormats[i] = toVkFormat(d.colorFormats[i]);
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.colorCount;
    cb.pAttachments = blends.data();
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = d.colorCount;
    rendering.pColorAttachmentFormats = colorFormats.data();
    rendering.depthAttachmentFormat = toVkFormat(d.depthFormat);
    rendering.stencilAttachmentFormat = hasStencil(d.depthFormat) ? toVkFormat(d.depthFormat) : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = fs ? 2 : 1;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dyn;
    ci.layout = m_pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = m_vk.vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &pipeline);
    m_vk.vkDestroyShaderModule(m_device, vs, nullptr);
    if (fs) m_vk.vkDestroyShaderModule(m_device, fs, nullptr);
    if (r != VK_SUCCESS) return vkError(r, std::format("vkCreateGraphicsPipelines('{}')", d.name));
    setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(pipeline), d.name);
    return pipeline;
}

Result<VkPipeline> VulkanDevice::buildComputePipeline(const ComputePipelineDesc& d) {
    HELIOS_TRY_ASSIGN(VkShaderModule cs, createShaderModule(d.compute.spirv, d.name));
    const std::string entry(d.compute.entryPoint);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = entry.c_str();
    ci.layout = m_pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = m_vk.vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &pipeline);
    m_vk.vkDestroyShaderModule(m_device, cs, nullptr);
    if (r != VK_SUCCESS) return vkError(r, std::format("vkCreateComputePipelines('{}')", d.name));
    setObjectName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<u64>(pipeline), d.name);
    return pipeline;
}

namespace {
/// Deep copy of a pipeline description for asynchronous compilation.
struct OwnedShader {
    std::vector<u32> spirv;
    std::string entry;
    explicit OwnedShader(const ShaderDesc& s) : spirv(s.spirv.begin(), s.spirv.end()), entry(s.entryPoint) {}
    ShaderDesc view() const { return ShaderDesc{spirv, entry}; }
};

jobs::Priority jobPriority(PsoPriority p) {
    switch (p) {
    case PsoPriority::High: return jobs::Priority::High;
    case PsoPriority::Low: return jobs::Priority::Low;
    default: return jobs::Priority::Normal;
    }
}
} // namespace

Result<PipelineH> VulkanDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc, PsoPriority priority) {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "device lost"};
    HELIOS_TRY(detail::validateGraphicsPipelineDesc(desc));
    const VkSampleCountFlags samples = toVkSamples(desc.sampleCount);
    const VkPhysicalDeviceLimits& limits = m_properties.limits;
    if ((desc.colorCount > 0 && !(limits.framebufferColorSampleCounts & samples)) ||
        (desc.depthFormat != Format::Unknown && !(limits.framebufferDepthSampleCounts & samples))) {
        return makeError(ErrorCode::Unsupported, "pipeline '{}': {} samples are not supported for its attachments",
                         desc.name, desc.sampleCount);
    }
    PipelineRes res;
    res.state = std::make_shared<PipelineState>();
    res.bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    res.name = std::string(desc.name);
    if (priority == PsoPriority::Immediate || !m_desc.pipelineCompilePool) {
        HELIOS_TRY_ASSIGN(VkPipeline pipeline, buildGraphicsPipeline(desc));
        res.state->pipeline.store(pipeline);
        res.state->status.store(1);
        m_pipelineCount.fetch_add(1, std::memory_order_relaxed);
        return m_pipelines.create(std::move(res));
    }
    std::shared_ptr<PipelineState> state = res.state;
    m_pipelineCount.fetch_add(1, std::memory_order_relaxed);
    const PipelineH h = m_pipelines.create(std::move(res));
    m_pendingCompiles.fetch_add(1, std::memory_order_acq_rel);
    auto owned = std::make_shared<std::tuple<GraphicsPipelineDesc, OwnedShader, OwnedShader, std::string>>(
        desc, OwnedShader(desc.vertex), OwnedShader(desc.fragment), std::string(desc.name));
    m_desc.pipelineCompilePool->run(
        [this, state, owned] {
            auto& [d, vs, fs, name] = *owned;
            d.vertex = vs.view();
            d.fragment = fs.view();
            d.name = name;
            auto pipeline = buildGraphicsPipeline(d);
            if (pipeline) {
                state->pipeline.store(*pipeline);
                state->status.store(1);
            } else {
                state->status.store(2);
                reportError(std::format("async pipeline '{}' failed: {}", name, pipeline.error().message));
            }
            m_pendingCompiles.fetch_sub(1, std::memory_order_acq_rel);
        },
        nullptr, jobPriority(priority));
    return h;
}

Result<PipelineH> VulkanDevice::createComputePipeline(const ComputePipelineDesc& desc, PsoPriority priority) {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "device lost"};
    HELIOS_TRY(detail::validateComputePipelineDesc(desc));
    PipelineRes res;
    res.state = std::make_shared<PipelineState>();
    res.bindPoint = VK_PIPELINE_BIND_POINT_COMPUTE;
    res.name = std::string(desc.name);
    if (priority == PsoPriority::Immediate || !m_desc.pipelineCompilePool) {
        HELIOS_TRY_ASSIGN(VkPipeline pipeline, buildComputePipeline(desc));
        res.state->pipeline.store(pipeline);
        res.state->status.store(1);
        m_pipelineCount.fetch_add(1, std::memory_order_relaxed);
        return m_pipelines.create(std::move(res));
    }
    std::shared_ptr<PipelineState> state = res.state;
    m_pipelineCount.fetch_add(1, std::memory_order_relaxed);
    const PipelineH h = m_pipelines.create(std::move(res));
    m_pendingCompiles.fetch_add(1, std::memory_order_acq_rel);
    auto owned = std::make_shared<std::pair<OwnedShader, std::string>>(OwnedShader(desc.compute), std::string(desc.name));
    m_desc.pipelineCompilePool->run(
        [this, state, owned] {
            ComputePipelineDesc d;
            d.compute = owned->first.view();
            d.name = owned->second;
            auto pipeline = buildComputePipeline(d);
            if (pipeline) {
                state->pipeline.store(*pipeline);
                state->status.store(1);
            } else {
                state->status.store(2);
                reportError(std::format("async pipeline '{}' failed: {}", owned->second, pipeline.error().message));
            }
            m_pendingCompiles.fetch_sub(1, std::memory_order_acq_rel);
        },
        nullptr, jobPriority(priority));
    return h;
}

bool VulkanDevice::isReady(PipelineH h) const {
    const PipelineRes* p = m_pipelines.get(h);
    return p && p->state->status.load(std::memory_order_acquire) == 1;
}

void VulkanDevice::destroy(PipelineH h) {
    PipelineRes* p = m_pipelines.get(h);
    if (!p) return;
    std::shared_ptr<PipelineState> state = p->state;
    m_pipelines.destroy(h);
    m_pipelineCount.fetch_sub(1, std::memory_order_relaxed);
    // An in-flight compile finishes first: the deferred item re-queues itself until it has.
    std::function<void()> release;
    release = [this, state]() {
        if (state->status.load(std::memory_order_acquire) == 0) {
            defer([this, state] {
                // Still compiling after a full frame: wait for it (compiles are bounded).
                while (state->status.load(std::memory_order_acquire) == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (VkPipeline pl = state->pipeline.exchange(VK_NULL_HANDLE)) m_vk.vkDestroyPipeline(m_device, pl, nullptr);
            });
            return;
        }
        if (VkPipeline pl = state->pipeline.exchange(VK_NULL_HANDLE)) m_vk.vkDestroyPipeline(m_device, pl, nullptr);
    };
    defer(std::move(release));
}

std::vector<u8> VulkanDevice::pipelineCacheData() const {
    usize size = 0;
    if (m_vk.vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) return {};
    std::vector<u8> data(size);
    if (m_vk.vkGetPipelineCacheData(m_device, m_pipelineCache, &size, data.data()) != VK_SUCCESS) return {};
    data.resize(size);
    return data;
}

// =================================================================================================
// Command lists, submission, frames
// =================================================================================================
u32 VulkanDevice::threadIndex() {
    const auto id = std::this_thread::get_id();
    auto it = m_threadIndices.find(id);
    if (it != m_threadIndices.end()) return it->second;
    const u32 index = static_cast<u32>(m_threadIndices.size());
    m_threadIndices.emplace(id, index);
    return index;
}

CommandList* VulkanDevice::acquireCommandList(Queue queue, std::string_view name) {
    if (isDeviceLost()) return nullptr;
    const u32 q = queueIndex(queue);
    VulkanCommandList* list = nullptr;
    {
        std::lock_guard lock(m_contextMutex);
        FrameSlot& frame = m_frames[m_frameIndex.load(std::memory_order_relaxed) % m_frames.size()];
        const u32 thread = threadIndex();
        while (frame.threads.size() <= thread) frame.threads.push_back(std::make_unique<ThreadContext>());
        CommandPoolSet& set = frame.threads[thread]->perQueue[q];
        if (!set.pool) {
            VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            ci.queueFamilyIndex = m_queues[q].family;
            if (m_vk.vkCreateCommandPool(m_device, &ci, nullptr, &set.pool) != VK_SUCCESS) {
                reportError("vkCreateCommandPool failed");
                return nullptr;
            }
        }
        if (set.used < set.lists.size()) {
            list = set.lists[set.used].get();
        } else {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = set.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            if (m_vk.vkAllocateCommandBuffers(m_device, &ai, &cmd) != VK_SUCCESS) {
                reportError("vkAllocateCommandBuffers failed");
                return nullptr;
            }
            set.lists.push_back(std::make_unique<VulkanCommandList>(*this, queue, cmd));
            list = set.lists.back().get();
        }
        ++set.used;
    }
    list->begin(name, m_frameIndex.load(std::memory_order_relaxed));
    return list;
}

Result<TimelinePoint> VulkanDevice::submit(Queue queue, std::span<CommandList* const> lists,
                                           std::span<const TimelinePoint> waits) {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
    std::vector<VkCommandBufferSubmitInfo> buffers;
    buffers.reserve(lists.size());
    const u64 frame = m_frameIndex.load(std::memory_order_relaxed);
    // Reject the whole submit before touching any list: each of these would be undefined behavior.
    for (CommandList* base : lists) {
        if (!base) {
            reportError("submit: null command list");
            return Error{ErrorCode::InvalidArgument, "submit: null command list"};
        }
        if (base->deviceTag() != static_cast<const Device*>(this)) {
            reportError("submit: command list was not acquired from this device");
            return Error{ErrorCode::InvalidArgument, "submit: foreign command list"};
        }
        auto* list = static_cast<VulkanCommandList*>(base);
        if (list->queue() != queue) {
            // Its pool may belong to another queue family.
            reportError(std::format("submit: list '{}' was acquired for {} but submitted to {}", list->name(),
                                    queueName(list->queue()), queueName(queue)));
            return Error{ErrorCode::InvalidArgument, "submit: command list belongs to another queue"};
        }
        if (list->isSubmitted()) {
            // One-time-submit buffers may not be resubmitted (and may still be pending).
            reportError(std::format("submit: list '{}' was already submitted", list->name()));
            return Error{ErrorCode::InvalidArgument, "submit: command list was already submitted"};
        }
        if (list->isRecycled() || list->acquireFrame() != frame) {
            // Its pool was (or is about to be) reset by beginFrame/waitIdle; the GPU would run a
            // command buffer in the initial state, or one whose pool is reset while pending.
            reportError(std::format("submit: list '{}' was acquired in frame {} and recycled by beginFrame/waitIdle "
                                    "before it was submitted (frame {})",
                                    list->name(), list->acquireFrame(), frame));
            return Error{ErrorCode::InvalidArgument, "submit: command list is stale (recycled)"};
        }
        if (list->isOpen() && list->owner() != std::this_thread::get_id()) {
            // Ending it here would race with the recording thread's use of its (per-thread) pool.
            reportError(std::format("submit: list '{}' is still recording on another thread; call end() there first",
                                    list->name()));
            return Error{ErrorCode::InvalidArgument, "submit: command list still recording on another thread"};
        }
    }
    for (usize i = 0; i < lists.size(); ++i) {
        for (usize j = 0; j < i; ++j) {
            if (lists[j] == lists[i]) {
                reportError("submit: the same command list appears twice in one submit");
                return Error{ErrorCode::InvalidArgument, "submit: duplicate command list"};
            }
        }
    }
    std::vector<VkSemaphoreSubmitInfo> waitInfos;
    for (const TimelinePoint& w : waits) {
        if (w.isNull()) continue;
        if (w.value > m_queues[queueIndex(w.queue)].submitted.load(std::memory_order_acquire)) {
            // Wait-before-signal across threads is not supported: it would deadlock if never signalled.
            reportError(std::format("submit {}: waits for {}:{} which was never submitted", queueName(queue),
                                    queueName(w.queue), w.value));
            return Error{ErrorCode::InvalidArgument, "submit waits for a timeline value that was never submitted"};
        }
        VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        info.semaphore = m_queues[queueIndex(w.queue)].timeline;
        info.value = w.value;
        info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waitInfos.push_back(info);
    }
    // Everything is valid: close lists still recording (on this, their recording thread).
    for (CommandList* base : lists) {
        auto* list = static_cast<VulkanCommandList*>(base);
        if (list->isOpen()) list->end();
        VkCommandBufferSubmitInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        info.commandBuffer = list->handle();
        buffers.push_back(info);
    }
    QueueSlot& slot = m_queues[queueIndex(queue)];
    u64 value = 0;
    {
        std::lock_guard lock(*slot.submitMutex);
        value = slot.submitted.load(std::memory_order_relaxed) + 1;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = slot.timeline;
        signal.value = value;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.waitSemaphoreInfoCount = static_cast<u32>(waitInfos.size());
        si.pWaitSemaphoreInfos = waitInfos.data();
        si.commandBufferInfoCount = static_cast<u32>(buffers.size());
        si.pCommandBufferInfos = buffers.data();
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signal;
        const VkResult r = m_vk.vkQueueSubmit2(slot.queue, 1, &si, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) return vkError(r, "vkQueueSubmit2");
        slot.submitted.store(value, std::memory_order_release);
    }
    for (CommandList* base : lists) static_cast<VulkanCommandList*>(base)->markSubmitted();
    if (m_desc.debugDeviceLostAfterSubmits != 0 &&
        m_submitCount.fetch_add(1, std::memory_order_acq_rel) + 1 == m_desc.debugDeviceLostAfterSubmits) {
        // Fault injection: let the work finish so breadcrumbs land, then take the device-lost path.
        {
            auto locks = lockAllQueues();
            m_vk.vkDeviceWaitIdle(m_device);
        }
        return deviceLost("vkQueueSubmit2 (injected)");
    }
    {
        // The lists were acquired in this frame (checked above), so this slot's pools hold them:
        // beginFrame must not reset those pools before this value completes.
        std::lock_guard lock(m_contextMutex);
        u64& last = m_frames[frame % m_frames.size()].lastSubmitted[queueIndex(queue)];
        last = std::max(last, value);
    }
    return TimelinePoint{queue, value};
}

u64 VulkanDevice::completedValue(Queue queue) const {
    u64 value = 0;
    if (m_vk.vkGetSemaphoreCounterValue(m_device, m_queues[queueIndex(queue)].timeline, &value) != VK_SUCCESS) {
        return 0;
    }
    return value;
}

Result<void> VulkanDevice::wait(TimelinePoint point, u64 timeoutNs) {
    if (point.isNull()) return {};
    const QueueSlot& slot = m_queues[queueIndex(point.queue)];
    if (point.value > slot.submitted.load(std::memory_order_acquire)) {
        return makeError(ErrorCode::InvalidArgument, "wait for {}:{} which was never submitted",
                         queueName(point.queue), point.value);
    }
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &slot.timeline;
    wi.pValues = &point.value;
    const VkResult r = m_vk.vkWaitSemaphores(m_device, &wi, timeoutNs);
    if (r == VK_TIMEOUT) {
        return makeError(ErrorCode::Timeout, "timed out waiting for {}:{}", queueName(point.queue), point.value);
    }
    if (r != VK_SUCCESS) return vkError(r, "vkWaitSemaphores");
    return {};
}

std::array<u64, kQueueCount> VulkanDevice::submittedValues() const {
    std::array<u64, kQueueCount> v{};
    for (u32 q = 0; q < kQueueCount; ++q) v[q] = m_queues[q].submitted.load(std::memory_order_acquire);
    return v;
}

std::array<u64, kQueueCount> VulkanDevice::completedValues() const {
    std::array<u64, kQueueCount> v{};
    for (u32 q = 0; q < kQueueCount; ++q) v[q] = completedValue(static_cast<Queue>(q));
    return v;
}

Result<void> VulkanDevice::waitValues(const std::array<u64, kQueueCount>& values, u64 timeoutNs) {
    for (u32 q = 0; q < kQueueCount; ++q) {
        if (values[q] == 0) continue;
        HELIOS_TRY(wait(TimelinePoint{static_cast<Queue>(q), values[q]}, timeoutNs));
    }
    return {};
}

std::vector<std::unique_lock<std::mutex>> VulkanDevice::lockAllQueues() {
    std::vector<std::unique_lock<std::mutex>> locks;
    for (QueueSlot& q : m_queues) {  // fixed order; aliased logical queues share one mutex
        const bool seen = std::any_of(locks.begin(), locks.end(),
                                      [&](const std::unique_lock<std::mutex>& l) { return l.mutex() == q.submitMutex; });
        if (!seen) locks.emplace_back(*q.submitMutex);
    }
    return locks;
}

Result<void> VulkanDevice::deviceWaitIdle(std::string_view where) {
    VkResult r = VK_SUCCESS;
    {
        auto locks = lockAllQueues();
        r = m_vk.vkDeviceWaitIdle(m_device);
    }
    if (r != VK_SUCCESS) return vkError(r, where);
    return {};
}

void CommandPoolSet::reset(const VolkDeviceTable& vk, VkDevice device) {
    if (pool && used > 0) vk.vkResetCommandPool(device, pool, 0);
    for (u32 i = 0; i < used && i < lists.size(); ++i) lists[i]->markRecycled();
    used = 0;
}

Result<void> VulkanDevice::waitIdle() {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
    HELIOS_TRY(deviceWaitIdle("vkDeviceWaitIdle"));
    sealGarbage();
    collectGarbage(true);
    std::lock_guard lock(m_contextMutex);
    for (FrameSlot& frame : m_frames) {
        for (auto& thread : frame.threads) {
            for (CommandPoolSet& set : thread->perQueue) set.reset(m_vk, m_device);
        }
    }
    return {};
}

Result<void> VulkanDevice::beginFrame() {
    if (isDeviceLost()) return Error{ErrorCode::InvalidState, "GPU device lost"};
    sealGarbage();
    const u64 next = m_frameIndex.load(std::memory_order_relaxed) + 1;
    FrameSlot& frame = m_frames[next % m_frames.size()];
    HELIOS_TRY(waitValues(frame.lastSubmitted, ~0ull));
    {
        std::lock_guard lock(m_contextMutex);
        for (auto& thread : frame.threads) {
            for (CommandPoolSet& set : thread->perQueue) set.reset(m_vk, m_device);
        }
        m_frameIndex.store(next, std::memory_order_relaxed);
    }
    collectGarbage(false);
    return {};
}

void VulkanDevice::defer(std::function<void()> item) {
    std::lock_guard lock(m_garbageMutex);
    m_pendingGarbage.push_back(std::move(item));
}

void VulkanDevice::sealGarbage() {
    std::lock_guard lock(m_garbageMutex);
    if (m_pendingGarbage.empty()) return;
    GarbageBatch batch;
    batch.values = submittedValues();
    batch.items = std::move(m_pendingGarbage);
    m_pendingGarbage.clear();
    m_sealedGarbage.push_back(std::move(batch));
}

void VulkanDevice::collectGarbage(bool all) {
    // Items may defer more work (pipelines still compiling); loop until nothing is ready.
    for (;;) {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard lock(m_garbageMutex);
            const std::array<u64, kQueueCount> done = all ? std::array<u64, kQueueCount>{} : completedValues();
            while (!m_sealedGarbage.empty()) {
                const GarbageBatch& batch = m_sealedGarbage.front();
                bool complete = true;
                for (u32 q = 0; q < kQueueCount && !all; ++q) complete = complete && done[q] >= batch.values[q];
                if (!complete) break;
                for (auto& item : m_sealedGarbage.front().items) ready.push_back(std::move(item));
                m_sealedGarbage.pop_front();
            }
        }
        if (ready.empty()) return;
        for (auto& item : ready) item();
        if (!all) return;
        sealGarbage();  // anything the items deferred (e.g. pipelines finishing compiles)
    }
}

} // namespace helios::rhi::vk

namespace helios::rhi::detail {

Result<std::unique_ptr<Device>> createVulkanDevice(const DeviceDesc& desc) {
    if (desc.framesInFlight == 0) return Error{ErrorCode::InvalidArgument, "framesInFlight must be >= 1"};
    auto device = std::make_unique<vk::VulkanDevice>(desc);
    HELIOS_TRY(device->init());
    return std::unique_ptr<Device>(std::move(device));
}

Result<std::vector<AdapterInfo>> enumerateVulkanAdapters() {
    VolkInstanceTable vki{};
    HELIOS_TRY_ASSIGN(VkInstance instance, vk::createEnumerationInstance(vki));
    u32 count = 0;
    vki.vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vki.vkEnumeratePhysicalDevices(instance, &count, devices.data());
    devices.resize(count);
    std::vector<AdapterInfo> out;
    for (u32 i = 0; i < count; ++i) out.push_back(vk::describeAdapter(vki, devices[i], i));
    vki.vkDestroyInstance(instance, nullptr);
    return out;
}

} // namespace helios::rhi::detail
