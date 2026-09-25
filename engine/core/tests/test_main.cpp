// doctest runner for engine/core.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/core/log.h"

namespace {
// Keep test output readable: engine Info chatter is hidden; tests that inspect log output install
// their own sinks and levels.
const bool g_quietLogs = (helios::log::setLevel(helios::log::Level::Warn), true);
} // namespace
