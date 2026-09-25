// JSONC reading (yyjson, raw numbers) and the canonical writer.

#include "helios/reflect/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <system_error>
#include <vector>

#include <yyjson.h>

namespace helios::refl {

namespace {
yyjson_val* mval(const void* p) noexcept { return const_cast<yyjson_val*>(static_cast<const yyjson_val*>(p)); }

/// Container nesting depth of a parsed document, computed iteratively (yyjson parses without
/// recursion, so a hostile document can nest arbitrarily deep; everything that walks a JsonValue
/// recursively relies on this bound). Stops counting once past kMaxJsonDepth.
usize nestingDepth(yyjson_doc* doc) {
    struct Frame {
        yyjson_val* next; // next child to visit (object: next key)
        usize left;       // children (object: members) left
        bool object;
    };
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_ctn(root)) return 0;
    std::vector<Frame> stack;
    auto push = [&](yyjson_val* c) {
        const usize n = unsafe_yyjson_get_len(c);
        if (n != 0) stack.push_back({unsafe_yyjson_get_first(c), n, yyjson_is_obj(c)});
    };
    usize deepest = 1;
    push(root);
    while (!stack.empty()) {
        Frame& f = stack.back();
        if (f.left == 0) {
            stack.pop_back();
            continue;
        }
        yyjson_val* child = f.object ? f.next + 1 : f.next; // object members: key, then value
        f.next = unsafe_yyjson_get_next(child);
        --f.left;
        if (yyjson_is_ctn(child)) {
            deepest = std::max(deepest, stack.size() + 1);
            if (deepest > kMaxJsonDepth) return deepest;
            push(child);
        }
    }
    return deepest;
}

template <class T>
bool parseExact(std::string_view text, T& out) noexcept {
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last = first + text.size();
    // std::from_chars rejects a leading '+', which JSON never produces anyway.
    const auto r = std::from_chars(first, last, out);
    return r.ec == std::errc() && r.ptr == last;
}

template <class F>
bool parseFloatText(std::string_view text, F& out) noexcept {
    if (text == "nan" || text == "NaN") {
        out = std::numeric_limits<F>::quiet_NaN();
        return true;
    }
    if (text == "inf" || text == "Infinity") {
        out = std::numeric_limits<F>::infinity();
        return true;
    }
    if (text == "-inf" || text == "-Infinity") {
        out = -std::numeric_limits<F>::infinity();
        return true;
    }
    if (text.empty()) return false;
    // JSON grammar is a subset of from_chars' general format except for leading '-', which it
    // handles; out-of-range values report result_out_of_range and are rejected.
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out, std::chars_format::general);
    return r.ec == std::errc() && r.ptr == text.data() + text.size();
}

template <class F>
usize formatShortest(F v, char* out) noexcept {
    if (std::isnan(v)) {
        std::memcpy(out, "nan", 3);
        return 3;
    }
    if (std::isinf(v)) {
        if (v < 0) {
            std::memcpy(out, "-inf", 4);
            return 4;
        }
        std::memcpy(out, "inf", 3);
        return 3;
    }
    if (v == 0) {
        if (std::signbit(v)) {
            std::memcpy(out, "-0.0", 4);
            return 4;
        }
        out[0] = '0';
        return 1;
    }
    char sci[48];
    const auto r = std::to_chars(sci, sci + sizeof(sci), v, std::chars_format::scientific);
    const std::string_view s(sci, static_cast<usize>(r.ptr - sci));
    // s = "[-]d[.ddd]e[+-]xx"
    usize i = 0;
    const bool negative = s[0] == '-';
    if (negative) ++i;
    char digits[32];
    usize n = 0;
    for (; i < s.size() && s[i] != 'e'; ++i) {
        if (s[i] != '.') digits[n++] = s[i];
    }
    int exponent = 0;
    std::from_chars(s.data() + i + 1 + (s[i + 1] == '+' ? 1 : 0), s.data() + s.size(), exponent);
    // Trim trailing zeros of the significand (to_chars shortest never emits them, but be safe).
    while (n > 1 && digits[n - 1] == '0') --n;

    char* p = out;
    if (negative) *p++ = '-';
    const int ni = static_cast<int>(n);
    if (exponent >= -6 && exponent <= 20) {
        if (exponent >= ni - 1) {
            std::memcpy(p, digits, n);
            p += n;
            for (int z = 0; z < exponent - (ni - 1); ++z) *p++ = '0';
        } else if (exponent >= 0) {
            const usize intDigits = static_cast<usize>(exponent) + 1;
            std::memcpy(p, digits, intDigits);
            p += intDigits;
            *p++ = '.';
            std::memcpy(p, digits + intDigits, n - intDigits);
            p += n - intDigits;
        } else {
            *p++ = '0';
            *p++ = '.';
            for (int z = 0; z < -exponent - 1; ++z) *p++ = '0';
            std::memcpy(p, digits, n);
            p += n;
        }
    } else {
        *p++ = digits[0];
        if (n > 1) {
            *p++ = '.';
            std::memcpy(p, digits + 1, n - 1);
            p += n - 1;
        }
        *p++ = 'e';
        *p++ = exponent < 0 ? '-' : '+';
        const auto er = std::to_chars(p, p + 8, exponent < 0 ? -exponent : exponent);
        p = er.ptr;
    }
    return static_cast<usize>(p - out);
}
} // namespace

