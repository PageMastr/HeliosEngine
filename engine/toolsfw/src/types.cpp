#include "helios/toolsfw/types.h"

#include <array>
#include <charconv>
#include <format>
#include <string>

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
    for (; i < text.size() && i < maxBytes; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        const auto next = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0;
        if (c < 0x20 || c == 0x7F) {
            out += std::format("\\x{:02x}", c);
        } else if (c == 0xC2 && next >= 0x80 && next <= 0x9F) {
            out += std::format("\\u{:04x}", next);
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    if (i < text.size()) out += "...";
    return out;
}

} // namespace helios::tf
