// `--emit go`: Go structs with JSON tags and a tagged binary codec byte-identical to the C++ one.
// Output is one package: `<stem>.go` per schema file, `helios_codecs.go` (per-type wire helpers),
// `helios_runtime.go` (wire primitives + vocabulary types) and `helios_schema_test.go`.

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "code_writer.h"
#include "generators.h"
#include "go_names.h"
#include "samples.h"
#include "text.h"

namespace helios::schemac {

extern const unsigned char kGoRuntimeSource[];

namespace {

std::string goPrim(Prim p) {
    switch (p) {
    case Prim::Bool: return "bool";
    case Prim::I8: return "int8";
    case Prim::I16: return "int16";
    case Prim::I32: return "int32";
    case Prim::I64: return "int64";
    case Prim::U8: return "uint8";
    case Prim::U16: return "uint16";
    case Prim::U32: return "uint32";
    case Prim::U64: return "uint64";
    case Prim::F32: return "float32";
    case Prim::F64: return "float64";
    case Prim::String:
    case Prim::Name: return "string";
    default: return "?";
    }
}

std::string goBuiltin(Builtin b) {
    switch (b) {
    case Builtin::Vec2f: return "Vec2";
    case Builtin::Vec3f: return "Vec3";
    case Builtin::Vec4f: return "Vec4";
    case Builtin::Vec3d: return "DVec3";
    case Builtin::Quatf: return "Quat";
    case Builtin::Quatd: return "DQuat";
    case Builtin::Color: return "RGBA";
    default: return std::string(builtinName(b));
    }
}

bool isStringLike(const Type* t) {
    if (t->kind == TypeKind::Prim) return t->prim == Prim::String || t->prim == Prim::Name;
    if (t->kind == TypeKind::Builtin) return t->builtin == Builtin::LocString || t->builtin == Builtin::TagQuery || t->builtin == Builtin::HxlExpr;
    return false;
}

bool identityTuple(Builtin b) { return b == Builtin::Quatf || b == Builtin::Quatd || b == Builtin::Color; }

std::string goFloat(f64 v, bool f32) {
    std::string s = f32 ? formatF32(static_cast<float>(v)) : formatF64(v);
    if (s == "-0.0") return "math.Copysign(0, -1)";
    return s;
}

} // namespace

std::string goFieldName(const std::string& name) { return pascalCase(name); }

std::string goRecordRefName(const Decl* record) {
    const std::string& n = record->goName;
    const std::string base = n.size() > 3 && n.ends_with("Def") ? n.substr(0, n.size() - 3) : n;
    return base + "Ref";
}

std::string goEnumConst(const Decl* enumDecl, const std::string& value) { return enumDecl->goName + value; }

std::string goTypeName(const Type* t) {
    switch (t->kind) {
    case TypeKind::Prim: return goPrim(t->prim);
    case TypeKind::Builtin: return goBuiltin(t->builtin);
    case TypeKind::Enum:
    case TypeKind::Flags:
    case TypeKind::Struct:
    case TypeKind::Variant: return t->decl->goName;
    case TypeKind::List:
    case TypeKind::Set:
        // encoding/json writes []uint8 as base64; U8List keeps the schema's JSON number array.
        if (t->element->kind == TypeKind::Prim && t->element->prim == Prim::U8) return "U8List";
        return "[]" + goTypeName(t->element);
    case TypeKind::KeyedList: return "[]Keyed[" + goTypeName(t->element) + "]";
    case TypeKind::Map: return "map[" + goTypeName(t->key) + "]" + goTypeName(t->element);
    case TypeKind::Optional: return "*" + goTypeName(t->element);
    case TypeKind::Array: return "[" + std::to_string(t->arraySize) + "]" + goTypeName(t->element);
    case TypeKind::RecordRef: return goRecordRefName(t->decl);
    case TypeKind::AssetRef: return "AssetRef";
    }
    return "any";
}

std::string goValueLiteral(const Value& v, const Type* t) {
    switch (t->kind) {
    case TypeKind::Prim:
        switch (t->prim) {
        case Prim::Bool: return v.b ? "true" : "false";
        case Prim::F32: return "float32(" + goFloat(v.f, true) + ")";
        case Prim::F64: return "float64(" + goFloat(v.f, false) + ")";
        case Prim::String:
        case Prim::Name: return goQuote(v.s);
        default: return goPrim(t->prim) + "(" + (v.kind == Value::Kind::UInt ? std::to_string(v.u) : std::to_string(v.i)) + ")";
        }
    case TypeKind::Builtin:
        switch (t->builtin) {
        case Builtin::LocString:
        case Builtin::TagQuery:
        case Builtin::HxlExpr: return goQuote(v.s);
        case Builtin::Guid: {
            std::string out = "Guid{";
            for (int i = 0; i < 16; ++i) {
                const u64 half = i < 8 ? v.u : v.u2;
                out += std::format("{}{:#04x}", i ? ", " : "", static_cast<u32>((half >> (8 * (7 - (i % 8)))) & 0xFF));
            }
            return out + "}";
        }
        case Builtin::Duration: return std::format("Duration({})", v.i);
        case Builtin::EntityId: return std::format("EntityId({})", v.u);
        case Builtin::NetHandle: return std::format("NetHandle({})", v.u);
        case Builtin::Tick: return std::format("uint64({})", v.u);
        default: {
            const bool f64 = tupleIsF64(t->builtin);
            std::string out = goBuiltin(t->builtin) + "{";
            for (usize i = 0; i < v.items.size(); ++i) out += (i ? ", " : "") + goFloat(v.items[i].f, !f64);
            return out + "}";
        }
        }
    case TypeKind::Enum: return goEnumConst(t->decl, v.s);
    case TypeKind::Flags: return std::format("{}({})", t->decl->goName, v.u);
    case TypeKind::Array: {
        std::string out = goTypeName(t) + "{";
        for (usize i = 0; i < v.items.size(); ++i) out += (i ? ", " : "") + goValueLiteral(v.items[i], t->element);
        return out + "}";
    }
    default: return goZeroValue(t);
    }
}

std::string goZeroValue(const Type* t) {
    switch (t->kind) {
    case TypeKind::Prim:
        if (t->prim == Prim::Bool) return "false";
        if (isStringLike(t)) return "\"\"";
        return goPrim(t->prim) + "(0)";
    case TypeKind::Builtin:
        if (isStringLike(t)) return "\"\"";
        if (identityTuple(t->builtin)) return goBuiltin(t->builtin) + "{0, 0, 0, 1}";
        switch (t->builtin) {
        case Builtin::Guid:
        case Builtin::TagSet:
        case Builtin::Vec2f:
        case Builtin::Vec3f:
        case Builtin::Vec4f:
        case Builtin::Vec3d:
        case Builtin::WorldPos: return goBuiltin(t->builtin) + "{}";
        case Builtin::Tick: return "uint64(0)";
        default: return goBuiltin(t->builtin) + "(0)";
        }
    case TypeKind::Enum: return t->decl->values.empty() ? t->decl->goName + "(0)" : goEnumConst(t->decl, t->decl->values.front().name);
    case TypeKind::Flags: return t->decl->goName + "(0)";
    case TypeKind::Struct:
    case TypeKind::Variant: return "New" + t->decl->goName + "()";
    case TypeKind::List:
    case TypeKind::Set:
    case TypeKind::KeyedList:
    case TypeKind::Map:
    case TypeKind::Optional: return "(" + goTypeName(t) + ")(nil)";
    case TypeKind::Array: {
        const std::string el = goZeroValue(t->element);
        std::string out = goTypeName(t) + "{";
        for (u32 i = 0; i < t->arraySize; ++i) out += (i ? ", " : "") + el;
        return out + "}";
    }
    case TypeKind::RecordRef: return goRecordRefName(t->decl) + "(0)";
    case TypeKind::AssetRef: return "AssetRef{}";
    }
    return "nil";
}

namespace {

/// Per-type wire helpers (enc_/encv_/dec_/decv_/def_/zero_), generated once per signature.
class GoCodecs {
public:
    std::string id(const Type* t) {
        auto it = m_ids.find(t->signature);
        if (it != m_ids.end()) return it->second;
        std::string clean;
        for (const char c : t->signature) clean += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
        if (clean.size() > 40) clean = clean.substr(clean.size() - 40);
        const std::string name = std::format("{}_{}", clean, m_ids.size());
        m_ids.emplace(t->signature, name);
        m_pending.push_back(t);
        return name;
    }

