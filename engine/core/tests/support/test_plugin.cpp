// Minimal plugin for the DynamicLibrary test: exports C entry points via HELIOS_PLUGIN_EXPORT.
#include "helios/core/platform.h"

HELIOS_PLUGIN_EXPORT int helios_test_plugin_add(int a, int b) { return a + b; }

HELIOS_PLUGIN_EXPORT const char* helios_test_plugin_name() { return "helios-test-plugin"; }

extern "C" {
HELIOS_API_EXPORT int helios_test_plugin_counter = 42;
}
