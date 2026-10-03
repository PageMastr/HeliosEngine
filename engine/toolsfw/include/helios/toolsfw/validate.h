#pragma once
// Schema validation of reflected objects: the pre-commit hook of 07 §1.2 ("pre-commit hooks
// enforce schema ranges and reference integrity") and `helios-tool validate`.
//
// Checks: `@range(min, max)` on numeric fields (and the elements of numeric lists), finite floats,
// `@max(n)` element counts, keyed-list keys (non-nil, unique), @keyed(field) key uniqueness, and
// record headers (valid `$rid`, non-empty `$name`).
//
// Threading: pure functions.

#include <string>
#include <string_view>
#include <vector>

#include "helios/reflect/record.h"
#include "helios/reflect/type_info.h"
#include "helios/toolsfw/transaction.h"

namespace helios::tf {

class Framework;

struct Issue {
    enum class Severity : u8 { Error, Warning };
    Severity severity = Severity::Error;
    std::string file;     ///< Project-relative file, if known.
    std::string path;     ///< Property path ("" for the whole object / header).
    std::string rule;     ///< "range", "finite", "max", "key", "header", "canonical", "parse".
    std::string message;
};

std::string_view severityName(Issue::Severity severity) noexcept;
/// "file: path: message (rule)".
std::string formatIssue(const Issue& issue);

/// Validates a whole object; issues are appended to `out`.
void validateObject(const refl::TypeInfo& type, const void* object, std::vector<Issue>& out);
/// Validates the value at `path` (and everything below it).
void validateAt(const refl::TypeInfo& type, const void* object, std::string_view path, std::vector<Issue>& out);
void validateHeader(const refl::RecordHeader& header, std::vector<Issue>& out);

/// The built-in pre-commit hook: every Set/Insert op's value is validated after the transaction's
/// ops were applied; Error issues reject the transaction.
Result<void> validateTransaction(const Framework& framework, const Transaction& tx);

} // namespace helios::tf
