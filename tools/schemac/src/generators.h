#pragma once
// Code generators (02 §3.5): `--emit cpp`, `--emit go`, `--emit json`, `--emit luau`, `--emit sql`.

#include <string>
#include <vector>

#include "compiler.h"
#include "diagnostics.h"
#include "lock.h"
#include "model.h"

namespace helios::schemac {

/// `<logical>.gen.h` / `.gen.cpp` per generated file (+ `.samples.gen.h` with options.samples).
std::vector<OutputFile> generateCpp(const Schema& schema, const CompileOptions& options);
/// One Go package: `<stem>.go` per file, `helios_runtime.go` and `helios_schema_test.go`.
std::vector<OutputFile> generateGo(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags);
/// Machine-readable schema description (editor tools, later generators).
std::string generateSchemaJson(const Schema& schema);
/// `<logical>.luau.gen.h/.cpp` (scriptlib glue on engine/script's Binder) per generated file, plus
/// `schema.d.luau` and `fuel_costs.defaults.json` in options.luauOut. Reports signatures that cannot
/// cross the Luau boundary.
std::vector<OutputFile> generateLuau(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags);
/// `<schema>/schema.sql` (snapshot) and `<schema>/migration.sql` (goose stub from `baseline`) per
/// service schema with @sql structs, in options.sqlOut. Reports fields that cannot be columns.
std::vector<OutputFile> generateSql(const Schema& schema, const Lock& baseline, const CompileOptions& options, DiagnosticEngine& diags);

/// C++ identifier for a schema field name (keywords get a trailing '_').
std::string cppFieldName(const std::string& name);

} // namespace helios::schemac
