#include "lexer.h"

#include <format>

#include "helios/core/utf.h"

namespace helios::schemac {

namespace {
bool isIdentStart(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isIdentChar(char c) noexcept { return isIdentStart(c) || (c >= '0' && c <= '9'); }
bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
bool isHex(char c) noexcept { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

usize utf8Length(unsigned char c) noexcept {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}
} // namespace

std::vector<Token> tokenize(std::string_view text, u32 fileIndex, DiagnosticEngine& diags) {
    std::vector<Token> tokens;
    usize i = 0;
    u32 line = 1;
    usize lineStart = 0;
    bool newline = true;
    bool space = true;
    // Skip a UTF-8 BOM.
    if (text.size() >= 3 && static_cast<u8>(text[0]) == 0xEF && static_cast<u8>(text[1]) == 0xBB &&
        static_cast<u8>(text[2]) == 0xBF)
        i = lineStart = 3;

    auto loc = [&](usize at) { return SourceLoc{fileIndex, line, static_cast<u32>(at - lineStart + 1)}; };
    // Doc comments and strings are copied into C++ and Go sources, which must be valid UTF-8.
    if (!isValidUtf8(text)) {
        u32 badLine = 1;
        usize start = 0;
        while (start <= text.size()) {
            const usize nl = text.find('\n', start);
            const std::string_view lineText = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
            if (!isValidUtf8(lineText)) break;
            if (nl == std::string_view::npos) break;
            start = nl + 1;
            ++badLine;
        }
        diags.error(SourceLoc{fileIndex, badLine, 1}, "invalid UTF-8 (schema files must be UTF-8 encoded)");
    }
    auto push = [&](Tok kind, usize start, usize end, std::string_view slice, SourceLoc where) {
        tokens.push_back(Token{kind, slice, where, static_cast<u32>(start), static_cast<u32>(end), newline, space});
        newline = false;
        space = false;
    };

    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n') {
            ++line;
            lineStart = ++i;
            newline = true;
            space = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            space = true;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            const usize eol = text.find('\n', i);
            const usize stop = eol == std::string_view::npos ? text.size() : eol;
            const bool doc = i + 2 < text.size() && text[i + 2] == '/' && !(i + 3 < text.size() && text[i + 3] == '/');
            if (doc) {
                usize b = i + 3;
                if (b < stop && text[b] == ' ') ++b;
                usize e = stop;
                if (e > b && text[e - 1] == '\r') --e;
                push(Tok::Doc, i, stop, text.substr(b, e - b), loc(i));
            } else {
                space = true;
            }
            i = stop;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const SourceLoc start = loc(i);
            usize j = i + 2;
            bool closed = false;
            while (j < text.size()) {
                if (text[j] == '*' && j + 1 < text.size() && text[j + 1] == '/') {
                    j += 2;
                    closed = true;
                    break;
                }
                if (text[j] == '\n') {
                    ++line;
                    lineStart = j + 1;
                    newline = true;
                }
                ++j;
            }
            if (!closed) diags.error(start, "unterminated block comment");
            i = j;
            space = true;
            continue;
        }
        const usize start = i;
        const SourceLoc where = loc(i);
        if (isIdentStart(c)) {
            while (i < text.size() && isIdentChar(text[i])) ++i;
            push(Tok::Ident, start, i, text.substr(start, i - start), where);
            continue;
        }
        if (isDigit(c)) {
            bool isFloat = false;
            if (c == '0' && i + 1 < text.size() && (text[i + 1] == 'x' || text[i + 1] == 'X')) {
                i += 2;
                while (i < text.size() && (isHex(text[i]) || text[i] == '_')) ++i;
            } else {
                while (i < text.size() && (isDigit(text[i]) || text[i] == '_')) ++i;
                if (i + 1 < text.size() && text[i] == '.' && isDigit(text[i + 1])) {
                    isFloat = true;
                    ++i;
                    while (i < text.size() && isDigit(text[i])) ++i;
                }
                if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                    usize j = i + 1;
                    if (j < text.size() && (text[j] == '+' || text[j] == '-')) ++j;
                    if (j < text.size() && isDigit(text[j])) {
                        isFloat = true;
                        i = j;
                        while (i < text.size() && isDigit(text[i])) ++i;
                    }
                }
            }
            push(isFloat ? Tok::Float : Tok::Int, start, i, text.substr(start, i - start), where);
            continue;
        }
        if (c == '"') {
            ++i;
            bool closed = false;
            while (i < text.size()) {
                if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] != '\n') {
                    i += 2;
                    continue;
                }
                if (text[i] == '"') {
                    ++i;
                    closed = true;
                    break;
                }
                if (text[i] == '\n') break;
                ++i;
            }
            if (!closed) {
                diags.error(where, "unterminated string literal");
                continue;
            }
            push(Tok::String, start, i, text.substr(start, i - start), where);
            continue;
        }
        if (c == '-' && i + 1 < text.size() && text[i + 1] == '>') {
            i += 2;
            push(Tok::Punct, start, i, text.substr(start, 2), where);
            continue;
        }
        const usize n = utf8Length(static_cast<unsigned char>(c));
        i = std::min(text.size(), i + n);
        push(Tok::Punct, start, i, text.substr(start, i - start), where);
    }
    tokens.push_back(Token{Tok::End, {}, loc(i), static_cast<u32>(i), static_cast<u32>(i), true, true});
    return tokens;
}

bool decodeString(std::string_view quoted, std::string& out) {
    out.clear();
    if (quoted.size() < 2) return false;
    const std::string_view s = quoted.substr(1, quoted.size() - 2);
    for (usize i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i >= s.size()) return false;
        switch (s[i]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '0': out += '\0'; break;
        case 'u': {
            u32 cp = 0;
            for (int k = 1; k <= 4; ++k) {
                if (i + static_cast<usize>(k) >= s.size()) return false;
                const char h = s[i + static_cast<usize>(k)];
                if (!isHex(h)) return false;
                cp = cp * 16 + static_cast<u32>(isDigit(h) ? h - '0' : (h | 0x20) - 'a' + 10);
            }
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDFFF) return false; // surrogates are not scalar values
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            break;
        }
        default: return false;
        }
    }
    return true;
}

} // namespace helios::schemac
