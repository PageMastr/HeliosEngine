#pragma once
// Sample record types for the Phase 0 editor, helios-tool, the UI test fixture and the
// ToolsFramework tests (schemas/sample/*.hschema; see engine/toolsfw/CMakeLists.txt).
//
// Threading: registerSampleTypes() is thread-safe (TypeRegistry serializes registration).

#include <string>

#include "helios/core/result.h"
#include "helios/reflect/registry.h"

namespace helios::tf::samples {

/// Registers sample.common, sample.ship and sample.items (idempotent).
Result<void> registerSampleTypes(refl::TypeRegistry& registry = refl::TypeRegistry::global());

/// Canonical record file of the Frigate hull (`records/hull/frigate.hrec`), the M0 demo record.
std::string sampleHullRecordText();

} // namespace helios::tf::samples
