// PakMountTable: paks overlaid by AssetId, later mounts on top (02 §6.3 "Patching").

#include "helios/asset/mount_table.h"

#include <algorithm>
#include <mutex>

namespace helios::asset {

Result<PakMountId> PakMountTable::mount(std::shared_ptr<const HpakReader> pak) {
    if (!pak) return Error{ErrorCode::InvalidArgument, "PakMountTable::mount: null pak"};
    std::unique_lock lock(m_mutex);
    for (const Mounted& m : m_mounts)
        if (m.pak == pak) return makeError(ErrorCode::AlreadyExists, "'{}' is already mounted", pak->name());
    const PakMountId id = m_nextId++;
    m_mounts.push_back(Mounted{id, std::move(pak)});
    indexMount(static_cast<u32>(m_mounts.size() - 1));
    return id;
}

bool PakMountTable::unmount(PakMountId id) {
    std::shared_ptr<const HpakReader> dropped; // released after the lock
    std::unique_lock lock(m_mutex);
    const auto it = std::find_if(m_mounts.begin(), m_mounts.end(), [id](const Mounted& m) { return m.id == id; });
    if (it == m_mounts.end()) return false;
    dropped = std::move(it->pak);
    m_mounts.erase(it);
    m_index.clear();
    for (u32 i = 0; i < m_mounts.size(); ++i) indexMount(i);
    lock.unlock();
    return true;
}

void PakMountTable::indexMount(u32 mountIndex) {
    const std::span<const HpakEntry> entries = m_mounts[mountIndex].pak->entries();
    m_index.reserve(m_index.size() + entries.size());
    for (u32 i = 0; i < entries.size(); ++i) m_index.insert_or_assign(entries[i].id, Slot{mountIndex, i});
}

std::optional<AssetLocation> PakMountTable::find(AssetId id) const {
    std::shared_lock lock(m_mutex);
    const auto it = m_index.find(id);
    if (it == m_index.end()) return std::nullopt;
    const Mounted& m = m_mounts[it->second.mountIndex];
    return AssetLocation{m.pak, &m.pak->entries()[it->second.entryIndex], m.id};
}

Result<std::vector<u8>> PakMountTable::read(AssetId id) const {
    const std::optional<AssetLocation> loc = find(id);
    if (!loc) return makeError(ErrorCode::NotFound, "no mounted pak has asset {}", id);
    return loc->pak->read(*loc->entry);
}

usize PakMountTable::assetCount() const {
    std::shared_lock lock(m_mutex);
    return m_index.size();
}

std::vector<PakMountId> PakMountTable::mounts() const {
    std::shared_lock lock(m_mutex);
    std::vector<PakMountId> out;
    out.reserve(m_mounts.size());
    for (const Mounted& m : m_mounts) out.push_back(m.id);
    return out;
}

} // namespace helios::asset
