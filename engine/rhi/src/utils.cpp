// Blocking upload/readback helpers on top of the public Device API (see helios/rhi/utils.h).

#include "helios/rhi/utils.h"

#include <cstring>

#include "rhi_internal.h"

namespace helios::rhi {
namespace {

struct Staging {
    Device& device;
    BufferH buffer;
    ~Staging() { device.destroy(buffer); }
};

Result<BufferH> createStaging(Device& device, u64 size, MemoryUsage memory, std::string_view name) {
    BufferDesc desc;
    desc.size = size;
    desc.usage = memory == MemoryUsage::Upload ? BufferUsage::TransferSrc : BufferUsage::TransferDst;
    desc.memory = memory;
    desc.name = name;
    return device.createBuffer(desc);
}

Result<CommandList*> acquire(Device& device, Queue queue, std::string_view name) {
    CommandList* cmd = device.acquireCommandList(queue, name);
    if (!cmd) return Error{ErrorCode::InvalidState, "could not acquire a command list (device lost?)"};
    return cmd;
}

Result<TimelinePoint> submitOne(Device& device, Queue queue, CommandList* cmd, TimelinePoint waitFor) {
    cmd->end();
    CommandList* lists[] = {cmd};
    if (waitFor.isNull()) return device.submit(queue, lists);
    const TimelinePoint waits[] = {waitFor};
    return device.submit(queue, lists, waits);
}

} // namespace

Result<TimelinePoint> uploadBuffer(Device& device, BufferH dst, u64 dstOffset, std::span<const std::byte> data,
                                   ResourceState state, Queue queue) {
    if (data.empty()) return TimelinePoint{queue, 0};
    HELIOS_TRY_ASSIGN(BufferH stagingHandle, createStaging(device, data.size(), MemoryUsage::Upload, "uploadBuffer staging"));
    Staging staging{device, stagingHandle};
    void* mapped = device.map(staging.buffer);
    if (!mapped) return Error{ErrorCode::InvalidState, "uploadBuffer: staging buffer is not mappable"};
    std::memcpy(mapped, data.data(), data.size());
    device.flushMapped(staging.buffer);

    HELIOS_TRY_ASSIGN(CommandList * cmd, acquire(device, queue, "uploadBuffer"));
    const bool transition = state != ResourceState::CopyDest && state != ResourceState::General;
    if (transition) cmd->barrier(Barrier::bufferState(dst, state, ResourceState::CopyDest));
    cmd->copyBuffer(staging.buffer, 0, dst, dstOffset, data.size());
    if (transition && state != ResourceState::Undefined) {
        cmd->barrier(Barrier::bufferState(dst, ResourceState::CopyDest, state));
    }
    return submitOne(device, queue, cmd, {});
}

Result<TimelinePoint> uploadTexture(Device& device, TextureH dst, std::span<const std::byte> texels,
                                    const TextureRegion& regionIn, ResourceState before, ResourceState after,
                                    Queue queue) {
    const TextureDesc desc = device.textureDesc(dst);
    if (desc.width == 0) return Error{ErrorCode::InvalidArgument, "uploadTexture: invalid texture handle"};
    u64 bytes = 0;
    HELIOS_TRY_ASSIGN(const TextureRegion region, detail::resolveCopyRegion(desc, regionIn, {}, &bytes));
    if (texels.size() != bytes) {
        return makeError(ErrorCode::InvalidArgument, "uploadTexture '{}': got {} bytes, region needs {}", desc.name,
                         texels.size(), bytes);
    }
    HELIOS_TRY_ASSIGN(BufferH stagingHandle, createStaging(device, bytes, MemoryUsage::Upload, "uploadTexture staging"));
    Staging staging{device, stagingHandle};
    void* mapped = device.map(staging.buffer);
    if (!mapped) return Error{ErrorCode::InvalidState, "uploadTexture: staging buffer is not mappable"};
    std::memcpy(mapped, texels.data(), texels.size());
    device.flushMapped(staging.buffer);

    const SubresourceRange range{region.mip, 1, region.baseLayer, region.layerCount};
    HELIOS_TRY_ASSIGN(CommandList * cmd, acquire(device, queue, "uploadTexture"));
    if (before != ResourceState::CopyDest) cmd->barrier(Barrier::textureState(dst, before, ResourceState::CopyDest, range));
    cmd->copyBufferToTexture(staging.buffer, {}, dst, region);
    if (after != ResourceState::CopyDest) cmd->barrier(Barrier::textureState(dst, ResourceState::CopyDest, after, range));
    return submitOne(device, queue, cmd, {});
}

Result<std::vector<u8>> readbackBuffer(Device& device, BufferH src, u64 offset, u64 size, ResourceState state,
                                       TimelinePoint waitFor, Queue queue) {
    const BufferDesc desc = device.bufferDesc(src);
    if (desc.size == 0) return Error{ErrorCode::InvalidArgument, "readbackBuffer: invalid buffer handle"};
    if (offset > desc.size) return Error{ErrorCode::OutOfRange, "readbackBuffer: offset beyond the buffer"};
    if (size == kWholeSize) size = desc.size - offset;
    if (!detail::rangeFits(desc.size, offset, size)) {
        return Error{ErrorCode::OutOfRange, "readbackBuffer: range beyond the buffer"};
    }
    if (size == 0) return std::vector<u8>{};

    HELIOS_TRY_ASSIGN(BufferH stagingHandle, createStaging(device, size, MemoryUsage::Readback, "readbackBuffer staging"));
    Staging staging{device, stagingHandle};
    HELIOS_TRY_ASSIGN(CommandList * cmd, acquire(device, queue, "readbackBuffer"));
    const bool transition = state != ResourceState::CopySource;
    // The staging memory may be a just-freed allocation that earlier GPU work wrote: discard it
    // with a barrier so this copy is ordered after those writes on the device timeline.
    Barrier before[2] = {Barrier::bufferState(staging.buffer, ResourceState::Undefined, ResourceState::CopyDest)};
    u32 beforeCount = 1;
    if (transition) before[beforeCount++] = Barrier::bufferState(src, state, ResourceState::CopySource);
    cmd->barrier(std::span<const Barrier>(before, beforeCount));
    cmd->copyBuffer(src, offset, staging.buffer, 0, size);
    Barrier after[2] = {Barrier::bufferState(staging.buffer, ResourceState::CopyDest, ResourceState::HostRead)};
    u32 afterCount = 1;
    if (transition && state != ResourceState::Undefined) {
        after[afterCount++] = Barrier::bufferState(src, ResourceState::CopySource, state);
    }
    cmd->barrier(std::span<const Barrier>(after, afterCount));
    HELIOS_TRY_ASSIGN(TimelinePoint done, submitOne(device, queue, cmd, waitFor));
    HELIOS_TRY(device.wait(done));
    device.invalidateMapped(staging.buffer);
    const void* mapped = device.map(staging.buffer);
    if (!mapped) return Error{ErrorCode::InvalidState, "readbackBuffer: staging buffer is not mappable"};
    std::vector<u8> out(static_cast<usize>(size));
    std::memcpy(out.data(), mapped, out.size());
    return out;
}

Result<std::vector<u8>> readbackTexture(Device& device, TextureH src, ResourceState state, u32 mip, u32 layer,
                                        TimelinePoint waitFor, Queue queue) {
    const TextureDesc desc = device.textureDesc(src);
    if (desc.width == 0) return Error{ErrorCode::InvalidArgument, "readbackTexture: invalid texture handle"};
    if (mip >= desc.mipLevels || layer >= desc.arrayLayers) {
        return Error{ErrorCode::OutOfRange, "readbackTexture: mip/layer out of range"};
    }
    TextureRegion region;
    region.mip = mip;
    region.baseLayer = layer;
    u64 bytes = 0;
    HELIOS_TRY(detail::resolveCopyRegion(desc, region, {}, &bytes));
    HELIOS_TRY_ASSIGN(BufferH stagingHandle, createStaging(device, bytes, MemoryUsage::Readback, "readbackTexture staging"));
    Staging staging{device, stagingHandle};

    const SubresourceRange range{mip, 1, layer, 1};
    HELIOS_TRY_ASSIGN(CommandList * cmd, acquire(device, queue, "readbackTexture"));
    Barrier before[2] = {Barrier::bufferState(staging.buffer, ResourceState::Undefined, ResourceState::CopyDest)};
    u32 beforeCount = 1;
    if (state != ResourceState::CopySource) {
        before[beforeCount++] = Barrier::textureState(src, state, ResourceState::CopySource, range);
    }
    cmd->barrier(std::span<const Barrier>(before, beforeCount));
    cmd->copyTextureToBuffer(src, region, staging.buffer, {});
    Barrier after[2] = {Barrier::bufferState(staging.buffer, ResourceState::CopyDest, ResourceState::HostRead)};
    u32 afterCount = 1;
    if (state != ResourceState::CopySource && state != ResourceState::Undefined) {
        after[afterCount++] = Barrier::textureState(src, ResourceState::CopySource, state, range);
    }
    cmd->barrier(std::span<const Barrier>(after, afterCount));
    HELIOS_TRY_ASSIGN(TimelinePoint done, submitOne(device, queue, cmd, waitFor));
    HELIOS_TRY(device.wait(done));
    device.invalidateMapped(staging.buffer);
    const void* mapped = device.map(staging.buffer);
    if (!mapped) return Error{ErrorCode::InvalidState, "readbackTexture: staging buffer is not mappable"};
    std::vector<u8> out(static_cast<usize>(bytes));
    std::memcpy(out.data(), mapped, out.size());
    return out;
}

} // namespace helios::rhi
