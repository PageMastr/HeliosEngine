#pragma once
// ImGui form of the semantic theme tokens (theme.h semanticColor()), shared by the shell and the
// property grid. UI thread.

#include <string_view>

#include "helios/editorui/theme.h"
#include "imgui.h"

namespace helios::edui {

inline ImVec4 semanticColorVec4(std::string_view token) noexcept {
    const Color c = semanticColor(token);
    return ImVec4(c.r, c.g, c.b, c.a);
}

} // namespace helios::edui