std::string_view jsonTypeName(JsonType type) noexcept {
    switch (type) {
    case JsonType::Invalid: return "missing";
    case JsonType::Null: return "null";
    case JsonType::Bool: return "bool";
    case JsonType::Number: return "number";
    case JsonType::String: return "string";
    case JsonType::Array: return "array";
    case JsonType::Object: return "object";
    }
    return "?";
}

namespace detail {
const void* jsonFirstChild(const void* v) noexcept { return unsafe_yyjson_get_first(mval(v)); }
const void* jsonNextSibling(const void* v) noexcept { return unsafe_yyjson_get_next(mval(v)); }
} // namespace detail

JsonType JsonValue::type() const noexcept {
    if (!m_val) return JsonType::Invalid;
    switch (yyjson_get_type(mval(m_val))) {
    case YYJSON_TYPE_NULL: return JsonType::Null;
    case YYJSON_TYPE_BOOL: return JsonType::Bool;
    case YYJSON_TYPE_RAW:
    case YYJSON_TYPE_NUM: return JsonType::Number;
    case YYJSON_TYPE_STR: return JsonType::String;
    case YYJSON_TYPE_ARR: return JsonType::Array;
    case YYJSON_TYPE_OBJ: return JsonType::Object;
    default: return JsonType::Invalid;
    }
}

bool JsonValue::asBool() const noexcept { return m_val && yyjson_is_true(mval(m_val)); }

std::string_view JsonValue::asString() const noexcept {
    if (!m_val) return {};
    yyjson_val* v = mval(m_val);
    if (yyjson_is_str(v)) return {yyjson_get_str(v), yyjson_get_len(v)};
    if (yyjson_is_raw(v)) return {yyjson_get_raw(v), yyjson_get_len(v)};
    return {};
}

bool JsonValue::getU64(u64& out) const noexcept { return isNumber() && parseExact(asString(), out); }
bool JsonValue::getI64(i64& out) const noexcept { return isNumber() && parseExact(asString(), out); }

bool JsonValue::getF64(f64& out) const noexcept {
    if (!isNumber() && !isString()) return false;
    const std::string_view t = asString();
    if (isString() && t != "nan" && t != "inf" && t != "-inf") return false;
    return parseFloatText(t, out);
}

bool JsonValue::getF32(f32& out) const noexcept {
    if (!isNumber() && !isString()) return false;
    const std::string_view t = asString();
    if (isString() && t != "nan" && t != "inf" && t != "-inf") return false;
    return parseFloatText(t, out);
}

usize JsonValue::size() const noexcept {
    if (!m_val) return 0;
    yyjson_val* v = mval(m_val);
    if (yyjson_is_arr(v) || yyjson_is_obj(v)) return unsafe_yyjson_get_len(v);
    return 0;
}

JsonValue JsonValue::get(std::string_view key) const noexcept {
    if (!isObject()) return {};
    return JsonValue(yyjson_obj_getn(mval(m_val), key.data(), key.size()));
}

JsonValue::Member JsonValue::ObjectIterator::operator*() const noexcept {
    yyjson_val* k = mval(m_key);
    return Member{{yyjson_get_str(k), yyjson_get_len(k)}, JsonValue(k + 1)};
}

JsonValue::ObjectIterator& JsonValue::ObjectIterator::operator++() noexcept {
    if (--m_left != 0) m_key = unsafe_yyjson_get_next(mval(m_key) + 1);
    return *this;
}

JsonValue::Range<JsonValue::ArrayIterator> JsonValue::elements() const noexcept {
    if (!isArray() || size() == 0) return {ArrayIterator(nullptr, 0), ArrayIterator(nullptr, 0)};
    return {ArrayIterator(detail::jsonFirstChild(m_val), size()), ArrayIterator(nullptr, 0)};
}

