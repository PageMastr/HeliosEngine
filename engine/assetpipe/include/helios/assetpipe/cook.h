#pragma once
// Cooking one asset through the local DDC (02 §6.2): load and validate the source's `.meta`, hash the
// source bytes, form the DDC key, and return the cached product on a hit; on a miss run the importer's
// build step and store the product. This is the v0 seam that helios-assetd's job DAG (02 §6.4, 07 §3.1)
// and helios-pack (08) build on; it has no dependency tracking or job scheduling yet.
//
// Threading: cookAsset() may run concurrently for different (or the same) assets: it reads the registry,
// LocalDdc is thread-safe and the importer's build step must be (ImporterInfo::build).

#include <string_view>
#include <vector>

#include "helios/asset/hpak_format.h"
#include "helios/assetpipe/ddc.h"
#include "helios/assetpipe/importer.h"
#include "helios/assetpipe/meta.h"
#include "helios/core/fs.h"
#include "helios/core/hash.h"
#include "helios/core/result.h"

namespace helios::assetpipe {

struct CookRequest {
    const ImporterRegistry* importers = nullptr; ///< Required.
    LocalDdc* ddc = nullptr;                     ///< Required.
    fs::Path root;                               ///< Project root.
    std::string_view path;                       ///< Project-relative source path (with a sidecar).
    asset::HpakPlatform platform = asset::HpakPlatform::PcClient;
};

struct CookResult {
    AssetMeta meta;
    Hash128 key; ///< The DDC key the product is stored under.
    std::vector<u8> product;
    bool hit = false;    ///< Served by the DDC (the importer did not run).
    bool stored = false; ///< A miss whose product was written to the DDC.
};

/// Cooks `request.path` for `request.platform`. The key uses the registered importer's version (not the
/// sidecar's), the sidecar's canonical settings with the settings type's layout hash, the source bytes'
/// hash and kCookerVersion. A damaged DDC entry is a miss. Errors: the sidecar's (loadMeta), the source
/// read's, Unsupported for an importer without a build step, and the build step's own. A failed DDC put
/// is logged and leaves `stored` false: the product is still returned.
Result<CookResult> cookAsset(const CookRequest& request);

} // namespace helios::assetpipe
