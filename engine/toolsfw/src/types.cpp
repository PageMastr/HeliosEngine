#include "helios/toolsfw/types.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <string>

#include "helios/core/utf.h"

namespace helios::tf {

namespace {
constexpr std::array<std::string_view, kOriginCount> kOriginNames = {"ui", "ui-scripted", "luau", "rpc",
                                                                     "cli", "import", "collab"};
} // namespace

std::string_view originName(Origin origin) noexcept {
    const auto i = static_cast<usize>(origin);
    return i < kOriginNames.size() ? kOriginNames[i] : std::string_view("unknown");
}

std::optional<Origin> parseOrigin(std::string_view text) noexcept {
    for (usize i = 0; i < kOriginNames.size(); ++i) {
        if (kOriginNames[i] == text) return static_cast<Origin>(i);
    }
    return std::nullopt;
}

std::string TxId::toString() const {
    return user + ":" + std::to_string(lamport);
}

std::string hashHex(u64 hash) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[static_cast<usize>(i)] = kDigits[hash & 0xF];
        hash >>= 4;
    }
    return s;
}

std::optional<u64> parseHashHex(std::string_view text) noexcept {
    if (text.empty() || text.size() > 16) return std::nullopt;
    u64 v = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v, 16);
    if (ec != std::errc() || ptr != text.data() + text.size()) return std::nullopt;
    return v;
}

std::string printable(std::string_view text, usize maxBytes) {
    std::string out;
    out.reserve(std::min(text.size(), maxBytes));
    usize i = 0;
    while (i < text.size() && i < maxBytes) {
        const usize start = i;
        const char32_t cp = decodeUtf8(text, i);
        const std::string_view bytes = text.substr(start, i - start);
        if (cp == kReplacementChar && bytes != "\xEF\xBF\xBD") {
            // Not UTF-8: every byte escaped, so a lone 0x9B (the 8-bit CSI) never reaches a terminal.
            for (const char b : bytes) out += std::format("\\x{:02x}", static_cast<unsigned char>(b));
        } else if (cp < 0x20 || cp == 0x7F) {
            out += std::format("\\x{:02x}", static_cast<u32>(cp));
        } else if (cp >= 0x80 && cp <= 0x9F) {
            out += std::format("\\u{:04x}", static_cast<u32>(cp));  // C1 (U+009B is CSI)
        } else {
            out += bytes;
        }
    }
    if (i < text.size()) out += "...";
    return out;
}

} // namespace helios::tf