    static bool wrapped(const Type* e) {
        return e->kind == TypeKind::List || e->kind == TypeKind::KeyedList || e->kind == TypeKind::Set || e->kind == TypeKind::Map ||
               e->kind == TypeKind::Array || e->kind == TypeKind::Optional;
    }

    static std::string wire(const Type* t) {
        switch (t->kind) {
        case TypeKind::Prim:
            if (t->prim == Prim::F32) return "wireI32";
            if (t->prim == Prim::F64) return "wireI64";
            if (isStringLike(t)) return "wireLen";
            return "wireVarint";
        case TypeKind::Builtin:
            if (t->builtin == Builtin::EntityId) return "wireI64";
            if (t->builtin == Builtin::NetHandle || t->builtin == Builtin::Tick || t->builtin == Builtin::Duration) return "wireVarint";
            return "wireLen";
        case TypeKind::Enum:
        case TypeKind::Flags: return "wireVarint";
        case TypeKind::RecordRef: return "wireI64";
        default: return "wireLen";
        }
    }
    static std::string elementWire(const Type* e) { return wrapped(e) ? "wireLen" : wire(e); }
    static bool packable(const Type* e) { return elementWire(e) != "wireLen"; }

    /// Emits helpers for every type requested so far (and their dependencies).
    std::string emitAll() {
        CodeWriter w(1, '\t');
        while (!m_pending.empty()) {
            const Type* t = m_pending.front();
            m_pending.erase(m_pending.begin());
            emit(w, t);
        }
        return w.take();
    }

private:
    static std::string signedRange(Prim p) {
        switch (p) {
        case Prim::I8: return "math.MinInt8, math.MaxInt8";
        case Prim::I16: return "math.MinInt16, math.MaxInt16";
        case Prim::I32: return "math.MinInt32, math.MaxInt32";
        default: return "math.MinInt64, math.MaxInt64";
        }
    }
    static std::string unsignedMax(Prim p) {
        switch (p) {
        case Prim::U8: return "math.MaxUint8";
        case Prim::U16: return "math.MaxUint16";
        case Prim::U32: return "math.MaxUint32";
        default: return "math.MaxUint64";
        }
    }

    void emit(CodeWriter& w, const Type* t) {
        const std::string n = m_ids.at(t->signature);
        const std::string T = goTypeName(t);
        // zero value
        w.line(std::format("func zero_{}() {} {{ return {} }}", n, T, goZeroValue(t)));
        w.line();
        // implicit default test
        w.open(std::format("func def_{}(v {}) bool {{", n, T));
        w.line("return " + defaultExpr(t, "v"));
        w.close();
        w.line();
        const bool repeated = t->isContainer();
        if (!repeated && t->kind != TypeKind::Optional) emitValueCodec(w, t, n, T);
        emitFieldCodec(w, t, n, T);
    }

    std::string defaultExpr(const Type* t, const std::string& v) {
        switch (t->kind) {
        case TypeKind::Prim:
            if (t->prim == Prim::Bool) return "!" + v;
            if (t->prim == Prim::F32) return "math.Float32bits(" + v + ") == 0";
            if (t->prim == Prim::F64) return "math.Float64bits(" + v + ") == 0";
            if (isStringLike(t)) return v + " == \"\"";
            return v + " == 0";
        case TypeKind::Builtin:
            if (isStringLike(t)) return v + " == \"\"";
            if (tupleSize(t->builtin) != 0) {
                const std::string zero = "zero_" + id(t) + "()";
                return std::format("func() bool {{ z := {}; return {}({}[:], z[:]) }}()", zero,
                                   tupleIsF64(t->builtin) ? "f64TupleBits" : "f32TupleBits", v);
            }
            if (t->builtin == Builtin::Guid) return v + " == Guid{}";
            if (t->builtin == Builtin::TagSet) return "len(" + v + ") == 0";
            return v + " == 0";
        case TypeKind::Enum: return v + " == " + goZeroValue(t);
        case TypeKind::Flags: return v + " == 0";
        case TypeKind::Struct:
        case TypeKind::Variant: return v + ".isDefault()";
        case TypeKind::List:
        case TypeKind::Set:
        case TypeKind::KeyedList:
        case TypeKind::Map: return "len(" + v + ") == 0";
        case TypeKind::Optional: return v + " == nil";
        case TypeKind::Array: {
            const std::string e = id(t->element);
            return std::format("func() bool {{ for _, e := range {} {{ if !def_{}(e) {{ return false }} }}; return true }}()", v, e);
        }
        case TypeKind::RecordRef: return v + " == 0";
        case TypeKind::AssetRef: return v + " == AssetRef{}";
        }
        return "false";
    }

