#pragma once
// Code generators (02 §3.5): `--emit cpp`, `--emit go`, `--emit json`, `--emit luau`, `--emit sql`, `--emit repl`,
// `--emit lint`.

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
/// `<logical>.repl.gen.h/.cpp` per generated file: ComponentRepDesc tables, typed full-state codecs,
/// rpc and event tables and the file's protocol hash (helios/reflect/repl.h). Reports @quant errors
/// and replicated fields the Phase 0 full-state codec cannot carry.
std::vector<OutputFile> generateRepl(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags);
/// Runs the size-budget lints (warnings) and returns the lint report (options.lintOut): rule counts,
/// the SEC-1 classification of client->server rpcs and every finding. Call after the other generators.
std::vector<OutputFile> generateLint(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags);
/// size.unreliable's budget in bytes: the largest message engine/net sends unfragmented on every
/// unreliable channel, wire::maxPayloadFor(Channel::Latest, wire::kMaxPacketPayload) (04 §2.1: the
/// 1,200 B netcode payload less reliable's and the message's headers). schemac_tests pins it to wire.h.
inline constexpr u64 kLintUnreliableBudget = 1186;

/// C++ identifier for a schema field name (keywords get a trailing '_').
std::string cppFieldName(const std::string& name);

} // namespace helios::schemac
