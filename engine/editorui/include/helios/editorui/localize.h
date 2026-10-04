#pragma once
// UI text for the editor shell. Every label goes through tr(), which is the identity today
// (editor localization comes with T25) and applies pseudo-localization in test mode: accented
// characters, +40 % length and brackets, so the layout lint of 07 §4.4 catches text that does not
// fit. ImGui id suffixes ("##id", "###id") are kept unchanged, so ids stay stable.
//
// Threading: owner (UI) thread only.

#include <string>
#include <string_view>

namespace helios::edui {

/// Enables or disables pseudo-localization for subsequent tr() calls.
void setPseudoLocalization(bool enabled) noexcept;
bool pseudoLocalization() noexcept;

/// The display text of `text` (see the header comment). The returned pointer stays valid until
/// the next tr() call with pseudo-localization on (a small ring of buffers), or is `text`'s own
/// storage otherwise; copy it if it must live longer.
const char* tr(const char* text);
/// Pseudo-localized copy of `text` (always, regardless of the switch).
std::string pseudoLocalize(std::string_view text);

} // namespace helios::edui
