#include "text.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace helios::schemac {

namespace {
template <class F>
std::string formatShortest(F v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    if (v == 0) return std::signbit(v) ? "-0.0" : "0";
    char sci[48];
    const auto r = std::to_chars(sci, sci + sizeof(sci), v, std::chars_format::scientific);
    const std::string_view s(sci, static_cast<usize>(r.ptr - sci));
    usize i = 0;
    const bool negative = s[0] == '-';
    if (negative) ++i;
    std::string digits;
    for (; i < s.size() && s[i] != 'e'; ++i) {
        if (s[i] != '.') digits += s[i];
    }
    int exponent = 0;
    std::from_chars(s.data() + i + 1 + (s[i + 1] == '+' ? 1 : 0), s.data() + s.size(), exponent);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    const int n = static_cast<int>(digits.size());
    std::string out = negative ? "-" : "";
    if (exponent >= -6 && exponent <= 20) {
        if (exponent >= n - 1) {
            out += digits;
            out.append(static_cast<usize>(exponent - (n - 1)), '0');
        } else if (exponent >= 0) {
            out += digits.substr(0, static_cast<usize>(exponent) + 1);
            out += '.';
            out += digits.substr(static_cast<usize>(exponent) + 1);
        } else {
            out += "0.";
            out.append(static_cast<usize>(-exponent - 1), '0');
            out += digits;
        }
    } else {
        out += digits[0];
        if (n > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += exponent < 0 ? "e-" : "e+";
        out += std::to_string(exponent < 0 ? -exponent : exponent);
    }
    return out;
}

struct DurationUnit {
    std::string_view suffix;
    i64 nanos;
};
constexpr DurationUnit kUnits[] = {
    {"d", 86'400'000'000'000}, {"h", 3'600'000'000'000}, {"m", 60'000'000'000}, {"s", 1'000'000'000},
    {"ms", 1'000'000},         {"us", 1'000},             {"ns", 1},
};

bool isHexDigit(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
u32 hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') return static_cast<u32>(c - '0');
    return static_cast<u32>((c | 0x20) - 'a' + 10);
}
} // namespace

bool parseSchemaNumber(std::string_view text, f64& out) {
    std::string t(text);
    std::erase(t, '_');
    if (t.empty()) return false;
    const char* b = t.data();
    const char* e = t.data() + t.size();
    bool negative = false;
    if (*b == '-' || *b == '+') {
        negative = *b == '-';
        ++b;
    }
    if (e - b > 2 && b[0] == '0' && (b[1] == 'x' || b[1] == 'X')) {
        u64 v = 0;
        const auto r = std::from_chars(b + 2, e, v, 16);
        if (r.ec != std::errc() || r.ptr != e) return false;
        out = negative ? -static_cast<f64>(v) : static_cast<f64>(v);
        return true;
    }
    // Decimal digits only: from_chars would also accept "inf", "nan" and "infinity", which are not
    // schema numbers (and would be pasted into generated C++ verbatim).
    if (b == e || !((*b >= '0' && *b <= '9') || (*b == '.' && e - b > 1 && b[1] >= '0' && b[1] <= '9'))) return false;
    f64 v = 0;
    const auto r = std::from_chars(b, e, v);
    if (r.ec != std::errc() || r.ptr != e || !std::isfinite(v)) return false;
    out = negative ? -v : v;
    return true;
}

bool parseSchemaUnsigned(std::string_view text, u64& out) {
    std::string t(text);
    std::erase(t, '_');
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        const auto r = std::from_chars(t.data() + 2, t.data() + t.size(), out, 16);
        return r.ec == std::errc() && r.ptr == t.data() + t.size();
    }
    const auto r = std::from_chars(t.data(), t.data() + t.size(), out);
    return r.ec == std::errc() && r.ptr == t.data() + t.size();
}

std::string formatF64(f64 v) { return formatShortest(v); }
std::string formatF32(f32 v) { return formatShortest(v); }

std::string formatDuration(i64 nanos) {
    if (nanos == 0) return "0s";
    for (const DurationUnit& u : kUnits) {
        if (nanos % u.nanos == 0) return std::to_string(nanos / u.nanos) + std::string(u.suffix);
    }
    return std::to_string(nanos) + "ns";
}

std::optional<i64> parseDuration(std::string_view text) {
    bool negative = false;
    if (!text.empty() && text[0] == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    usize i = 0;
    while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) ++i;
    const std::string_view number = text.substr(0, i);
    const std::string_view suffix = text.substr(i);
    if (number.empty() || number == ".") return std::nullopt;
    const DurationUnit* unit = nullptr;
    for (const DurationUnit& u : kUnits) {
        if (u.suffix == suffix) unit = &u;
    }
    if (!unit) return std::nullopt;
    const usize dot = number.find('.');
    const std::string_view intPart = number.substr(0, dot);
    const std::string_view fracPart = dot == std::string_view::npos ? std::string_view() : number.substr(dot + 1);
    if (fracPart.find('.') != std::string_view::npos) return std::nullopt;
    u64 whole = 0;
    if (!intPart.empty()) {
        const auto r = std::from_chars(intPart.data(), intPart.data() + intPart.size(), whole);
        if (r.ec != std::errc() || r.ptr != intPart.data() + intPart.size()) return std::nullopt;
    }
    const u64 unitNanos = static_cast<u64>(unit->nanos);
    if (whole > static_cast<u64>(std::numeric_limits<i64>::max()) / unitNanos) return std::nullopt;
    u64 total = whole * unitNanos;
    u64 frac = 0;
    u64 scale = unitNanos;
    for (const char c : fracPart) {
        if (c < '0' || c > '9') return std::nullopt;
        const u64 digit = static_cast<u64>(c - '0');
        if (scale % 10 == 0) {
            scale /= 10;
            frac += digit * scale;
        } else {
            const u64 num = digit * scale;
            frac += num / 10 + (num % 10 >= 5 ? 1 : 0);
            break;
        }
    }
    total += frac;
    if (total > static_cast<u64>(std::numeric_limits<i64>::max())) return std::nullopt;
    return negative ? -static_cast<i64>(total) : static_cast<i64>(total);
}

std::string formatGuid(u64 high, u64 low) {
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", static_cast<u32>(high >> 32),
                       static_cast<u32>((high >> 16) & 0xFFFF), static_cast<u32>(high & 0xFFFF),
                       static_cast<u32>(low >> 48), low & 0xFFFFFFFFFFFFull);
}