    void emitValueCodec(CodeWriter& w, const Type* t, const std::string& n, const std::string& T) {
        // encv: one value (LEN values include their length prefix)
        w.open(std::format("func encv_{}(b []byte, v {}) []byte {{", n, T));
        switch (t->kind) {
        case TypeKind::Prim:
            if (t->prim == Prim::Bool) {
                w.line("return appendBool(b, v)");
            } else if (t->prim == Prim::F32) {
                w.line("return appendF32(b, v)");
            } else if (t->prim == Prim::F64) {
                w.line("return appendF64(b, v)");
            } else if (isStringLike(t)) {
                w.line("return appendString(b, v)");
            } else if (isSignedPrim(t->prim)) {
                w.line("return appendZigzag(b, int64(v))");
            } else {
                w.line("return appendVarint(b, uint64(v))");
            }
            break;
        case TypeKind::Builtin:
            if (isStringLike(t)) {
                w.line(t->builtin == Builtin::LocString ? "return appendString(b, string(v))" : "return appendString(b, v)");
            } else if (tupleSize(t->builtin) != 0) {
                w.line(std::format("return {}(b, v[:])", tupleIsF64(t->builtin) ? "appendF64Tuple" : "appendF32Tuple"));
            } else if (t->builtin == Builtin::Guid) {
                w.line("return appendBytes(b, v[:])");
            } else if (t->builtin == Builtin::EntityId) {
                w.line("return appendFixed64(b, uint64(v))");
            } else if (t->builtin == Builtin::Duration) {
                w.line("return appendZigzag(b, int64(v))");
            } else if (t->builtin == Builtin::TagSet) {
                w.line("b, mk := beginLen(b)");
                w.open("for _, tag := range sortedSet([]string(v)) { // canonical: sorted, unique (like refl::TagSet)");
                w.line("b = appendTag(b, 1, wireLen)");
                w.line("b = appendString(b, tag)");
                w.close();
                w.line("return endLen(b, mk)");
            } else {
                w.line("return appendVarint(b, uint64(v))");
            }
            break;
        case TypeKind::Enum:
            w.line(isSignedPrim(t->decl->underlying) ? "return appendZigzag(b, int64(v))" : "return appendVarint(b, uint64(v))");
            break;
        case TypeKind::Flags: w.line("return appendVarint(b, uint64(v))"); break;
        case TypeKind::Struct:
            w.line("b, mk := beginLen(b)");
            w.line("b = v.appendFields(b)");
            w.line("return endLen(b, mk)");
            break;
        case TypeKind::Variant: w.line("return v.appendValue(b)"); break;
        case TypeKind::RecordRef: w.line("return appendFixed64(b, uint64(v))"); break;
        case TypeKind::AssetRef: w.line("return appendBytes(b, v[:])"); break;
        default: w.line("return b"); break;
        }
        w.close();
        w.line();
        // decv
        w.open(std::format("func decv_{}(r *wireReader, wt int, v *{}) error {{", n, T));
        switch (t->kind) {
        case TypeKind::Prim:
            if (t->prim == Prim::Bool) {
                w.line("x, err := readVarintField(r, wt, \"bool\")");
                w.line("*v = x != 0");
                w.line("return err");
            } else if (t->prim == Prim::F32) {
                w.line("x, err := readF32(r, wt)");
                w.line("*v = x");
                w.line("return err");
            } else if (t->prim == Prim::F64) {
                w.line("x, err := readF64(r, wt)");
                w.line("*v = x");
                w.line("return err");
            } else if (isStringLike(t)) {
                w.line("x, err := readStringField(r, wt)");
                w.line("*v = x");
                w.line("return err");
            } else if (isSignedPrim(t->prim)) {
                w.line(std::format("x, err := readSigned(r, wt, {})", signedRange(t->prim)));
                w.line(std::format("*v = {}(x)", T));
                w.line("return err");
            } else {
                w.line(std::format("x, err := readUnsigned(r, wt, {})", unsignedMax(t->prim)));
                w.line(std::format("*v = {}(x)", T));
                w.line("return err");
            }
            break;
        case TypeKind::Builtin:
            if (isStringLike(t)) {
                w.line("x, err := readStringField(r, wt)");
                w.line(t->builtin == Builtin::LocString ? "*v = LocString(x)" : "*v = x");
                w.line("return err");
            } else if (tupleSize(t->builtin) != 0) {
                w.line(std::format("return {}(r, wt, v[:])", tupleIsF64(t->builtin) ? "readF64Tuple" : "readF32Tuple"));
            } else if (t->builtin == Builtin::Guid) {
                w.line("x, err := readGuid(r, wt)");
                w.line("*v = x");
                w.line("return err");
            } else if (t->builtin == Builtin::EntityId) {
                w.line("x, err := readFixed64Field(r, wt, \"EntityId\")");
                w.line("*v = EntityId(x)");
                w.line("return err");
            } else if (t->builtin == Builtin::NetHandle) {
                w.line("x, err := readUnsigned(r, wt, math.MaxUint32)");
                w.line("*v = NetHandle(x)");
                w.line("return err");
            } else if (t->builtin == Builtin::Tick) {
                w.line("x, err := readUnsigned(r, wt, math.MaxUint64)");
                w.line("*v = x");
                w.line("return err");
            } else if (t->builtin == Builtin::Duration) {
                w.line("x, err := readSigned(r, wt, math.MinInt64, math.MaxInt64)");
                w.line("*v = Duration(x)");
                w.line("return err");
            } else if (t->builtin == Builtin::TagSet) {
                w.open("if wt != wireLen {");
                w.line("return wireMismatch(\"TagSet\", wt, wireLen)");
                w.close();
                w.line("sub, err := r.message()");
                w.open("if err != nil {");
                w.line("return err");
                w.close();
                w.open("for !sub.atEnd() {");
                w.line("id, wt2, err := sub.tag()");
                w.open("if err != nil {");
                w.line("return err");
                w.close();
                w.open("if id == 1 && wt2 == wireLen {");
                w.line("s, err := readStringField(sub, wt2)");
                w.open("if err != nil {");
                w.line("return err");
                w.close();
                w.line("*v = tagSetAdd(*v, s)");
                w.close("} else if err := sub.skip(wt2); err != nil {");
                w.indent();
                w.line("return err");
                w.close();
                w.close();
                w.line("return nil");
            }
            break;
        case TypeKind::Enum:
            if (isSignedPrim(t->decl->underlying)) {
                w.line(std::format("x, err := readSigned(r, wt, {})", signedRange(t->decl->underlying)));
            } else {
                w.line(std::format("x, err := readUnsigned(r, wt, {})", unsignedMax(t->decl->underlying)));
            }
            w.line(std::format("*v = {}(x)", T));
            w.line("return err");
            break;
        case TypeKind::Flags:
            w.line(std::format("x, err := readUnsigned(r, wt, {})", unsignedMax(t->decl->underlying)));
            w.line(std::format("*v = {}(x)", T));
            w.line("return err");
            break;
        case TypeKind::Struct:
            w.open("if wt != wireLen {");
            w.line(std::format("return wireMismatch(\"{}\", wt, wireLen)", t->decl->name));
            w.close();
            w.line("sub, err := r.message()");
            w.open("if err != nil {");
            w.line("return err");
            w.close();
            w.line("return v.readFields(sub)");
            break;
        case TypeKind::Variant: w.line("return v.readValue(r, wt)"); break;
        case TypeKind::RecordRef:
            w.line("x, err := readFixed64Field(r, wt, \"record reference\")");
            w.line(std::format("*v = {}(x)", T));
            w.line("return err");
            break;
        case TypeKind::AssetRef:
            w.line("x, err := readGuid(r, wt)");
            w.line("*v = AssetRef(x)");
            w.line("return err");
            break;
        default: w.line("return nil"); break;
        }
        w.close();
        w.line();
    }