JsonValue::Range<JsonValue::ObjectIterator> JsonValue::members() const noexcept {
    if (!isObject() || size() == 0) return {ObjectIterator(nullptr, 0), ObjectIterator(nullptr, 0)};
    return {ObjectIterator(detail::jsonFirstChild(m_val), size()), ObjectIterator(nullptr, 0)};
}

JsonDocument::~JsonDocument() {
    if (m_doc) yyjson_doc_free(static_cast<yyjson_doc*>(m_doc));
}

JsonDocument& JsonDocument::operator=(JsonDocument&& o) noexcept {
    if (this != &o) {
        if (m_doc) yyjson_doc_free(static_cast<yyjson_doc*>(m_doc));
        m_doc = std::exchange(o.m_doc, nullptr);
    }
    return *this;
}

Result<JsonDocument> JsonDocument::parse(std::string_view text, std::string_view sourceName) {
    const yyjson_read_flag flags = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS |
                                   YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_ALLOW_BOM | YYJSON_READ_NUMBER_AS_RAW;
    yyjson_read_err err{};
    // yyjson only writes to the buffer with YYJSON_READ_INSITU, which is not used.
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(text.data()), text.size(), flags, nullptr, &err);
    if (!doc) {
        usize line = 1;
        usize col = 1;
        for (usize i = 0; i < err.pos && i < text.size(); ++i) {
            if (text[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        return makeError(ErrorCode::ParseError, "{}:{}:{}: {}", sourceName, line, col, err.msg ? err.msg : "invalid JSON");
    }
    JsonDocument d;
    d.m_doc = doc;
    if (const usize depth = nestingDepth(doc); depth > kMaxJsonDepth) {
        return makeError(ErrorCode::LimitExceeded, "{}: JSON nests {}+ levels deep (limit {})", sourceName, depth, kMaxJsonDepth);
    }
    return d;
}

JsonValue JsonDocument::root() const noexcept {
    return m_doc ? JsonValue(yyjson_doc_get_root(static_cast<yyjson_doc*>(m_doc))) : JsonValue();
}

usize formatJsonF64(f64 v, char* out) noexcept { return formatShortest(v, out); }
usize formatJsonF32(f32 v, char* out) noexcept { return formatShortest(v, out); }

// ---------------------------------------------------------------------------------------------
// JsonWriter
// ---------------------------------------------------------------------------------------------

void JsonWriter::newline(usize depth) {
    m_out += '\n';
    m_out.append(depth * 2, ' ');
}

void JsonWriter::beforeValue() {
    if (m_afterKey) {
        m_afterKey = false;
        return;
    }
    if (m_stack.empty()) return;
    Frame& f = m_stack.back();
    HELIOS_ASSERT(!f.object, "JsonWriter: object member without key()");
    if (f.count > 0) m_out += ',';
    if (m_style == JsonStyle::Pretty) {
        if (f.inlined) {
            if (f.count > 0) m_out += ' ';
        } else {
            newline(m_stack.size());
        }
    }
    ++f.count;
}

void JsonWriter::key(std::string_view k) {
    HELIOS_ASSERT(!m_stack.empty() && m_stack.back().object && !m_afterKey, "JsonWriter: key() outside an object");
    Frame& f = m_stack.back();
    if (f.count > 0) m_out += ',';
    if (m_style == JsonStyle::Pretty) {
        if (f.inlined) {
            if (f.count > 0) m_out += ' ';
        } else {
            newline(m_stack.size());
        }
    }
    ++f.count;
    writeEscaped(k);
    m_out += m_style == JsonStyle::Pretty ? ": " : ":";
    m_afterKey = true;
}

void JsonWriter::beginObject() {
    beforeValue();
    m_out += '{';
    const bool inl = !m_stack.empty() && m_stack.back().inlined;
    m_stack.push_back({true, inl, 0});
}

void JsonWriter::endObject() {
    HELIOS_ASSERT(!m_stack.empty() && m_stack.back().object);
    const Frame f = m_stack.back();
    m_stack.pop_back();
    if (m_style == JsonStyle::Pretty && !f.inlined && f.count > 0) newline(m_stack.size());
    m_out += '}';
}

void JsonWriter::beginArray(bool inlineElements) {
    beforeValue();
    m_out += '[';
    const bool inl = inlineElements || (!m_stack.empty() && m_stack.back().inlined);
    m_stack.push_back({false, inl, 0});
}

void JsonWriter::endArray() {
    HELIOS_ASSERT(!m_stack.empty() && !m_stack.back().object);
    const Frame f = m_stack.back();
    m_stack.pop_back();
    if (m_style == JsonStyle::Pretty && !f.inlined && f.count > 0) newline(m_stack.size());
    m_out += ']';
}

void JsonWriter::null() {
    beforeValue();
    m_out += "null";
}

void JsonWriter::boolean(bool v) {
    beforeValue();
    m_out += v ? "true" : "false";
}

void JsonWriter::integer(i64 v) {
    beforeValue();
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    m_out.append(buf, r.ptr);
}

void JsonWriter::unsignedInteger(u64 v) {
    beforeValue();
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    m_out.append(buf, r.ptr);
}

void JsonWriter::number(f64 v) {
    char buf[48];
    const usize n = formatJsonF64(v, buf);
    if (!std::isfinite(v)) {
        string(std::string_view(buf, n));
        return;
    }
    beforeValue();
    m_out.append(buf, n);
}

void JsonWriter::numberF32(f32 v) {
    char buf[48];
    const usize n = formatJsonF32(v, buf);
    if (!std::isfinite(v)) {
        string(std::string_view(buf, n));
        return;
    }
    beforeValue();
    m_out.append(buf, n);
}

void JsonWriter::string(std::string_view v) {
    beforeValue();
    writeEscaped(v);
}

void JsonWriter::raw(std::string_view json) {
    beforeValue();
    m_out += json;
}

void JsonWriter::copy(JsonValue v) {
    switch (v.type()) {
    case JsonType::Invalid:
    case JsonType::Null: null(); return;
    case JsonType::Bool: boolean(v.asBool()); return;
    case JsonType::Number:
        beforeValue();
        m_out += v.asString();
        return;
    case JsonType::String: string(v.asString()); return;
    case JsonType::Array: {
        bool scalars = true;
        for (JsonValue e : v.elements()) scalars = scalars && !e.isArray() && !e.isObject();
        beginArray(scalars);
        for (JsonValue e : v.elements()) copy(e);
        endArray();
        return;
    }
    case JsonType::Object:
        beginObject();
        for (const JsonValue::Member m : v.members()) {
            key(m.key);
            copy(m.value);
        }
        endObject();
        return;
    }
}

void JsonWriter::writeEscaped(std::string_view s) {
    static constexpr char kHex[] = "0123456789abcdef";
    m_out += '"';
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"': m_out += "\\\""; break;
        case '\\': m_out += "\\\\"; break;
        case '\n': m_out += "\\n"; break;
        case '\r': m_out += "\\r"; break;
        case '\t': m_out += "\\t"; break;
        case '\b': m_out += "\\b"; break;
        case '\f': m_out += "\\f"; break;
        default:
            if (u < 0x20) {
                m_out += "\\u00";
                m_out += kHex[u >> 4];
                m_out += kHex[u & 0xF];
            } else {
                m_out += c;
            }
        }
    }
    m_out += '"';
}

