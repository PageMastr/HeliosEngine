#pragma once
// `.meta` sidecars v0 (02 §6.1, 07 T24): every imported source file `<name>` has `<name>.meta` next to
// it, in canonical JSONC (02 §3.7, written with engine/reflect's JsonWriter):
//
//   {
//     "$meta": 0,                                     // sidecar format version
//     "guid": "8c0f4d1e-2b7a-4c39-9e51-0d6a3f2b7c18", // minted once on first import, never changed
//     "importer": "png",
//     "importerVersion": 2,                           // the importer version the settings were written for
//     "settings": {"mips": false},                    // the importer's settings type, defaults omitted
//     "labels": ["hull", "scout"],                  // sorted, unique
//     "source": "art/src/scout/hull.blend",         // source-DCC path (project-relative), if any
//     "provenance": {                                 // required (01 §5.2)
//       "origin": "original",                         // original | commissioned | cc0 | ai-assisted
//       "author": "Owner",
//       "licence": "MIT",                             // an SPDX id on 01 §5.2's list; fail closed
//       "url": "...",                                 // cc0: where it was obtained (required)
//       "rights": "...",                              // commissioned: how the rights were assigned (required)
//       "ai": {"tool": "...", "model": "...", "prompt": "...", "inputs": []}, // ai-assisted only (required)
//       "notes": "..."
//     }
//   }
//
// Keys are written in that order, values at their default (empty settings, labels, source and the empty
// optional provenance strings) are omitted, and writeMeta(parseMeta(text)) == text for canonical text.
// The GUID follows the file: a sidecar stores no path of its own, and moveAsset() moves both files.
//
// Paths (Windows first): sidecar functions take a project root and a '/'-separated path relative to it,
// checked by checkProjectPath(): valid UTF-8, no '\', no "." or ".." or empty component, no component above
// 255 bytes, no character Windows forbids in a name (<>:"|?* and control characters), and no component that
// engine/core's fs::isNonPortableComponent rejects (device names such as CON or nul.png, names ending in '.'
// or ' '). A source's file name is at most kMaxSourceNameBytes, so that its sidecar's name fits; the
// writers' temporary files have short names of their own (".<GUID>.tmp", ".<GUID>.moving"), so every such
// name can get a sidecar. Two paths that differ only in ASCII case name one file on Windows: scanMetas()
// reports them, moveAsset() refuses a target that a different file (or directory spelling) already holds in
// another case, ensureMeta() refuses a path whose directories exist in another spelling, and saveMeta()
// takes only the exact spelling. Case is folded for ASCII only: NTFS also folds other letters ("Ä" and
// "ä"), which these checks do not see (a gap, see the README).
//
// Threading: the free functions are stateless and may run concurrently, except that the sidecar writers
// (ensureMeta, saveMeta, moveAsset) assume one writer per project (helios-assetd, 02 §6.1); two
// processes creating the same sidecar at once can each mint a GUID, and the last rename wins. Cost: the
// writers list each directory on their path once per call for the case checks, O(entries there), and write
// durably (flushed temp file, persisted rename); the README states the budget.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/asset/asset_id.h"
#include "helios/assetpipe/importer.h"
#include "helios/core/fs.h"
#include "helios/core/guid.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::assetpipe {

inline constexpr u32 kMetaVersion = 0;
inline constexpr std::string_view kMetaExtension = ".meta";
/// A larger sidecar is refused before it is parsed (hostile or broken files).
inline constexpr u64 kMaxMetaBytes = 1 * kMiB;
inline constexpr usize kMaxLabels = 64;
inline constexpr usize kMaxLabelBytes = 64;
/// A source's file name leaves room for its sidecar's ".meta" in a 255-byte component (NTFS and ext4).
inline constexpr usize kMaxSourceNameBytes = 255 - kMetaExtension.size();

/// Where an asset came from (01 §5.2; the same kinds as docs/concept's sidecars).
enum class Origin : u8 {
    Original,     ///< Made for the project by its owner or a contributor.
    Commissioned, ///< Made under contract, with the rights assigned (`rights` says how).
    Cc0,          ///< Third-party CC0 work (`url` says where it came from; licence CC0-1.0).
    AiAssisted,   ///< Made with a generator (`ai` records the tool, model, prompt and inputs).
};

