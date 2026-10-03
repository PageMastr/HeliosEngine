#pragma once
// PakMountTable: the ordered set of mounted paks and the merged AssetId index over them. A pak
// mounted later overlays every earlier one by AssetId (02 §6.3 "Patching": the SWG TRE lesson), so a
// patch pak replaces exactly the assets it carries and leaves the rest to the paks below it.
//
// Lookups return the pak by shared_ptr, so unmounting while a read is in flight is safe: the read
// finishes against the pak it resolved. Mounting changes what later lookups resolve to; instances
// already loaded keep their version until the asset layer swaps them at a frame or tick boundary
// (02 §6.4, AssetStore::commitSwaps()).
//
// Threading: all member functions are thread-safe (a reader/writer lock: lookups share it, mount and
// unmount take it exclusively). read() decodes outside the lock.

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "helios/asset/asset_id.h"
#include "helios/asset/hpak_reader.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::asset {

using PakMountId = u32;

/// Where a lookup resolved: the pak (kept alive by this value) and its TOC entry.
struct AssetLocation {
    std::shared_ptr<const HpakReader> pak;
    const HpakEntry* entry = nullptr;
    PakMountId mount = 0;
};

class PakMountTable {
public:
    /// Mounts `pak` above every mounted pak. Errors: InvalidArgument for null, AlreadyExists when this
    /// reader is already mounted. Cost: O(assets in `pak`).
    Result<PakMountId> mount(std::shared_ptr<const HpakReader> pak);
    /// Unmounts; the assets it overlaid resolve to the paks below again. False for an unknown id.
    /// Cost: rebuilds the index, O(assets in all mounted paks).
    bool unmount(PakMountId id);

    /// The topmost pak's entry for `id`, or nullopt.
    std::optional<AssetLocation> find(AssetId id) const;
    /// find() + HpakReader::read(). NotFound when no mounted pak has `id`.
    Result<std::vector<u8>> read(AssetId id) const;

    /// Distinct asset ids visible through the table.
    usize assetCount() const;
    /// Mount ids, bottom (first mounted) to top.
    std::vector<PakMountId> mounts() const;

private:
    struct Mounted {
        PakMountId id = 0;
        std::shared_ptr<const HpakReader> pak;
    };
    struct Slot {
        u32 mountIndex = 0; ///< Index into m_mounts.
        u32 entryIndex = 0; ///< Index into that pak's entries().
    };

    void indexMount(u32 mountIndex);

    mutable std::shared_mutex m_mutex;
    std::vector<Mounted> m_mounts;
    std::unordered_map<AssetId, Slot> m_index;
    PakMountId m_nextId = 1;
};

} // namespace helios::asset
