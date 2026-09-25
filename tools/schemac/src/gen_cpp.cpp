// `--emit cpp`: plain structs/enums/variants in the package namespace, TypeInfo registration,
// compiled tagged + JSONC codecs (Codec<T> on top of helios/reflect/codec.h), equality, and
// Mut<C> dirty-bit mutators for replicated components.

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <set>

#include "code_writer.h"
#include "generators.h"
#include "samples.h"
#include "text.h"

namespace helios::schemac {

std::string cppFieldName(const std::string& name) { return isCppKeyword(name) ? name + "_" : name; }

namespace {

std::string cppNamespace(const std::string& pkg) {
    std::string out;
    for (const char c : pkg) out += c == '.' ? std::string("::") : std::string(1, c);
    return out;
}

std::string declCpp(const Decl* d) { return "::" + cppNamespace(d->package) + "::" + join(d->cppPath, "::"); }

std::string primCpp(Prim p) {
    switch (p) {
    case Prim::Bool: return "bool";
    case Prim::String: return "std::string";
    case Prim::Name: return "::helios::Name";
    default: return "::helios::" + std::string(primName(p));
    }
}

std::string builtinCpp(Builtin b) {
    switch (b) {
    case Builtin::Guid: return "::helios::Guid";
    case Builtin::Vec2f: return "::helios::Vec2";
    case Builtin::Vec3f: return "::helios::Vec3";
    case Builtin::Vec4f: return "::helios::Vec4";
    case Builtin::Vec3d: return "::helios::DVec3";
    case Builtin::Quatf: return "::helios::Quat";
    case Builtin::Quatd: return "::helios::DQuat";
    case Builtin::Color: return "::helios::Color";
    default: return "::helios::refl::" + std::string(builtinName(b));
    }
}

} // namespace

std::string cppTypeName(const Type* t) {
    switch (t->kind) {
    case TypeKind::Prim: return primCpp(t->prim);
    case TypeKind::Builtin: return builtinCpp(t->builtin);
    case TypeKind::Enum:
    case TypeKind::Flags:
    case TypeKind::Struct:
    case TypeKind::Variant: return declCpp(t->decl);
    case TypeKind::List: return "std::vector<" + cppTypeName(t->element) + ">";
    case TypeKind::KeyedList: return "::helios::refl::KeyedList<" + cppTypeName(t->element) + ">";
    case TypeKind::Set: return "std::set<" + cppTypeName(t->element) + ">";
    case TypeKind::Map: return "std::map<" + cppTypeName(t->key) + ", " + cppTypeName(t->element) + ">";
    case TypeKind::Optional: return "std::optional<" + cppTypeName(t->element) + ">";
    case TypeKind::Array: return "std::array<" + cppTypeName(t->element) + ", " + std::to_string(t->arraySize) + ">";
    case TypeKind::RecordRef: return "::helios::refl::RecordRef<" + declCpp(t->decl) + ">";
    case TypeKind::AssetRef: return "::helios::refl::AssetRef";
    }
    return "void";
}

namespace {

std::string floatLiteral(f64 v, bool f32) {
    std::string s = f32 ? formatF32(static_cast<float>(v)) : formatF64(v);
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return f32 ? s + "f" : s;
}

std::string intLiteral(const Value& v, Prim p) {
    if (v.kind == Value::Kind::UInt) {
        return std::to_string(v.u) + (p == Prim::U64 ? "ull" : "u");
    }
    if (v.i == std::numeric_limits<i64>::min()) return "(-9223372036854775807ll - 1)";
    return std::to_string(v.i) + (p == Prim::I64 ? "ll" : "");
}

} // namespace

std::string cppValueLiteral(const Value& v, const Type* t) {
    switch (t->kind) {
    case TypeKind::Prim:
        switch (t->prim) {
        case Prim::Bool: return v.b ? "true" : "false";
        case Prim::F32: return floatLiteral(v.f, true);
        case Prim::F64: return floatLiteral(v.f, false);
        case Prim::String: return "std::string(" + cppQuote(v.s) + ")";
        case Prim::Name: return "::helios::Name(" + cppQuote(v.s) + ")";
        default: return intLiteral(v, t->prim);
        }
    case TypeKind::Builtin:
        switch (t->builtin) {
        case Builtin::LocString:
        case Builtin::TagQuery:
        case Builtin::HxlExpr: return builtinCpp(t->builtin) + "{" + cppQuote(v.s) + "}";
        case Builtin::Guid: return std::format("::helios::Guid({:#x}ull, {:#x}ull)", v.u, v.u2);
        case Builtin::Duration: return std::format("::helios::refl::Duration({}ll)", v.i);
        case Builtin::EntityId: return std::format("::helios::refl::EntityId({}ull)", v.u);
        case Builtin::NetHandle: return std::format("::helios::refl::NetHandle({}u)", v.u);
        case Builtin::Tick: return std::format("{}ull", v.u);
        default: {
            const bool f64 = tupleIsF64(t->builtin);
            std::string args;
            for (usize i = 0; i < v.items.size(); ++i) args += (i ? ", " : "") + floatLiteral(v.items[i].f, !f64);
            if (t->builtin == Builtin::WorldPos) return "::helios::refl::WorldPos{::helios::DVec3(" + args + ")}";
            return builtinCpp(t->builtin) + "(" + args + ")";
        }
        }
    case TypeKind::Enum: return cppTypeName(t) + "::" + v.s;
    case TypeKind::Flags: return std::format("static_cast<{}>({}ull)", cppTypeName(t), v.u);
    case TypeKind::Array: {
        std::string out = cppTypeName(t) + "{";
        for (usize i = 0; i < v.items.size(); ++i) out += (i ? ", " : "") + cppValueLiteral(v.items[i], t->element);
        return out + "}";
    }
    default: return "{}";
    }
}

namespace {

/// Implicit member initializer: enums start at their first value.
std::string memberInitializer(const Field& f) {
    if (f.defaultValue) return " = " + cppValueLiteral(*f.defaultValue, f.type);
    if (f.type->kind == TypeKind::Enum && !f.type->decl->values.empty())
        return " = " + cppTypeName(f.type) + "::" + f.type->decl->values.front().name;
    // Containers get no initializer: a `{}` member initializer would instantiate the container's
    // constructor (and destructor) at the end of the class, where a list element type declared
    // later in the file (or the class itself) is still incomplete.
    switch (f.type->kind) {
    case TypeKind::List:
    case TypeKind::KeyedList:
    case TypeKind::Set:
    case TypeKind::Map: return "";
    default: return "{}";
    }
}

/// Declarations embedded by value in `d` (they must be defined first).
void valueDeps(const Type* t, std::vector<const Decl*>& out) {
    if (!t) return;
    switch (t->kind) {
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::Flags: out.push_back(t->decl); break;
    case TypeKind::Variant:
        out.push_back(t->decl);
        for (const Alternative& a : t->decl->alternatives) out.push_back(a.type);
        break;
    case TypeKind::Map:
        valueDeps(t->key, out);
        valueDeps(t->element, out);
        break;
    case TypeKind::Optional:
    case TypeKind::Array:
    case TypeKind::Set: valueDeps(t->element, out); break;
    default: break;
    }
}

struct AttrTables {
    CodeWriter* w = nullptr;
    int counter = 0;
    std::string emit(const std::vector<Attr>& attrs, const Type* fieldType = nullptr) {
        std::vector<std::string> entries;
        bool hasAsset = false;
        for (const Attr& a : attrs) {
            if (a.name == "asset") hasAsset = true;
        }
        std::vector<Attr> all = attrs;
        if (fieldType && !hasAsset) {
            const Type* t = fieldType;
            while (t->kind == TypeKind::List || t->kind == TypeKind::Optional || t->kind == TypeKind::Array) t = t->element;
            if (t->kind == TypeKind::AssetRef) all.push_back(Attr{"asset", {AttrArg{"", t->assetKind, false, {}}}, true, {}});
        }
        if (all.empty()) return "{}";
        const int id = counter++;
        std::string entriesText;
        for (usize i = 0; i < all.size(); ++i) {
            const Attr& a = all[i];
            std::string argsName = "{}";
            if (!a.args.empty()) {
                argsName = std::format("kArgs{}_{}", id, i);
                std::string list;
                for (const AttrArg& arg : a.args) list += std::format("{{{}, {}}}, ", cppQuote(arg.key), cppQuote(arg.value));
                w->line(std::format("constexpr ::helios::refl::AttrArg {}[] = {{{}}};", argsName, list));
            }
            std::string typed = "nullptr";
            std::string kind = "0";
            auto num = [](const std::string& s) {
                std::string t = s;
                std::erase(t, '_');
                return t.find_first_of(".eE") == std::string::npos && t.find("0x") == std::string::npos ? t + ".0" : t;
            };
            const std::string payload = std::format("kTyped{}_{}", id, i);
            if (a.name == "range" && a.args.size() == 2) {
                w->line(std::format("constexpr ::helios::refl::attrs::Range {}{{{}, {}}};", payload, num(a.args[0].value), num(a.args[1].value)));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Range::kKind";
            } else if (a.name == "unit" && a.args.size() == 1) {
                w->line(std::format("constexpr ::helios::refl::attrs::Unit {}{{{}}};", payload, cppQuote(a.args[0].value)));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Unit::kKind";
            } else if (a.name == "step" && a.args.size() == 1) {
                w->line(std::format("constexpr ::helios::refl::attrs::Step {}{{{}}};", payload, num(a.args[0].value)));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Step::kKind";
            } else if (a.name == "max" && a.args.size() == 1) {
                u64 n = 0;
                parseSchemaUnsigned(a.args[0].value, n); // validated by sema; the text may be "1_000" or hex
                w->line(std::format("constexpr ::helios::refl::attrs::Max {}{{{}ull}};", payload, n));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Max::kKind";
            } else if (a.name == "keyed") {
                w->line(std::format("constexpr ::helios::refl::attrs::Keyed {}{{{}}};", payload, cppQuote(a.args.empty() ? "" : a.args[0].value)));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Keyed::kKind";
            } else if (a.name == "table" && a.args.size() == 1) {
                w->line(std::format("constexpr ::helios::refl::attrs::Table {}{{{}}};", payload, cppQuote(a.args[0].value)));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Table::kKind";
            } else if (a.name == "asset" && a.args.size() == 1) {
                w->line(std::format("constexpr ::helios::refl::attrs::Asset {}{{{}}};", payload, cppQuote(a.args[0].value)));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Asset::kKind";
            } else if (a.name == "doc" && a.args.size() == 1) {
                w->line(std::format("constexpr ::helios::refl::attrs::Doc {}{{{}}};", payload, cppQuote(a.args[0].value)));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Doc::kKind";
            } else if (a.name == "replicate" && a.args.size() == 1) {
                const std::string& v = a.args[0].value;
                const std::string aud = v == "all" ? "All" : v == "owner" ? "Owner" : v == "server" ? "Server" : "None";
                w->line(std::format("constexpr ::helios::refl::attrs::Replicate {}{{::helios::refl::Audience::{}}};", payload, aud));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Replicate::kKind";
            } else if (a.name == "editor") {
                std::string category, widget, order = "0";
                for (const AttrArg& arg : a.args) {
                    if (arg.key == "category") category = arg.value;
                    if (arg.key == "widget") widget = arg.value;
                    if (arg.key == "order") {
                        f64 n = 0;
                        parseSchemaNumber(arg.value, n); // validated by sema: an integer that fits i32
                        order = std::to_string(static_cast<i64>(n));
                    }
                }
                w->line(std::format("constexpr ::helios::refl::attrs::Editor {}{{{}, {}, {}}};", payload, cppQuote(category), cppQuote(widget), order));
                typed = "&" + payload;
                kind = "::helios::refl::attrs::Editor::kKind";
            }
            entriesText += std::format("{{{}, {}, {}, {}}}, ", cppQuote(a.name), argsName, typed, kind);
        }
        const std::string name = std::format("kAttrs{}", id);
        w->line(std::format("constexpr ::helios::refl::Attr {}[] = {{{}}};", name, entriesText));
        return name;
    }
};

class CppGenerator {
public:
    CppGenerator(const Schema& s, const CompileOptions& o) : S(s), O(o) {}

