// Out-of-line codecs for builtin types and their TypeInfos.

#include "helios/reflect/codec.h"

#include "helios/core/utf.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <limits>

namespace helios::refl {

// ---------------------------------------------------------------------------------------------
// detail helpers
// ---------------------------------------------------------------------------------------------

namespace detail {

Result<u64> readVarintField(TaggedReader& r, WireType wire, std::string_view what) {
    if (wire != WireType::Varint) return wireTypeMismatch(what, wire, WireType::Varint);
    return r.readVarint();
}

bool hasFieldAliases(const TypeInfo& type) noexcept {
    for (const FieldInfo& f : type.fields) {
        if (!f.was.empty()) return true;
    }
    return false;
}

bool isFieldAlias(const TypeInfo& type, std::string_view key) noexcept {
    return type.field(key) == nullptr && type.fieldOrAlias(key) != nullptr;
}

Result<std::string_view> readUtf8(TaggedReader& r, std::string_view what) {
    HELIOS_TRY_ASSIGN(const auto bytes, r.readLen());
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    // Text is UTF-8 everywhere (JSONC, Go strings, UI); reject bad bytes here, like protobuf, rather
    // than let them reach canonical JSON that no reader accepts.
    if (!isValidUtf8(text)) return makeError(ErrorCode::Corrupt, "{}: invalid UTF-8", what);
    return text;
}

Result<u64> readFixed64Field(TaggedReader& r, WireType wire, std::string_view what) {
    if (wire != WireType::I64) return wireTypeMismatch(what, wire, WireType::I64);
    return r.readFixed64();
}

Error integerOutOfRange(std::string_view what, i64 v) {
    return makeError(ErrorCode::OutOfRange, "{} value {} out of range", what, v);
}

Error integerOutOfRangeU(std::string_view what, u64 v) {
    return makeError(ErrorCode::OutOfRange, "{} value {} out of range", what, v);
}

Result<void> readRecordIdJson(JsonValue in, RecordId& v, ReadCtx& ctx) {
    u64 id = 0;
    if (!in.getU64(id)) return ctx.typeError("record id (unsigned integer)", in);
    if ((id & ~kRecordIdMask) != 0) return ctx.error("record id exceeds 63 bits");
    v = id;
    return {};
}

i64 enumDefaultValue(const TypeInfo& type) noexcept {
    if (type.kind == Kind::Enum && !type.enumValues.empty()) return type.enumValues.front().value;
    return 0;
}

void writeEnumJson(JsonWriter& w, const TypeInfo& type, i64 value) {
    if (type.kind == Kind::Flags) {
        w.beginArray(true);
        u64 remaining = static_cast<u64>(value);
        for (const EnumValue& ev : type.enumValues) {
            const u64 bit = static_cast<u64>(ev.value);
            if (bit != 0 && (bit & (bit - 1)) == 0 && (remaining & bit) != 0) {
                w.string(ev.name);
                remaining &= ~bit;
            }
        }
        if (remaining != 0) w.unsignedInteger(remaining);
        w.endArray();
        return;
    }
    if (const EnumValue* ev = type.enumByValue(value)) {
        w.string(ev->name);
    } else {
        w.integer(value);
    }
}

Result<i64> readEnumJson(JsonValue in, const TypeInfo& type, ReadCtx& ctx) {
    auto one = [&](JsonValue v) -> Result<i64> {
        if (v.isString()) {
            if (const EnumValue* ev = type.enumByName(v.asString())) return ev->value;
            return ctx.error(std::format("unknown {} value '{}'", type.qualifiedName, v.asString()));
        }
        i64 s = 0;
        if (v.getI64(s)) return s;
        u64 u = 0;
        if (v.getU64(u)) return static_cast<i64>(u);
        return ctx.typeError("enum value name or integer", v);
    };
    if (type.kind == Kind::Flags) {
        if (in.isArray()) {
            u64 bits = 0;
            usize i = 0;
            for (JsonValue e : in.elements()) {
                ReadCtx::Scope s(ctx, i++);
                HELIOS_TRY_ASSIGN(const i64 b, one(e));
                bits |= static_cast<u64>(b);
            }
            return static_cast<i64>(bits);
        }
        if (in.isNumber()) return one(in);
        return ctx.typeError("array of flag names", in);
    }
    return one(in);
}

Result<Guid> readKeyedListKey(JsonValue element, ReadCtx& ctx) {
    if (!element.isObject()) return ctx.typeError("object", element);
    const JsonValue key = element.get("$key");
    if (!key.isValid()) {
        const Guid minted = Guid::generate();
        ctx.warn("element has no $key; minted " + minted.toString() + " (re-save to persist it)");
        return minted;
    }
    if (!key.isString()) return ctx.typeError("$key string", key);
    std::string_view text = key.asString();
    if (text.starts_with("guid:")) text.remove_prefix(5);
    auto parsed = Guid::parse(text);
    if (!parsed || parsed->isNil()) return ctx.error("invalid $key '" + std::string(key.asString()) + "'");
    return *parsed;
}

Error unknownAlternative(std::string_view variant, std::string_view name) {
    return makeError(ErrorCode::ParseError, "unknown alternative '{}' of {}", name, variant);
}

} // namespace detail

// ---------------------------------------------------------------------------------------------
// Scalars and text
// ---------------------------------------------------------------------------------------------

Result<void> Codec<bool>::readValue(TaggedReader& r, WireType wire, bool& v) {
    HELIOS_TRY_ASSIGN(const u64 raw, detail::readVarintField(r, wire, "bool"));
    v = raw != 0;
    return {};
}

Result<void> Codec<bool>::readJson(JsonValue in, bool& v, ReadCtx& ctx) {
    if (!in.isBool()) return ctx.typeError("bool", in);
    v = in.asBool();
    return {};
}

Result<void> Codec<f32>::readValue(TaggedReader& r, WireType wire, f32& v) {
    if (wire != WireType::I32) return wireTypeMismatch("f32", wire, WireType::I32);
    HELIOS_TRY_ASSIGN(v, r.readF32());
    return {};
}

Result<void> Codec<f32>::readJson(JsonValue in, f32& v, ReadCtx& ctx) {
    if (!in.getF32(v)) return ctx.typeError("number", in);
    return {};
}

Result<void> Codec<f64>::readValue(TaggedReader& r, WireType wire, f64& v) {
    if (wire == WireType::I32) {
        HELIOS_TRY_ASSIGN(const f32 narrow, r.readF32());
        v = static_cast<f64>(narrow);
        return {};
    }
    if (wire != WireType::I64) return wireTypeMismatch("f64", wire, WireType::I64);
    HELIOS_TRY_ASSIGN(v, r.readF64());
    return {};
}

Result<void> Codec<f64>::readJson(JsonValue in, f64& v, ReadCtx& ctx) {
    if (!in.getF64(v)) return ctx.typeError("number", in);
    return {};
}

Result<void> Codec<std::string>::readValue(TaggedReader& r, WireType wire, std::string& v) {
    if (wire != WireType::Len) return wireTypeMismatch("string", wire, WireType::Len);
    HELIOS_TRY_ASSIGN(const std::string_view text, detail::readUtf8(r, "string"));
    v.assign(text);
    return {};
}

Result<void> Codec<std::string>::readJson(JsonValue in, std::string& v, ReadCtx& ctx) {
    if (!in.isString()) return ctx.typeError("string", in);
    v.assign(in.asString());
    return {};
}

Result<void> Codec<Name>::readValue(TaggedReader& r, WireType wire, Name& v) {
    if (wire != WireType::Len) return wireTypeMismatch("Name", wire, WireType::Len);
    HELIOS_TRY_ASSIGN(const std::string_view text, detail::readUtf8(r, "Name"));
    v = Name(text);
    return {};
}

Result<void> Codec<Name>::readJson(JsonValue in, Name& v, ReadCtx& ctx) {
    if (!in.isString()) return ctx.typeError("string", in);
    v = Name(in.asString());
    return {};
}

// ---------------------------------------------------------------------------------------------
// Vocabulary types
// ---------------------------------------------------------------------------------------------

void Codec<Guid>::writeValue(TaggedWriter& w, const Guid& v) {
    const std::array<u8, 16> bytes = v.toBytes();
    w.writeLenBytes(bytes.data(), bytes.size());
}

Result<void> Codec<Guid>::readValue(TaggedReader& r, WireType wire, Guid& v) {
    if (wire != WireType::Len) return wireTypeMismatch("Guid", wire, WireType::Len);
    HELIOS_TRY_ASSIGN(const auto bytes, r.readLen());
    if (bytes.size() != 16) return makeError(ErrorCode::Corrupt, "Guid: {} bytes, expected 16", bytes.size());
    std::array<u8, 16> b{};
    std::memcpy(b.data(), bytes.data(), 16);
    v = Guid::fromBytes(b);
    return {};
}

void Codec<Guid>::writeJson(JsonWriter& w, const Guid& v) { w.string(v.toString()); }

Result<void> Codec<Guid>::readJson(JsonValue in, Guid& v, ReadCtx& ctx) {
    if (!in.isString()) return ctx.typeError("GUID string", in);
    std::string_view t = in.asString();
    if (t.starts_with("guid:")) t.remove_prefix(5);
    auto g = Guid::parse(t);
    if (!g) return ctx.error("invalid GUID '" + std::string(in.asString()) + "'");
    v = *g;
    return {};
}

bool KeyTraits<Guid>::fromText(std::string_view t, Guid& k) {
    auto g = Guid::parse(t);
    if (!g) return false;
    k = *g;
    return true;
}

Result<void> Codec<EntityId>::readValue(TaggedReader& r, WireType wire, EntityId& v) {
    HELIOS_TRY_ASSIGN(v.value, detail::readFixed64Field(r, wire, "EntityId"));
    return {};
}

void Codec<EntityId>::writeJson(JsonWriter& w, EntityId v) { w.string("ent:" + std::to_string(v.value)); }

Result<void> Codec<EntityId>::readJson(JsonValue in, EntityId& v, ReadCtx& ctx) {
    if (in.isString()) {
        std::string_view t = in.asString();
        if (t.starts_with("ent:")) t.remove_prefix(4);
        const auto r = std::from_chars(t.data(), t.data() + t.size(), v.value);
        if (t.empty() || r.ec != std::errc() || r.ptr != t.data() + t.size())
            return ctx.error("invalid entity id '" + std::string(in.asString()) + "' (expected \"ent:<id>\")");
        return {};
    }
    if (!in.getU64(v.value)) return ctx.typeError("entity id (\"ent:<id>\" or unsigned integer)", in);
    return {};
}

void Codec<AssetRef>::writeJson(JsonWriter& w, const AssetRef& v) { w.string("guid:" + v.guid.toString()); }

Result<void> Codec<NetHandle>::readValue(TaggedReader& r, WireType wire, NetHandle& v) {
    HELIOS_TRY_ASSIGN(const u64 raw, detail::readVarintField(r, wire, "NetHandle"));
    if (raw > std::numeric_limits<u32>::max()) return detail::integerOutOfRangeU("NetHandle", raw);
    v.value = static_cast<u32>(raw);
    return {};
}

Result<void> Codec<NetHandle>::readJson(JsonValue in, NetHandle& v, ReadCtx& ctx) {
    u64 raw = 0;
    if (!in.getU64(raw) || raw > std::numeric_limits<u32>::max()) return ctx.typeError("u32 net handle", in);
    v.value = static_cast<u32>(raw);
    return {};
}

namespace {
struct DurationUnit {
    std::string_view suffix;
    i64 nanos;
};
// Largest first: formatting picks the first unit that divides exactly.
constexpr DurationUnit kDurationUnits[] = {
    {"d", 86'400'000'000'000}, {"h", 3'600'000'000'000}, {"m", 60'000'000'000}, {"s", 1'000'000'000},
    {"ms", 1'000'000},         {"us", 1'000},             {"ns", 1},
};
} // namespace

std::string formatDuration(Duration d) {
    if (d.nanos == 0) return "0s";
    for (const DurationUnit& u : kDurationUnits) {
        if (d.nanos % u.nanos == 0) return std::to_string(d.nanos / u.nanos) + std::string(u.suffix);
    }
    return std::to_string(d.nanos) + "ns";
}

Result<Duration> parseDuration(std::string_view text) {
    const std::string original(text);
    auto fail = [&] { return Error{ErrorCode::ParseError, "invalid duration '" + original + "' (e.g. \"500ms\", \"1.5s\", \"30d\")"}; };
    bool negative = false;
    if (!text.empty() && text[0] == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    usize i = 0;
    while (i < text.size() && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.')) ++i;
    const std::string_view number = text.substr(0, i);
    const std::string_view suffix = text.substr(i);
    if (number.empty() || number == ".") return fail();
    const DurationUnit* unit = nullptr;
    for (const DurationUnit& u : kDurationUnits) {
        if (u.suffix == suffix) unit = &u;
    }
    if (!unit) return fail();
    const usize dot = number.find('.');
    const std::string_view intPart = number.substr(0, dot);
    const std::string_view fracPart = dot == std::string_view::npos ? std::string_view() : number.substr(dot + 1);
    if (fracPart.find('.') != std::string_view::npos) return fail();
    u64 whole = 0;
    if (!intPart.empty()) {
        const auto r = std::from_chars(intPart.data(), intPart.data() + intPart.size(), whole);
        if (r.ec != std::errc() || r.ptr != intPart.data() + intPart.size()) return fail();
    }
    const u64 unitNanos = static_cast<u64>(unit->nanos);
    if (whole > static_cast<u64>(std::numeric_limits<i64>::max()) / unitNanos) return fail();
    u64 total = whole * unitNanos;
    // Fraction: exact while the unit has factors of ten left, then rounded at the nanosecond.
    u64 fracNanos = 0;
    u64 scale = unitNanos;
    for (const char c : fracPart) {
        if (c < '0' || c > '9') return fail();
        const u64 digit = static_cast<u64>(c - '0');
        if (scale % 10 == 0) {
            scale /= 10;
            fracNanos += digit * scale;
        } else {
            const u64 num = digit * scale; // contribution is num / 10 nanoseconds
            fracNanos += num / 10 + (num % 10 >= 5 ? 1 : 0);
            break;
        }
    }
    total += fracNanos;
    if (total > static_cast<u64>(std::numeric_limits<i64>::max())) return fail();
    const i64 signedTotal = static_cast<i64>(total);
    return Duration(negative ? -signedTotal : signedTotal);
}

Result<void> Codec<Duration>::readValue(TaggedReader& r, WireType wire, Duration& v) {
    HELIOS_TRY_ASSIGN(const u64 raw, detail::readVarintField(r, wire, "Duration"));
    v.nanos = zigzagDecode(raw);
    return {};
}

void Codec<Duration>::writeJson(JsonWriter& w, Duration v) { w.string(formatDuration(v)); }

Result<void> Codec<Duration>::readJson(JsonValue in, Duration& v, ReadCtx& ctx) {
    if (in.isString()) {
        auto d = parseDuration(in.asString());
        if (!d) return ctx.error(d.error().message);
        v = *d;
        return {};
    }
    f64 seconds = 0;
    if (in.getF64(seconds) && std::isfinite(seconds) && std::fabs(seconds) < 9.2e9) {
        v.nanos = static_cast<i64>(std::llround(seconds * 1e9));
        return {};
    }
    return ctx.typeError("duration string (\"500ms\") or seconds", in);
}

void Codec<TagSet>::writeValue(TaggedWriter& w, const TagSet& v) {
    const usize m = w.beginLen();
    for (const Name tag : v.tags()) {
        w.writeTag(1, WireType::Len);
        w.writeString(tag.view());
    }
    w.endLen(m);
}

Result<void> Codec<TagSet>::readValue(TaggedReader& r, WireType wire, TagSet& v) {
    if (wire != WireType::Len) return wireTypeMismatch("TagSet", wire, WireType::Len);
    HELIOS_TRY_ASSIGN(TaggedReader sub, r.readMessage());
    while (!sub.atEnd()) {
        HELIOS_TRY_ASSIGN(const FieldTag tag, sub.readTag());
        if (tag.id == 1 && tag.wire == WireType::Len) {
            HELIOS_TRY_ASSIGN(const std::string_view text, detail::readUtf8(sub, "tag"));
            v.add(text);
        } else {
            HELIOS_TRY(sub.skip(tag.wire));
        }
    }
    return {};
}

void Codec<TagSet>::writeJson(JsonWriter& w, const TagSet& v) {
    w.beginArray(true);
    for (const Name tag : v.tags()) w.string(tag.view());
    w.endArray();
}

Result<void> Codec<TagSet>::readJson(JsonValue in, TagSet& v, ReadCtx& ctx) {
    if (in.isNull()) { // Go writes an empty (nil) TagSet as null
        v.clear();
        return {};
    }
    if (!in.isArray()) return ctx.typeError("array of tag names", in);
    v.clear();
    usize i = 0;
    for (JsonValue e : in.elements()) {
        ReadCtx::Scope s(ctx, i++);
        if (!e.isString()) return ctx.typeError("tag name", e);
        v.add(e.asString());
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// Builtin TypeInfos
// ---------------------------------------------------------------------------------------------

namespace {
template <class T>
TypeInfo builtin(std::string_view name, Kind kind, TypeFlags flags = TypeFlags::InlineJson,
                 std::span<const FieldInfo> fields = {}) {
    TypeInfo t;
    t.qualifiedName = name;
    t.name = name;
    t.id = detail::typeIdFromName(name);
    t.size = static_cast<u32>(sizeof(T));
    t.align = static_cast<u32>(alignof(T));
    t.kind = kind;
    t.wire = Codec<T>::kWire;
    t.flags = flags;
    t.fields = fields;
    t.ops = &makeOps<T>();
    t.layoutHash = t.id;
    return t;
}

template <class F>
constexpr FieldInfo tupleField(std::string_view name, u32 id, u32 offset) {
    FieldInfo f;
    f.name = name;
    f.id = id;
    f.offset = offset;
    f.typeFn = &TypeOf<F>::get;
    return f;
}
} // namespace

#define HELIOS_REFLECT_DEFINE_BUILTIN_(T, NAME, KIND, FLAGS)                                        \
    const TypeInfo& TypeOf<T>::get() noexcept {                                                     \
        static const TypeInfo info = builtin<T>(NAME, KIND, FLAGS);                                 \
        return info;                                                                                \
    }

HELIOS_REFLECT_DEFINE_BUILTIN_(bool, "bool", Kind::Bool, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(i8, "i8", Kind::I8, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(i16, "i16", Kind::I16, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(i32, "i32", Kind::I32, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(i64, "i64", Kind::I64, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(u8, "u8", Kind::U8, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(u16, "u16", Kind::U16, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(u32, "u32", Kind::U32, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(u64, "u64", Kind::U64, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(f32, "f32", Kind::F32, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(f64, "f64", Kind::F64, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(std::string, "string", Kind::String, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(Name, "Name", Kind::Name, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(Guid, "Guid", Kind::Builtin, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(EntityId, "EntityId", Kind::Builtin, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(NetHandle, "NetHandle", Kind::Builtin, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(Duration, "Duration", Kind::Builtin, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(AssetRef, "AssetRef", Kind::Builtin, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(LocString, "LocString", Kind::Builtin, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(TagSet, "TagSet", Kind::Builtin, TypeFlags::None)
HELIOS_REFLECT_DEFINE_BUILTIN_(TagQuery, "TagQuery", Kind::Builtin, TypeFlags::InlineJson)
HELIOS_REFLECT_DEFINE_BUILTIN_(HxlExpr, "HxlExpr", Kind::Builtin, TypeFlags::InlineJson)

#undef HELIOS_REFLECT_DEFINE_BUILTIN_

#define HELIOS_REFLECT_DEFINE_TUPLE_(T, F, NAME, ...)                                               \
    const TypeInfo& TypeOf<T>::get() noexcept {                                                     \
        static const FieldInfo fields[] = {__VA_ARGS__};                                            \
        static const TypeInfo info = builtin<T>(NAME, Kind::Builtin, TypeFlags::Tuple, fields);     \
        return info;                                                                                \
    }

HELIOS_REFLECT_DEFINE_TUPLE_(Vec2, f32, "vec2f", tupleField<f32>("x", 1, 0), tupleField<f32>("y", 2, 4))
HELIOS_REFLECT_DEFINE_TUPLE_(Vec3, f32, "vec3f", tupleField<f32>("x", 1, 0), tupleField<f32>("y", 2, 4),
                             tupleField<f32>("z", 3, 8))
HELIOS_REFLECT_DEFINE_TUPLE_(Vec4, f32, "vec4f", tupleField<f32>("x", 1, 0), tupleField<f32>("y", 2, 4),
                             tupleField<f32>("z", 3, 8), tupleField<f32>("w", 4, 12))
HELIOS_REFLECT_DEFINE_TUPLE_(DVec3, f64, "vec3d", tupleField<f64>("x", 1, 0), tupleField<f64>("y", 2, 8),
                             tupleField<f64>("z", 3, 16))
HELIOS_REFLECT_DEFINE_TUPLE_(Quat, f32, "quatf", tupleField<f32>("x", 1, 0), tupleField<f32>("y", 2, 4),
                             tupleField<f32>("z", 3, 8), tupleField<f32>("w", 4, 12))
HELIOS_REFLECT_DEFINE_TUPLE_(DQuat, f64, "quatd", tupleField<f64>("x", 1, 0), tupleField<f64>("y", 2, 8),
                             tupleField<f64>("z", 3, 16), tupleField<f64>("w", 4, 24))
HELIOS_REFLECT_DEFINE_TUPLE_(Color, f32, "color", tupleField<f32>("r", 1, 0), tupleField<f32>("g", 2, 4),
                             tupleField<f32>("b", 3, 8), tupleField<f32>("a", 4, 12))
HELIOS_REFLECT_DEFINE_TUPLE_(WorldPos, f64, "WorldPos", tupleField<f64>("x", 1, 0), tupleField<f64>("y", 2, 8),
                             tupleField<f64>("z", 3, 16))

#undef HELIOS_REFLECT_DEFINE_TUPLE_

static_assert(sizeof(Vec3) == 12 && sizeof(DVec3) == 24 && sizeof(Quat) == 16 && sizeof(DQuat) == 32);
static_assert(sizeof(Color) == 16 && sizeof(WorldPos) == 24 && sizeof(Vec2) == 8 && sizeof(Vec4) == 16);

} // namespace helios::refl
