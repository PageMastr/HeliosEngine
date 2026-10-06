#pragma once
// The editor font (07 §1.3): Roboto 2.138 Regular (Apache-2.0, third_party/roboto), embedded by
// CMakeLists.txt at build time so every machine renders the same glyphs (the ED-15 goldens pin them).

#include "helios/core/types.h"
#include "helios/editor_api.h"

namespace helios::edui::detail {
// HELIOS_EDITOR_API: editorui_tests reads the bytes from outside helios_editor in a modular build (02 §1.4).
extern HELIOS_EDITOR_API const unsigned char kRobotoRegular[];
extern HELIOS_EDITOR_API const usize kRobotoRegularSize;
} // namespace helios::edui::detail
