#pragma once
// Null backend inspection (03 §1.4).
//
// The Null backend implements the whole Device API without a GPU. It
//   * validates usage: usage flags vs. operations, push-constant sizes, handle lifetimes (also
//     at submit time for lists recorded before a destroy), queue capabilities, rendering scopes,
//     label balance, and resource states (every command's expected state vs. the state tracked
//     from barriers, per texture subresource) in submission order;
//   * emulates memory for buffer and texture *copies* (upload -> copy -> readback round trips give
//     real data; shaders and clears are not executed);
//   * completes every submission immediately (timelines advance at submit);
//   * records a canonical, deterministic command-stream trace — the "trace goldens" of 03 §8.4.
//
// Trace format: one line per event, two-space indentation per nesting level, resources named by
// their debug name (or "buffer#<index>" / "texture#<index>" when unnamed):
//
//   submit Graphics #1 waits=[AsyncCompute:1]
//     list "Frame"
//       barrier texture "Color" Undefined->RenderTarget mips=0+* layers=0+*
//       beginRendering area=0,0 256x256 color0="Color" mip=0 layer=0 load=Clear(0,0,0,1) store=Store
//       ...
//
// Threading: like Device. trace()/validationErrors() may be called from any thread.

#include <string>
#include <vector>

#include "helios/rhi/device.h"

namespace helios::rhi {

class NullDevice : public Device {
public:
    /// The Null view of `device`, or nullptr for other backends (no RTTI needed).
    static NullDevice* from(Device& device) noexcept {
        return device.backend() == Backend::Null ? static_cast<NullDevice*>(&device) : nullptr;
    }

    /// Everything recorded by submit() since creation or the last clearTrace().
    virtual std::string trace() const = 0;
    virtual void clearTrace() = 0;
    /// Validation errors since creation or the last clearValidationErrors() (also sent to
    /// DeviceDesc::onMessage and counted by validationErrorCount()).
    virtual std::vector<std::string> validationErrors() const = 0;
    virtual void clearValidationErrors() = 0;
    /// Tracked state of one texture subresource (Undefined when the handle is stale).
    virtual ResourceState textureState(TextureH texture, u32 mip = 0, u32 layer = 0) const = 0;
};

} // namespace helios::rhi
