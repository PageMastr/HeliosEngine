// doctest runner for engine/authority.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/core/log.h"

namespace {
// Keep test output readable: engine Info chatter is hidden.
const bool g_quietLogs = (helios::log::setLevel(helios::log::Level::Warn), true);
} // namespace
