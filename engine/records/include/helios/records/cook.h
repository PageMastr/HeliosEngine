#pragma once
// The records cook (02 §3.3, §3.5 `records`, §6.5): `.hrec` canonical JSONC sources → resolved
// records → `records.client.hrdb` and `records.server.hrdb`.
//
// 1. Load. Each source is one record file (02 §3.7): `$rid` (63-bit RecordId, minted once), `$name`
//    (required, unique), optional `$parent` and `$comment`, then fields. Its type comes from the
//    caller (collectSources() maps `records/<table>/…` to the record type with that @table).
// 2. Inherit. `$parent` names another record of the same type by `$name`; parents resolve first.
//    Fields override, nested structs merge field by field, lists replace unless the field is
//    @merge(append), keyed lists merge by key (`$key` GUIDs, or the @keyed(field) value): a child
//    element with a parent's key merges into that element, a new key appends. Every other value
//    (sets, maps, optionals, arrays, variants, scalars) replaces. Unknown `$parent`, cycles, a parent of
//    another type, duplicate `$rid` (T28: reused ids are rejected) and duplicate `$name` are errors.
// 3. Check. Every RecordRef resolves to a record of its declared type; tags are valid; every HxlExpr
//    compiles (engine/hxl) with the parameters CookOptions::hxlParams, or those of its own `formula
//    Name(a, b) = …` form; and the AAA-SEC-4 layout rules hold (layout.h).
// 4. Cook. The tag table holds every tag that a TagSet value uses or a tag-declaration record declares,
//    with their ancestors, numbered in byte-wise name order (the same TagIndex values gameplay's
//    TagRegistry assigns). Both cooks carry the same table, so indices agree on client and server; the
//    client cook withholds the names of tags that only server-only data uses. Records go to the cooks
//    their type allows (a @server_only record type never reaches the client cook), in RecordId order,
//    encoded by their cooked layout for that audience.
//
// Errors are collected over all sources and reported together in a deterministic order (by source
// path, then message). Output bytes depend only on the inputs and the types: identical inputs give
// identical files on every run and toolchain.
//
// Threading: cook() and collectSources() are reentrant; they share no state.

#include <span>
#include <string>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/records/hrdb_format.h"
#include "helios/reflect/registry.h"

namespace helios::records {

inline constexpr std::string_view kClientDbFile = "records.client.hrdb";
inline constexpr std::string_view kServerDbFile = "records.server.hrdb";

/// One `.hrec` source.
struct SourceRecord {
    std::string path;                    ///< For diagnostics ("records/hull/frigate.hrec").
    const refl::TypeInfo* type = nullptr; ///< A record type (DeclKind::Record).
    std::string text;                    ///< The file's JSONC.
};

/// A record type whose records declare tags (06 §1.1): a Name field with the tag and an enum field
/// with its replication audience (values named Server, Owner, All).
struct TagDeclarationType {
    std::string typeName;
    std::string tagField;
    std::string audienceField;
};

struct CookOptions {
    /// Unknown fields are errors (a typo would otherwise drop data silently).
    bool strictUnknownFields = true;
    /// Parameters of HxlExpr values written as bare expressions.
    std::vector<std::string> hxlParams{"self"};
    std::vector<TagDeclarationType> tagDeclarations{{"helios.gameplay.TagDef", "tag", "replicate"}};
};

struct CookDiagnostic {
    std::string path;    ///< Source path ("" for errors that belong to no source).
    std::string message; ///< Starts with the property path when there is one ("thrusters[0].maxForce: …").
    friend bool operator==(const CookDiagnostic&, const CookDiagnostic&) = default;
};

struct CookStats {
    usize records = 0;
    usize clientRecords = 0;
    usize serverRecords = 0;
    usize inherited = 0;     ///< Records with a $parent.
    usize tags = 0;          ///< Entries of the tag table.
    usize withheldTags = 0;  ///< Tag names the client cook withholds.
    usize formulas = 0;      ///< HxlExpr values compiled (distinct sources).
};

struct CookOutput {
    std::vector<u8> client;
    std::vector<u8> server;
    CookStats stats;
};

/// Cooks `sources`. On failure the error lists every diagnostic (also returned in `diagnostics`).
Result<CookOutput> cook(std::span<const SourceRecord> sources, const CookOptions& options = {},
                        std::vector<CookDiagnostic>* diagnostics = nullptr);

/// Reads every `records/<table>/**/*.hrec` under `projectRoot` (sorted by path), typing each by the
/// record type of `registry` whose @table is <table>. Paths in the result are project-relative.
Result<std::vector<SourceRecord>> collectSources(const fs::Path& projectRoot, const refl::TypeRegistry& registry);

/// Writes kClientDbFile and kServerDbFile into `outDir` (created if missing), atomically.
Result<void> writeCookOutput(const CookOutput& output, const fs::Path& outDir);

/// Valid tag name: dotted segments of [A-Za-z_][A-Za-z0-9_]*, at most 256 bytes (gameplay's rule).
bool isValidTagName(std::string_view name) noexcept;

} // namespace helios::records