/// "original", "commissioned", "cc0", "ai-assisted".
std::string_view originName(Origin origin) noexcept;

struct AiProvenance {
    std::string tool;                ///< The generator product.
    std::string model;               ///< Model name and version.
    std::string prompt;              ///< The full prompt, negative prompt included.
    std::vector<std::string> inputs; ///< Every input: an asset GUID or a description (never a game capture).
    friend bool operator==(const AiProvenance&, const AiProvenance&) = default;
};

struct Provenance {
    Origin origin = Origin::Original;
    std::string author;             ///< Required.
    std::string licence;            ///< Required: an SPDX id, see checkLicence().
    std::string url;                ///< Required for Cc0.
    std::string rights;             ///< Required for Commissioned.
    std::optional<AiProvenance> ai; ///< Required for AiAssisted, absent otherwise.
    std::string notes;
    friend bool operator==(const Provenance&, const Provenance&) = default;
};

struct AssetMeta {
    Guid guid;
    std::string importer;
    u32 importerVersion = 0;
    std::string settings = "{}";     ///< Canonical compact JSON (resolveSettings()).
    std::vector<std::string> labels; ///< Sorted, unique.
    std::string source;              ///< Source-DCC path, project-relative; empty when none.
    Provenance provenance;
    friend bool operator==(const AssetMeta&, const AssetMeta&) = default;
};

/// The licences 01 §5.2 allows, as SPDX ids: MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, Zlib,
/// BSL-1.0, ISC, PostgreSQL, CC0-1.0. OFL-1.1 is added for font importers only (ADR-010).
std::span<const std::string_view> allowedLicences() noexcept;

/// OK for an allowed licence (exact SPDX spelling); InvalidArgument otherwise, naming the list (an
/// unknown or misspelled id fails closed; a different-case spelling is told the right one).
Result<void> checkLicence(std::string_view spdx, bool fontImporter);

/// The project-path rule above. InvalidArgument naming the path and the reason.
Result<void> checkProjectPath(std::string_view path);

/// Reads `settingsJson` (a JSON object) through `importer.settings` with unknown fields refused and
/// returns its canonical compact text (schema order, defaults omitted; "{}" when all are default). An
/// importer without a settings type takes only "{}". ParseError naming the setting on failure.
Result<std::string> resolveSettings(const ImporterInfo& importer, std::string_view settingsJson);

/// Checks a sidecar's values against the rules above and the registry: a non-nil GUID, a registered
/// importer, importerVersion in 1..the registered version (a higher one is VersionMismatch: the sidecar
/// was written by a newer importer), settings already canonical, sorted unique labels, the source path,
/// the provenance rules, and valid UTF-8 in every string. Errors name the field.
Result<void> validateMeta(const AssetMeta& meta, const ImporterRegistry& importers);

/// Parses and validates sidecar text. Fails closed: an unknown or duplicate key, a missing required
/// field, a wrong type, a non-canonical GUID spelling or settings the importer's type rejects is an
/// error (ParseError, NotFound for an unknown importer, VersionMismatch for a newer `$meta` or
/// importerVersion, InvalidArgument for a rule), prefixed with `sourceName` and the JSON path. Labels
/// are sorted and deduplicated and the settings canonicalized, so writeMeta() of the result is canonical.
Result<AssetMeta> parseMeta(std::string_view text, const ImporterRegistry& importers,
                            std::string_view sourceName = "<meta>");

/// The canonical text of a valid sidecar (validateMeta() first; nothing invalid is ever written).
Result<std::string> writeMeta(const AssetMeta& meta, const ImporterRegistry& importers);

/// `<path>.meta`.
std::string metaPathFor(std::string_view sourcePath);

/// Loads and validates the sidecar of `root`/`path` (NotFound when it has none; InvalidArgument for a path
/// failing the rules above, including a source name too long to have a sidecar).
Result<AssetMeta> loadMeta(const fs::Path& root, std::string_view path, const ImporterRegistry& importers);

