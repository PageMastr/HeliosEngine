#pragma once
// Importers as the pipeline knows them in v0 (02 §6.2; 07 §1.10's `IImporter{extensions, settingsSchema,
// version, import}` replaces ImporterInfo when the Phase 1 importers land): an id, a version that changes
// whenever the importer's code changes its products, the source extensions it claims, the reflected type of
// its settings (the `.meta` "settings" object is read and canonicalized through it) and a build step.
//
// Threading: an ImporterRegistry is filled on one thread (add) and then only read; its const members
// may run concurrently from any thread. ImporterInfo::build must be safe to call concurrently when
// several cooks run at once. hashSettingsType() is pure.

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/asset/hpak_format.h"
#include "helios/core/hash.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/reflect/type_info.h"

namespace helios::assetpipe {

/// What a build step gets: exactly the inputs the DDC key covers (cook.h), so that a cached product is the
/// one a clean build would make (07 §4.1.1, "incremental equals clean"). The asset's identity is
/// deliberately absent (its GUID, path, labels, source-DCC path and provenance): two assets with the same
/// bytes and settings share one product. A builder that needs more needs it in the key first.
struct BuildContext {
    std::span<const u8> source; ///< The source file's bytes.
    /// Canonical compact JSON of the settings, defaults omitted: read it through ImporterInfo::settings
    /// (refl::fromJson into a default-constructed object) to see every value.
    std::string_view settings;
    asset::HpakPlatform platform = asset::HpakPlatform::PcClient; ///< The consumer (02 §6.5).
};

/// Turns a source into the cooked product for one platform. Must be deterministic: the same context
/// gives the same bytes (the DDC key assumes it, 07 §4.1.1 "incremental equals clean").
using BuildFn = std::function<Result<std::vector<u8>>(const BuildContext&)>;

/// The deepest settings type hashSettingsType() walks (nested structs, containers and variants).
inline constexpr u32 kMaxSettingsTypeDepth = 64;

/// The DDC key's fingerprint of a settings type (02 §6.2's "layout hashes", made exact for settings):
/// XXH3-128 over a walk of the type and every type reachable from it, recording each type's kind,
/// qualified name, id and version; each struct field's name, id, flags, type and default value (the value
/// a default-constructed object holds, as canonical JSON); enum values; variant alternatives; container
/// element and key types; and array sizes. A type reached twice is recorded once and then referred to by
/// position, so recursive types terminate. So a field added, removed, renamed or retyped, or a default
/// changed, changes the fingerprint (TypeInfo::layoutHash has no defaults, and StructBuilder's has no field
/// types). The zero hash for null (an importer without settings). LimitExceeded for a type nested deeper
/// than kMaxSettingsTypeDepth. Pure; the walk's encoding is part of the key (ddc.h's key format).
Result<Hash128> hashSettingsType(const refl::TypeInfo* type);

struct ImporterInfo {
    /// 1-64 characters of [a-z0-9._-], starting with a letter or digit ("png", "gltf").
    std::string id;
    /// ≥ 1. Bump it whenever the build step's code changes its products: it is in the DDC key. Changes
    /// to the settings type (fields, types, defaults) change the key by themselves (hashSettingsType()).
    u32 version = 1;
    /// Source extensions it imports, lower case with the dot (".png"), never ".meta"; matched ignoring
    /// ASCII case.
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
    /// InvalidArgument for a malformed id or extension (".meta" included), version 0, or a settings type
    /// that is not a struct or that hashSettingsType() refuses; AlreadyExists for an id already added or an
    /// extension another importer claims. Computes the settings type's fingerprint once.
    Result<void> add(ImporterInfo info);
    /// The importer with this id, or null.
    const ImporterInfo* find(std::string_view id) const noexcept;
    /// The importer claiming `fileName`'s extension (the text after its last '.', any ASCII case), or null.
    const ImporterInfo* forFile(std::string_view fileName) const noexcept;
    /// hashSettingsType() of importer `id`'s settings type, as add() computed it; the zero hash for an
    /// importer without settings or an unknown id.
    Hash128 settingsTypeHash(std::string_view id) const noexcept;
    usize size() const noexcept { return m_importers.size(); }

private:
    struct Registered {
        ImporterInfo info;
        Hash128 settingsType;
    };
    std::vector<std::unique_ptr<Registered>> m_importers; // stable addresses for find()
};

/// True when `fileName` ends in one of `importer`'s extensions (ASCII case ignored).
bool importsFile(const ImporterInfo& importer, std::string_view fileName) noexcept;

} // namespace helios::assetpipe
