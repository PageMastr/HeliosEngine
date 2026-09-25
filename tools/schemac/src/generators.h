#pragma once
// Code generators (02 §3.5): `--emit cpp`, `--emit go`, `--emit json`.

#include <string>
#include <vector>

#include "compiler.h"
#include "diagnostics.h"
#include "model.h"

namespace helios::schemac {

/// `<logical>.gen.h` / `.gen.cpp` per generated file (+ `.samples.gen.h` with options.samples).
std::vector<OutputFile> generateCpp(const Schema& schema, const CompileOptions& options);
/// One Go package: `<stem>.go` per file, `helios_runtime.go` and `helios_schema_test.go`.
std::vector<OutputFile> generateGo(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags);
/// Machine-readable schema description (editor tools, later generators).
std::string generateSchemaJson(const Schema& schema);

/// C++ identifier for a schema field name (keywords get a trailing '_').
std::string cppFieldName(const std::string& name);

} // namespace helios::schemac
