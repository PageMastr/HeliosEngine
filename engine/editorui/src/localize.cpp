#include "helios/editorui/localize.h"

#include <algorithm>
#include <array>
#include <string>

namespace helios::edui {

namespace {
bool g_pseudo = false;
std::array<std::string, 64> g_ring;
unsigned g_next = 0;

/// Accented stand-ins that the default font covers (Latin-1 supplement).
std::string_view accent(char c) {
    switch (c) {
    case 'a': return "\xC3\xA5";  // å
    case 'e': return "\xC3\xA9";  // é
    case 'i': return "\xC3\xAF";  // ï
    case 'o': return "\xC3\xB6";  // ö
    case 'u': return "\xC3\xBC";  // ü
    case 'A': return "\xC3\x85";  // Å
    case 'E': return "\xC3\x89";  // É
    case 'O': return "\xC3\x96";  // Ö
    case 'n': return "\xC3\xB1";  // ñ
    case 'c': return "\xC3\xA7";  // ç
    default: return {};
    }
}
} // namespace

void setPseudoLocalization(bool enabled) noexcept {
    g_pseudo = enabled;
}

bool pseudoLocalization() noexcept {
    return g_pseudo;
}

std::string pseudoLocalize(std::string_view text) {
    // Keep ImGui id suffixes untouched.
    const std::size_t hash = text.find("##");
    const std::string_view visible = text.substr(0, hash);
    const std::string_view suffix = hash == std::string_view::npos ? std::string_view() : text.substr(hash);
    if (visible.empty()) return std::string(text);
    std::string out = "[";
    std::size_t glyphs = 0;
    for (char c : visible) {
        const std::string_view a = accent(c);
        if (a.empty()) {
            out.push_back(c);
        } else {
            out.append(a);
        }
        ++glyphs;
    }
    // +40 % length (at least two extra glyphs), then the closing bracket.
    const std::size_t pad = std::max<std::size_t>(2, (glyphs * 4 + 9) / 10);
    out.append(pad, '~');
    out.push_back(']');
    out.append(suffix);
    return out;
}

const char* tr(const char* text) {
    if (!g_pseudo || text == nullptr) return text;
    std::string& slot = g_ring[g_next++ % g_ring.size()];
    slot = pseudoLocalize(text);
    return slot.c_str();
}

} // namespace helios::edui
