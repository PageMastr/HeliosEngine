// doctest runner for engine/asset.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/core/log.h"

namespace {
// The hostile-input tests provoke many checksum warnings; keep the output to real failures.
const bool g_quietLogs = (helios::log::setLevel(helios::log::Level::Error), true);
} // namespace
