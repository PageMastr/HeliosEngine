#pragma once
// Importers as the pipeline knows them in v0 (02 §6.2; 07 §1.10's `IImporter{extensions, settingsSchema,
// version, import}` replaces ImporterInfo when the Phase 1 importers land): an id, a version that changes
// whenever the importer's products change, the source extensions it claims, the reflected type of its
// settings (the `.meta` "settings" object is read and canonicalized through it) and a build step.
//
// Threading: an ImporterRegistry is filled on one thread (add) and then only read; its const members
// may run concurrently from any thread. ImporterInfo::build must be safe to call concurrently when
// several cooks run at once.

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/asset/hpak_format.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/reflect/type_info.h"

namespace helios::assetpipe {

struct AssetMeta;

/// What a build step gets: the source bytes, its sidecar, the resolved settings and the consumer.
struct BuildContext {
    std::span<const u8> source;             ///< The source file's bytes.
    const AssetMeta& meta;                  ///< Its validated `.meta`.
    std::string_view settings;              ///< Canonical compact JSON of the resolved settings.
    asset::HpakPlatform platform = asset::HpakPlatform::PcClient; ///< The consumer (02 §6.5).
};

/// Turns a source into the cooked product for one platform. Must be deterministic: the same context
/// gives the same bytes (the DDC key assumes it, 07 §4.1.1 "incremental equals clean").
using BuildFn = std::function<Result<std::vector<u8>>(const BuildContext&)>;

struct ImporterInfo {
    /// 1-64 characters of [a-z0-9._-], starting with a letter or digit ("png", "gltf").
    std::string id;
    /// ≥ 1. Bump it whenever the products change (code, defaults or settings layout): it is in the DDC key.
    u32 version = 1;
    /// Source extensions it imports, lower case with the dot (".png"); matched ignoring ASCII case.
    std::vector<std::string> extensions;
    /// Its settings: a reflected struct type, or null for an importer without settings.
    const refl::TypeInfo* settings = nullptr;
    /// Imports fonts, whose sources may also carry SIL OFL-1.1 (ADR-010).
    bool fonts = false;
    /// The build step (cookAsset() fails with Unsupported when it is empty).
    BuildFn build;
};

class ImporterRegistry {
public:
    /// InvalidArgument for a malformed id or extension, version 0 or a settings type that is not a
    /// struct; AlreadyExists for an id already added or an extension another importer claims.
    Result<void> add(ImporterInfo info);
    /// The importer with this id, or null.
    const ImporterInfo* find(std::string_view id) const noexcept;
    /// The importer claiming `fileName`'s extension (the text after its last '.', any ASCII case), or null.
    const ImporterInfo* forFile(std::string_view fileName) const noexcept;
    usize size() const noexcept { return m_importers.size(); }

private:
    std::vector<std::unique_ptr<ImporterInfo>> m_importers; // stable addresses for find()
};

/// True when `fileName` ends in one of `importer`'s extensions (ASCII case ignored).
bool importsFile(const ImporterInfo& importer, std::string_view fileName) noexcept;

} // namespace helios::assetpipe
