#pragma once
// The append-only schema lock (02 §3.4): stable u32 type ids and per-type field / enum value /
// variant alternative ids, recorded in a committed JSONC file (schemas/schema.lock.jsonc).
//
// Rules enforced by applyLock():
//   * ids are minted once and never reused; deleted fields/values/alternatives become tombstones;
//   * `@was("old")` on a field (or type) renames it and keeps the id;
//   * a field's type may only widen (i8→i16→i32→i64, u8→…→u64, f32→f64, T→T?); anything else
//     needs a new field;
//   * explicit defaults are part of the wire contract (writers omit default values) and may only
//     change with --allow-default-change;
//   * enum values keep their numbers; a removed value's number is never reused;
//   * the lock is validated for hand edits (duplicate ids, deleted entries, bad counters).
// Types that are not part of the current compilation are left untouched, so several schema sets
// may share one lock.

#include <map>
#include <string>
#include <vector>

#include "diagnostics.h"
#include "model.h"

namespace helios::schemac {

struct LockField {
    u32 id = 0;
    std::string name;
    std::string type;
    std::string defaultJson; ///< explicit default ("" = implicit)
    bool tombstone = false;
    std::vector<std::string> was;
};

struct LockEnumValue {
    std::string name;
    i64 value = 0;
    bool tombstone = false;
};

struct LockType {
    u32 id = 0;
    std::string kind; ///< "struct", "enum", "flags", "variant"
    u32 version = 0;
    std::string base; ///< enum/flags underlying type
    std::vector<std::string> was;
    std::vector<LockField> fields; ///< struct fields or variant alternatives, sorted by id
    u32 nextField = 1;
    std::vector<LockEnumValue> values;
};

struct Lock {
    std::map<std::string, LockType> types;
};

/// Parses lock text. Errors are reported against `fileIndex` (registered with `diags`).
bool loadLock(std::string_view text, u32 fileIndex, Lock& out, DiagnosticEngine& diags);
/// Canonical lock text (sorted by type name, entries by id).
std::string writeLock(const Lock& lock);

struct LockOptions {
    bool allowDefaultChange = false;
    u32 lockFile = 0; ///< diagnostics file index of the lock (for consistency errors)
};

/// Assigns ids from `lock` to every lockable declaration of the generated files, extending the
/// lock for new ones. Human-readable change descriptions go to `changes`. Returns false on errors.
bool applyLock(Schema& schema, Lock& lock, DiagnosticEngine& diags, const LockOptions& options,
               std::vector<std::string>& changes);

/// True if a field of type `from` may become `to` and keep its id.
bool isWidening(std::string_view from, std::string_view to);

} // namespace helios::schemac
