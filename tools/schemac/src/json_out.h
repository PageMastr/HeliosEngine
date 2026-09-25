#pragma once
// Minimal canonical JSON writer for schemac's own outputs (lock file, schema description). Same
// layout rules as helios::refl::JsonWriter: 2-space indent, LF, one member per line, inline
// containers on request.

#include <string>
#include <string_view>
#include <vector>

#include "helios/core/types.h"

namespace helios::schemac {

class JsonOut {
public:
    void beginObject(bool inlined = false);
    void endObject();
    void beginArray(bool inlined = false);
    void endArray();
    void key(std::string_view k);
    void str(std::string_view v);
    void num(i64 v);
    void unum(u64 v);
    void boolean(bool v);
    void null();
    /// Pre-rendered JSON value.
    void raw(std::string_view json);
    /// Complete document with a final newline.
    std::string take();

private:
    struct Frame {
        bool object;
        bool inlined;
        usize count;
    };
    void beforeValue();
    void newline();
    std::string m_out;
    std::vector<Frame> m_stack;
    bool m_afterKey = false;
};

} // namespace helios::schemac