    std::vector<OutputFile> run() {
        std::vector<OutputFile> out;
        for (const auto& fp : S.files) {
            const SourceFile* f = fp.get();
            if (!f->generate) continue;
            const std::vector<Decl*> decls = S.declsOfFile(f);
            out.push_back(OutputFile{joinOut(cppHeaderPath(f->logicalPath)), header(f, decls)});
            out.push_back(OutputFile{joinOut(cppSourcePath(f->logicalPath)), source(f, decls)});
            if (O.samples) {
                const usize dot = f->logicalPath.rfind(".hschema");
                const std::string base = dot == std::string::npos ? f->logicalPath : f->logicalPath.substr(0, dot);
                out.push_back(OutputFile{joinOut(base + ".samples.gen.h"), generateCppSamples(S, *f)});
            }
        }
        return out;
    }

private:
    std::string joinOut(const std::string& rel) const {
        return O.cppOut.empty() || O.cppOut == "." ? rel : O.cppOut + "/" + rel;
    }

    static std::string registerFunctionName(const SourceFile* f) { return "register" + pascalCase(f->stem) + "Types"; }

    // --- header --------------------------------------------------------------------------------
    std::string header(const SourceFile* f, const std::vector<Decl*>& decls) {
        CodeWriter w;
        w.line(std::format("// {} — generated by helios-schemac from {}. DO NOT EDIT.", cppHeaderPath(f->logicalPath).substr(cppHeaderPath(f->logicalPath).rfind('/') + 1), f->logicalPath));
        w.line("// Schema language: tools/schemac/README.md (normative: docs/plan/02-engine-runtime.md §3).");
        w.line("#pragma once");
        w.line();
        w.line("#include \"helios/reflect/generated.h\"");
        for (const SourceFile* imp : f->imports) w.line(std::format("#include \"{}\"", cppHeaderPath(imp->logicalPath)));
        w.line();
        w.line(std::format("namespace {} {{", cppNamespace(f->ast.package)));
        w.line();
        // Forward declarations (records need them for their Ref aliases).
        bool any = false;
        for (const Decl* d : f->decls) {
            if (d->isStructLike()) {
                w.line(std::format("struct {};", d->name));
                any = true;
            }
        }
        for (const Decl* d : f->decls) {
            if (d->kind == DeclKind::Record) {
                const std::string base = d->name.size() > 3 && d->name.ends_with("Def") ? d->name.substr(0, d->name.size() - 3) : d->name;
                w.line(std::format("/// Reference to a {} record (serialized as its RecordId).", d->name));
                w.line(std::format("using {}Ref = ::helios::refl::RecordRef<{}>;", base, d->name));
                any = true;
            }
        }
        if (any) w.line();
        for (const Decl* d : f->decls) {
            if (d->kind == DeclKind::Const) emitConst(w, d);
        }
        for (const Decl* d : orderTopLevel(f)) emitDecl(w, d);
        w.line(std::format("/// Registers every type declared in {} (call once at startup, before lookups).", f->logicalPath));
        w.line(std::format("::helios::Result<void> {}(::helios::refl::TypeRegistry& registry = ::helios::refl::TypeRegistry::global());",
                           registerFunctionName(f)));
        w.line();
        w.line(std::format("}} // namespace {}", cppNamespace(f->ast.package)));
        w.line();
        w.line("namespace helios::refl {");
        w.line();
        for (const Decl* d : decls) {
            if (!d->isLockable()) continue;
            const std::string name = declCpp(d);
            w.line("template <>");
            w.open(std::format("struct TypeOf<{}> {{", name));
            w.line("static const TypeInfo& get() noexcept;");
            w.close("};");
            if (d->isStructLike()) {
                w.line("template <>");
                w.open(std::format("struct Codec<{}> : StructCodec<{}> {{", name, name));
                w.line(std::format("static bool isDefault(const {}& v) noexcept;", name));
                w.line(std::format("static bool equals(const {}& a, const {}& b) noexcept;", name, name));
                w.line(std::format("static void writeFields(TaggedWriter& w, const {}& v);", name));
                w.line(std::format("static Result<bool> readField(TaggedReader& r, FieldTag tag, {}& v);", name));
                w.line(std::format("static void writeJsonFields(JsonWriter& w, const {}& v);", name));
                w.line(std::format("static Result<bool> readJsonMember(std::string_view key, JsonValue in, {}& v, ReadCtx& ctx);", name));
                w.close("};");
            }
        }
        for (const Decl* d : decls) {
            if (d->isReplicatedComponent()) emitMut(w, d);
        }
        w.line();
        w.line("} // namespace helios::refl");
        return w.take();
    }

