#pragma once
// Indentation-aware text builder for generated sources.

#include <string>
#include <string_view>

#include "helios/core/types.h"

namespace helios::schemac {

class CodeWriter {
public:
    explicit CodeWriter(usize indentWidth = 4, char indentChar = ' ') : m_width(indentWidth), m_char(indentChar) {}

    /// Appends one line at the current indentation (empty text -> blank line).
    void line(std::string_view text = {}) {
        if (!text.empty()) m_out.append(m_depth * m_width, m_char);
        m_out += text;
        m_out += '\n';
    }
    /// Appends verbatim text (no indentation handling).
    void raw(std::string_view text) { m_out += text; }
    void indent() { ++m_depth; }
    void dedent() {
        if (m_depth > 0) --m_depth;
    }
    /// "text {" + indent
    void open(std::string_view text) {
        line(text);
        indent();
    }
    /// dedent + "text"
    void close(std::string_view text = "}") {
        dedent();
        line(text);
    }
    const std::string& str() const noexcept { return m_out; }
    std::string take() { return std::move(m_out); }

private:
    std::string m_out;
    usize m_depth = 0;
    usize m_width;
    char m_char;
};

} // namespace helios::schemac