    std::string encElement(const Type* e, const std::string& v) {
        if (wrapped(e)) {
            return std::format("{{ var mk int; b, mk = beginLen(b); b = enc_{}(b, 1, {}); b = endLen(b, mk) }}", id(e), v);
        }
        return std::format("b = encv_{}(b, {})", id(e), v);
    }

    void emitDecElement(CodeWriter& w, const Type* e, const std::string& reader, const std::string& wt, const std::string& target) {
        if (!wrapped(e)) {
            w.open(std::format("if err := decv_{}({}, {}, {}); err != nil {{", id(e), reader, wt, target));
            w.line("return err");
            w.close();
            return;
        }
        w.open(std::format("if {} != wireLen {{", wt));
        w.line(std::format("return wireMismatch(\"wrapped element\", {}, wireLen)", wt));
        w.close();
        w.line(std::format("esub, err := {}.message()", reader));
        w.open("if err != nil {");
        w.line("return err");
        w.close();
        w.open("for !esub.atEnd() {");
        w.line("eid, ewt, err := esub.tag()");
        w.open("if err != nil {");
        w.line("return err");
        w.close();
        w.open("if eid == 1 {");
        w.open(std::format("if err := dec_{}(esub, ewt, {}); err != nil {{", id(e), target));
        w.line("return err");
        w.close();
        w.close("} else if err := esub.skip(ewt); err != nil {");
        w.indent();
        w.line("return err");
        w.close();
        w.close();
    }

    std::string sortedExpr(const Type* keyType, const std::string& v, bool map) {
        if (keyType->kind == TypeKind::Builtin && keyType->builtin == Builtin::Guid) return (map ? "sortedGuidKeys(" : "sortedGuidSet(") + v + ")";
        return (map ? "sortedKeys(" : "sortedSet(") + v + ")";
    }

    void emitFieldCodec(CodeWriter& w, const Type* t, const std::string& n, const std::string& T) {
        // enc: every occurrence of field `id`
        w.open(std::format("func enc_{}(b []byte, id uint32, v {}) []byte {{", n, T));
        switch (t->kind) {
        case TypeKind::List:
        case TypeKind::Set: {
            const Type* e = t->element;
            const std::string items = t->kind == TypeKind::Set ? sortedExpr(e, "v", false) : "v";
            w.open("if len(v) == 0 {");
            w.line("return b");
            w.close();
            if (packable(e)) {
                w.line("b = appendTag(b, id, wireLen)");
                w.line("b, mk := beginLen(b)");
                w.open(std::format("for _, e := range {} {{", items));
                w.line(encElement(e, "e"));
                w.close();
                w.line("return endLen(b, mk)");
            } else {
                w.open(std::format("for _, e := range {} {{", items));
                w.line(std::format("b = appendTag(b, id, {})", elementWire(e)));
                w.line(encElement(e, "e"));
                w.close();
                w.line("return b");
            }
            break;
        }
        case TypeKind::Array: {
            const Type* e = t->element;
            w.line("b = appendTag(b, id, wireLen)");
            w.line("b, mk := beginLen(b)");
            w.open("for _, e := range v {");
            if (!packable(e)) w.line(std::format("b = appendTag(b, 1, {})", elementWire(e)));
            w.line(encElement(e, "e"));
            w.close();
            w.line("return endLen(b, mk)");
            break;
        }
        case TypeKind::KeyedList:
            w.open("for _, e := range v {");
            w.line("b = appendTag(b, id, wireLen)");
            w.line("var mk int");
            w.line("b, mk = beginLen(b)");
            w.line("b = appendTag(b, 1, wireLen)");
            w.line("b = appendBytes(b, e.Key[:])");
            w.line(std::format("b = enc_{}(b, 2, e.Value)", id(t->element)));
            w.line("b = endLen(b, mk)");
            w.close();
            w.line("return b");
            break;
        case TypeKind::Map:
            w.open(std::format("for _, k := range {} {{", sortedExpr(t->key, "v", true)));
            w.line("b = appendTag(b, id, wireLen)");
            w.line("var mk int");
            w.line("b, mk = beginLen(b)");
            w.line(std::format("b = enc_{}(b, 1, k)", id(t->key)));
            w.line(std::format("b = enc_{}(b, 2, v[k])", id(t->element)));
            w.line("b = endLen(b, mk)");
            w.close();
            w.line("return b");
            break;
        case TypeKind::Optional:
            w.open("if v == nil {");
            w.line("return b");
            w.close();
            w.line(std::format("return enc_{}(b, id, *v)", id(t->element)));
            break;
        default:
            w.line(std::format("b = appendTag(b, id, {})", wire(t)));
            w.line(std::format("return encv_{}(b, v)", n));
            break;
        }
        w.close();
        w.line();
        // dec: one occurrence
        w.open(std::format("func dec_{}(r *wireReader, wt int, v *{}) error {{", n, T));
        switch (t->kind) {
        case TypeKind::List:
        case TypeKind::Set: {
            const Type* e = t->element;
            if (packable(e)) {
                w.open("if wt == wireLen {");
                w.line("p, err := r.lenBytes()");
                w.open("if err != nil {");
                w.line("return err");
                w.close();
                w.line("sub := &wireReader{data: p, depth: r.depth}");
                w.open("for !sub.atEnd() {");
                w.line(std::format("e := zero_{}()", id(e)));
                w.open(std::format("if err := decv_{}(sub, {}, &e); err != nil {{", id(e), wire(e)));
                w.line("return err");
                w.close();
                w.line("*v = append(*v, e)");
                w.close();
                w.line("return nil");
                w.close();
            }
            w.line(std::format("e := zero_{}()", id(e)));
            emitDecElement(w, e, "r", "wt", "&e");
            w.line("*v = append(*v, e)");
            w.line("return nil");
            break;
        }
        case TypeKind::Array: {
            const Type* e = t->element;
            w.open("if wt != wireLen {");
            w.line("return wireMismatch(\"array\", wt, wireLen)");
            w.close();
            w.line("count := 0");
            if (packable(e)) {
                w.line("p, err := r.lenBytes()");
                w.open("if err != nil {");
                w.line("return err");
                w.close();
                w.line("sub := &wireReader{data: p, depth: r.depth}");
                w.open("for !sub.atEnd() {");
                w.open(std::format("if count >= {} {{", t->arraySize));
                w.line(std::format("return fmt.Errorf(\"%w: array: more than {} elements\", ErrCorrupt)", t->arraySize));
                w.close();
                w.line(std::format("e := zero_{}()", id(e)));
                w.open(std::format("if err := decv_{}(sub, {}, &e); err != nil {{", id(e), wire(e)));
                w.line("return err");
                w.close();
                w.line("v[count] = e");
                w.line("count++");
                w.close();
                w.line("return nil");
            } else {
                w.line("sub, err := r.message()");
                w.open("if err != nil {");
                w.line("return err");
                w.close();
                w.open("for !sub.atEnd() {");
                w.line("aid, awt, err := sub.tag()");
                w.open("if err != nil {");
                w.line("return err");
                w.close();
                w.open("if aid != 1 {");
                w.open("if err := sub.skip(awt); err != nil {");
                w.line("return err");
                w.close();
                w.line("continue");
                w.close();
                w.open(std::format("if count >= {} {{", t->arraySize));
                w.line(std::format("return fmt.Errorf(\"%w: array: more than {} elements\", ErrCorrupt)", t->arraySize));
                w.close();
                w.line(std::format("e := zero_{}()", id(e)));
                emitDecElement(w, e, "sub", "awt", "&e");
                w.line("v[count] = e");
                w.line("count++");
                w.close();
                w.line("return nil");
            }
            break;
        }
        case TypeKind::KeyedList:
            w.open("if wt != wireLen {");
            w.line("return wireMismatch(\"keyed list entry\", wt, wireLen)");
            w.close();
            w.line("sub, err := r.message()");
            w.open("if err != nil {");
            w.line("return err");
            w.close();
            w.line("var key Guid");
            w.line(std::format("val := zero_{}()", id(t->element)));
            w.open("for !sub.atEnd() {");
            w.line("fid, fwt, err := sub.tag()");
            w.open("if err != nil {");
            w.line("return err");
            w.close();
            w.open("switch fid {");
            w.line("case 1:");
            w.indent();
            w.open("if key, err = readGuid(sub, fwt); err != nil {");
            w.line("return err");
            w.close();
            w.dedent();
            w.line("case 2:");
            w.indent();
            w.open(std::format("if err := dec_{}(sub, fwt, &val); err != nil {{", id(t->element)));
            w.line("return err");
            w.close();
            w.dedent();
            w.line("default:");
            w.indent();
            w.open("if err := sub.skip(fwt); err != nil {");
            w.line("return err");
            w.close();
            w.dedent();
            w.close();
            w.close();
            w.open("if key == (Guid{}) {");
            w.line("return fmt.Errorf(\"%w: keyed list entry without key\", ErrCorrupt)");
            w.close();
            w.open("for _, e := range *v {");
            w.open("if e.Key == key {");
            w.line("return fmt.Errorf(\"%w: duplicate keyed list key %s\", ErrCorrupt, key)");
            w.close();
            w.close();
            w.line(std::format("*v = append(*v, Keyed[{}]{{Key: key, Value: val}})", goTypeName(t->element)));
            w.line("return nil");
            break;
        case TypeKind::Map:
            w.open("if wt != wireLen {");
            w.line("return wireMismatch(\"map entry\", wt, wireLen)");
            w.close();
            w.line("sub, err := r.message()");
            w.open("if err != nil {");
            w.line("return err");
            w.close();
            w.line(std::format("key := zero_{}()", id(t->key)));
            w.line(std::format("val := zero_{}()", id(t->element)));
            w.open("for !sub.atEnd() {");
            w.line("fid, fwt, err := sub.tag()");
            w.open("if err != nil {");
            w.line("return err");
            w.close();
            w.open("switch fid {");
            w.line("case 1:");
            w.indent();
            w.open(std::format("if err := dec_{}(sub, fwt, &key); err != nil {{", id(t->key)));
            w.line("return err");
            w.close();
            w.dedent();
            w.line("case 2:");
            w.indent();
            w.open(std::format("if err := dec_{}(sub, fwt, &val); err != nil {{", id(t->element)));
            w.line("return err");
            w.close();
            w.dedent();
            w.line("default:");
            w.indent();
            w.open("if err := sub.skip(fwt); err != nil {");
            w.line("return err");
            w.close();
            w.dedent();
            w.close();
            w.close();
            w.open("if *v == nil {");
            w.line(std::format("*v = {}{{}}", T));
            w.close();
            w.line("(*v)[key] = val");
            w.line("return nil");
            break;
        case TypeKind::Optional:
            w.open("if *v == nil {");
            w.line(std::format("x := zero_{}()", id(t->element)));
            w.line("*v = &x");
            w.close();
            w.line(std::format("return dec_{}(r, wt, *v)", id(t->element)));
            break;
        default: w.line(std::format("return decv_{}(r, wt, v)", n)); break;
        }
        w.close();
        w.line();
    }

