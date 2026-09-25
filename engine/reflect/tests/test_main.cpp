// doctest runner for engine/reflect.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "helios/core/log.h"

namespace {
const bool g_quietLogs = (helios::log::setLevel(helios::log::Level::Warn), true);
} // namespace
