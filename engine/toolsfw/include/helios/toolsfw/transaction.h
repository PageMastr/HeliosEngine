#pragma once
// Transactions with property-path diffs (07 §1.2, R08-ED-P0-03).
//
//   Op           Set{path, before?, after?}                 value at a property path (absent = no
//                                                           such map key / element)
//                | Insert{list, index, key, value} | Remove{list, index, key, value}
//                                                           list elements, keyed by GUID (keyed lists)
//                                                           or by their key field (@keyed(field))
//                | Move{list, from, to, key}
//                | Create{doc, type, file, snapshot} | Destroy{doc, type, file, snapshot}
//   Transaction  {id: (user, lamport), kind, label, ops[], baseRev{doc -> revision},
//                 afterHash{doc -> content hash}, mergeKey, author, origin, time}
//
// Values are compact canonical JSON from the schema text codec (refl::getJson), so journals, RPC
// results and diffs are readable and byte-exact: applying a transaction and then its inverse
// restores the canonical JSONC bytes (ED-1). Every op checks its precondition (the `before`
// value, the element key at an index) when applied, so replaying onto the wrong base, or undoing
// under a later foreign edit, fails with a conflict instead of corrupting the document.
//
// Threading: value types; no shared state.

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/reflect/json.h"
#include "helios/toolsfw/types.h"

namespace helios::tf {

enum class OpKind : u8 { Set, Insert, Remove, Move, Create, Destroy };

std::string_view opKindName(OpKind kind) noexcept;

struct Op {
    OpKind kind = OpKind::Set;
    DocId doc;
    /// Set: the value's property path. Insert/Remove/Move: the list's path. Create/Destroy: "".
    std::string path;
    /// Set: value before/after (compact JSON; nullopt = absent). Insert: `after` is the element.
    /// Remove: `before` is the element. Create: `after` is the record file text. Destroy: `before`.
    std::optional<std::string> before;
    std::optional<std::string> after;
    /// Insert/Remove/Move: the element's key text (keyed lists: 32 hex digits; @keyed(field): the
    /// field value's key text; plain lists: "").
    std::string key;
    u64 index = 0;   ///< Insert/Remove: element index. Move: source index.
    u64 toIndex = 0; ///< Move: destination index (in the list after the element was taken out).
    /// Create/Destroy: qualified type name and project-relative file.
    std::string typeName;
    std::string file;

    /// The op that undoes this one.
    Op inverse() const;
    friend bool operator==(const Op&, const Op&) = default;
};

enum class TxKind : u8 {
    Do,   ///< An edit.
    Undo, ///< The inverse of `target`, appended as a new transaction (07 §1.2).
    Redo, ///< `target` applied again after its undo.
};

std::string_view txKindName(TxKind kind) noexcept;

struct Transaction {
    TxId id;
    TxKind kind = TxKind::Do;
    TxId target;           ///< Undo/Redo: the Do transaction they revert or re-apply.
    std::string label;
    std::vector<Op> ops;
    std::map<DocId, u64> baseRev;   ///< Revision of each touched document before the transaction.
    std::map<DocId, u64> afterHash; ///< Content hash of each touched document after it.
    std::string mergeKey;  ///< Continuous gestures with equal keys coalesce into one undo step.
    std::string author;
    Origin origin = Origin::Ui;
    i64 time = 0;          ///< Unix nanoseconds at commit.

    bool empty() const noexcept { return ops.empty(); }
    /// Distinct documents the ops touch, in first-touch order.
    std::vector<DocId> documents() const;
    /// Approximate memory footprint (history cap accounting).
    usize byteSize() const noexcept;

    /// Compact single-line JSON (journal records, RPC results).
    std::string toJson() const;
    void writeJson(refl::JsonWriter& out) const;
    static Result<Transaction> fromJson(std::string_view text);
    static Result<Transaction> fromJson(refl::JsonValue value);
};

void writeOpJson(refl::JsonWriter& out, const Op& op);
Result<Op> readOpJson(refl::JsonValue value);

} // namespace helios::tf