    std::map<std::string, std::string> m_ids;
    std::vector<const Type*> m_pending;
};

class GoGenerator {
public:
    GoGenerator(const Schema& s, const CompileOptions& o, DiagnosticEngine& d) : S(s), O(o), D(d) {}

    std::vector<OutputFile> run() {
        std::vector<OutputFile> out;
        m_pkg = O.goPackage;
        if (m_pkg.empty()) {
            for (const auto& f : S.files) {
                if (f->generate) {
                    m_pkg = splitDots(f->ast.package).back();
                    break;
                }
            }
        }
        static const std::set<std::string> kGoKeywords = {"break", "case", "chan", "const", "continue", "default", "defer", "else",
                                                          "fallthrough", "for", "func", "go", "goto", "if", "import", "interface",
                                                          "map", "package", "range", "return", "select", "struct", "switch", "type", "var"};
        const bool validPkg = !m_pkg.empty() && !(m_pkg[0] >= '0' && m_pkg[0] <= '9') &&
                              std::all_of(m_pkg.begin(), m_pkg.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
        if (!validPkg || kGoKeywords.contains(m_pkg)) {
            D.error({}, std::format("'{}' is not a valid Go package name (pass --go-package / GO_PACKAGE)", m_pkg));
            return out;
        }
        checkGoNames();
        for (const auto& fp : S.files) {
            if (!fp->generate) continue;
            out.push_back(OutputFile{path(fp->stem + ".go"), file(fp.get())});
        }
        std::string runtime(reinterpret_cast<const char*>(kGoRuntimeSource));
        const std::string placeholder = "package heliosruntime";
        runtime.replace(runtime.find(placeholder), placeholder.size(), "package " + m_pkg);
        out.push_back(OutputFile{path("helios_runtime.go"), runtime});
        out.push_back(OutputFile{path("helios_schema_test.go"), generateGoTest(S, m_pkg)});
        // Codecs last: files and tests request helpers while being generated.
        CodeWriter w(1, '\t');
        w.line("// Code generated by helios-schemac. DO NOT EDIT.");
        w.line("//");
        w.line("// Tagged wire helpers per schema type (enc_ = field, encv_ = value, dec_/decv_ = decode,");
        w.line("// def_ = implicit-default test, zero_ = implicit default value).");
        w.line();
        w.line("package " + m_pkg);
        w.line();
        w.line("import (");
        w.line("\t\"fmt\"");
        w.line("\t\"math\"");
        w.line(")");
        w.line();
        w.line("var _ = fmt.Errorf");
        w.line("var _ = math.MaxInt8");
        w.line();
        w.raw(m_codecs.emitAll());
        out.push_back(OutputFile{path("helios_codecs.go"), w.take()});
        return out;
    }

private:
    std::string path(const std::string& name) const { return O.goOut.empty() || O.goOut == "." ? name : O.goOut + "/" + name; }

    void checkGoNames() {
        // Exported names of helios_runtime.go, which is copied into every generated package.
        static const std::set<std::string> kRuntimeNames = {
            "AssetRef", "DQuat", "DVec3", "Duration", "EntityId", "Guid", "HxlExpr", "Keyed", "LocString", "NetHandle",
            "ParseDuration", "ParseGuid", "Quat", "RGBA", "RecordId", "TagQuery", "TagSet", "Tick", "U8List", "Vec2", "Vec3", "Vec4", "WorldPos"};
        std::map<std::string, const Decl*> names;
        for (const Decl* d : S.decls) {
            if (!d->emitted) continue;
            if (!d->isLockable() && d->kind != DeclKind::Alias && d->kind != DeclKind::Const) continue;
            if (d->kind == DeclKind::Alias && d->aliasTarget && d->aliasTarget->decl && d->aliasTarget->decl->qualifiedName == d->qualifiedName) continue;
            if (kRuntimeNames.contains(d->goName)) {
                D.error(d->loc, std::format("Go name '{}' of '{}' collides with the Go schema runtime type '{}' (helios_runtime.go); rename the type",
                                            d->goName, d->qualifiedName, d->goName));
            }
            if (auto [it, ok] = names.emplace(d->goName, d); !ok) {
                D.error(d->loc, std::format("Go name '{}' of '{}' collides with '{}' (Go output is one package)", d->goName, d->qualifiedName,
                                            it->second->qualifiedName));
            }
            // Methods of generated Go structs / variants: a field or alternative of that name would not compile.
            static const std::set<std::string> kMethodNames = {"MarshalHelios", "UnmarshalHelios", "MarshalJSON", "UnmarshalJSON"};
            if (d->isStructLike()) {
                std::map<std::string, const Field*> fields;
                for (const Field& f : d->fields) {
                    const std::string goField = goFieldName(f.name);
                    if (goField.empty() || kMethodNames.contains(goField)) {
                        D.error(f.loc, std::format("field '{}' cannot be generated in Go (its Go name '{}' is empty or a generated method); rename it",
                                                   f.name, goField));
                    }
                    if (auto [it, ok] = fields.emplace(goField, &f); !ok)
                        D.error(f.loc, std::format("fields '{}' and '{}' map to the same Go field name", it->second->name, f.name));
                }
            }
            if (d->kind == DeclKind::Variant) {
                for (const Alternative& a : d->alternatives) {
                    if (kMethodNames.contains(a.name))
                        D.error(a.loc, std::format("alternative '{}' of '{}' collides with a generated Go method; rename it", a.name, d->qualifiedName));
                }
            }
        }
        for (const Decl* d : S.decls) {
            if (!d->emitted || !d->isStructLike()) continue;
            for (const Field& f : d->fields) checkTypeInPackage(f.type, f.loc);
        }
    }

    void checkTypeInPackage(const Type* t, SourceLoc loc) {
        if (!t) return;
        if (t->decl && !t->decl->emitted) {
            D.error(loc, std::format("Go output needs '{}' in the same package; add its schema file to the Go generation inputs",
                                     t->decl->qualifiedName));
        }
        checkTypeInPackage(t->element, loc);
        checkTypeInPackage(t->key, loc);
    }

    void emitDoc(CodeWriter& w, const std::string& doc) {
        if (doc.empty()) return;
        usize start = 0;
        while (true) {
            const usize nl = doc.find('\n', start);
            const std::string line = doc.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            w.line(line.empty() ? "//" : "// " + line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }

    std::string file(const SourceFile* f) {
        CodeWriter w(1, '\t');
        w.line(std::format("// Code generated by helios-schemac from {}. DO NOT EDIT.", f->logicalPath));
        w.line();
        w.line("package " + m_pkg);
        w.line();
        w.line("import (");
        w.line("\t\"encoding/json\"");
        w.line("\t\"fmt\"");
        w.line("\t\"math\"");
        w.line(")");
        w.line();
        w.line("var _ = json.Marshal");
        w.line("var _ = fmt.Errorf");
        w.line("var _ = math.MaxInt8");
        w.line();
        // Top-level declarations; nested types are emitted by their owners (emitStruct).
        for (const Decl* d : S.declsOfFile(f)) {
            if (!d->outer) emitDecl(w, d);
        }
        return w.take();
    }

    void emitDecl(CodeWriter& w, const Decl* d) {
        switch (d->kind) {
        case DeclKind::Enum:
        case DeclKind::Flags: emitEnum(w, d); break;
        case DeclKind::Variant: emitVariant(w, d); break;
        case DeclKind::Const: emitConst(w, d); break;
        case DeclKind::Alias:
            if (d->aliasTarget && !(d->aliasTarget->decl && d->aliasTarget->decl->qualifiedName == d->qualifiedName)) {
                emitDoc(w, d->doc);
                w.line(std::format("type {} = {}", d->goName, goTypeName(d->aliasTarget)));
                w.line();
            }
            break;
        case DeclKind::Service:
        case DeclKind::Formula: break;
        default:
            if (d->isStructLike()) emitStruct(w, d);
            break;
        }
    }

    void emitConst(CodeWriter& w, const Decl* d) {
        if (!d->constType || d->constValue.kind == Value::Kind::None) return;
        emitDoc(w, d->doc);
        const Type* t = d->constType;
        if (t->kind == TypeKind::Prim && (t->prim == Prim::F32 || t->prim == Prim::F64)) {
            w.line(std::format("const {} {} = {}", d->goName, goTypeName(t), formatF64(d->constValue.f)));
        } else {
            w.line(std::format("const {} = {}", d->goName, goValueLiteral(d->constValue, t)));
        }
        w.line();
    }

    void emitEnum(CodeWriter& w, const Decl* d) {
        const std::string T = d->goName;
        emitDoc(w, d->doc);
        w.line(std::format("type {} {}", T, goPrim(d->underlying)));
        w.line();
        w.line("const (");
        w.indent();
        for (const EnumVal& v : d->values) {
            emitDoc(w, v.doc);
            w.line(std::format("{} {} = {}", goEnumConst(d, v.name), T, v.value));
        }
        w.dedent();
        w.line(")");
        w.line();
        std::string names;
        for (const EnumVal& v : d->values) names += std::format("{{{}, {}}}, ", goQuote(v.name), v.value);
        const std::string table = "names" + T;
        w.line(std::format("var {} = []enumName{{{}}}", table, names));
        w.line();
        if (d->kind == DeclKind::Flags) {
            w.line(std::format("func (v {}) MarshalJSON() ([]byte, error) {{ return flagsToJSON({}, uint64(v)) }}", T, table));
            w.open(std::format("func (v *{}) UnmarshalJSON(b []byte) error {{", T));
            w.line(std::format("x, err := flagsFromJSON({}, b)", table));
            w.line(std::format("*v = {}(x)", T));
            w.line("return err");
            w.close();
        } else {
            w.line(std::format("func (v {}) MarshalJSON() ([]byte, error) {{ return enumToJSON({}, int64(v)) }}", T, table));
            w.open(std::format("func (v *{}) UnmarshalJSON(b []byte) error {{", T));
            w.line(std::format("x, err := enumFromJSON({}, b)", table));
            w.line(std::format("*v = {}(x)", T));
            w.line("return err");
            w.close();
            w.line(std::format("func (v {}) MarshalText() ([]byte, error) {{ return enumToText({}, int64(v)), nil }}", T, table));
            w.open(std::format("func (v *{}) UnmarshalText(b []byte) error {{", T));
            w.line(std::format("x, err := enumFromText({}, string(b))", table));
            w.line(std::format("*v = {}(x)", T));
            w.line("return err");
            w.close();
        }
        w.line();
    }

    std::string defaultVar(const Decl* d, const Field& f) { return "dflt" + d->goName + goFieldName(f.name); }

    void emitStruct(CodeWriter& w, const Decl* d) {
        const std::string T = d->goName;
        for (const Decl* n : d->nested) {
            if (n->kind != DeclKind::Alternative) emitDecl(w, n);
        }
        emitDoc(w, d->doc);
        if (d->kind == DeclKind::Rpc)
            w.line(std::format("// {} holds the arguments of rpc {}{}.", T, d->service ? d->service->name + "." : "", d->rpcName.empty() ? d->name : d->rpcName));
        w.open(std::format("type {} struct {{", T));
        for (const Field& f : d->fields) {
            emitDoc(w, f.doc);
            const bool omit = f.type->kind == TypeKind::List || f.type->kind == TypeKind::Set || f.type->kind == TypeKind::KeyedList ||
                              f.type->kind == TypeKind::Map || f.type->kind == TypeKind::Optional;
            w.line(std::format("{} {} `json:\"{}{}\"`", goFieldName(f.name), goTypeName(f.type), f.name, omit ? ",omitempty" : ""));
        }
        w.close();
        w.line();
        for (const Field& f : d->fields) {
            if (f.defaultValue) w.line(std::format("var {} = {}", defaultVar(d, f), goValueLiteral(*f.defaultValue, f.type)));
        }
        // Constructor with schema defaults.
        w.line(std::format("// New{} returns a {} with every field at its schema default.", T, T));
        w.open(std::format("func New{}() {} {{", T, T));
        std::string inits;
        for (const Field& f : d->fields) {
            if (f.defaultValue) {
                inits += std::format("{}: {}, ", goFieldName(f.name), defaultVar(d, f));
            } else if (f.type->kind != TypeKind::List && f.type->kind != TypeKind::Set && f.type->kind != TypeKind::KeyedList &&
                       f.type->kind != TypeKind::Map && f.type->kind != TypeKind::Optional) {
                inits += std::format("{}: zero_{}(), ", goFieldName(f.name), m_codecs.id(f.type));
            }
        }
        w.line(std::format("return {}{{{}}}", T, inits));
        w.close();
        w.line();
        // isDefault
        w.open(std::format("func (m *{}) isDefault() bool {{", T));
        std::string expr;
        for (const Field& f : d->fields) expr += (expr.empty() ? "" : " &&\n\t\t") + defaultCheck(d, f);
        w.line("return " + (expr.empty() ? std::string("true") : expr));
        w.close();
        w.line();
        // appendFields
        w.open(std::format("func (m *{}) appendFields(b []byte) []byte {{", T));
        for (const Field& f : d->fields) {
            w.open(std::format("if !({}) {{", defaultCheck(d, f)));
            w.line(std::format("b = enc_{}(b, {}, m.{})", m_codecs.id(f.type), f.id, goFieldName(f.name)));
            w.close();
        }
        w.line("return b");
        w.close();
        w.line();
        // readFields
        w.open(std::format("func (m *{}) readFields(r *wireReader) error {{", T));
        w.open("for !r.atEnd() {");
        w.line("id, wt, err := r.tag()");
        w.open("if err != nil {");
        w.line("return err");
        w.close();
        w.open("switch id {");
        for (const Field& f : d->fields) {
            w.line(std::format("case {}:", f.id));
            w.indent();
            w.line(std::format("err = dec_{}(r, wt, &m.{})", m_codecs.id(f.type), goFieldName(f.name)));
            w.dedent();
        }
        w.line("default:");
        w.indent();
        w.line("err = r.skip(wt)");
        w.dedent();
        w.close();
        w.open("if err != nil {");
        w.line("return err");
        w.close();
        w.close();
        w.line("return nil");
        w.close();
        w.line();
        w.line(std::format("// MarshalHelios encodes the tagged binary message (identical bytes to the C++ codec)."));
        w.line(std::format("func (m *{}) MarshalHelios() []byte {{ return m.appendFields(nil) }}", T));
        w.line();
        w.line(std::format("// UnmarshalHelios decodes a tagged message; missing fields take their schema defaults."));
        w.open(std::format("func (m *{}) UnmarshalHelios(b []byte) error {{", T));
        w.line(std::format("*m = New{}()", T));
        w.line("return m.readFields(newWireReader(b))");
        w.close();
        w.line();
        w.line("// UnmarshalJSON reads JSON (e.g. canonical JSONC written by the C++ codec); missing fields keep defaults.");
        w.open(std::format("func (m *{}) UnmarshalJSON(b []byte) error {{", T));
        std::string renames;
        for (const Field& f : d->fields) {
            for (const std::string& old : f.was) renames += std::format("{}: {}, ", goQuote(old), goQuote(f.name));
        }
        if (!renames.empty()) {
            // @was: readers accept the old key (02 §3.4).
            w.line(std::format("b, err := renameJSONKeys(b, map[string]string{{{}}})", renames.substr(0, renames.size() - 2)));
            w.open("if err != nil {");
            w.line("return err");
            w.close();
        }
        w.line(std::format("type plain {}", T));
        w.line(std::format("p := plain(New{}())", T));
        w.open("if err := json.Unmarshal(b, &p); err != nil {");
        w.line("return err");
        w.close();
        w.line(std::format("*m = {}(p)", T));
        w.line("return nil");
        w.close();
        w.line();
        if (d->kind == DeclKind::Record) {
            w.line(std::format("// {} references a {} record by RecordId.", goRecordRefName(d), T));
            w.line(std::format("type {} uint64", goRecordRefName(d)));
            w.line();
        }
    }

    std::string defaultCheck(const Decl* d, const Field& f) {
        const std::string m = "m." + goFieldName(f.name);
        if (!f.defaultValue) return std::format("def_{}({})", m_codecs.id(f.type), m);
        const Type* t = f.type;
        const std::string dv = defaultVar(d, f);
        if (t->kind == TypeKind::Prim && t->prim == Prim::F32) return std::format("f32Bits({}, {})", m, dv);
        if (t->kind == TypeKind::Prim && t->prim == Prim::F64) return std::format("f64Bits({}, {})", m, dv);
        if (t->kind == TypeKind::Builtin && tupleSize(t->builtin) != 0)
            return std::format("{}({}[:], {}[:])", tupleIsF64(t->builtin) ? "f64TupleBits" : "f32TupleBits", m, dv);
        if (t->kind == TypeKind::Array && t->element->kind == TypeKind::Prim && isFloatPrim(t->element->prim)) {
            return std::format("{}({}[:], {}[:])", t->element->prim == Prim::F64 ? "f64TupleBits" : "f32TupleBits", m, dv);
        }
        return std::format("{} == {}", m, dv);
    }

    void emitVariant(CodeWriter& w, const Decl* v) {
        const std::string T = v->goName;
        for (const Alternative& a : v->alternatives) emitStruct(w, a.type);
        emitDoc(w, v->doc);
        w.line(std::format("// {} is a variant: exactly one alternative is set (none set = the first alternative's default).", T));
        w.open(std::format("type {} struct {{", T));
        for (const Alternative& a : v->alternatives) w.line(std::format("{} *{}", a.name, a.type->goName));
        w.close();
        w.line();
        const Alternative& first = v->alternatives.front();
        w.open(std::format("func New{}() {} {{", T, T));
        w.line(std::format("x := New{}()", first.type->goName));
        w.line(std::format("return {}{{{}: &x}}", T, first.name));
        w.close();
        w.line();
        w.open(std::format("func (m *{}) index() int {{", T));
        w.open("switch {");
        for (usize i = 0; i < v->alternatives.size(); ++i) {
            w.line(std::format("case m.{} != nil:", v->alternatives[i].name));
            w.line(std::format("\treturn {}", i));
        }
        w.close();
        w.line("return -1");
        w.close();
        w.line();
        w.open(std::format("func (m *{}) isDefault() bool {{", T));
        w.open("switch m.index() {");
        w.line("case -1:");
        w.line("\treturn true");
        w.line("case 0:");
        w.line(std::format("\treturn m.{}.isDefault()", first.name));
        w.close();
        w.line("return false");
        w.close();
        w.line();
        w.open(std::format("func (m *{}) appendValue(b []byte) []byte {{", T));
        w.line("b, mk := beginLen(b)");
        w.open("switch m.index() {");
        w.line("case -1:");
        w.line(std::format("\tb = enc_{}(b, {}, New{}())", m_codecs.id(declTypeOf(first.type)), first.id, first.type->goName));
        for (usize i = 0; i < v->alternatives.size(); ++i) {
            const Alternative& a = v->alternatives[i];
            w.line(std::format("case {}:", i));
            w.line(std::format("\tb = enc_{}(b, {}, *m.{})", m_codecs.id(declTypeOf(a.type)), a.id, a.name));
        }
        w.close();
        w.line("return endLen(b, mk)");
        w.close();
        w.line();
        w.open(std::format("func (m *{}) readValue(r *wireReader, wt int) error {{", T));
        w.open("if wt != wireLen {");
        w.line(std::format("return wireMismatch(\"{}\", wt, wireLen)", v->name));
        w.close();
        w.line("sub, err := r.message()");
        w.open("if err != nil {");
        w.line("return err");
        w.close();
        w.open("for !sub.atEnd() {");
        w.line("id, awt, err := sub.tag()");
        w.open("if err != nil {");
        w.line("return err");
        w.close();
        w.open("switch id {");
        for (const Alternative& a : v->alternatives) {
            w.line(std::format("case {}:", a.id));
            w.indent();
            w.open(std::format("if m.{} == nil {{", a.name));
            w.line(std::format("x := New{}()", a.type->goName));
            w.line(std::format("*m = {}{{{}: &x}}", T, a.name));
            w.close();
            w.line(std::format("err = dec_{}(sub, awt, m.{})", m_codecs.id(declTypeOf(a.type)), a.name));
            w.dedent();
        }
        w.line("default:");
        w.line("\terr = sub.skip(awt)");
        w.close();
        w.open("if err != nil {");
        w.line("return err");
        w.close();
        w.close();
        w.line("return nil");
        w.close();
        w.line();
        // JSON: unit alternatives as "Name", others as {"Name": {...}}.
        w.open(std::format("func (m {}) MarshalJSON() ([]byte, error) {{", T));
        w.open("switch m.index() {");
        for (usize i = 0; i < v->alternatives.size(); ++i) {
            const Alternative& a = v->alternatives[i];
            w.line(std::format("case {}:", i));
            if (a.type->fields.empty()) {
                w.line(std::format("\treturn json.Marshal({})", goQuote(a.name)));
            } else {
                w.line(std::format("\treturn json.Marshal(map[string]any{{{}: m.{}}})", goQuote(a.name), a.name));
            }
        }
        w.close();
        if (first.type->fields.empty()) {
            w.line(std::format("return json.Marshal({})", goQuote(first.name)));
        } else {
            w.line(std::format("return json.Marshal(map[string]any{{{}: New{}()}})", goQuote(first.name), first.type->goName));
        }
        w.close();
        w.line();
        w.open(std::format("func (m *{}) UnmarshalJSON(b []byte) error {{", T));
        w.line("var name string");
        w.line("var body json.RawMessage");
        w.open("if err := json.Unmarshal(b, &name); err != nil {");
        w.line("var obj map[string]json.RawMessage");
        w.open("if err := json.Unmarshal(b, &obj); err != nil || len(obj) != 1 {");
        w.line(std::format("return fmt.Errorf(\"helios: {} must be an alternative name or {{\\\"Alternative\\\": {{...}}}}\")", v->qualifiedName));
        w.close();
        w.open("for k, v := range obj {");
        w.line("name, body = k, v");
        w.close();
        w.close();
        w.open("switch name {");
        for (const Alternative& a : v->alternatives) {
            w.line(std::format("case {}:", goQuote(a.name)));
            w.indent();
            w.line(std::format("x := New{}()", a.type->goName));
            w.open("if body != nil {");
            w.open("if err := json.Unmarshal(body, &x); err != nil {");
            w.line("return err");
            w.close();
            w.close();
            w.line(std::format("*m = {}{{{}: &x}}", T, a.name));
            w.line("return nil");
            w.dedent();
        }
        w.close();
        w.line(std::format("return fmt.Errorf(\"helios: unknown alternative %q of {}\", name)", v->qualifiedName));
        w.close();
        w.line();
    }

    const Type* declTypeOf(const Decl* d) { return S.typesBySignature.at(d->qualifiedName); }

    const Schema& S;
    const CompileOptions& O;
    DiagnosticEngine& D;
    std::string m_pkg;
    GoCodecs m_codecs;
};

} // namespace

std::vector<OutputFile> generateGo(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags) {
    GoGenerator g(schema, options, diags);
    return g.run();
}

} // namespace helios::schemac