/// Updates the existing sidecar of `root`/`path` atomically (a flushed temp file renamed over it), only when
/// its bytes change. It never creates one: that mints a GUID, which is ensureMeta()'s alone. NotFound when
/// the source or its sidecar does not exist spelled exactly so (another ASCII-case spelling is one file on
/// Windows and two on Linux, so it is refused on both); InvalidArgument when `meta`'s importer does not
/// import the source's extension; InvalidState when the sidecar's GUID differs from `meta.guid` or cannot be
/// read: a GUID never changes.
Result<void> saveMeta(const fs::Path& root, std::string_view path, const AssetMeta& meta,
                      const ImporterRegistry& importers);

/// What a first import records.
struct NewMeta {
    std::string importer;        ///< Empty: the importer that claims the file's extension.
    std::string settings = "{}"; ///< JSON object, resolved through the importer.
    std::vector<std::string> labels;
    std::string source;
    Provenance provenance;
};

struct EnsuredMeta {
    AssetMeta meta;
    bool created = false;
};

/// Create on first import: returns the existing sidecar of `root`/`path` (validated, unchanged), or mints
/// a GUID and writes a new one from `init` (importerVersion = the registered version). Fails when the
/// path breaks the rules above, the source is missing, no importer claims it, `init` is invalid, or a
/// sidecar or one of the path's directories exists in another ASCII case (InvalidState: minting a second
/// GUID for one file on Windows, or splitting one Windows directory into two).
Result<EnsuredMeta> ensureMeta(const fs::Path& root, std::string_view path, const NewMeta& init,
                               const ImporterRegistry& importers);

/// GUID-stable move or rename: moves `root`/`from` and its sidecar to `to` (creating directories); the
/// sidecar is moved, not rewritten, so the GUID follows the file. A case-only rename goes through a
/// hidden temporary name next to the file (".<GUID>.moving"). Refused (nothing moved): a path failing the rules above, a source without a valid
/// sidecar, a target that another file (or sidecar) holds in any ASCII case of any of its components, or
/// whose directory exists only in another spelling (AlreadyExists), or a target extension its importer
/// does not claim. Where Linux holds two spellings of one Windows directory, a move from one into the
/// other is allowed: it merges them. If the sidecar cannot follow, the source is moved back; a crash
/// between the two renames leaves a source without a sidecar and an orphan sidecar, which scanMetas()
/// reports. A crash inside a case-only rename can also leave the file or its sidecar under its hidden
/// temporary name, which the scan skips (it reports the other half as missing or orphaned).
Result<void> moveAsset(const fs::Path& root, std::string_view from, std::string_view to,
                       const ImporterRegistry& importers);

struct ScannedAsset {
    std::string path; ///< Project-relative source path.
    AssetMeta meta;
};

struct MetaProblem {
    std::string path; ///< The file the problem is about.
    ErrorCode code = ErrorCode::InvalidArgument;
    std::string message;
};

struct MetaScan {
    std::vector<ScannedAsset> assets;  ///< Sources with a valid sidecar, by path.
    std::vector<MetaProblem> problems; ///< Everything wrong, in path order.
};

/// Validates every sidecar under `root` (recursively; the registry rebuild's input, 02 §6.1): a source
/// (a file an importer claims) without a sidecar, an orphan sidecar, a sidecar whose name differs from its
/// source's only in case, a sidecar extension not in lower case, an invalid sidecar, a source its
/// importer does not claim, two paths that differ only in case, a non-portable path, a duplicate GUID
/// (naming both files) and an AssetId fold collision (02 §6.1; tests pass a weak `fold` to reach it).
/// Hidden files and directories (a component starting with '.') are skipped. Fails only when `root`
/// cannot be listed.
Result<MetaScan> scanMetas(const fs::Path& root, const ImporterRegistry& importers,
                           asset::AssetIdSet::FoldFn fold = &asset::AssetId::fromGuid);

} // namespace helios::assetpipe
