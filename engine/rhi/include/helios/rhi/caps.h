#pragma once
// Adapter description, optional capabilities and limits (03 §1.2).
//
// Required features (Vulkan 1.3 dynamic rendering, synchronization2, timeline semaphores, buffer
// device address, descriptor indexing, draw-indirect-count, scalar block layout) are not listed:
// a device without them is rejected at creation. CapBit covers optional paths only; every bit can be
// masked off with DeviceDesc::capsMask or the HELIOS_RHI_CAPS_MASK environment variable
// (hex/decimal mask of bits to KEEP) so CI exercises each fallback on hardware that has the feature.
// Threading: plain values.

#include <array>
#include <string>

#include "helios/core/types.h"
#include "helios/rhi/handles.h"

namespace helios::rhi {

enum class AdapterType : u8 { Other, Integrated, Discrete, Virtual, Cpu };

struct AdapterInfo {
    u32 index = 0;           ///< Position in Device::enumerateAdapters().
    std::string name;
    AdapterType type = AdapterType::Other;
    u32 vendorId = 0;
    u32 deviceId = 0;
    std::string driverName;  ///< "llvmpipe", "NVIDIA", ...
    std::string driverInfo;  ///< Driver version string as reported by the driver.
    u32 driverVersion = 0;   ///< Raw vendor-encoded version (workaround table key).
    u32 apiVersion = 0;      ///< Packed like VK_MAKE_API_VERSION.
    u64 deviceLocalBytes = 0;
    std::array<u8, 16> deviceUuid{};  ///< Pipeline-cache key together with driverVersion.
    bool meetsRequirements = false;   ///< Supports everything Helios requires (see header comment).
    std::string missingRequirement;   ///< First missing requirement when !meetsRequirements.
};

std::string_view adapterTypeName(AdapterType type) noexcept;

/// Optional features. Bits marked "(enabled)" and the queue bits are active on the device. The
/// others (mesh shaders, ray tracing, descriptor buffers, pipeline libraries/binaries, present wait,
/// calibrated timestamps, VRS, memory priority, int64 image atomics) report adapter support; the
/// extension is enabled by the phase that first uses it (03 §9.2).
enum class CapBit : u64 {
    None = 0,
    MeshShader = 1ull << 0,               ///< VK_EXT_mesh_shader (meshlet path, Phase 3).
    RayQuery = 1ull << 1,                 ///< VK_KHR_ray_query.
    AccelerationStructure = 1ull << 2,    ///< VK_KHR_acceleration_structure.
    DescriptorBuffer = 1ull << 3,         ///< VK_EXT_descriptor_buffer.
    GraphicsPipelineLibrary = 1ull << 4,  ///< VK_EXT_graphics_pipeline_library.
    PipelineBinary = 1ull << 5,           ///< VK_KHR_pipeline_binary.
    MemoryBudget = 1ull << 6,             ///< VK_EXT_memory_budget (enabled).
    MemoryPriority = 1ull << 7,           ///< VK_EXT_memory_priority.
    DeviceFault = 1ull << 8,              ///< VK_EXT_device_fault (enabled; used on device loss).
    DebugUtils = 1ull << 9,               ///< Object names and labels reach tools (enabled).
    AsyncComputeQueue = 1ull << 10,       ///< Queue::AsyncCompute is a distinct hardware queue.
    TransferQueue = 1ull << 11,           ///< Queue::Transfer is a distinct hardware queue.
    TimestampQueries = 1ull << 12,
    ImageInt64Atomics = 1ull << 13,       ///< VK_EXT_shader_image_atomic_int64.
    StorageImageWithoutFormat = 1ull << 14, ///< Typeless storage-image read and write (enabled).
    PresentWait = 1ull << 15,             ///< VK_KHR_present_wait + present_id.
    CalibratedTimestamps = 1ull << 16,    ///< VK_EXT_calibrated_timestamps.
    FragmentShadingRate = 1ull << 17,     ///< VK_KHR_fragment_shading_rate.
    ValidationLayer = 1ull << 18,         ///< Khronos validation is active (enabled).
    Swapchain = 1ull << 19,               ///< Surfaces/swapchains can be created (enabled).
    SamplerAnisotropy = 1ull << 20,       ///< (enabled)
    ShaderInt64 = 1ull << 21,             ///< 64-bit integers in shaders (enabled).
    ShaderFloat16 = 1ull << 22,           ///< 16-bit floats in shaders (enabled).
    FillModeNonSolid = 1ull << 23,        ///< PolygonMode::Line (enabled).
};
HELIOS_ENUM_FLAGS(CapBit)

/// Name of a single capability bit ("MeshShader"); "" for combinations.
std::string_view capBitName(CapBit bit) noexcept;

struct Limits {
    u32 maxBindlessSampledImages = 0;   ///< Clamped from 131,072 (03 §1.1).
    u32 maxBindlessStorageImages = 0;   ///< Clamped from 16,384.
    u32 maxBindlessStorageBuffers = 0;  ///< Clamped from 65,536.
    u32 maxBindlessSamplers = 0;        ///< Clamped from 128.
    u32 maxPushConstantBytes = 0;       ///< Always kMaxPushConstantBytes (128).
    u32 maxTextureDimension2D = 0;
    u32 maxTextureDimension3D = 0;
    u32 maxTextureArrayLayers = 0;
    std::array<u32, 3> maxComputeWorkGroupCount{};
    std::array<u32, 3> maxComputeWorkGroupSize{};
    u32 maxComputeWorkGroupInvocations = 0;
    u64 minStorageBufferOffsetAlignment = 0;
    u64 minUniformBufferOffsetAlignment = 0;
    u32 optimalBufferCopyRowPitchAlignment = 1;
    f64 timestampPeriodNs = 0.0;
    u32 subgroupSize = 0;
    f32 maxSamplerAnisotropy = 1.0f;
};

struct Caps {
    Backend backend = Backend::Null;
    AdapterInfo adapter;
    CapBit bits = CapBit::None;
    Limits limits;

    bool has(CapBit bit) const noexcept { return hasFlag(bits, bit); }
};

} // namespace helios::rhi
