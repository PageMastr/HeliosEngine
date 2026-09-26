// Item definition and instance validation (06 §2).
#include "helios/gameplay/items.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace helios::gameplay {
namespace {

bool finiteNonNegative(f64 v) noexcept { return std::isfinite(v) && v >= 0.0; }

Result<void> checkQuery(const std::optional<refl::TagQuery>& q, const TagRegistry* tags, std::string_view what) {
    if (!q || !tags) return {};
    auto compiled = tags->compileQuery(q->text);
    if (!compiled) return makeError(compiled.errorCode(), "{}: {}", what, compiled.error().message);
    return {};
}

} // namespace

Result<void> validateItemDef(const ItemDef& def, const TagRegistry* tags) {
    if (def.stackMax < 1 || def.stackMax > kMaxStackSize) {
        return makeError(ErrorCode::InvalidArgument, "stackMax must be in [1, {}]", kMaxStackSize);
    }
    if (!finiteNonNegative(def.volume) || !finiteNonNegative(def.mass)) {
        return Error{ErrorCode::InvalidArgument, "volume and mass must be finite and non-negative"};
    }
    for (Name t : def.tags.tags()) {
        if (!isValidTagName(t.view())) return makeError(ErrorCode::InvalidArgument, "invalid tag '{}'", t.view());
        if (tags && tags->find(t.view()) == kInvalidTag) return makeError(ErrorCode::NotFound, "undeclared tag '{}'", t.view());
    }
    for (usize i = 0; i < def.arrangements.size(); ++i) {
        const auto& slots = def.arrangements[i];
        if (slots.empty()) return makeError(ErrorCode::InvalidArgument, "arrangement {} is empty", i);
        std::vector<std::string_view> seen;
        for (Name s : slots) {
            if (!isValidTagName(s.view())) return makeError(ErrorCode::InvalidArgument, "invalid slot name '{}'", s.view());
            if (std::find(seen.begin(), seen.end(), s.view()) != seen.end()) {
                return makeError(ErrorCode::InvalidArgument, "arrangement {} names slot '{}' twice", i, s.view());
            }
            seen.push_back(s.view());
        }
    }
    const bool perInstanceState = !def.sockets.empty() || def.container.has_value() || def.decay.has_value();
    if (def.stackMax > 1 && perInstanceState) {
        return Error{ErrorCode::InvalidArgument, "items with sockets, a container or decay cannot stack (stackMax must be 1)"};
    }
    for (const SocketEntry& s : def.sockets) {
        if (s.type.isNone()) return Error{ErrorCode::InvalidArgument, "socket without a type"};
    }
    if (def.container) {
        const ContainerSpec& c = *def.container;
        if (!finiteNonNegative(c.maxVolume)) return Error{ErrorCode::InvalidArgument, "container maxVolume must be >= 0"};
        HELIOS_TRY(checkQuery(c.allowed, tags, "container filter"));
        std::vector<std::string_view> slotNames;
        for (const SlotDescriptor& s : c.slots) {
            if (!isValidTagName(s.slot.view())) return makeError(ErrorCode::InvalidArgument, "invalid container slot name '{}'", s.slot.view());
            if (std::find(slotNames.begin(), slotNames.end(), s.slot.view()) != slotNames.end()) {
                return makeError(ErrorCode::InvalidArgument, "container slot '{}' is declared twice", s.slot.view());
            }
            slotNames.push_back(s.slot.view());
            HELIOS_TRY(checkQuery(s.accepts, tags, "slot filter"));
        }
    }
    if (def.port) {
        if (def.port->minSize > def.port->maxSize) return Error{ErrorCode::InvalidArgument, "item port minSize > maxSize"};
        HELIOS_TRY(checkQuery(def.port->accepts, tags, "item port filter"));
    }
    if (def.decay) {
        const DecaySpec& d = *def.decay;
        if (!(std::isfinite(d.maxDurability) && d.maxDurability > 0.0f) || !finiteNonNegative(d.perUse) ||
            !finiteNonNegative(d.perHour)) {
            return Error{ErrorCode::InvalidArgument, "decay needs maxDurability > 0 and non-negative rates"};
        }
    }
    HELIOS_TRY(checkQuery(def.equipReq.tags, tags, "equip requirement"));
    for (const AttrMinimum& m : def.equipReq.attrs) {
        if (std::isnan(m.min)) return makeError(ErrorCode::InvalidArgument, "equip requirement on attribute {} is NaN", m.attr.id);
    }
    for (const auto& [attr, value] : def.baseAttrs) {
        if (!std::isfinite(value)) return makeError(ErrorCode::InvalidArgument, "base attribute {} is not finite", attr.id);
    }
    return {};
}

Result<void> validateItemInstance(const ItemInstance& item, const ItemDef& def) {
    if (item.quantity < 1 || item.quantity > def.stackMax) {
        return makeError(ErrorCode::OutOfRange, "quantity {} outside [1, {}]", item.quantity, def.stackMax);
    }
    const f32 maxDurability = def.decay ? def.decay->maxDurability : 1.0f;
    const f32 d = item.payload.durability;
    if (!(std::isfinite(d) && d >= 0.0f && d <= maxDurability)) {
        return makeError(ErrorCode::OutOfRange, "durability {} outside [0, {}]", d, maxDurability);
    }
    for (const auto& [attr, value] : item.payload.rolled) {
        if (!std::isfinite(value)) return makeError(ErrorCode::InvalidArgument, "rolled attribute {} is not finite", attr.id);
    }
    if (item.payload.plugs.size() > def.sockets.size()) {
        return makeError(ErrorCode::OutOfRange, "{} plugs for {} sockets", item.payload.plugs.size(), def.sockets.size());
    }
    return {};
}

} // namespace helios::gameplay
