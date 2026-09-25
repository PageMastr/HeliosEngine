#pragma once
// Semantic analysis: name resolution, inline-type materialization, component splitting,
// attribute validation, default values and lints (AAA-SEC-1, AAA-SEC-4, ledger rules, naming).

#include "diagnostics.h"
#include "model.h"

namespace helios::schemac {

struct SemaOptions {
    /// Warn about type/field/value names that break the naming conventions.
    bool namingLints = true;
};

/// Resolves every file in `schema` (files[] must already hold parsed ASTs with imports linked).
/// Returns false if errors were reported.
bool analyze(Schema& schema, DiagnosticEngine& diags, const SemaOptions& options = {});

/// Built-in scalar/vocabulary type by name ("u8", "vec3f", "Tick", ...).
bool lookupBuiltinType(std::string_view name, Prim& prim, Builtin& builtin) noexcept;

} // namespace helios::schemac
