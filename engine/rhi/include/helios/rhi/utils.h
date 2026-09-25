#pragma once
// Backend-agnostic convenience helpers built only on the public Device API: blocking uploads and
// readbacks through transient staging buffers. Meant for tools, tests, loading screens and
// screenshots — streaming paths use the upload ring of the render layer instead.
//
// Threading: each helper acquires its own command list and submits it; call from any thread that
// may record (not concurrently with beginFrame/waitIdle). The blocking ones wait on the CPU.

#include <span>
#include <vector>

#include "helios/core/result.h"
#include "helios/rhi/device.h"

namespace helios::rhi {

/// Copies `data` into `dst` at `dstOffset` via a staging buffer on `queue`. The destination must be
/// in `state` (Undefined/General/CopyDest-compatible) before and is left in `state` afterwards
/// (Undefined -> CopyDest). Returns the point to wait on before using the data on another queue.
Result<TimelinePoint> uploadBuffer(Device& device, BufferH dst, u64 dstOffset, std::span<const std::byte> data,
                                   ResourceState state = ResourceState::Undefined, Queue queue = Queue::Graphics);

/// Uploads tightly packed texels of one subresource (mip/layer of `region`, full extent when the
/// region size is 0) and transitions it from `before` to `after` (default: sampling).
Result<TimelinePoint> uploadTexture(Device& device, TextureH dst, std::span<const std::byte> texels,
                                    const TextureRegion& region = {}, ResourceState before = ResourceState::Undefined,
                                    ResourceState after = ResourceState::ShaderResource,
                                    Queue queue = Queue::Graphics);

/// Reads `size` bytes (kWholeSize = to the end) of `src` back to the CPU; blocks. `state` is the
/// buffer's current state, restored afterwards. `waitFor` is waited for on the GPU first.
Result<std::vector<u8>> readbackBuffer(Device& device, BufferH src, u64 offset = 0, u64 size = kWholeSize,
                                       ResourceState state = ResourceState::General,
                                       TimelinePoint waitFor = {}, Queue queue = Queue::Graphics);

/// Reads one subresource (tightly packed rows) back to the CPU; blocks. The texture is moved from
/// `state` to CopySource and back.
Result<std::vector<u8>> readbackTexture(Device& device, TextureH src, ResourceState state, u32 mip = 0, u32 layer = 0,
                                        TimelinePoint waitFor = {}, Queue queue = Queue::Graphics);

} // namespace helios::rhi
