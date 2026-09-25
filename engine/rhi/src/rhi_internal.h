#pragma once
// Internal helpers shared by the RHI backends (not installed).

#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/handle.h"
#include "helios/core/log.h"
#include "helios/core/memory.h"
#include "helios/core/result.h"
#include "helios/rhi/device.h"

namespace helios {
HELIOS_LOG_CHANNEL(LogRhi, "RHI");
}

namespace helios::rhi::detail {

// -- Backend factories (src/null, src/vulkan) ---------------------------------------------------
Result<std::unique_ptr<Device>> createNullDevice(const DeviceDesc& desc);
Result<std::unique_ptr<Device>> createVulkanDevice(const DeviceDesc& desc);
Result<std::vector<AdapterInfo>> enumerateVulkanAdapters();
AdapterInfo nullAdapterInfo();

// -- Environment overrides ------------------------------------------------------------------------
/// Value of an environment variable, or nullopt when unset/empty.
std::optional<std::string> envVar(const char* name);
/// DeviceDesc with HELIOS_RHI_VALIDATION / HELIOS_RHI_CAPS_MASK applied.
DeviceDesc applyEnvironment(const DeviceDesc& desc);

/// Memory tags used when a desc leaves `tag` as Unknown (registered once, thread-safe).
MemoryTag defaultBufferTag();
MemoryTag defaultTextureTag();

// -- Descriptor validation shared by all backends -------------------------------------------------
Result<void> validateBufferDesc(const BufferDesc& desc);
Result<void> validateTextureDesc(const TextureDesc& desc, const Limits& limits);
Result<void> validateGraphicsPipelineDesc(const GraphicsPipelineDesc& desc);
Result<void> validateComputePipelineDesc(const ComputePipelineDesc& desc);
/// Resolves a ViewDesc against a texture (kAll* expanded, Default type made concrete).
Result<ViewDesc> resolveView(const TextureDesc& texture, const ViewDesc& view);
/// Mip/layer counts of a range resolved against a texture (kAll* expanded, clamped).
SubresourceRange resolveRange(const TextureDesc& texture, const SubresourceRange& range);

/// True when [offset, offset + size) lies inside a buffer of `bufferSize` bytes (overflow-safe).
constexpr bool rangeFits(u64 bufferSize, u64 offset, u64 size) noexcept {
    return offset <= bufferSize && size <= bufferSize - offset;
}

/// Buffer <-> texture copy checks shared by every backend, so a bad copy is rejected the same way
/// everywhere (the Vulkan backend must never hand an out-of-bounds copy to the driver: a software
/// rasterizer would write past its allocations). Resolves 0 extents ("to the end of the mip"),
/// checks mip/layer/region bounds, block alignment, single-sampled textures and formats whose
/// buffer image layout is well defined, and validates `layout` (row pitch, offset alignment).
/// On success returns the resolved region; `bufferBytes` receives the bytes the buffer image spans.
Result<TextureRegion> resolveCopyRegion(const TextureDesc& texture, const TextureRegion& region,
                                        const BufferTextureLayout& layout, u64* bufferBytes = nullptr);

// -- SPIR-V inspection ------------------------------------------------------------------------------
enum class ShaderStage : u8 { Vertex, Fragment, Compute, Other };
struct SpirvEntryPoint {
    ShaderStage stage = ShaderStage::Other;
    std::string name;
};
/// Parses the entry points of a SPIR-V module (header + OpEntryPoint scan).
Result<std::vector<SpirvEntryPoint>> spirvEntryPoints(std::span<const u32> words);
/// Verifies that `shader` is SPIR-V containing entry point `entryPoint` for `stage`.
Result<void> validateShader(const ShaderDesc& shader, ShaderStage stage, std::string_view what);

// -- Thread-safe handle pool --------------------------------------------------------------------------
/// core HandlePool guarded by a reader/writer lock. Element addresses are stable (HandlePool
/// stores chunks), so a pointer obtained under the shared lock stays valid until the element is
/// destroyed, which the RHI defers past GPU use anyway.
template <class T, class Tag>
class LockedPool {
public:
    template <class... Args>
    Handle<Tag> create(Args&&... args) {
        std::unique_lock lock(m_mutex);
        return m_pool.create(std::forward<Args>(args)...);
    }
    T* get(Handle<Tag> handle) const {
        std::shared_lock lock(m_mutex);
        return const_cast<T*>(m_pool.get(handle));
    }
    bool destroy(Handle<Tag> handle) {
        std::unique_lock lock(m_mutex);
        return m_pool.destroy(handle);
    }
    u32 size() const {
        std::shared_lock lock(m_mutex);
        return m_pool.size();
    }
    /// Calls fn(handle, T&) for every live element under the exclusive lock.
    template <class F>
    void forEach(F&& fn) {
        std::unique_lock lock(m_mutex);
        m_pool.forEach(std::forward<F>(fn));
    }

private:
    mutable std::shared_mutex m_mutex;
    HandlePool<T, Tag> m_pool;
};

/// Free-list allocator of bindless slots [first, capacity). Thread-safe.
class SlotAllocator {
public:
    void reset(u32 capacity, u32 first = 1) {
        std::lock_guard lock(m_mutex);
        m_capacity = capacity;
        m_next = first;
        m_free.clear();
    }
    BindlessIndex allocate() {
        std::lock_guard lock(m_mutex);
        if (!m_free.empty()) {
            const u32 v = m_free.back();
            m_free.pop_back();
            return v;
        }
        return m_next < m_capacity ? m_next++ : kInvalidBindless;
    }
    void release(BindlessIndex index) {
        if (index == kInvalidBindless || index == 0) return;
        std::lock_guard lock(m_mutex);
        m_free.push_back(index);
    }
    u32 capacity() const noexcept { return m_capacity; }

private:
    std::mutex m_mutex;
    u32 m_capacity = 0;
    u32 m_next = 1;
    std::vector<u32> m_free;
};

/// Formats a handle for logs/traces: its debug name, or "<kind>#<index>".
inline std::string resourceLabel(std::string_view kind, std::string_view name, u32 index) {
    if (!name.empty()) return std::string(name);
    return std::string(kind) + "#" + std::to_string(index);
}

} // namespace helios::rhi::detail
