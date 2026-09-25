#pragma once
// JSONC text I/O for schema data (02 §3.7 "Text").
//
// * JsonDocument / JsonValue: read-only DOM parsed with yyjson (comments, trailing commas and a
//   BOM are accepted). Numbers are kept as raw text and converted with std::from_chars, so f32
//   fields round-trip exactly (no decimal -> f64 -> f32 double rounding) on every compiler.
// * JsonWriter: canonical writer. Pretty style = one property per line, 2-space indent, LF, UTF-8,
//   final newline; arrays of scalars stay on one line ("[0, 0, -1]"). Floats use the shortest
//   round-trip digits in JavaScript notation (fixed for 1e-6 <= |x| < 1e21, else "1.5e+21"),
//   -0.0 is written as "-0.0" and non-finite values as the strings "nan", "inf", "-inf".
//   Compact style emits no whitespace (property-path patch values).
// * ReadCtx: error/warning context with the JSON path of the value being read.
//
// Threading: documents are immutable after parse (concurrent reads are fine); writers and
// ReadCtx are single-threaded.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::refl {

enum class JsonType : u8 { Invalid, Null, Bool, Number, String, Array, Object };

/// Deepest array/object nesting JsonDocument::parse() accepts (deeper documents fail with
/// ErrorCode::LimitExceeded). Readers, JsonWriter::copy() and patches recurse per level, so this
/// keeps hostile input from overflowing the stack (the tagged format's limit is kMaxTaggedDepth).
inline constexpr usize kMaxJsonDepth = 128;

std::string_view jsonTypeName(JsonType type) noexcept;

class JsonValue;
struct JsonMember;

namespace detail {
const void* jsonFirstChild(const void* val) noexcept;
const void* jsonNextSibling(const void* val) noexcept;
} // namespace detail

/// Non-owning view of a value inside a JsonDocument (valid while the document lives).
class JsonValue {
public:
    JsonValue() noexcept = default;
    explicit JsonValue(const void* raw) noexcept : m_val(raw) {}

    bool isValid() const noexcept { return m_val != nullptr; }
    JsonType type() const noexcept;
    std::string_view typeName() const noexcept { return jsonTypeName(type()); }
    bool isNull() const noexcept { return type() == JsonType::Null; }
    bool isBool() const noexcept { return type() == JsonType::Bool; }
    bool isNumber() const noexcept { return type() == JsonType::Number; }
    bool isString() const noexcept { return type() == JsonType::String; }
    bool isArray() const noexcept { return type() == JsonType::Array; }
    bool isObject() const noexcept { return type() == JsonType::Object; }

    bool asBool() const noexcept;
    /// Raw number text ("1.5e3") or string contents; empty for other types.
    std::string_view asString() const noexcept;
    std::string_view numberText() const noexcept { return isNumber() ? asString() : std::string_view(); }
    /// Exact conversions of a number; false if not a number, not integral or out of range.
    bool getU64(u64& out) const noexcept;
    bool getI64(i64& out) const noexcept;
    /// Any number (correctly rounded); also accepts the strings "nan", "inf", "-inf".
    bool getF64(f64& out) const noexcept;
    bool getF32(f32& out) const noexcept;

    /// Element / member count (0 for scalars).
    usize size() const noexcept;
    /// Object member lookup (linear); invalid JsonValue if absent.
    JsonValue get(std::string_view key) const noexcept;

    class ArrayIterator {
    public:
        ArrayIterator(const void* cur, usize left) noexcept : m_cur(cur), m_left(left) {}
        JsonValue operator*() const noexcept { return JsonValue(m_cur); }
        ArrayIterator& operator++() noexcept {
            if (--m_left != 0) m_cur = detail::jsonNextSibling(m_cur);
            return *this;
        }
        bool operator==(const ArrayIterator& o) const noexcept { return m_left == o.m_left; }

    private:
        const void* m_cur;
        usize m_left;
    };
    using Member = JsonMember;
    class ObjectIterator {
    public:
        ObjectIterator(const void* key, usize left) noexcept : m_key(key), m_left(left) {}
        Member operator*() const noexcept;
        ObjectIterator& operator++() noexcept;
        bool operator==(const ObjectIterator& o) const noexcept { return m_left == o.m_left; }

    private:
        const void* m_key;
        usize m_left;
    };
    template <class It>
    struct Range {
        It b, e;
        It begin() const noexcept { return b; }
        It end() const noexcept { return e; }
    };
    /// Array elements (empty range for non-arrays).
    Range<ArrayIterator> elements() const noexcept;
    /// Object members in document order (empty range for non-objects).
    Range<ObjectIterator> members() const noexcept;

