#pragma once
// The editor font (07 §1.3): Roboto 2.138 Regular (Apache-2.0, third_party/roboto), embedded by
// CMakeLists.txt at build time so every machine renders the same glyphs (the ED-15 goldens pin them).

#include "helios/core/types.h"

namespace helios::edui::detail {
extern const unsigned char kRobotoRegular[];
extern const usize kRobotoRegularSize;
} // namespace helios::edui::detail