bool parseGuid(std::string_view text, u64& high, u64& low) {
    if (text.starts_with("guid:")) text.remove_prefix(5);
    std::string hex;
    if (text.size() == 36) {
        for (usize i = 0; i < 36; ++i) {
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash != (text[i] == '-')) return false;
            if (!dash) hex += text[i];
        }
    } else if (text.size() == 32) {
        hex = std::string(text);
    } else {
        return false;
    }
    high = low = 0;
    for (usize i = 0; i < 32; ++i) {
        if (!isHexDigit(hex[i])) return false;
        if (i < 16) {
            high = (high << 4) | hexValue(hex[i]);
        } else {
            low = (low << 4) | hexValue(hex[i]);
        }
    }
    return true;
}

std::string jsonQuote(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (u < 0x20) {
                out += "\\u00";
                out += kHex[u >> 4];
                out += kHex[u & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::string cppQuote(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    bool lastWasHex = false;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        const bool hexDigit = isHexDigit(c);
        if (lastWasHex && hexDigit) out += "\"\""; // stop the previous \x escape from swallowing this digit
        lastWasHex = false;
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '?': out += "\\?"; break; // no trigraphs
        default:
            if (u < 0x20 || u >= 0x7f) {
                out += "\\x";
                out += kHex[u >> 4];
                out += kHex[u & 0xF];
                lastWasHex = true;
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::string goQuote(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (u < 0x20 || u >= 0x7f) {
                out += "\\x";
                out += kHex[u >> 4];
                out += kHex[u & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::string pascalCase(std::string_view s) {
    std::string out;
    bool upper = true;
    for (const char c : s) {
        if (c == '_') {
            upper = true;
            continue;
        }
        if (upper && c >= 'a' && c <= 'z') {
            out += static_cast<char>(c - 'a' + 'A');
        } else {
            out += c;
        }
        upper = false;
    }
    return out;
}

std::string camelCase(std::string_view s) {
    std::string out = pascalCase(s);
    if (!out.empty() && out[0] >= 'A' && out[0] <= 'Z') out[0] = static_cast<char>(out[0] - 'A' + 'a');
    return out;
}

bool isPascalCase(std::string_view s) noexcept {
    if (s.empty() || !(s[0] >= 'A' && s[0] <= 'Z')) return false;
    return s.find('_') == std::string_view::npos;
}

bool isCamelCase(std::string_view s) noexcept {
    if (s.empty() || !(s[0] >= 'a' && s[0] <= 'z')) return false;
    return s.find('_') == std::string_view::npos;
}

usize editDistance(std::string_view a, std::string_view b) {
    // Optimal string alignment distance: insertions, deletions, substitutions and adjacent
    // transpositions ("rnage" -> "range") each cost 1.
    std::vector<usize> prev2(b.size() + 1);
    std::vector<usize> prev(b.size() + 1);
    std::vector<usize> cur(b.size() + 1);
    for (usize j = 0; j <= b.size(); ++j) prev[j] = j;
    for (usize i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (usize j = 1; j <= b.size(); ++j) {
            const usize cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) cur[j] = std::min(cur[j], prev2[j - 2] + 1);
        }
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

std::string suggest(std::string_view word, const std::vector<std::string>& candidates) {
    std::string best;
    usize bestDist = std::max<usize>(2, word.size() / 3) + 1;
    for (const std::string& c : candidates) {
        const usize d = editDistance(word, c);
        if (d < bestDist) {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

std::vector<std::string> splitDots(std::string_view s) {
    std::vector<std::string> out;
    usize start = 0;
    while (true) {
        const usize dot = s.find('.', start);
        out.emplace_back(s.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (usize i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

u64 fnv1a64(std::string_view s, u64 seed) noexcept {
    u64 h = seed;
    for (const char c : s) {
        h ^= static_cast<u8>(c);
        h *= 0x100000001b3ull;
    }
    return h;
}

u32 fnv1a32(std::string_view s) noexcept {
    u32 h = 0x811c9dc5u;
    for (const char c : s) {
        h ^= static_cast<u8>(c);
        h *= 0x01000193u;
    }
    return h;
}

bool isCppKeyword(std::string_view s) noexcept {
    static constexpr std::string_view kWords[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
        "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
        // Reserved by generated code.
        "_dirty", "kReplicatedFields"};
    for (const std::string_view w : kWords) {
        if (w == s) return true;
    }
    return false;
}

} // namespace helios::schemac
