#include "helios/core/utf.h"

namespace helios {

namespace {

/// Decodes one code point; `ok` is false for malformed input (U+FFFD returned, maximal invalid
/// subpart consumed as per the Unicode "best practice for U+FFFD substitution").
char32_t decode(std::string_view s, usize& pos, bool& ok) noexcept {
    ok = true;
    const u8 b0 = static_cast<u8>(s[pos]);
    if (b0 < 0x80) {
        ++pos;
        return b0;
    }
    usize need;
    char32_t cp;
    u8 lo = 0x80;
    u8 hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 1;
        cp = b0 & 0x1Fu;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2;
        cp = b0 & 0x0Fu;
        if (b0 == 0xE0) lo = 0xA0; // overlong
        if (b0 == 0xED) hi = 0x9F; // surrogates
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3;
        cp = b0 & 0x07u;
        if (b0 == 0xF0) lo = 0x90; // overlong
        if (b0 == 0xF4) hi = 0x8F; // > U+10FFFF
    } else {
        ++pos;
        ok = false;
        return kReplacementChar;
    }
    ++pos;
    for (usize k = 0; k < need; ++k) {
        if (pos >= s.size()) {
            ok = false;
            return kReplacementChar;
        }
        const u8 b = static_cast<u8>(s[pos]);
        if (b < lo || b > hi) { // do not consume: it may start the next sequence
            ok = false;
            return kReplacementChar;
        }
        lo = 0x80;
        hi = 0xBF;
        cp = (cp << 6) | (b & 0x3Fu);
        ++pos;
    }
    return cp;
}

bool isSurrogate(char32_t cp) noexcept { return cp >= 0xD800 && cp <= 0xDFFF; }

template <class Out>
void appendUtf16(Out& out, char32_t cp) {
    if (cp > 0x10FFFF || isSurrogate(cp)) cp = kReplacementChar;
    if (cp < 0x10000) {
        out.push_back(static_cast<typename Out::value_type>(cp));
    } else {
        cp -= 0x10000;
        out.push_back(static_cast<typename Out::value_type>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<typename Out::value_type>(0xDC00 + (cp & 0x3FF)));
    }
}

template <class CharT>
std::string fromUtf16(std::basic_string_view<CharT> text) {
    std::string out;
    out.reserve(text.size());
    for (usize i = 0; i < text.size(); ++i) {
        char32_t cp = static_cast<char16_t>(text[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 < text.size()) {
                const char32_t next = static_cast<char16_t>(text[i + 1]);
                if (next >= 0xDC00 && next <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (next - 0xDC00);
                    ++i;
                } else {
                    cp = kReplacementChar;
                }
            } else {
                cp = kReplacementChar;
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = kReplacementChar;
        }
        appendUtf8(out, cp);
    }
    return out;
}

template <class CharT>
std::string fromUtf32(std::basic_string_view<CharT> text) {
    std::string out;
    out.reserve(text.size());
    for (const CharT c : text) appendUtf8(out, static_cast<char32_t>(c));
    return out;
}

} // namespace

bool isValidUtf8(std::string_view text) noexcept {
    usize pos = 0;
    bool ok = true;
    while (pos < text.size()) {
        decode(text, pos, ok);
        if (!ok) return false;
    }
    return true;
}

usize utf8Length(std::string_view text) noexcept {
    usize pos = 0;
    usize count = 0;
    bool ok;
    while (pos < text.size()) {
        decode(text, pos, ok);
        ++count;
    }
    return count;
}

char32_t decodeUtf8(std::string_view text, usize& pos) noexcept {
    if (pos >= text.size()) return kReplacementChar;
    bool ok;
    return decode(text, pos, ok);
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF || isSurrogate(cp)) cp = kReplacementChar;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::u16string utf8ToUtf16(std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    usize pos = 0;
    bool ok;
    while (pos < text.size()) appendUtf16(out, decode(text, pos, ok));
    return out;
}

std::string utf16ToUtf8(std::u16string_view text) { return fromUtf16(text); }

std::u32string utf8ToUtf32(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    usize pos = 0;
    bool ok;
    while (pos < text.size()) out.push_back(decode(text, pos, ok));
    return out;
}

std::string utf32ToUtf8(std::u32string_view text) { return fromUtf32(text); }

std::wstring utf8ToWide(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    usize pos = 0;
    bool ok;
    while (pos < text.size()) {
        const char32_t cp = decode(text, pos, ok);
        if constexpr (sizeof(wchar_t) == 2) {
            appendUtf16(out, cp);
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

std::string wideToUtf8(std::wstring_view text) {
    if constexpr (sizeof(wchar_t) == 2) {
        return fromUtf16(text);
    } else {
        return fromUtf32(text);
    }
}

std::string sanitizeUtf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    usize pos = 0;
    bool ok;
    while (pos < text.size()) appendUtf8(out, decode(text, pos, ok));
    return out;
}

} // namespace helios
