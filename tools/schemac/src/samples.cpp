#include "samples.h"

#include <algorithm>
#include <format>

#include "code_writer.h"
#include "compiler.h"
#include "generators.h"
#include "go_names.h"
#include "text.h"

namespace helios::schemac {

namespace {

u64 mix(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

struct Sample {
    enum class Kind : u8 { Scalar, RecordRef, AssetRef, TagSet, List, Set, Array, Optional, Map, KeyedList, Struct, Variant };
    Kind kind = Kind::Scalar;
    const Type* type = nullptr;
    Value value;
    u64 id = 0;     ///< RecordRef id
    u64 g1 = 0;     ///< AssetRef guid
    u64 g2 = 0;
    std::vector<std::string> tags;
    std::vector<Sample> items;
    std::vector<Sample> mapKeys; ///< map entries (parallel to mapValues; a pair<Sample, Sample> member
    std::vector<Sample> mapValues; ///< would need Sample to be complete)
    std::vector<std::pair<u64, u64>> keys;
    usize alt = 0;
    bool isNew = false; ///< struct at the depth limit: default-constructed
};

constexpr int kMaxDepth = 3;

class SampleBuilder {
public:
    Sample make(const Type* t, u64 seed, int depth, const std::string& label) {
        Sample s;
        s.type = t;
        switch (t->kind) {
        case TypeKind::Prim: s.value = prim(t->prim, seed, label); break;
        case TypeKind::Builtin: builtin(s, t->builtin, seed, label); break;
        case TypeKind::Enum: {
            const auto& values = t->decl->values;
            const EnumVal& v = values.size() > 1 ? values[1 + seed % (values.size() - 1)] : values.front();
            s.value.kind = Value::Kind::Enum;
            s.value.s = v.name;
            s.value.i = v.value;
            break;
        }
        case TypeKind::Flags: {
            u64 bits = 0;
            int taken = 0;
            for (const EnumVal& v : t->decl->values) {
                const u64 b = static_cast<u64>(v.value);
                if (b != 0 && (b & (b - 1)) == 0 && taken < 2 && ((seed >> taken) & 1) == 0) {
                    bits |= b;
                    ++taken;
                }
            }
            if (bits == 0) {
                for (const EnumVal& v : t->decl->values) bits |= static_cast<u64>(v.value) & (0 - static_cast<u64>(v.value));
            }
            s.value.kind = Value::Kind::Flags;
            s.value.u = bits;
            break;
        }
        case TypeKind::RecordRef:
            s.kind = Sample::Kind::RecordRef;
            s.id = (mix(seed) & ((1ull << 63) - 1)) | 1;
            break;
        case TypeKind::AssetRef:
            s.kind = Sample::Kind::AssetRef;
            s.g1 = mix(seed) | 1;
            s.g2 = mix(seed + 1);
            break;
        case TypeKind::List:
        case TypeKind::Set:
        case TypeKind::KeyedList: {
            s.kind = t->kind == TypeKind::List ? Sample::Kind::List : t->kind == TypeKind::Set ? Sample::Kind::Set : Sample::Kind::KeyedList;
            if (depth >= kMaxDepth) break;
            for (u64 i = 0; i < 2; ++i) {
                Sample e = make(t->element, mix(seed + 17 * (i + 1)), depth + 1, label);
                if (t->kind == TypeKind::Set) {
                    bool dup = false;
                    for (const Sample& x : s.items) dup = dup || keyText(x) == keyText(e);
                    if (dup) continue;
                }
                s.items.push_back(std::move(e));
                if (t->kind == TypeKind::KeyedList) s.keys.emplace_back(mix(seed + 101 * (i + 1)) | 1, mix(seed + 103 * (i + 1)));
            }
            if (t->kind == TypeKind::Set) {
                std::sort(s.items.begin(), s.items.end(), [&](const Sample& a, const Sample& b) { return keyLess(a, b); });
            }
            break;
        }
        case TypeKind::Array:
            s.kind = Sample::Kind::Array;
            for (u32 i = 0; i < t->arraySize; ++i) s.items.push_back(make(t->element, mix(seed + 31 * (i + 1)), depth + 1, label));
            break;
        case TypeKind::Optional:
            s.kind = Sample::Kind::Optional;
            if (depth < kMaxDepth) s.items.push_back(make(t->element, mix(seed + 7), depth + 1, label));
            break;
        case TypeKind::Map: {
            s.kind = Sample::Kind::Map;
            if (depth >= kMaxDepth) break;
            for (u64 i = 0; i < 2; ++i) {
                Sample k = make(t->key, mix(seed + 13 * (i + 1)), depth + 1, label + "Key");
                bool dup = false;
                for (const Sample& e : s.mapKeys) dup = dup || keyText(e) == keyText(k);
                if (dup) continue;
                s.mapValues.push_back(make(t->element, mix(seed + 19 * (i + 1)), depth + 1, label));
                s.mapKeys.push_back(std::move(k));
            }
            break;
        }
        case TypeKind::Struct:
            s.kind = Sample::Kind::Struct;
            if (depth >= kMaxDepth + 1) {
                s.isNew = true;
                break;
            }
            for (const Field& f : t->decl->fields) {
                s.items.push_back(make(f.type, mix(seed ^ fnv1a64(f.name)), depth + 1, f.name));
            }
            break;
        case TypeKind::Variant: {
            s.kind = Sample::Kind::Variant;
            const auto& alts = t->decl->alternatives;
            s.alt = alts.size() - 1;
            s.items.push_back(make(m_schema.typesBySignature.at(alts[s.alt].type->qualifiedName), mix(seed + 3), depth + 1, label));
            break;
        }
        }
        return s;
    }

    explicit SampleBuilder(const Schema& schema) : m_schema(schema) {}

    /// Sample of a struct declaration.
    Sample makeStruct(const Decl* d) {
        return make(m_schema.typesBySignature.at(d->qualifiedName), fnv1a64(d->qualifiedName), 0, d->name);
    }

private:
    Value prim(Prim p, u64 seed, const std::string& label) {
        Value v;
        switch (p) {
        case Prim::Bool:
            v.kind = Value::Kind::Bool;
            v.b = true;
            break;
        case Prim::F32:
        case Prim::F64:
            v.kind = Value::Kind::Float;
            v.f = static_cast<f64>(static_cast<i64>(seed % 4001) - 2000) / 8.0 + 0.125; // exact in f32
            break;
        case Prim::String:
            v.kind = Value::Kind::String;
            v.s = std::format("{}_{} \xC3\xA9\"q\"", label, seed % 1000);
            break;
        case Prim::Name:
            v.kind = Value::Kind::String;
            v.s = std::format("{}_{}", label, seed % 1000);
            break;
        case Prim::I8: v = signedValue(-100 - static_cast<i64>(seed % 28)); break;
        case Prim::I16: v = signedValue(-30000 + static_cast<i64>(seed % 1000)); break;
        case Prim::I32: v = signedValue(-2000000000 + static_cast<i64>(seed % 1000)); break;
        case Prim::I64: v = signedValue(-(static_cast<i64>(seed >> 2)) - 1); break;
        case Prim::U8: v = unsignedValue(128 + seed % 128); break;
        case Prim::U16: v = unsignedValue(40000 + seed % 20000); break;
        case Prim::U32: v = unsignedValue(3000000000ull + seed % 1000000); break;
        case Prim::U64: v = unsignedValue(seed | (1ull << 63)); break;
        default: break;
        }
        return v;
    }
    static Value signedValue(i64 x) {
        Value v;
        v.kind = Value::Kind::Int;
        v.i = x;
        return v;
    }
    static Value unsignedValue(u64 x) {
        Value v;
        v.kind = Value::Kind::UInt;
        v.u = x;
        return v;
    }

    void builtin(Sample& s, Builtin b, u64 seed, const std::string& label) {
        Value& v = s.value;
        switch (b) {
        case Builtin::Guid:
            v.kind = Value::Kind::Guid;
            v.u = mix(seed) | 1;
            v.u2 = mix(seed + 1);
            break;
        case Builtin::EntityId: v = unsignedValue((1ull << 63) | (seed >> 3)); break;
        case Builtin::NetHandle: v = unsignedValue(1 + seed % 16777215); break;
        case Builtin::Tick: v = unsignedValue(1 + seed % 1000000000); break;
        case Builtin::Duration:
            v.kind = Value::Kind::Duration;
            v.i = static_cast<i64>(1 + seed % 5000) * 1000000;
            break;
        case Builtin::LocString:
        case Builtin::TagQuery:
        case Builtin::HxlExpr:
            v.kind = Value::Kind::String;
            v.s = std::format("{}.{}", label, seed % 100);
            break;
        case Builtin::TagSet:
            s.kind = Sample::Kind::TagSet;
            s.tags = {std::format("Tag.{}", seed % 10), std::format("Tag.Z{}", seed % 7)};
            std::sort(s.tags.begin(), s.tags.end());
            break;
        default: {
            const u32 n = tupleSize(b);
            v.kind = Value::Kind::List;
            for (u32 i = 0; i < n; ++i) {
                Value c;
                c.kind = Value::Kind::Float;
                c.f = static_cast<f64>(static_cast<i64>((seed >> (8 * i)) % 64) - 32) / 4.0;
                v.items.push_back(c);
            }
            break;
        }
        }
    }

    static std::string keyText(const Sample& s) {
        if (s.kind == Sample::Kind::RecordRef) return std::to_string(s.id);
        const Value& v = s.value;
        switch (v.kind) {
        case Value::Kind::Int: return std::to_string(v.i);
        case Value::Kind::UInt: return std::to_string(v.u);
        case Value::Kind::String: return v.s;
        case Value::Kind::Enum: return v.s;
        case Value::Kind::Guid: return formatGuid(v.u, v.u2);
        default: return {};
        }
    }

    static bool keyLess(const Sample& a, const Sample& b) {
        if (a.kind == Sample::Kind::RecordRef) return a.id < b.id;
        const Value& x = a.value;
        const Value& y = b.value;
        switch (x.kind) {
        case Value::Kind::Int: return x.i < y.i;
        case Value::Kind::UInt: return x.u < y.u;
        case Value::Kind::Enum: return x.i < y.i;
        case Value::Kind::Guid: return x.u != y.u ? x.u < y.u : x.u2 < y.u2;
        default: return x.s < y.s;
        }
    }

    const Schema& m_schema;
};

// --- C++ ------------------------------------------------------------------------------------

std::string cppExpr(const Sample& s) {
    const std::string T = cppTypeName(s.type);
    switch (s.kind) {
    case Sample::Kind::Scalar: return cppValueLiteral(s.value, s.type);
    case Sample::Kind::RecordRef: return std::format("{}({}ull)", T, s.id);
    case Sample::Kind::AssetRef: return std::format("::helios::refl::AssetRef{{::helios::Guid({:#x}ull, {:#x}ull)}}", s.g1, s.g2);
    case Sample::Kind::TagSet: {
        std::string out = "[] { ::helios::refl::TagSet t; ";
        for (const std::string& tag : s.tags) out += std::format("t.add({}); ", cppQuote(tag));
        return out + "return t; }()";
    }
    case Sample::Kind::List:
    case Sample::Kind::Set:
    case Sample::Kind::Array: {
        std::string out = T + "{";
        for (usize i = 0; i < s.items.size(); ++i) out += (i ? ", " : "") + cppExpr(s.items[i]);
        return out + "}";
    }
    case Sample::Kind::Optional: return s.items.empty() ? T + "{}" : T + "{" + cppExpr(s.items[0]) + "}";
    case Sample::Kind::Map: {
        std::string out = T + "{";
        for (usize i = 0; i < s.mapKeys.size(); ++i)
            out += (i ? ", " : "") + std::format("{{{}, {}}}", cppExpr(s.mapKeys[i]), cppExpr(s.mapValues[i]));
        return out + "}";
    }
    case Sample::Kind::KeyedList: {
        std::string out = "[] { " + T + " l; ";
        for (usize i = 0; i < s.items.size(); ++i)
            out += std::format("l.add(::helios::Guid({:#x}ull, {:#x}ull), {}); ", s.keys[i].first, s.keys[i].second, cppExpr(s.items[i]));
        return out + "return l; }()";
    }
    case Sample::Kind::Struct: {
        if (s.isNew) return T + "{}";
        std::string out = "[] { " + T + " v{}; ";
        const auto& fields = s.type->decl->fields;
        for (usize i = 0; i < fields.size(); ++i) out += std::format("v.{} = {}; ", cppFieldName(fields[i].name), cppExpr(s.items[i]));
        return out + "return v; }()";
    }
    case Sample::Kind::Variant: return std::format("{}{{std::in_place_index<{}>, {}}}", T, s.alt, cppExpr(s.items[0]));
    }
    return "{}";
}

// --- Go ---------------------------------------------------------------------------------------

std::string goGuid(u64 hi, u64 lo) {
    Value v;
    v.kind = Value::Kind::Guid;
    v.u = hi;
    v.u2 = lo;
    Type t;
    t.kind = TypeKind::Builtin;
    t.builtin = Builtin::Guid;
    return goValueLiteral(v, &t);
}

std::string goExpr(const Sample& s) {
    const std::string T = goTypeName(s.type);
    switch (s.kind) {
    case Sample::Kind::Scalar: return goValueLiteral(s.value, s.type);
    case Sample::Kind::RecordRef: return std::format("{}({})", T, s.id);
    case Sample::Kind::AssetRef: return "AssetRef(" + goGuid(s.g1, s.g2) + ")";
    case Sample::Kind::TagSet: {
        std::string out = "TagSet{";
        for (usize i = 0; i < s.tags.size(); ++i) out += (i ? ", " : "") + goQuote(s.tags[i]);
        return out + "}";
    }
    case Sample::Kind::List:
    case Sample::Kind::Set:
        if (s.items.empty()) return T + "(nil)";
        [[fallthrough]];
    case Sample::Kind::Array: {
        std::string out = T + "{";
        for (usize i = 0; i < s.items.size(); ++i) out += (i ? ", " : "") + goExpr(s.items[i]);
        return out + "}";
    }
    case Sample::Kind::Optional: return s.items.empty() ? "(" + T + ")(nil)" : "ptrTo(" + goExpr(s.items[0]) + ")";
    case Sample::Kind::Map: {
        if (s.mapKeys.empty()) return T + "(nil)";
        std::string out = T + "{";
        for (usize i = 0; i < s.mapKeys.size(); ++i) out += (i ? ", " : "") + goExpr(s.mapKeys[i]) + ": " + goExpr(s.mapValues[i]);
        return out + "}";
    }
    case Sample::Kind::KeyedList: {
        if (s.items.empty()) return T + "(nil)";
        std::string out = T + "{";
        for (usize i = 0; i < s.items.size(); ++i)
            out += (i ? ", " : "") + std::format("{{Key: {}, Value: {}}}", goGuid(s.keys[i].first, s.keys[i].second), goExpr(s.items[i]));
        return out + "}";
    }
    case Sample::Kind::Struct: {
        const std::string name = s.type->decl->goName;
        if (s.isNew) return "New" + name + "()";
        std::string out = "func() " + name + " { v := New" + name + "(); ";
        const auto& fields = s.type->decl->fields;
        for (usize i = 0; i < fields.size(); ++i) out += std::format("v.{} = {}; ", goFieldName(fields[i].name), goExpr(s.items[i]));
        return out + "return v }()";
    }
    case Sample::Kind::Variant: {
        const Alternative& a = s.type->decl->alternatives[s.alt];
        return std::format("{}{{{}: ptrTo({})}}", T, a.name, goExpr(s.items[0]));
    }
    }
    return "nil";
}

/// Struct declarations that get samples (every emitted struct-like type of the file / package).
std::vector<const Decl*> sampleDecls(const Schema& schema, const SourceFile* file) {
    std::vector<const Decl*> out;
    for (const Decl* d : schema.decls) {
        if (!d->emitted || !d->isStructLike()) continue;
        if (file && d->file != file) continue;
        out.push_back(d);
    }
    return out;
}

std::string cppNamespaceOf(const std::string& pkg) {
    std::string out;
    for (const char c : pkg) out += c == '.' ? std::string("::") : std::string(1, c);
    return out;
}

} // namespace

std::string generateCppSamples(const Schema& schema, const SourceFile& file) {
    SampleBuilder builder(schema);
    CodeWriter w;
    w.line(std::format("// Sample values for {} — generated by helios-schemac --samples. DO NOT EDIT.", file.logicalPath));
    w.line("// The Go test (helios_schema_test.go) builds identical values for cross-language checks.");
    w.line("#pragma once");
    w.line();
    w.line(std::format("#include \"{}\"", cppHeaderPath(file.logicalPath).substr(cppHeaderPath(file.logicalPath).rfind('/') + 1)));
    w.line();
    w.line(std::format("namespace {}::samples {{", cppNamespaceOf(file.ast.package)));
    w.line();
    const auto decls = sampleDecls(schema, &file);
    for (const Decl* d : decls) {
        const Sample s = builder.makeStruct(d);
        w.line(std::format("inline {} make{}() {{", cppTypeName(schema.typesBySignature.at(d->qualifiedName)), d->goName));
        w.line("    return " + cppExpr(s) + ";");
        w.line("}");
        w.line();
    }
    w.line("struct SampleEntry {");
    w.line("    const ::helios::refl::TypeInfo& (*type)() noexcept;");
    w.line("    void (*fill)(void* object);");
    w.line("};");
    w.line();
    w.line("inline constexpr SampleEntry kSamples[] = {");
    for (const Decl* d : decls) {
        const std::string T = cppTypeName(schema.typesBySignature.at(d->qualifiedName));
        w.line(std::format("    {{&::helios::refl::TypeOf<{}>::get, [](void* p) {{ *static_cast<{}*>(p) = make{}(); }}}},", T, T, d->goName));
    }
    if (decls.empty()) w.line("    {nullptr, nullptr},");
    w.line("};");
    w.line();
    w.line(std::format("}} // namespace {}::samples", cppNamespaceOf(file.ast.package)));
    return w.take();
}

std::string generateGoTest(const Schema& schema, const std::string& package) {
    SampleBuilder builder(schema);
    CodeWriter w(1, '\t');
    w.line("// Code generated by helios-schemac. DO NOT EDIT.");
    w.line("//");
    w.line("// Round-trip tests for the generated types, plus TestHeliosCppVectors, which compares the Go");
    w.line("// encoding of every sample with the bytes the C++ codec produced (testdata/cpp_vectors.json).");
    w.line();
    w.line("package " + package);
    w.line();
    w.line("import (");
    w.line("\t\"encoding/hex\"");
    w.line("\t\"encoding/json\"");
    w.line("\t\"math\"");
    w.line("\t\"os\"");
    w.line("\t\"reflect\"");
    w.line("\t\"testing\"");
    w.line(")");
    w.line();
    w.line("var _ = math.MaxInt8");
    w.line();
    const auto decls = sampleDecls(schema, nullptr);
    for (const Decl* d : decls) {
        const Sample s = builder.makeStruct(d);
        w.line(std::format("func sample{}() {} {{ return {} }}", d->goName, d->goName, goExpr(s)));
        w.line();
    }
    w.line("type heliosCase struct {");
    w.line("\tname       string");
    w.line("\tsample     func() any");
    w.line("\tempty      func() any");
    w.line("\tencode     func(any) []byte");
    w.line("\tdecode     func([]byte) (any, error)");
    w.line("\tdecodeJSON func([]byte) (any, error)");
    w.line("}");
    w.line();
    w.line("var heliosCases = []heliosCase{");
    for (const Decl* d : decls) {
        const std::string T = d->goName;
        w.line(std::format("\t{{{}, func() any {{ return sample{}() }}, func() any {{ return New{}() }},", goQuote(d->qualifiedName), T, T));
        w.line(std::format("\t\tfunc(v any) []byte {{ x := v.({}); return x.MarshalHelios() }},", T));
        w.line(std::format("\t\tfunc(b []byte) (any, error) {{ var x {}; err := x.UnmarshalHelios(b); return x, err }},", T));
        w.line(std::format("\t\tfunc(b []byte) (any, error) {{ x := New{}(); err := json.Unmarshal(b, &x); return x, err }}}},", T));
    }
    w.line("}");
    w.line();
    w.raw(R"GO(func TestHeliosRoundTrip(t *testing.T) {
	for _, c := range heliosCases {
		v := c.sample()
		b := c.encode(v)
		back, err := c.decode(b)
		if err != nil {
			t.Fatalf("%s: decode: %v", c.name, err)
		}
		if !reflect.DeepEqual(back, v) {
			t.Errorf("%s: binary round trip mismatch\n got %#v\nwant %#v", c.name, back, v)
		}
		if again := c.encode(back); string(again) != string(b) {
			t.Errorf("%s: re-encoding differs", c.name)
		}
		j, err := json.Marshal(v)
		if err != nil {
			t.Fatalf("%s: json: %v", c.name, err)
		}
		fromJSON, err := c.decodeJSON(j)
		if err != nil {
			t.Fatalf("%s: json decode: %v\n%s", c.name, err, j)
		}
		if !reflect.DeepEqual(fromJSON, v) {
			t.Errorf("%s: JSON round trip mismatch\n got %#v\nwant %#v\njson %s", c.name, fromJSON, v, j)
		}
		if e := c.encode(c.empty()); len(e) != 0 {
			t.Errorf("%s: the default value encodes to %d bytes, want 0", c.name, len(e))
		}
	}
}

func TestHeliosCorruptInput(t *testing.T) {
	for _, c := range heliosCases {
		b := c.encode(c.sample())
		for cut := 0; cut < len(b); cut++ {
			_, _ = c.decode(b[:cut]) // must not panic
		}
		for i := range b {
			m := append([]byte(nil), b...)
			m[i] ^= 0x5a
			_, _ = c.decode(m)
		}
	}
}

type heliosVector struct {
	Type   string          `json:"type"`
	Tagged string          `json:"tagged"`
	JSON   json.RawMessage `json:"json"`
}

func TestHeliosCppVectors(t *testing.T) {
	data, err := os.ReadFile("testdata/cpp_vectors.json")
	if os.IsNotExist(err) {
		t.Skip("no testdata/cpp_vectors.json (written by the C++ schemac tests)")
	}
	if err != nil {
		t.Fatal(err)
	}
	var in struct {
		Vectors []heliosVector `json:"vectors"`
	}
	if err := json.Unmarshal(data, &in); err != nil {
		t.Fatal(err)
	}
	byName := map[string]heliosCase{}
	for _, c := range heliosCases {
		byName[c.name] = c
	}
	var out []heliosVector
	for _, v := range in.Vectors {
		c, ok := byName[v.Type]
		if !ok {
			t.Errorf("C++ vector for unknown type %s", v.Type)
			continue
		}
		want := c.sample()
		got := hex.EncodeToString(c.encode(want))
		if got != v.Tagged {
			t.Errorf("%s: Go bytes differ from C++\n  go: %s\n cpp: %s", v.Type, got, v.Tagged)
		}
		raw, err := hex.DecodeString(v.Tagged)
		if err != nil {
			t.Fatal(err)
		}
		back, err := c.decode(raw)
		if err != nil {
			t.Errorf("%s: decoding C++ bytes: %v", v.Type, err)
		} else if !reflect.DeepEqual(back, want) {
			t.Errorf("%s: C++ bytes decode to\n %#v\nwant %#v", v.Type, back, want)
		}
		fromJSON, err := c.decodeJSON(v.JSON)
		if err != nil {
			t.Errorf("%s: reading C++ JSONC: %v", v.Type, err)
		} else if !reflect.DeepEqual(fromJSON, want) {
			t.Errorf("%s: C++ JSONC decodes to\n %#v\nwant %#v", v.Type, fromJSON, want)
		}
		j, err := json.Marshal(want)
		if err != nil {
			t.Fatal(err)
		}
		out = append(out, heliosVector{Type: v.Type, Tagged: got, JSON: j})
	}
	if len(in.Vectors) != len(heliosCases) {
		t.Errorf("C++ wrote %d vectors, Go has %d sample types", len(in.Vectors), len(heliosCases))
	}
	b, err := json.MarshalIndent(map[string]any{"vectors": out}, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile("testdata/go_vectors.json", b, 0o644); err != nil {
		t.Fatal(err)
	}
}
)GO");
    return w.take();
}

} // namespace helios::schemac
