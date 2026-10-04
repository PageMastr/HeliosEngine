#pragma once
// AssetId: the 64-bit runtime and pak identity of an asset, folded from its 128-bit GUID (02 §6.1),
// and AssetIdSet, the collision check packaging runs before it writes a pak.
//
// The fold is XXH3-64 of the GUID's 16 canonical (big-endian) bytes. A plain `high ^ low` would
// map structured GUIDs (name-derived, or with equal halves) onto the same id; XXH3 is stable across
// platforms, compilers and endianness, so ids written into paks never change. The nil GUID folds to
// the invalid id 0. Two GUIDs may still fold to one id (about 2^-64 per pair), so packaging rejects
// collisions (AssetIdSet) instead of assuming there are none.
//
// Threading: AssetId is a value type and fromGuid() is pure. AssetIdSet is not thread-safe; use one
// per packaging job or guard it externally.

#include <compare>
#include <format>
#include <functional>
#include <string>
#include <unordered_map>

#include "helios/core/guid.h"
#include "helios/core/hash.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::asset {

class AssetId {
public:
    constexpr AssetId() noexcept = default;
    constexpr explicit AssetId(u64 value) noexcept : m_value(value) {}

    /// The fold of `guid` (0 for the nil GUID).
    static AssetId fromGuid(const Guid& guid) noexcept;

    constexpr u64 value() const noexcept { return m_value; }
    /// False for the reserved id 0.
    constexpr bool isValid() const noexcept { return m_value != 0; }
    constexpr explicit operator bool() const noexcept { return isValid(); }
    /// 16 lower-case hex digits.
    std::string toString() const;

    friend constexpr bool operator==(AssetId, AssetId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(AssetId, AssetId) noexcept = default;

private:
    u64 m_value = 0;
};

/// The ids of a set of GUIDs, with every fold collision rejected (02 §6.1: "packaging rejects
/// collisions"). The fold is injectable so tests can force collisions; packaging uses the default.
class AssetIdSet {
public:
    using FoldFn = AssetId (*)(const Guid&);

    explicit AssetIdSet(FoldFn fold = &AssetId::fromGuid) : m_fold(fold) {}

    /// Adds `guid` and returns its id. Errors: InvalidArgument for the nil GUID, for a GUID that folds
    /// to the reserved id 0, or for one whose id another GUID in the set already has (the message names
    /// both GUIDs); AlreadyExists when `guid` itself is already in the set. A failed insert changes
    /// nothing.
    Result<AssetId> insert(const Guid& guid);
    /// What insert(guid) would return, without adding it.
    Result<AssetId> check(const Guid& guid) const;
    /// The GUID that owns `id`, or nullptr.
    const Guid* find(AssetId id) const noexcept;
    usize size() const noexcept { return m_byId.size(); }

private:
    FoldFn m_fold;
    std::unordered_map<u64, Guid> m_byId;
};

} // namespace helios::asset

template <>
struct std::hash<helios::asset::AssetId> {
    std::size_t operator()(helios::asset::AssetId id) const noexcept {
        // Ids are already hash outputs; mixing keeps power-of-two tables safe for hand-made ids too.
        return static_cast<std::size_t>(helios::mix64(id.value()));
    }
};

template <>
struct std::formatter<helios::asset::AssetId> : std::formatter<std::string_view> {
    template <class Ctx>
    auto format(helios::asset::AssetId id, Ctx& ctx) const {
        return std::formatter<std::string_view>::format(id.toString(), ctx);
    }
};