    /// Top-level declarations with by-value dependencies first (declaration order otherwise).
    std::vector<const Decl*> orderTopLevel(const SourceFile* f) {
        std::vector<const Decl*> out;
        std::set<const Decl*> done;
        std::set<const Decl*> visiting;
        auto topOf = [&](const Decl* d) {
            while (d->outer) d = d->outer;
            return d;
        };
        // `alias X = { ... }` materializes the inline type as a separate decl named X, which is
        // emitted in place of the alias: dependencies on it resolve to the alias.
        std::map<const Decl*, const Decl*> namedByAlias;
        for (const Decl* d : f->decls) {
            if (const Decl* n = inlineAliasTarget(d)) namedByAlias[n] = d;
        }
        std::function<void(const Decl*)> visit = [&](const Decl* d) {
            if (done.contains(d) || visiting.contains(d)) return;
            visiting.insert(d);
            std::vector<const Decl*> deps;
            std::function<void(const Decl*)> collect = [&](const Decl* x) {
                for (const Field& fl : x->fields) valueDeps(fl.type, deps);
                for (const Decl* n : x->nested) collect(n);
                for (const Alternative& a : x->alternatives) collect(a.type);
            };
            if (const Decl* named = inlineAliasTarget(d)) {
                collect(named);
            } else {
                collect(d);
                if (d->kind == DeclKind::Alias && d->aliasTarget) valueDeps(d->aliasTarget, deps);
            }
            for (const Decl* dep : deps) {
                const Decl* top = topOf(dep);
                if (auto it = namedByAlias.find(top); it != namedByAlias.end()) top = it->second;
                if (top != d && top->file == f) visit(top);
            }
            visiting.erase(d);
            done.insert(d);
            out.push_back(d);
        };
        for (const Decl* d : f->decls) visit(d);
        return out;
    }

