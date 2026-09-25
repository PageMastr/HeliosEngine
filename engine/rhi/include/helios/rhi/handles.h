#pragma once
// RHI handle types, queues and timeline points (docs/plan/03-rendering.md §1.1).
//
// Every GPU object is referenced by a generational helios::Handle (core/handle.h): a stale handle
// (used after destroy) is detected instead of dangling, and the default-constructed handle is the
// null handle. Handles are process-local and only meaningful for the Device that issued them.
// Threading: plain values, freely copyable across threads.

#include <string_view>

#include "helios/core/handle.h"
#include "helios/core/types.h"

namespace helios::rhi {

struct BufferTag;
struct TextureTag;
struct PipelineTag;
struct SwapchainTag;

using BufferH = Handle<BufferTag>;
using TextureH = Handle<TextureTag>;
using PipelineH = Handle<PipelineTag>;
using SwapchainH = Handle<SwapchainTag>;

/// Slot in one of the global bindless descriptor arrays (shaders/core/bindless.slang). Index 0 of
/// every array is a default resource; kInvalidBindless marks "no slot".
using BindlessIndex = u32;
inline constexpr BindlessIndex kInvalidBindless = 0xFFFFFFFFu;

/// Implementation behind a Device. D3D12 is a reserved seam (Phase 3/4 gate, 03 §1.6).
enum class Backend : u8 { Null, Vulkan, D3D12 };

/// Logical queues. AsyncCompute and Transfer map to dedicated hardware queues when the adapter has
/// them and alias the graphics queue otherwise (Caps tells which); either way each logical queue
/// owns its own timeline, so synchronization code is identical on every adapter.
enum class Queue : u8 { Graphics = 0, AsyncCompute = 1, Transfer = 2 };
inline constexpr u32 kQueueCount = 3;

/// A point on a queue's timeline: work submitted to `queue` that signalled `value`. Value 0 is the
/// "null" point, which is always complete. Values on one queue increase monotonically with each
/// submit and never wrap in practice (2^64 submits).
struct TimelinePoint {
    Queue queue = Queue::Graphics;
    u64 value = 0;

    constexpr bool isNull() const noexcept { return value == 0; }
    friend constexpr bool operator==(const TimelinePoint&, const TimelinePoint&) = default;
};

/// "Null", "Vulkan", "D3D12".
std::string_view backendName(Backend backend) noexcept;
/// "Graphics", "AsyncCompute", "Transfer".
std::string_view queueName(Queue queue) noexcept;

constexpr u32 queueIndex(Queue queue) noexcept { return static_cast<u32>(queue); }

} // namespace helios::rhi
