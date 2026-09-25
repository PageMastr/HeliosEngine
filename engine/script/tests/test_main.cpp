// doctest runner for engine/script.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/core/log.h"

namespace {
// Keep test output readable: engine Info chatter (script print) is hidden; tests that inspect
// output install their own sinks.
const bool g_quietLogs = (helios::log::setLevel(helios::log::Level::Warn), true);
} // namespace
