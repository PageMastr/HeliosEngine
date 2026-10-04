#pragma once
// Test projects for the ToolsFramework tests and the ED-1 burst child: a directory with
// records/hull/frigate.hrec and a deterministic FrameworkConfig.

#include <string_view>

#include "helios/core/fs.h"
#include "helios/toolsfw/framework.h"

namespace helios::tf::test {

/// Registers the sample types once (thread-safe, idempotent).
void ensureSampleTypes();

/// Creates (or empties) `root` and writes records/hull/frigate.hrec into it.
Result<void> writeProject(const fs::Path& root, bool frigate = true);

/// A framework config over `root` with a fixed clock and seeded keyed-list keys, so identical runs
/// produce identical documents. `journalRoot` empty = no journal.
FrameworkConfig deterministicConfig(const fs::Path& root, const fs::Path& journalRoot, u64 seed,
                                    std::string_view session = "test");

/// Opens records/hull/frigate.hrec.
Result<DocId> openSampleHull(Framework& fw);

} // namespace helios::tf::test
