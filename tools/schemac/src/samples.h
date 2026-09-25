#pragma once
// Deterministic sample values for every generated struct, emitted as literal code in both C++
// (`<file>.samples.gen.h`) and Go (`helios_schema_test.go`). Both languages therefore build the
// same values, which the cross-language tests encode and compare byte for byte.

#include <string>

#include "model.h"

namespace helios::schemac {

/// C++ header with `<pkg>::samples::make<Type>()` and a kSamples table (type + fill function).
std::string generateCppSamples(const Schema& schema, const SourceFile& file);
/// Go test file: samples, round-trip tests and the C++ vector comparison (TestCppVectors).
std::string generateGoTest(const Schema& schema, const std::string& package);

} // namespace helios::schemac
