#include "json_out.h"

#include "text.h"

namespace helios::schemac {

void JsonOut::newline() {
    m_out += '\n';
    m_out.append(m_stack.size() * 2, ' ');
}

void JsonOut::beforeValue() {
    if (m_afterKey) {
        m_afterKey = false;
        return;
    }
    if (m_stack.empty()) return;
    Frame& f = m_stack.back();
    if (f.count > 0) m_out += ',';
    if (f.inlined) {
        if (f.count > 0) m_out += ' ';
    } else {
        newline();
    }
    ++f.count;
}

void JsonOut::key(std::string_view k) {
    Frame& f = m_stack.back();
    if (f.count > 0) m_out += ',';
    if (f.inlined) {
        if (f.count > 0) m_out += ' ';
    } else {
        newline();
    }
    ++f.count;
    m_out += jsonQuote(k);
    m_out += ": ";
    m_afterKey = true;
}

void JsonOut::beginObject(bool inlined) {
    beforeValue();
    m_out += '{';
    m_stack.push_back({true, inlined || (!m_stack.empty() && m_stack.back().inlined), 0});
}

void JsonOut::endObject() {
    const Frame f = m_stack.back();
    m_stack.pop_back();
    if (!f.inlined && f.count > 0) newline();
    m_out += '}';
}

void JsonOut::beginArray(bool inlined) {
    beforeValue();
    m_out += '[';
    m_stack.push_back({false, inlined || (!m_stack.empty() && m_stack.back().inlined), 0});
}

void JsonOut::endArray() {
    const Frame f = m_stack.back();
    m_stack.pop_back();
    if (!f.inlined && f.count > 0) newline();
    m_out += ']';
}

void JsonOut::str(std::string_view v) {
    beforeValue();
    m_out += jsonQuote(v);
}

void JsonOut::num(i64 v) {
    beforeValue();
    m_out += std::to_string(v);
}

void JsonOut::unum(u64 v) {
    beforeValue();
    m_out += std::to_string(v);
}

void JsonOut::boolean(bool v) {
    beforeValue();
    m_out += v ? "true" : "false";
}

void JsonOut::null() {
    beforeValue();
    m_out += "null";
}

void JsonOut::raw(std::string_view json) {
    beforeValue();
    m_out += json;
}

std::string JsonOut::take() {
    m_out += '\n';
    std::string out = std::move(m_out);
    m_out.clear();
    return out;
}

} // namespace helios::schemac