std::string JsonWriter::take() {
    if (m_style == JsonStyle::Pretty && m_stack.empty() && !m_out.empty()) m_out += '\n';
    std::string out = std::move(m_out);
    m_out.clear();
    m_stack.clear();
    m_afterKey = false;
    return out;
}

// ---------------------------------------------------------------------------------------------
// ReadCtx
// ---------------------------------------------------------------------------------------------

std::string ReadCtx::path() const {
    std::string out;
    for (const Segment& s : m_path) {
        if (s.isIndex) {
            out += '[';
            out += std::to_string(s.index);
            out += ']';
        } else {
            if (!out.empty()) out += '.';
            out += s.key;
        }
    }
    return out;
}

Error ReadCtx::error(std::string_view message) const {
    const std::string p = path();
    if (p.empty()) return Error{ErrorCode::ParseError, std::string(message)};
    return Error{ErrorCode::ParseError, p + ": " + std::string(message)};
}

Error ReadCtx::typeError(std::string_view expected, JsonValue got) const {
    return error(std::format("expected {}, got {}", expected, got.typeName()));
}

void ReadCtx::warn(std::string_view message) {
    const std::string p = path();
    m_warnings.push_back(p.empty() ? std::string(message) : p + ": " + std::string(message));
}

Result<void> ReadCtx::unknownField(std::string_view key) {
    if (m_options.strictUnknownFields) return error("unknown field");
    // The scope already names the key; keep the message short.
    (void)key;
    warn("unknown field ignored");
    return {};
}

} // namespace helios::refl