    const void* raw() const noexcept { return m_val; }

private:
    const void* m_val = nullptr;
};

/// One object member (key + value).
struct JsonMember {
    std::string_view key;
    JsonValue value;
};

/// Owns a parsed JSONC document. Move-only.
class JsonDocument {
public:
    JsonDocument() noexcept = default;
    ~JsonDocument();
    JsonDocument(JsonDocument&& o) noexcept : m_doc(std::exchange(o.m_doc, nullptr)) {}
    JsonDocument& operator=(JsonDocument&& o) noexcept;
    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;

    /// Parses JSONC. Errors are ErrorCode::ParseError "<source>:<line>:<col>: <reason>", or
    /// ErrorCode::LimitExceeded for documents nested deeper than kMaxJsonDepth.
    static Result<JsonDocument> parse(std::string_view text, std::string_view sourceName = "<json>");

    JsonValue root() const noexcept;

private:
    void* m_doc = nullptr;
};

enum class JsonStyle : u8 { Pretty, Compact };

/// Shortest round-trip text of a double in the canonical notation described above ("1.5", "-0.0",
/// "1e+21", "nan"). Writes at most 32 chars to `out`; returns the length.
usize formatJsonF64(f64 v, char* out) noexcept;
/// Same for binary32 (shortest digits that round-trip as f32).
usize formatJsonF32(f32 v, char* out) noexcept;

/// Canonical streaming writer. Callers emit keys in canonical order ($-keys first, then schema
/// order); the writer only handles layout and escaping.
class JsonWriter {
public:
    explicit JsonWriter(JsonStyle style = JsonStyle::Pretty) : m_style(style) {}

    void beginObject();
    void endObject();
    /// `inlineElements`: keep the array on one line (used for arrays of scalars); nested containers
    /// inside an inline array are inline too.
    void beginArray(bool inlineElements = false);
    void endArray();
    void key(std::string_view k);

    void null();
    void boolean(bool v);
    void integer(i64 v);
    void unsignedInteger(u64 v);
    void number(f64 v);
    void numberF32(f32 v);
    void string(std::string_view v);
    /// Inserts an already rendered JSON value verbatim (e.g. a compact patch value).
    void raw(std::string_view json);
    /// Copies a parsed value (numbers keep their exact source text; object member order is kept).
    void copy(JsonValue v);

    JsonStyle style() const noexcept { return m_style; }
    /// The text so far. For a complete Pretty document, take() appends the final newline.
    const std::string& text() const noexcept { return m_out; }
    std::string take();

private:
    struct Frame {
        bool object = false;
        bool inlined = false;
        usize count = 0;
    };
    void beforeValue();
    void newline(usize depth);
    void writeEscaped(std::string_view s);

    JsonStyle m_style;
    std::string m_out;
    std::vector<Frame> m_stack;
    bool m_afterKey = false;
};

/// Error and warning context for JSON readers: tracks the path of the value being read so
/// messages read "thrusters[2].maxForce: expected number, got string".
class ReadCtx {
public:
    struct Options {
        /// Unknown object members produce an error instead of a warning.
        bool strictUnknownFields = false;
    };

    ReadCtx() = default;
    explicit ReadCtx(Options options) : m_options(options) {}

    /// RAII path segment.
    class Scope {
    public:
        Scope(ReadCtx& ctx, std::string_view key) : m_ctx(ctx) { ctx.m_path.push_back({key, 0, false}); }
        Scope(ReadCtx& ctx, usize index) : m_ctx(ctx) { ctx.m_path.push_back({{}, index, true}); }
        ~Scope() { m_ctx.m_path.pop_back(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        ReadCtx& m_ctx;
    };

    /// Dotted path of the current value ("" at the root).
    std::string path() const;
    /// ErrorCode::ParseError prefixed with the current path.
    Error error(std::string_view message) const;
    Error typeError(std::string_view expected, JsonValue got) const;
    void warn(std::string_view message);
    /// Records an unknown member (warning, or error in strict mode).
    Result<void> unknownField(std::string_view key);

    const std::vector<std::string>& warnings() const noexcept { return m_warnings; }
    const Options& options() const noexcept { return m_options; }

private:
    struct Segment {
        std::string_view key;
        usize index;
        bool isIndex;
    };
    Options m_options;
    std::vector<Segment> m_path;
    std::vector<std::string> m_warnings;
};

} // namespace helios::refl
