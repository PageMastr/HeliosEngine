// AssetId fold and AssetIdSet (02 §6.1).

#include "helios/asset/asset_id.h"

#include <array>

namespace helios::asset {

AssetId AssetId::fromGuid(const Guid& guid) noexcept {
    if (guid.isNil()) return AssetId{};
    const std::array<u8, 16> bytes = guid.toBytes();
    return AssetId{hash64(bytes.data(), bytes.size())};
}

std::string AssetId::toString() const { return std::format("{:016x}", m_value); }

Result<AssetId> AssetIdSet::check(const Guid& guid) const {
    if (guid.isNil()) return Error{ErrorCode::InvalidArgument, "the nil GUID is not an asset"};
    const AssetId id = m_fold(guid);
    if (!id.isValid())
        return makeError(ErrorCode::InvalidArgument, "asset {} folds to the reserved AssetId 0", guid);
    const auto it = m_byId.find(id.value());
    if (it == m_byId.end()) return id;
    if (it->second == guid)
        return makeError(ErrorCode::AlreadyExists, "asset {} is already in the set", guid);
    return makeError(ErrorCode::InvalidArgument, "AssetId collision: assets {} and {} both fold to {}",
                     it->second, guid, id);
}

Result<AssetId> AssetIdSet::insert(const Guid& guid) {
    HELIOS_TRY_ASSIGN(const AssetId id, check(guid));
    m_byId.emplace(id.value(), guid);
    return id;
}

const Guid* AssetIdSet::find(AssetId id) const noexcept {
    const auto it = m_byId.find(id.value());
    return it == m_byId.end() ? nullptr : &it->second;
}

} // namespace helios::asset