    /// The materialized type of `alias X = { ... }` (same qualified name), else null.
    static const Decl* inlineAliasTarget(const Decl* d) {
        if (d->kind != DeclKind::Alias || !d->aliasTarget || !d->aliasTarget->decl) return nullptr;
        return d->aliasTarget->decl->qualifiedName == d->qualifiedName ? d->aliasTarget->decl : nullptr;
    }

    void emitConst(CodeWriter& w, const Decl* d) {
        if (!d->constType || d->constValue.kind == Value::Kind::None) return;
        emitDocLines(w, d->doc);
        const Type* t = d->constType;
        if (t->kind == TypeKind::Prim && (t->prim == Prim::String || t->prim == Prim::Name)) {
            w.line(std::format("inline constexpr std::string_view {} = {};", d->name, cppQuote(d->constValue.s)));
        } else if (t->kind == TypeKind::Builtin && t->builtin == Builtin::Duration) {
            w.line(std::format("inline constexpr ::helios::refl::Duration {}{{{}ll}};", d->name, d->constValue.i));
        } else {
            w.line(std::format("inline constexpr {} {} = {};", cppTypeName(t), d->name, cppValueLiteral(d->constValue, t)));
        }
        w.line();
    }

    void emitDocLines(CodeWriter& w, const std::string& doc) {
        if (doc.empty()) return;
        usize start = 0;
        while (true) {
            const usize nl = doc.find('\n', start);
            const std::string line = doc.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            w.line(line.empty() ? "///" : "/// " + line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }

    void emitEnum(CodeWriter& w, const Decl* d) {
        emitDocLines(w, d->doc);
        w.open(std::format("enum class {} : {} {{", d->name, primCpp(d->underlying)));
        for (const EnumVal& v : d->values) {
            emitDocLines(w, v.doc);
            w.line(std::format("{} = {},", v.name, v.value));
        }
        w.close("};");
        if (d->kind == DeclKind::Flags) w.line(std::format("HELIOS_ENUM_FLAGS({})", d->name));
        w.line();
    }

    void emitVariant(CodeWriter& w, const Decl* v) {
        for (const Alternative& a : v->alternatives) emitStruct(w, a.type);
        emitDocLines(w, v->doc);
        std::string alts;
        for (usize i = 0; i < v->alternatives.size(); ++i) alts += (i ? ", " : "") + v->alternatives[i].type->cppPath.back();
        w.line(std::format("using {} = std::variant<{}>;", v->name, alts));
        w.line();
    }

    void emitDecl(CodeWriter& w, const Decl* d) {
        switch (d->kind) {
        case DeclKind::Enum:
        case DeclKind::Flags: emitEnum(w, d); break;
        case DeclKind::Variant: emitVariant(w, d); break;
        case DeclKind::Alias: {
            if (!d->aliasTarget) break;
            const Type* t = d->aliasTarget;
            // Aliases of inline types name the type itself; emit it here.
            if ((t->kind == TypeKind::Struct || t->kind == TypeKind::Enum || t->kind == TypeKind::Flags || t->kind == TypeKind::Variant) &&
                t->decl->qualifiedName == d->qualifiedName) {
                emitDecl(w, t->decl);
                break;
            }
            emitDocLines(w, d->doc);
            w.line(std::format("using {} = {};", d->name, cppTypeName(t)));
            w.line();
            break;
        }
        case DeclKind::Const:
        case DeclKind::Service:
        case DeclKind::Formula: break;
        default:
            if (d->isStructLike()) emitStruct(w, d);
            break;
        }
    }

    void emitStruct(CodeWriter& w, const Decl* d) {
        emitDocLines(w, d->doc);
        if (d->kind == DeclKind::Rpc) {
            w.line(std::format("/// Arguments of rpc {}{}{}.", d->service ? d->service->name + "." : "", d->rpcName.empty() ? d->name : d->rpcName,
                               d->direction.empty() ? "" : " (" + d->direction + ")"));
        }
        w.open(std::format("struct {} {{", d->cppPath.back()));
        // Nested types first (in dependency order among siblings).
        std::set<const Decl*> done;
        std::function<void(const Decl*)> emitNested = [&](const Decl* n) {
            if (done.contains(n)) return;
            done.insert(n);
            std::vector<const Decl*> deps;
            for (const Field& fl : n->fields) valueDeps(fl.type, deps);
            for (const Decl* nn : n->nested) {
                for (const Field& fl : nn->fields) valueDeps(fl.type, deps);
            }
            for (const Decl* dep : deps) {
                const Decl* x = dep;
                while (x->outer && x->outer != d) x = x->outer;
                if (x->outer == d && x != n) emitNested(x);
            }
            if (n->kind == DeclKind::Alternative) return; // emitted by its variant
            emitDecl(w, n);
        };
        for (const Decl* n : d->nested) {
            if (n->kind == DeclKind::ComponentPart) continue;
            emitNested(n);
        }
        for (const Decl* n : d->nested) {
            if (n->kind != DeclKind::ComponentPart) continue;
            w.line(n->name == "Server" ? "/// Server-only part (X::Server): cell and editor worlds only, never replicated."
                                       : "/// Client-only part (X::Client).");
            emitStruct(w, n);
        }
        for (const Field& f : d->fields) {
            emitDocLines(w, f.doc);
            w.line(std::format("{} {}{};", cppTypeName(f.type), cppFieldName(f.name), memberInitializer(f)));
        }
        if (d->isReplicatedComponent()) {
            if (!d->fields.empty()) w.line();
            w.line("/// Replication dirty bits (bit i = kReplicatedFields[i]); set by Mut<C> (02 §4.4).");
            w.line("::helios::u64 _dirty = 0;");
            std::string list;
            for (const Field& f : d->fields) list += std::format("{}&{}::{}", list.empty() ? "" : ", ", d->cppPath.back(), cppFieldName(f.name));
            w.line(std::format("static constexpr auto kReplicatedFields = std::make_tuple({});", list));
        }
        if (!d->fields.empty() || d->isReplicatedComponent()) w.line();
        w.line(std::format("/// Field-wise equality (floats compare bitwise{}).", d->isReplicatedComponent() ? "; _dirty is ignored" : ""));
        w.line(std::format("[[nodiscard]] bool operator==(const {}& other) const noexcept;", d->cppPath.back()));
        w.close("};");
        w.line();
    }

    void emitMut(CodeWriter& w, const Decl* d) {
        const std::string name = declCpp(d);
        w.line();
        w.line(std::format("/// Named dirty-bit mutators for {} (replicate({})).", d->qualifiedName, repAudienceName(d->replicate)));
        w.line("template <>");
        w.open(std::format("class Mut<{}> : public detail::MutBase<{}> {{", name, name));
        w.dedent();
        w.line("public:");
        w.indent();
        w.line("using MutBase::MutBase;");
        std::string all;
        for (const Field& f : d->fields) {
            w.line(std::format("static constexpr ::helios::u64 k{} = 1ull << {};", pascalCase(f.name), f.repIndex));
            all += (all.empty() ? "" : " | ") + std::format("k{}", pascalCase(f.name));
        }
        w.line(std::format("static constexpr ::helios::u64 kAllFields = {};", all.empty() ? "0" : all));
        for (const Field& f : d->fields) {
            const std::string t = cppTypeName(f.type);
            const std::string fn = cppFieldName(f.name);
            const std::string P = pascalCase(f.name);
            w.line(std::format("/// Writes {} and marks it dirty if the value changed.", f.name));
            w.open(std::format("void set{}(const {}& value) {{", P, t));
            w.line(std::format("if (::helios::refl::valuesEqual(m_c->{}, value)) return;", fn));
            w.line(std::format("m_c->{} = value;", fn));
            w.line(std::format("markBits(k{});", P));
            w.close();
            w.line(std::format("/// In-place access to {}; marks it dirty.", f.name));
            w.open(std::format("{}& edit{}() noexcept {{", t, P));
            w.line(std::format("markBits(k{});", P));
            w.line(std::format("return m_c->{};", fn));
            w.close();
        }
        w.line("/// Mutable access to the whole component; marks every replicated field dirty.");
        w.open(std::format("{}& raw() noexcept {{", name));
        w.line("markBits(kAllFields);");
        w.line("return *m_c;");
        w.close();
        w.close("};");
    }

    // --- source --------------------------------------------------------------------------------
    std::string defaultFn(const Decl* d, const Field& f) {
        std::string s = "dflt_" + join(d->cppPath, "_") + "_" + f.name;
        return s;
    }

    std::string source(const SourceFile* f, const std::vector<Decl*>& decls) {
        CodeWriter w;
        w.line(std::format("// {} — generated by helios-schemac from {}. DO NOT EDIT.", cppSourcePath(f->logicalPath).substr(cppSourcePath(f->logicalPath).rfind('/') + 1), f->logicalPath));
        w.line(std::format("#include \"{}\"", cppHeaderPath(f->logicalPath).substr(cppHeaderPath(f->logicalPath).rfind('/') + 1)));
        w.line();
        w.line("#include <cstddef>");
        w.line();
        // operator==
        w.line(std::format("namespace {} {{", cppNamespace(f->ast.package)));
        w.line();
        for (const Decl* d : decls) {
            if (!d->isStructLike()) continue;
            const std::string local = join(d->cppPath, "::");
            w.line(std::format("bool {}::operator==(const {}& other) const noexcept {{", local, local));
            w.line(std::format("    return ::helios::refl::Codec<{}>::equals(*this, other);", declCpp(d)));
            w.line("}");
            w.line();
        }
        w.line(std::format("::helios::Result<void> {}(::helios::refl::TypeRegistry& registry) {{", registerFunctionName(f)));
        w.indent();
        std::string list;
        for (const Decl* d : decls) {
            if (d->isLockable()) list += std::format("\n        &::helios::refl::typeOf<{}>(),", declCpp(d));
        }
        if (list.empty()) {
            w.line("(void)registry;");
            w.line("return {};");
        } else {
            w.line(std::format("static const ::helios::refl::TypeInfo* const types[] = {{{}\n    }};", list));
            w.line("return registry.add(types);");
        }
        w.close();
        w.line();
        w.line(std::format("}} // namespace {}", cppNamespace(f->ast.package)));
        w.line();
        w.line("namespace helios::refl {");
        w.line();
        // Defaults and attribute tables.
        w.line("namespace {");
        w.line();
        AttrTables tables;
        tables.w = &w;
        std::map<const void*, std::string> attrNames;
        for (const Decl* d : decls) {
            if (!d->isLockable()) continue;
            for (const Field& fl : d->fields) {
                if (fl.defaultValue) {
                    w.line(std::format("const {}& {}() {{", cppTypeName(fl.type), defaultFn(d, fl)));
                    w.line(std::format("    static const {} value = {};", cppTypeName(fl.type), cppValueLiteral(*fl.defaultValue, fl.type)));
                    w.line("    return value;");
                    w.line("}");
                }
                attrNames[&fl] = tables.emit(fl.attrs, fl.type);
                if (!fl.was.empty()) {
                    std::string was;
                    for (const std::string& x : fl.was) was += cppQuote(x) + ", ";
                    w.line(std::format("constexpr std::string_view kWas_{}_{}[] = {{{}}};", join(d->cppPath, "_"), fl.name, was));
                }
            }
            attrNames[d] = tables.emit(d->attrs);
        }
        w.line();
        w.line("} // namespace");
        w.line();
        for (const Decl* d : decls) {
            if (!d->isLockable()) continue;
            if (d->isStructLike()) emitCodec(w, d);
            emitTypeInfo(w, d, attrNames);
        }
        w.line("} // namespace helios::refl");
        return w.take();
    }

    std::string defaultCheck(const Decl* d, const Field& f, const std::string& obj) {
        const std::string member = obj + "." + cppFieldName(f.name);
        if (f.defaultValue) return std::format("::helios::refl::valuesEqual({}, {}())", member, defaultFn(d, f));
        return std::format("::helios::refl::isDefaultValue({})", member);
    }

    void emitCodec(CodeWriter& w, const Decl* d) {
        const std::string T = declCpp(d);
        const std::string mu = "[[maybe_unused]] ";
        // isDefault
        w.open(std::format("bool Codec<{}>::isDefault({}const {}& v) noexcept {{", T, mu, T));
        if (d->fields.empty()) {
            w.line("return true;");
        } else {
            std::string expr;
            for (const Field& f : d->fields) expr += (expr.empty() ? "" : "\n        && ") + defaultCheck(d, f, "v");
            w.line("return " + expr + ";");
        }
        w.close();
        w.line();
        // equals
        w.open(std::format("bool Codec<{}>::equals({}const {}& a, {}const {}& b) noexcept {{", T, mu, T, mu, T));
        if (d->fields.empty()) {
            w.line("return true;");
        } else {
            std::string expr;
            for (const Field& f : d->fields) {
                const std::string m = cppFieldName(f.name);
                expr += (expr.empty() ? "" : "\n        && ") + std::format("::helios::refl::valuesEqual(a.{}, b.{})", m, m);
            }
            w.line("return " + expr + ";");
        }
        w.close();
        w.line();
        // writeFields
        w.open(std::format("void Codec<{}>::writeFields({}TaggedWriter& w, {}const {}& v) {{", T, mu, mu, T));
        for (const Field& f : d->fields)
            w.line(std::format("if (!{}) ::helios::refl::writeField(w, {}u, v.{});", defaultCheck(d, f, "v"), f.id, cppFieldName(f.name)));
        w.close();
        w.line();
        // readField
        w.open(std::format("Result<bool> Codec<{}>::readField({}TaggedReader& r, {}FieldTag tag, {}{}& v) {{", T, mu, mu, mu, T));
        if (d->fields.empty()) {
            w.line("return false;");
        } else {
            w.open("switch (tag.id) {");
            for (const Field& f : d->fields)
                w.line(std::format("case {}u: return ::helios::refl::readKnownField(r, tag.wire, v.{});", f.id, cppFieldName(f.name)));
            w.line("default: return false;");
            w.close();
        }
        w.close();
        w.line();
        // writeJsonFields
        w.open(std::format("void Codec<{}>::writeJsonFields({}JsonWriter& w, {}const {}& v) {{", T, mu, mu, T));
        for (const Field& f : d->fields) {
            w.open(std::format("if (!{}) {{", defaultCheck(d, f, "v")));
            w.line(std::format("w.key({});", cppQuote(f.name)));
            w.line(std::format("::helios::refl::writeJsonValue(w, v.{});", cppFieldName(f.name)));
            w.close();
        }
        w.close();
        w.line();
        // readJsonMember
        w.open(std::format("Result<bool> Codec<{}>::readJsonMember({}std::string_view key, {}JsonValue in, {}{}& v, {}ReadCtx& ctx) {{",
                           T, mu, mu, mu, T, mu));
        for (const Field& f : d->fields) {
            std::string cond = std::format("key == {}", cppQuote(f.name));
            for (const std::string& was : f.was) cond += std::format(" || key == {}", cppQuote(was));
            w.line(std::format("if ({}) return ::helios::refl::readKnownMember(in, v.{}, ctx);", cond, cppFieldName(f.name)));
        }
        w.line("return false;");
        w.close();
        w.line();
    }

    std::string typeFlags(const Decl* d) {
        std::vector<std::string> flags;
        if (d->isReplicatedComponent()) flags.push_back("Replicated");
        if (d->kind == DeclKind::ComponentPart) flags.push_back(d->name == "Server" ? "ServerPart" : "ClientPart");
        if (d->attr("server_only")) flags.push_back("ServerOnly");
        if (d->attr("client_only")) flags.push_back("ClientOnly");
        if (d->attr("authoring")) flags.push_back("Authoring");
        if (d->kind == DeclKind::Enum || d->kind == DeclKind::Flags) flags.push_back("InlineJson");
        if (d->isStructLike() && d->fields.empty()) flags.push_back("Unit");
        if (flags.empty()) return "::helios::refl::TypeFlags::None";
        std::string out;
        for (const std::string& f : flags) out += (out.empty() ? "" : " | ") + std::string("::helios::refl::TypeFlags::") + f;
        return out;
    }

    static std::string declKindCpp(const Decl* d) {
        switch (d->kind) {
        case DeclKind::Struct: return "Struct";
        case DeclKind::Component:
        case DeclKind::ComponentPart: return "Component";
        case DeclKind::Record: return "Record";
        case DeclKind::Event: return "Event";
        case DeclKind::Message: return "Message";
        case DeclKind::ViewModel: return "ViewModel";
        case DeclKind::Relation: return "Relation";
        case DeclKind::Rpc: return "Rpc";
        case DeclKind::Alternative: return "Alternative";
        default: return "None";
        }
    }

    std::string fieldFlags(const Decl* d, const Field& f) {
        std::vector<std::string> flags;
        if (f.serverOnly) flags.push_back("ServerOnly");
        if (f.clientOnly) flags.push_back("ClientOnly");
        if (f.editorOnly) flags.push_back("EditorOnly");
        if (f.replicated) flags.push_back("Replicated");
        if (f.predicted) flags.push_back("Predicted");
        if (f.type->kind == TypeKind::KeyedList || !f.keyedBy.empty()) flags.push_back("Keyed");
        if (f.attr("hidden")) flags.push_back("Hidden");
        if (f.attr("readonly")) flags.push_back("ReadOnly");
        if (f.attr("opaque")) flags.push_back("Opaque");
        if (f.attr("deprecated")) flags.push_back("Deprecated");
        (void)d;
        if (flags.empty()) return "::helios::refl::FieldFlags::None";
        std::string out;
        for (const std::string& x : flags) out += (out.empty() ? "" : " | ") + std::string("::helios::refl::FieldFlags::") + x;
        return out;
    }

    static std::string audienceOf(const Decl* d, const Field& f) {
        if (!f.replicated) return "None";
        switch (d->replicate) {
        case RepAudience::All: return "All";
        case RepAudience::Owner: return "Owner";
        case RepAudience::Server: return "Server";
        default: return "None";
        }
    }

    void emitTypeInfo(CodeWriter& w, const Decl* d, std::map<const void*, std::string>& attrNames) {
        const std::string T = declCpp(d);
        w.open(std::format("const TypeInfo& TypeOf<{}>::get() noexcept {{", T));
        std::string fieldsRef = "{}";
        std::string valuesRef = "{}";
        std::string altsRef = "{}";
        std::string elementFn = "nullptr";
        if (d->isStructLike() && !d->fields.empty()) {
            w.line("HELIOS_GEN_OFFSETOF_BEGIN");
            w.open("static const FieldInfo fields[] = {");
            for (const Field& f : d->fields) {
                w.open("{");
                w.line(std::format(".name = {},", cppQuote(f.name)));
                w.line(std::format(".id = {}u,", f.id));
                w.line(std::format(".offset = static_cast<::helios::u32>(offsetof({}, {})),", T, cppFieldName(f.name)));
                w.line(std::format(".typeFn = &TypeOf<{}>::get,", cppTypeName(f.type)));
                w.line(std::format(".flags = {},", fieldFlags(d, f)));
                w.line(std::format(".audience = ::helios::refl::Audience::{},", audienceOf(d, f)));
                w.line(std::format(".repIndex = {},", f.replicated ? f.repIndex : 0xFF));
                if (!f.was.empty()) w.line(std::format(".was = kWas_{}_{},", join(d->cppPath, "_"), f.name));
                if (f.defaultValue) {
                    w.line(std::format(".defaultValue = &{}(),", defaultFn(d, f)));
                    w.line(std::format(".defaultJson = {},", cppQuote(f.defaultValue->json)));
                }
                w.line(std::format(".attrs = {},", attrNames[&f]));
                w.line(std::format(".doc = {},", cppQuote(f.doc)));
                w.close("},");
            }
            w.close("};");
            w.line("HELIOS_GEN_OFFSETOF_END");
            fieldsRef = "fields";
        }
        if (d->kind == DeclKind::Enum || d->kind == DeclKind::Flags) {
            w.open("static constexpr EnumValue values[] = {");
            for (const EnumVal& v : d->values) w.line(std::format("{{{}, {}, {}}},", cppQuote(v.name), v.value == std::numeric_limits<i64>::min() ? "(-9223372036854775807ll - 1)" : std::to_string(v.value) + "ll", cppQuote(v.doc)));
            w.close("};");
            valuesRef = "values";
            elementFn = std::format("&TypeOf<{}>::get", primCpp(d->underlying));
        }
        if (d->kind == DeclKind::Variant) {
            w.open("static const VariantAlt alternatives[] = {");
            for (const Alternative& a : d->alternatives)
                w.line(std::format("{{{}, {}u, &TypeOf<{}>::get}},", cppQuote(a.name), a.id, declCpp(a.type)));
            w.close("};");
            altsRef = "alternatives";
        }
        const std::string kind = d->kind == DeclKind::Enum ? "Enum" : d->kind == DeclKind::Flags ? "Flags" : d->kind == DeclKind::Variant ? "Variant" : "Struct";
        const std::string wire = (d->kind == DeclKind::Enum || d->kind == DeclKind::Flags) ? "Varint" : "Len";
        w.open("static const TypeInfo info{");
        w.line(std::format(".qualifiedName = {},", cppQuote(d->qualifiedName)));
        w.line(std::format(".name = {},", cppQuote(d->name)));
        w.line(std::format(".id = {:#010x}u,", d->typeId));
        w.line(std::format(".size = static_cast<::helios::u32>(sizeof({})),", T));
        w.line(std::format(".align = static_cast<::helios::u32>(alignof({})),", T));
        w.line(std::format(".kind = Kind::{},", kind));
        w.line(std::format(".wire = WireType::{},", wire));
        w.line(std::format(".decl = DeclKind::{},", declKindCpp(d)));
        w.line(std::format(".flags = {},", typeFlags(d)));
        w.line(std::format(".version = {}u,", d->version));
        w.line(std::format(".layoutHash = {:#018x}ull,", d->layoutHash));
        w.line(std::format(".fields = {},", fieldsRef));
        w.line(std::format(".enumValues = {},", valuesRef));
        w.line(std::format(".alternatives = {},", altsRef));
        w.line(std::format(".elementFn = {},", elementFn));
        w.line(std::format(".ops = &makeOps<{}>(),", T));
        w.line(std::format(".attrs = {},", attrNames.contains(d) ? attrNames[d] : std::string("{}")));
        w.line(std::format(".doc = {},", cppQuote(d->doc)));
        w.close("};");
        w.line("return info;");
        w.close();
        w.line();
    }

    const Schema& S;
    const CompileOptions& O;
};

} // namespace

std::vector<OutputFile> generateCpp(const Schema& schema, const CompileOptions& options) {
    CppGenerator g(schema, options);
    return g.run();
}

} // namespace helios::schemac
