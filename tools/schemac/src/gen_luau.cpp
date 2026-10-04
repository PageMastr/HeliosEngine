// `--emit luau` (02 §3.5, §7.4): what the Luau script host needs from the schema.
//   * <file>.luau.gen.h / .luau.gen.cpp, next to the C++ output: per scriptlib, the abstract C++
//     interface its hand-written functions implement and bind<Lib>(), which registers them on an
//     engine/script Binder. The glue charges each fn's fuel (cost and `each` per element of an `of`
//     argument before the call, `each` per result element right after it) and converts arguments
//     and results without ever running Luau code (raw table access only), within a per-call budget
//     of converted values and string bytes, never interning script strings as Names;
//   * schema.d.luau (--luau-out): Luau declarations for luau-lsp, each fn's charge in its doc;
//   * fuel_costs.defaults.json (--luau-out): the schema's (cost, each) per binding id, the defaults
//     that `--calibrate-fuel` replaces (02 §7.4).
// Binding ids are the fns' lock ids (lock entries of kind "fn").

#include <algorithm>
#include <format>
#include <map>
#include <set>

#include "code_writer.h"
#include "generators.h"
#include "go_names.h"
#include "json_out.h"
#include "text.h"

namespace helios::schemac {

namespace {

/// Luau's reserved words (`continue`, `export`, `type` and `typeof` are contextual and stay usable).
constexpr std::string_view kLuauKeywords[] = {"and", "break", "do",     "else",   "elseif", "end",  "false", "for",   "function", "if", "in",
                                              "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
/// Globals and types of the Luau standard library and of engine/script's helios.d.luau.
constexpr std::string_view kReservedNames[] = {"WorldPos", "Task", "ScriptError", "task", "wait", "require", "print", "math",
                                               "string",   "table", "buffer", "bit32", "utf8", "coroutine", "vector", "os",
                                               "debug",    "number", "boolean", "any", "unknown", "never", "thread", "userdata"};

bool isLuauKeyword(std::string_view s) { return std::find(std::begin(kLuauKeywords), std::end(kLuauKeywords), s) != std::end(kLuauKeywords); }
bool isReserved(std::string_view s) { return std::find(std::begin(kReservedNames), std::end(kReservedNames), s) != std::end(kReservedNames); }

std::string cppNamespace(const std::string& pkg) {
    std::string out;
    for (const char c : pkg) out += c == '.' ? std::string("::") : std::string(1, c);
    return out;
}

std::string basePath(const std::string& logical) {
    const usize dot = logical.rfind(".hschema");
    return dot == std::string::npos ? logical : logical.substr(0, dot);
}

/// Luau name of a declaration (flattened like Go names: ShipHullDef.Handling -> ShipHullDefHandling).
std::string luauDeclName(const Decl* d) { return d->kind == DeclKind::Record ? goRecordRefName(d) : d->goName; }

bool isContainerLike(const Type* t) {
    return t->kind == TypeKind::List || t->kind == TypeKind::Set || t->kind == TypeKind::Array ||
           (t->kind == TypeKind::Prim && (t->prim == Prim::String || t->prim == Prim::Name));
}

/// Doc comment lines ("" -> none), each prefixed.
void docLines(CodeWriter& w, const std::string& doc, std::string_view prefix) {
    usize start = 0;
    while (start < doc.size()) {
        const usize nl = doc.find('\n', start);
        const std::string line = doc.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        w.line(std::string(prefix) + line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

std::string costText(const Decl* fn) {
    const ScriptCost& c = fn->cost;
    std::string text = std::format("Fuel: {}", c.base);
    if (c.each != 0) {
        text += std::format(" + {} per {}", c.each, c.of == "result" ? "result element" : "item of '" + c.of + "'");
    }
    if (c.pure) text += "; pure";
    return text;
}

class LuauGenerator {
public:
    LuauGenerator(const Schema& s, const CompileOptions& o, DiagnosticEngine& d) : S(s), O(o), D(d) {}

    std::vector<OutputFile> run() {
        std::vector<OutputFile> out;
        const usize errorsBefore = D.errorCount();
        for (const Decl* d : S.decls) {
            if (d->emitted && d->kind == DeclKind::ScriptLib) checkLib(d);
        }
        if (D.errorCount() != errorsBefore) return out;
        for (const auto& fp : S.files) {
            if (!fp->generate) continue;
            std::vector<const Decl*> libs;
            for (const Decl* d : fp->decls) {
                if (d->kind == DeclKind::ScriptLib) libs.push_back(d);
            }
            const std::string base = basePath(fp->logicalPath);
            out.push_back(OutputFile{joinCpp(base + ".luau.gen.h"), header(fp.get(), libs)});
            out.push_back(OutputFile{joinCpp(base + ".luau.gen.cpp"), source(fp.get(), libs)});
        }
        out.push_back(OutputFile{joinLuau("schema.d.luau"), definitions()});
        out.push_back(OutputFile{joinLuau("fuel_costs.defaults.json"), fuelCosts()});
        return out;
    }

private:
    std::string joinCpp(const std::string& rel) const { return O.cppOut.empty() || O.cppOut == "." ? rel : O.cppOut + "/" + rel; }
    std::string joinLuau(const std::string& rel) const { return O.luauOut.empty() || O.luauOut == "." ? rel : O.luauOut + "/" + rel; }

    // --- validation ----------------------------------------------------------------------------
    /// Why `t` cannot cross the Luau boundary ("" if it can). `inStruct`: a field of a struct passed by
    /// value, where a WorldPos would lose its frame (only signature-level positions carry one) and, for
    /// arguments (`input`), a Name would intern script input into the process-wide Name table.
    std::string unsupported(const Type* t, bool inStruct, bool input, std::set<const Decl*>& seen) {
        switch (t->kind) {
        case TypeKind::Prim:
            if (t->prim == Prim::Name && inStruct && input)
                return "a Name inside a struct argument (converting it would intern script input into the process-wide Name table; "
                       "use string)";
            return {};
        case TypeKind::Builtin:
            switch (t->builtin) {
            case Builtin::Vec3f:
            case Builtin::EntityId:
            case Builtin::Duration:
            case Builtin::Tick:
            case Builtin::LocString:
            case Builtin::TagQuery:
            case Builtin::HxlExpr: return {};
            case Builtin::WorldPos: return inStruct ? "a WorldPos inside a struct (it would lose its frame; pass it as a parameter)" : "";
            default: return std::format("'{}'", t->signature);
            }
        case TypeKind::Enum:
        case TypeKind::RecordRef: return {};
        case TypeKind::Struct: {
            if (!seen.insert(t->decl).second) return {};
            for (const Field& f : t->decl->fields) {
                if (!f.type) continue;
                if (isLuauKeyword(f.name)) return std::format("field '{}' of '{}' (a Luau keyword)", f.name, t->decl->qualifiedName);
                std::string why = unsupported(f.type, true, input, seen);
                if (!why.empty()) return why + std::format(" in field '{}' of '{}'", f.name, t->decl->qualifiedName);
            }
            return {};
        }
        case TypeKind::List:
        case TypeKind::Set:
        case TypeKind::Array:
        case TypeKind::Optional: return unsupported(t->element, inStruct, input, seen);
        default: return std::format("'{}'", t->signature);
        }
    }

    void checkType(const Decl* fn, const Type* t, SourceLoc loc, const std::string& what, bool input) {
        std::set<const Decl*> seen;
        const std::string why = unsupported(t, false, input, seen);
        if (!why.empty()) {
            D.error(loc, std::format("{} of fn '{}' cannot cross the Luau boundary: {} is not supported by --emit luau (supported: bool, "
                                     "integers, floats, string, Name, vec3f, WorldPos, EntityId, Duration, Tick, LocString, TagQuery, "
                                     "HxlExpr, enums, record refs, structs of those, and list, set, T[N] and T? of them)",
                                     what, fn->name, why));
            return;
        }
        collectNames(t, loc, input);
    }

    /// Luau name of the table type of struct `d` as an argument (`input`: every field optional,
    /// because the glue defaults missing fields) or as a result.
    static std::string structTypeName(const Decl* d, bool input) { return luauDeclName(d) + (input ? "Input" : ""); }

    /// Registers the Luau type names `t` needs and reports collisions.
    void collectNames(const Type* t, SourceLoc loc, bool input) {
        const Decl* d = nullptr;
        if (t->kind == TypeKind::Enum || t->kind == TypeKind::Struct || t->kind == TypeKind::RecordRef) d = t->decl;
        if (t->element) collectNames(t->element, loc, input);
        if (!d) return;
        const bool isStruct = t->kind == TypeKind::Struct;
        const std::string name = isStruct ? structTypeName(d, input) : luauDeclName(d);
        auto [it, fresh] = m_names.emplace(name, NameUse{d, t->kind == TypeKind::RecordRef, isStruct && input});
        if (!fresh) {
            if (it->second.decl != d || it->second.ref != (t->kind == TypeKind::RecordRef) || it->second.input != (isStruct && input))
                D.error(loc, std::format("'{}' and '{}' both become the Luau type '{}'", it->second.decl->qualifiedName, d->qualifiedName, name));
            return;
        }
        if (isReserved(name) || name == "EntityId")
            D.error(loc, std::format("'{}' becomes the Luau type '{}', which the script host already declares", d->qualifiedName, name));
        if (isStruct) {
            for (const Field& f : d->fields) {
                if (f.type) collectNames(f.type, loc, input);
            }
        }
    }

    void checkLib(const Decl* lib) {
        if (isReserved(lib->name) || isLuauKeyword(lib->name))
            D.error(lib->loc, std::format("scriptlib '{}' would replace a Luau or script-host global of the same name", lib->name));
        if (auto [it, fresh] = m_libs.emplace(lib->name, lib); !fresh)
            D.error(lib->loc, std::format("scriptlibs '{}' and '{}' both become the Luau global '{}'", it->second->qualifiedName,
                                          lib->qualifiedName, lib->name));
        std::set<std::string> methods;
        std::map<std::string, const Decl*> constants;
        for (const Decl* fn : lib->methods) methods.insert(cppFieldName(fn->name));
        for (const Decl* fn : lib->methods) {
            if (auto [it, fresh] = constants.emplace(pascalCase(fn->name), fn); !fresh)
                D.error(fn->loc, std::format("fns '{}' and '{}' both get the C++ constant k{}Binding", it->second->name, fn->name,
                                             pascalCase(fn->name)));
            if (isLuauKeyword(fn->name)) D.error(fn->loc, std::format("fn name '{}' is a Luau keyword", fn->name));
            if (fn->cost.base > 0xFFFFFFFFull || fn->cost.each > 0xFFFFFFFFull / 1000)
                D.error(fn->loc, std::format("the fuel charge of fn '{}' exceeds the script host's range (cost ≤ 4294967295, each ≤ 4294967)",
                                             fn->name));
            for (const Field& p : fn->fields) {
                if (isLuauKeyword(p.name)) D.error(p.loc, std::format("parameter name '{}' is a Luau keyword", p.name));
                if (p.name == "L") D.error(p.loc, "parameter name 'L' is reserved for the lua_State of the generated C++ interface");
                if (p.type) checkType(fn, p.type, p.loc, std::format("parameter '{}'", p.name), true);
            }
            if (fn->result) checkType(fn, fn->result, fn->loc, "the result", false);
            if (needsItemHook(fn) && methods.contains(itemHookName(fn)))
                D.error(fn->loc, std::format("fn '{}' needs the item-count hook '{}', which is also the name of a fn", fn->name, itemHookName(fn)));
        }
    }

    // --- fn helpers ----------------------------------------------------------------------------
    static const Field* ofParam(const Decl* fn) {
        if (fn->cost.of.empty() || fn->cost.of == "result") return nullptr;
        for (const Field& p : fn->fields) {
            if (p.name == fn->cost.of) return &p;
        }
        return nullptr;
    }
    static int ofIndex(const Decl* fn) {
        for (usize i = 0; i < fn->fields.size(); ++i) {
            if (fn->fields[i].name == fn->cost.of) return static_cast<int>(i) + 1;
        }
        return 0;
    }
    /// The script host counts the items of a list, string or integer argument itself (FuelCost::itemsArg);
    /// any other `of` argument (a record ref such as a PrefabRef, 02 §7.4) needs a hand-written count.
    static bool needsItemHook(const Decl* fn) {
        const Field* p = ofParam(fn);
        if (!p || !p->type) return false;
        const Type* t = p->type;
        return !(isContainerLike(t) || (t->kind == TypeKind::Prim && isIntegerPrim(t->prim)));
    }
    static std::string itemHookName(const Decl* fn) { return fn->name + "ItemCount"; }
    static std::string methodName(const Decl* fn) { return cppFieldName(fn->name); }

    /// C++ type of a value at the boundary. At signature level (`member` false) WorldPos is the script
    /// host's frame-carrying FramePos and Name is std::string (never interned from script input; sets
    /// of names order lexically); inside structs (`member`) values have their generated types.
    static std::string sigCpp(const Type* t, bool member = false) {
        if (member) return cppTypeName(t);
        switch (t->kind) {
        case TypeKind::Prim: return t->prim == Prim::Name ? "std::string" : cppTypeName(t);
        case TypeKind::Builtin:
            if (t->builtin == Builtin::WorldPos) return "::helios::FramePos";
            return cppTypeName(t);
        case TypeKind::List: return "std::vector<" + sigCpp(t->element) + ">";
        case TypeKind::Set: return "std::set<" + sigCpp(t->element) + ">";
        case TypeKind::Array: return "std::array<" + sigCpp(t->element) + ", " + std::to_string(t->arraySize) + ">";
        case TypeKind::Optional: return "std::optional<" + sigCpp(t->element) + ">";
        case TypeKind::RecordRef: return "::" + cppNamespace(t->decl->package) + "::" + goRecordRefName(t->decl); // the C++ alias
        default: return cppTypeName(t);
        }
    }
    /// True if sigCpp(t, member) depends on `member` (a Name or WorldPos outside any struct).
    static bool contextual(const Type* t) {
        for (; t; t = t->kind == TypeKind::Struct ? nullptr : t->element) {
            if ((t->kind == TypeKind::Prim && t->prim == Prim::Name) || (t->kind == TypeKind::Builtin && t->builtin == Builtin::WorldPos))
                return true;
        }
        return false;
    }
    static bool byValue(const Type* t) {
        return t->kind == TypeKind::Prim ? t->prim != Prim::String && t->prim != Prim::Name
                                         : t->kind == TypeKind::Enum || t->kind == TypeKind::RecordRef ||
                                               (t->kind == TypeKind::Builtin &&
                                                (t->builtin == Builtin::EntityId || t->builtin == Builtin::Duration || t->builtin == Builtin::Tick ||
                                                 t->builtin == Builtin::Vec3f));
    }
    /// The @max of a field or parameter (0 = none).
    static u64 maxOf(const Field& f) {
        u64 n = 0;
        if (const Attr* m = f.attr("max"); m && !m->args.empty()) parseSchemaUnsigned(m->args[0].value, n);
        return n;
    }
    static std::string paramDecl(const Field& p) {
        return byValue(p.type) ? sigCpp(p.type) + " " + cppFieldName(p.name) : "const " + sigCpp(p.type) + "& " + cppFieldName(p.name);
    }
    std::string signature(const Decl* fn) const {
        std::string s = "lua_State* L";
        for (const Field& p : fn->fields) s += ", " + paramDecl(p);
        return s;
    }

    // --- C++ header ----------------------------------------------------------------------------
    std::string header(const SourceFile* f, const std::vector<const Decl*>& libs) {
        CodeWriter w;
        const std::string base = basePath(f->logicalPath);
        w.line(std::format("// {}.luau.gen.h — generated by helios-schemac (--emit luau) from {}. DO NOT EDIT.",
                           base.substr(base.rfind('/') + 1), f->logicalPath));
        w.line("// Luau glue of the file's scriptlibs (02 §3.1, §7.4; tools/schemac/README.md \"Generated Luau\").");
        w.line("#pragma once");
        w.line();
        w.line("#include <array>");
        w.line("#include <optional>");
        w.line("#include <set>");
        w.line("#include <string>");
        w.line("#include <string_view>");
        w.line("#include <vector>");
        w.line();
        w.line("#include \"helios/script/binding.h\"");
        w.line(std::format("#include \"{}\"", cppHeaderPath(f->logicalPath)));
        w.line();
        w.line(std::format("namespace {} {{", cppNamespace(f->ast.package)));
        for (const Decl* lib : libs) {
            w.line();
            w.line(std::format("/// Hand-written C++ side of scriptlib {} ({}). Implement every function; bind{}() calls", lib->name,
                               f->logicalPath, lib->name));
            w.line("/// them from Luau after the glue charged the fuel and converted the arguments.");
            docLines(w, lib->doc, "/// ");
            w.line("/// Threading: runs on the thread that owns the VM, inside a binding (helios/script/binding.h): an");
            w.line("/// implementation may raise Luau errors (helios::script::raiseError) and calls back into Luau only through");
            w.line("/// helios::script::callLuau.");
            w.open(std::format("class {} {{", lib->name));
            w.dedent();
            w.line("public:");
            w.indent();
            w.line(std::format("virtual ~{}() = default;", lib->name));
            w.line();
            w.line("/// Per-call caps of the generated argument conversion (hostile input: a table referenced from many places");
            w.line("/// converts once per reference). A call over a cap fails with a script error before converting further. Set");
            w.line("/// them before the VM runs scripts. 02 §7.4 and 04 §10.2 give no default; these keep one call's conversion");
            w.line("/// well under 1 ms.");
            w.open("struct GlueLimits {");
            w.line("::helios::u32 maxValues = 8192;         ///< values converted per call: numbers, strings, tables and elements");
            w.line("::helios::usize maxStringBytes = 262144; ///< string bytes converted per call (256 KiB)");
            w.close("};");
            w.line("GlueLimits glueLimits;");
            for (const Decl* fn : lib->methods) {
                w.line();
                docLines(w, fn->doc, "/// ");
                w.line(std::format("/// {}. Realms: {}. Binding id {:#010x}.", costText(fn), join(fn->cost.realms, ", "), fn->typeId));
                const std::string ret = fn->result ? sigCpp(fn->result) : "void";
                w.line(std::format("virtual {} {}({}) = 0;", ret, methodName(fn), signature(fn)));
                if (needsItemHook(fn)) {
                    const Field* p = ofParam(fn);
                    w.line(std::format("/// Items of '{}' that {}'s `each` charge counts (charged before the call).", p->name, fn->name));
                    w.line(std::format("virtual ::helios::u64 {}(lua_State* L, {}) = 0;", itemHookName(fn), paramDecl(*p)));
                }
            }
            w.line();
            for (const Decl* fn : lib->methods)
                w.line(std::format("static constexpr ::helios::u32 k{}Binding = {:#010x}u; ///< lock id of {}", pascalCase(fn->name), fn->typeId,
                                   fn->qualifiedName));
            w.close("};");
            w.line();
            w.line(std::format("/// Registers the {} functions whose @realm includes `realm` as the Luau library `{}` (call inside",
                               lib->name, lib->name));
            w.line("/// the ScriptVm::create registrar). `impl` must outlive the VM. `realm` must be \"server\" on a HostProfile::Cell VM,");
            w.line("/// \"client\" on a Client VM and \"editor\" on an Editor VM; anything else is InvalidArgument and registers nothing.");
            w.line("/// Returns how many functions were registered.");
            w.line(std::format("::helios::Result<::helios::u32> bind{}(::helios::script::Binder& binder, {}& impl, std::string_view realm);",
                               lib->name, lib->name));
        }
        w.line();
        w.line(std::format("}} // namespace {}", cppNamespace(f->ast.package)));
        return w.take();
    }

    // --- C++ source ----------------------------------------------------------------------------
    static constexpr u8 kPush = 1; ///< C++ -> Luau (results)
    static constexpr u8 kCheck = 2; ///< Luau -> C++ (arguments)

    struct CodecKey {
        const Type* type;
        bool member; ///< a struct field's generated C++ type (only for contextual() types)
        auto operator<=>(const CodecKey&) const = default;
    };

    /// Index of the codec of `t`, noting the directions `dirs` it needs (and those of its parts).
    usize codecOf(const Type* t, u8 dirs, bool member) {
        const CodecKey key{t, member && contextual(t)};
        auto it = m_codecIndex.find(key);
        if (it == m_codecIndex.end()) {
            it = m_codecIndex.emplace(key, m_codecs.size()).first;
            m_codecs.push_back(key);
            m_dirs.push_back(0);
        }
        const usize index = it->second;
        if ((m_dirs[index] & dirs) == dirs) return index;
        m_dirs[index] |= dirs;
        if (t->element) codecOf(t->element, dirs, key.member);
        if (t->kind == TypeKind::Struct) {
            for (const Field& f : t->decl->fields) {
                if (f.type) codecOf(f.type, dirs, true);
            }
        }
        return index;
    }

    static std::string typeLabel(const Type* t, bool input) {
        switch (t->kind) {
        case TypeKind::Enum:
        case TypeKind::RecordRef: return luauDeclName(t->decl);
        case TypeKind::Struct: return structTypeName(t->decl, input);
        case TypeKind::Optional: return typeLabel(t->element, input) + "?";
        case TypeKind::List:
        case TypeKind::Set:
        case TypeKind::Array: return "{" + typeLabel(t->element, input) + "}";
        case TypeKind::Builtin:
            if (t->builtin == Builtin::Vec3f) return "vector";
            if (t->builtin == Builtin::WorldPos || t->builtin == Builtin::EntityId) return std::string(builtinName(t->builtin));
            if (t->builtin == Builtin::Duration || t->builtin == Builtin::Tick) return "number";
            return "string";
        case TypeKind::Prim: return t->prim == Prim::Bool ? "boolean" : (t->prim == Prim::String || t->prim == Prim::Name) ? "string" : "number";
        default: return t->signature;
        }
    }

    void emitCodec(CodeWriter& w, usize i) {
        const CodecKey key = m_codecs[i];
        const std::string C = sigCpp(key.type, key.member);
        w.line(std::format("// {}{}", key.type->signature, key.member ? " (struct field)" : ""));
        if (m_dirs[i] & kPush) emitPush(w, key, i, C);
        if (m_dirs[i] & kCheck) emitCheck(w, key, i, C, cppQuote(typeLabel(key.type, true)));
        w.line();
    }

    /// C++ expression of the length @max bounds: bytes of a string or text builtin, elements of a container.
    static std::string sizeExpr(const Type* t, const std::string& v, bool member) {
        if (t->kind == TypeKind::Builtin && t->builtin == Builtin::LocString) return v + ".key.size()";
        if (t->kind == TypeKind::Builtin && (t->builtin == Builtin::TagQuery || t->builtin == Builtin::HxlExpr)) return v + ".text.size()";
        return t->kind == TypeKind::Prim && t->prim == Prim::Name && member ? v + ".view().size()" : v + ".size()";
    }

    void emitPush(CodeWriter& w, const CodecKey& key, usize i, const std::string& C) {
        const Type* t = key.type;
        w.open(std::format("void push{}(lua_State* L, const {}& v, int depth) {{", i, C));
        switch (t->kind) {
        case TypeKind::Prim:
            w.line("(void)depth;");
            if (t->prim == Prim::Bool) {
                w.line("lua_pushboolean(L, v ? 1 : 0);");
            } else if (t->prim == Prim::String || (t->prim == Prim::Name && !key.member)) {
                w.line("lua_pushlstring(L, v.data(), v.size());");
            } else if (t->prim == Prim::Name) {
                w.line("const std::string_view s = v.view();");
                w.line("lua_pushlstring(L, s.data(), s.size());");
            } else if (isFloatPrim(t->prim)) {
                w.line("lua_pushnumber(L, static_cast<double>(v));");
            } else {
                w.line("pushInteger(L, v);");
            }
            break;
        case TypeKind::Builtin:
            w.line("(void)depth;");
            switch (t->builtin) {
            case Builtin::Vec3f: w.line("lua_pushvector(L, v.x, v.y, v.z);"); break;
            case Builtin::WorldPos: w.line("::helios::script::pushWorldPos(L, v);"); break;
            case Builtin::EntityId: w.line("pushId(L, v.value, kEntityIdTag);"); break;
            case Builtin::Duration: w.line("lua_pushnumber(L, static_cast<double>(v.nanos) / 1e9);"); break;
            case Builtin::Tick: w.line("pushInteger(L, v);"); break;
            case Builtin::LocString: w.line("lua_pushlstring(L, v.key.data(), v.key.size());"); break;
            default: w.line("lua_pushlstring(L, v.text.data(), v.text.size());"); break;
            }
            break;
        case TypeKind::RecordRef:
            w.line("(void)depth;");
            w.line("pushId(L, v.id, kRecordRefTag);");
            break;
        case TypeKind::Enum:
            w.line("(void)depth;");
            w.open("switch (v) {");
            for (const EnumVal& e : t->decl->values)
                w.line(std::format("case {}::{}: lua_pushliteral(L, {}); return;", cppTypeName(t), cppFieldName(e.name), cppQuote(e.name)));
            w.close("}");
            w.line(std::format("fail(L, std::format(\"invalid {} value {{}}\", static_cast<long long>(v)));", t->decl->name));
            break;
        case TypeKind::Struct:
            w.line("enter(L, depth);");
            w.line(std::format("lua_createtable(L, 0, {});", t->decl->fields.size()));
            for (const Field& f : t->decl->fields) {
                if (!f.type) continue;
                const std::string v = "v." + cppFieldName(f.name);
                if (const u64 max = maxOf(f)) { // a field's @max bounds results as it bounds arguments
                    const bool opt = f.type->kind == TypeKind::Optional;
                    const Type* inner = opt ? f.type->element : f.type;
                    const std::string size = opt ? std::format("({0} ? {1} : 0)", v, sizeExpr(inner, "(*" + v + ")", true)) : sizeExpr(inner, v, true);
                    w.line(std::format("if ({0} > {1}u) fail(L, std::format(\"result field '{2}.{3}' has {{}} elements or bytes; its @max is {1}\", {0}));",
                                       size, max, t->decl->name, f.name));
                }
                w.line(std::format("push{}(L, {}, depth + 1);", codecOf(f.type, kPush, true), v));
                w.line(std::format("lua_rawsetfield(L, -2, {});", cppQuote(f.name)));
            }
            break;
        case TypeKind::Optional:
            w.open("if (!v) {");
            w.line("lua_pushnil(L);");
            w.line("return;");
            w.close();
            w.line(std::format("push{}(L, *v, depth);", codecOf(t->element, kPush, key.member)));
            break;
        default: // list, set, array
            w.line("enter(L, depth);");
            w.line("lua_createtable(L, static_cast<int>(v.size()), 0);");
            w.line("int n = 0;");
            if (t->kind == TypeKind::Set && key.member && t->element->kind == TypeKind::Prim && t->element->prim == Prim::Name) {
                // A generated struct's std::set<Name> orders by intern id, which depends on process history: push lexically.
                w.line("std::vector<std::string_view> sorted;");
                w.line("sorted.reserve(v.size());");
                w.line("for (const auto& e : v) sorted.push_back(e.view());");
                w.line("std::sort(sorted.begin(), sorted.end());");
                w.open("for (const std::string_view e : sorted) {");
                w.line("lua_pushlstring(L, e.data(), e.size());");
            } else {
                w.open("for (const auto& e : v) {");
                w.line(std::format("push{}(L, e, depth + 1);", codecOf(t->element, kPush, key.member)));
            }
            w.line("lua_rawseti(L, -2, ++n);");
            w.close();
            break;
        }
        w.close();
    }

    /// Checks the @max of a field or parameter on the raw value at `idx` (a string's bytes or a table's
    /// length) before anything is converted.
    static void emitMaxCheck(CodeWriter& w, const Field& f, const std::string& idx, const std::string& name) {
        if (const u64 max = maxOf(f)) w.line(std::format("checkMax(c, {}, {}u, {});", idx, max, cppQuote(name)));
    }

    void emitCheck(CodeWriter& w, const CodecKey& key, usize i, const std::string& C, const std::string& label) {
        const Type* t = key.type;
        w.open(std::format("{} check{}(Ctx& c, int idx) {{", C, i));
        if (t->kind != TypeKind::Optional) w.line("take(c, 1);");
        switch (t->kind) {
        case TypeKind::Prim:
            if (t->prim == Prim::Bool) {
                w.line("if (lua_type(c.L, idx) != LUA_TBOOLEAN) badType(c, idx, \"boolean\");");
                w.line("return lua_toboolean(c.L, idx) != 0;");
            } else if (t->prim == Prim::String || t->prim == Prim::Name) {
                w.line("return std::string(checkString(c, idx));");
            } else if (t->prim == Prim::F32) {
                w.line("return checkF32(c, idx);");
            } else if (t->prim == Prim::F64) {
                w.line("return checkNumber(c, idx);");
            } else {
                w.line(std::format("return checkInteger<{}>(c, idx, {});", C, cppQuote(primName(t->prim))));
            }
            break;
        case TypeKind::Builtin:
            switch (t->builtin) {
            case Builtin::Vec3f:
                w.line("const float* p = lua_tovector(c.L, idx);");
                w.line("if (!p) badType(c, idx, \"vector\");");
                w.line("checkFiniteVector(c, p);");
                w.line("return ::helios::Vec3(p[0], p[1], p[2]);");
                break;
            case Builtin::WorldPos:
                w.line("if (!::helios::script::toWorldPos(c.L, idx)) badType(c, idx, \"WorldPos\");");
                w.line("return ::helios::script::checkWorldPos(c.L, idx);");
                break;
            case Builtin::EntityId: w.line("return ::helios::refl::EntityId(checkId(c, idx, kEntityIdTag, \"EntityId\"));"); break;
            case Builtin::Duration: w.line("return checkDuration(c, idx);"); break;
            case Builtin::Tick: w.line("return checkInteger<::helios::u64>(c, idx, \"Tick\");"); break;
            case Builtin::LocString: w.line("return ::helios::refl::LocString{std::string(checkString(c, idx))};"); break;
            default: w.line(std::format("return {}{{std::string(checkString(c, idx))}};", C)); break;
            }
            break;
        case TypeKind::RecordRef: w.line(std::format("return {}(checkId(c, idx, kRecordRefTag, {}));", C, label)); break;
        case TypeKind::Enum: {
            w.line("const std::string_view s = checkString(c, idx);");
            std::string names;
            for (const EnumVal& e : t->decl->values) {
                w.line(std::format("if (s == {}) return {}::{};", cppQuote(e.name), C, cppFieldName(e.name)));
                names += (names.empty() ? "" : ", ") + e.name;
            }
            w.line(std::format("fail(c.L, std::format(\"{{}}: '{{}}' is not a {} (expected one of {})\", c.what, s));", t->decl->name, names));
            break;
        }
        case TypeKind::Struct:
            w.line("const Nest nest(c);");
            w.line(std::format("if (lua_type(c.L, idx) != LUA_TTABLE) badType(c, idx, {});", label));
            w.line("idx = lua_absindex(c.L, idx);");
            w.line(std::format("{} out{{}};", C));
            for (const Field& f : t->decl->fields) {
                if (!f.type) continue;
                w.open(std::format("if (lua_rawgetfield(c.L, idx, {}) != LUA_TNIL) {{", cppQuote(f.name)));
                emitMaxCheck(w, f, "-1", t->decl->name + "." + f.name);
                w.line(std::format("out.{} = check{}(c, -1);", cppFieldName(f.name), codecOf(f.type, kCheck, true)));
                w.close();
                w.line("lua_pop(c.L, 1);");
            }
            w.line("return out;");
            break;
        case TypeKind::Optional:
            // A nil costs a value too: otherwise a list<T?> of nils would loop over elements the cap
            // never sees (list<list<u32?>> converted cap^2/8 elements in one call).
            w.open("if (lua_isnoneornil(c.L, idx)) {");
            w.line("take(c, 1);");
            w.line("return std::nullopt;");
            w.close();
            w.line(std::format("return check{}(c, idx);", codecOf(t->element, kCheck, key.member)));
            break;
        default: { // list, set, array
            w.line("const Nest nest(c);");
            w.line(std::format("if (lua_type(c.L, idx) != LUA_TTABLE) badType(c, idx, {});", label));
            w.line("idx = lua_absindex(c.L, idx);");
            w.line("const int n = lua_objlen(c.L, idx);");
            if (t->kind == TypeKind::Array)
                w.line(std::format("if (n != {0}) fail(c.L, std::format(\"{{}}: {{}} elements, exactly {0} required\", c.what, n));", t->arraySize));
            w.line("reserve(c, n); // every element, nil included, takes at least one value: fail before converting any");
            w.line(std::format("{} out{{}};", C));
            if (t->kind == TypeKind::List) w.line("out.reserve(static_cast<usize>(n));");
            w.open("for (int i = 1; i <= n; ++i) {");
            w.line("lua_rawgeti(c.L, idx, i);");
            const std::string elem = std::format("check{}(c, -1)", codecOf(t->element, kCheck, key.member));
            if (t->kind == TypeKind::List) w.line(std::format("out.push_back({});", elem));
            if (t->kind == TypeKind::Set) w.line(std::format("out.insert({});", elem));
            if (t->kind == TypeKind::Array) w.line(std::format("out[static_cast<usize>(i - 1)] = {};", elem));
            w.line("lua_pop(c.L, 1);");
            w.close();
            w.line("return out;");
            break;
        }
        }
        w.close();
    }

    std::string source(const SourceFile* f, const std::vector<const Decl*>& libs) {
        m_codecs.clear();
        m_codecIndex.clear();
        m_dirs.clear();
        for (const Decl* lib : libs) {
            for (const Decl* fn : lib->methods) {
                for (const Field& p : fn->fields) {
                    if (p.type) codecOf(p.type, kCheck, false);
                }
                if (fn->result) codecOf(fn->result, kPush, false);
            }
        }
        CodeWriter w;
        const std::string base = basePath(f->logicalPath);
        w.line(std::format("// {}.luau.gen.cpp — generated by helios-schemac (--emit luau) from {}. DO NOT EDIT.",
                           base.substr(base.rfind('/') + 1), f->logicalPath));
        w.line(std::format("#include \"{}.luau.gen.h\"", base));
        if (libs.empty()) {
            w.line();
            w.line("// This file declares no scriptlib.");
            return w.take();
        }
        w.line();
        w.line("#include <algorithm>");
        w.line("#include <array>");
        w.line("#include <cfloat>");
        w.line("#include <cmath>");
        w.line("#include <cstdint>");
        w.line("#include <format>");
        w.line("#include <limits>");
        w.line("#include <optional>");
        w.line("#include <set>");
        w.line("#include <string>");
        w.line("#include <type_traits>");
        w.line("#include <vector>");
        w.line();
        w.line("#include \"helios/script/vm.h\"");
        w.line("#include \"lualib.h\"");
        w.line();
        w.line("namespace {");
        w.line();
        w.line("using ::helios::usize;");
        w.line();
        w.raw(kGlueRuntime);
        w.line();
        for (usize i = 0; i < m_codecs.size(); ++i) {
            const std::string C = sigCpp(m_codecs[i].type, m_codecs[i].member);
            if (m_dirs[i] & kPush) w.line(std::format("void push{}(lua_State* L, const {}& v, int depth);", i, C));
            if (m_dirs[i] & kCheck) w.line(std::format("{} check{}(Ctx& c, int idx);", C, i));
        }
        w.line();
        for (usize i = 0; i < m_codecs.size(); ++i) emitCodec(w, i);
        for (const Decl* lib : libs) {
            const std::string implType = "::" + cppNamespace(lib->package) + "::" + lib->name;
            for (const Decl* fn : lib->methods) emitGlue(w, lib, fn, implType);
        }
        w.line("} // namespace");
        w.line();
        w.line(std::format("namespace {} {{", cppNamespace(f->ast.package)));
        for (const Decl* lib : libs) {
            w.line();
            w.open(std::format("::helios::Result<::helios::u32> bind{}(::helios::script::Binder& binder, {}& impl, std::string_view realm) {{",
                               lib->name, lib->name));
            w.line("if (auto ok = checkRealm(binder, realm); !ok) return ok.error();");
            w.line("nameIdTags(binder.state());");
            w.line("::helios::u32 count = 0;");
            for (const Decl* fn : lib->methods) {
                std::string cond;
                for (const std::string& r : fn->cost.realms) cond += (cond.empty() ? "" : " || ") + std::format("realm == \"{}\"", r);
                if (cond.empty()) cond = "false";
                const int items = needsItemHook(fn) ? 0 : ofIndex(fn);
                w.open(std::format("if ({}) {{", cond));
                w.line(std::format("binder.function({}, {}, &{}, ::helios::script::FuelCost{{{}u, {}u, {}}}, &impl);", cppQuote(lib->name),
                                   cppQuote(fn->name), glueName(lib, fn), fn->cost.base, items > 0 ? fn->cost.each * 1000 : 0, items));
                w.line("++count;");
                w.close();
            }
            w.line("return count;");
            w.close();
        }
        w.line();
        w.line(std::format("}} // namespace {}", cppNamespace(f->ast.package)));
        return w.take();
    }

    static std::string glueName(const Decl* lib, const Decl* fn) { return std::format("glue_{}_{}", lib->name, fn->name); }

    void emitGlue(CodeWriter& w, const Decl* lib, const Decl* fn, const std::string& implType) {
        w.line(std::format("// {}.{}: {}", lib->name, fn->name, costText(fn)));
        w.open(std::format("int {}(lua_State* L) {{", glueName(lib, fn)));
        w.line(std::format("auto& impl = *static_cast<{}*>(::helios::script::bindingUserdata(L));", implType));
        std::string args;
        if (!fn->fields.empty()) w.line("Ctx c{L, \"\", 0, impl.glueLimits.maxValues, impl.glueLimits.maxStringBytes, impl.glueLimits.maxValues, impl.glueLimits.maxStringBytes};");
        for (usize i = 0; i < fn->fields.size(); ++i) {
            const Field& p = fn->fields[i];
            w.line(std::format("c.what = {};", cppQuote(std::format("{}.{} argument '{}'", lib->name, fn->name, p.name))));
            emitMaxCheck(w, p, std::to_string(i + 1), p.name); // @max bounds the argument before any of it is converted
            w.line(std::format("const auto a{} = check{}(c, {});", i, codecOf(p.type, kCheck, false), i + 1));
            args += std::format(", a{}", i);
        }
        if (needsItemHook(fn)) {
            // The whole charge is taken before the call (02 §7.4); the host charged `cost` already.
            w.line(std::format("::helios::script::chargeFuel(L, saturatingMul({}u, impl.{}(L, a{})));", fn->cost.each, itemHookName(fn),
                               ofIndex(fn) - 1));
        }
        if (!fn->result) {
            w.line(std::format("impl.{}(L{});", methodName(fn), args));
            w.line("return 0;");
            w.close();
            w.line();
            return;
        }
        w.line(std::format("const auto result = impl.{}(L{});", methodName(fn), args));
        if (fn->cost.of == "result") w.line(std::format("::helios::script::chargeFuel(L, saturatingMul({}u, result.size()));", fn->cost.each));
        if (const Attr* m = fn->attr("max"); m && !m->args.empty()) {
            u64 max = 0;
            parseSchemaUnsigned(m->args[0].value, max);
            const std::string size = fn->result->kind == TypeKind::Optional ? "(result ? result->size() : 0)" : "result.size()";
            w.line(std::format("if ({0} > {1}u) fail(L, std::format(\"{2}.{3} returned {{}} elements; its @max is {1}\", {0}));", size, max,
                               lib->name, fn->name));
        }
        w.line(std::format("push{}(L, result, 0);", codecOf(fn->result, kPush, false)));
        w.line("return 1;");
        w.close();
        w.line();
    }

    // --- schema.d.luau -------------------------------------------------------------------------
    std::string definitions() {
        CodeWriter w(4);
        w.line("--!strict");
        std::vector<std::string> files;
        for (const auto& fp : S.files) {
            if (fp->generate) files.push_back(fp->logicalPath);
        }
        w.line(std::format("-- Generated by helios-schemac (--emit luau) from {}. DO NOT EDIT.", join(files, ", ")));
        w.line("-- Luau declarations of the schemas' script-callable functions (scriptlib, 02 §3.1, §7.4) for luau-lsp;");
        w.line("-- load it after engine/script/defs/helios.d.luau. Fuel charges are the schema's defaults until");
        w.line("-- --calibrate-fuel replaces them (fuel_costs.defaults.json next to this file).");
        w.line("-- Realms: every fn of every realm is declared here; in a host of another realm it is nil at runtime (each");
        w.line("-- fn's doc lists its realms). Struct arguments use the <Type>Input tables, whose fields are optional");
        w.line("-- because the glue gives missing fields their schema defaults; results are the <Type> tables.");
        bool any = false;
        for (const auto& [name, use] : m_names) any = any || use.ref;
        bool entity = false;
        for (const Decl* d : S.decls) {
            if (!d->emitted || d->kind != DeclKind::ScriptFn) continue;
            auto visit = [&](auto&& self, const Type* t, std::set<const Type*>& seen) -> void {
                if (!t || !seen.insert(t).second) return;
                if (t->kind == TypeKind::Builtin && t->builtin == Builtin::EntityId) entity = true;
                self(self, t->element, seen);
                if (t->kind == TypeKind::Struct) {
                    for (const Field& f : t->decl->fields) self(self, f.type, seen);
                }
            };
            std::set<const Type*> seen;
            for (const Field& p : d->fields) visit(visit, p.type, seen);
            visit(visit, d->result, seen);
        }
        if (entity || any) w.line();
        if (entity) {
            w.line("-- An entity id (opaque: compare with ==, use as a table key; scripts cannot make one).");
            w.line("declare extern type EntityId with");
            w.line("end");
        }
        for (const auto& [name, use] : m_names) {
            if (!use.ref) continue;
            w.line(std::format("-- Reference to a {} record (opaque: compare with ==, use as a table key).", use.decl->qualifiedName));
            w.line(std::format("declare extern type {} with", name));
            w.line("end");
        }
        for (const auto& [name, use] : m_names) {
            if (use.ref) continue;
            const Decl* d = use.decl;
            w.line();
            docLines(w, d->doc, "-- ");
            if (d->kind == DeclKind::Enum) {
                std::string alts;
                for (const EnumVal& v : d->values) alts += (alts.empty() ? "" : " | ") + jsonQuote(v.name);
                w.line(std::format("export type {} = {}", name, alts));
                continue;
            }
            w.line(use.input ? std::format("-- {} as an argument (missing fields take their schema defaults; other keys are ignored).",
                                           d->qualifiedName)
                             : std::format("-- {} as a result.", d->qualifiedName));
            w.open(std::format("export type {} = {{", name));
            for (const Field& f : d->fields) {
                if (!f.type) continue;
                docLines(w, f.doc, "-- ");
                std::string label = typeLabel(f.type, use.input);
                if (use.input && !label.ends_with("?")) label += "?";
                w.line(std::format("{}: {},", f.name, label));
            }
            w.close("}");
        }
        for (const Decl* lib : S.decls) {
            if (!lib->emitted || lib->kind != DeclKind::ScriptLib) continue;
            w.line();
            docLines(w, lib->doc, "-- ");
            w.line(std::format("-- scriptlib {} ({})", lib->qualifiedName, lib->file ? lib->file->logicalPath : std::string()));
            w.open(std::format("declare {}: {{", lib->name));
            for (const Decl* fn : lib->methods) {
                docLines(w, fn->doc, "-- ");
                w.line(std::format("-- {}. Realms: {}.", costText(fn), join(fn->cost.realms, ", ")));
                std::string params;
                for (const Field& p : fn->fields) params += (params.empty() ? "" : ", ") + p.name + ": " + typeLabel(p.type, true);
                w.line(std::format("{}: ({}) -> {},", fn->name, params, fn->result ? typeLabel(fn->result, false) : "()"));
            }
            w.close("}");
        }
        return w.take();
    }

    // --- fuel_costs.defaults.json --------------------------------------------------------------
    std::string fuelCosts() {
        std::vector<const Decl*> fns;
        for (const Decl* d : S.decls) {
            if (d->emitted && d->kind == DeclKind::ScriptFn) fns.push_back(d);
        }
        std::sort(fns.begin(), fns.end(), [](const Decl* a, const Decl* b) { return a->typeId < b->typeId; });
        JsonOut o;
        o.beginObject();
        o.key("format");
        o.num(1);
        o.key("comment");
        o.str("Generated by helios-schemac --emit luau: the schemas' @script(cost, each) per binding id (the fn's lock id), the "
              "defaults until --calibrate-fuel (02 §7.4). DO NOT EDIT.");
        o.key("functions");
        o.beginObject();
        for (const Decl* fn : fns) {
            o.key(std::to_string(fn->typeId));
            o.beginObject(true);
            o.key("name");
            o.str(fn->qualifiedName);
            o.key("cost");
            o.unum(fn->cost.base);
            o.key("each");
            o.unum(fn->cost.each);
            if (!fn->cost.of.empty()) {
                o.key("of");
                o.str(fn->cost.of);
            }
            o.endObject();
        }
        o.endObject();
        o.endObject();
        return o.take();
    }

    struct NameUse {
        const Decl* decl;
        bool ref;
        bool input; ///< a struct's argument form (<Type>Input)
    };

    // Helpers compiled into every glue file (internal linkage). The light-userdata tags are the Phase 0
    // contract with the script host (tools/schemac/README.md "Generated Luau").
    static constexpr std::string_view kGlueRuntime = R"glue([[maybe_unused]] constexpr int kEntityIdTag = 1;  // light-userdata tag of EntityId values
[[maybe_unused]] constexpr int kRecordRefTag = 2; // light-userdata tag of record references
[[maybe_unused]] constexpr int kMaxDepth = 32;    // nesting of tables in one argument or result (cyclic tables stop here)
// Integers a Luau number holds exactly: |x| <= 2^53 - 1 (2^53 + 1 would round to 2^53 and pass as exact).
[[maybe_unused]] constexpr double kMaxExactInteger = 9007199254740991.0;
static_assert(sizeof(void*) == 8, "ids travel as 64-bit light userdata");

// Messages are formatted here with std::format; Luau only copies the text (up to its 512-byte buffer).
[[noreturn, maybe_unused]] void fail(lua_State* L, const std::string& message) { luaL_errorL(L, "%s", message.c_str()); }

// One call's argument conversion: the argument being converted, its table nesting, and the values and
// string bytes still allowed. Every converted value and string byte counts, so a table referenced from
// many places costs once per reference and one call's work stays within the caps (hostile input).
struct Ctx {
    lua_State* L;
    const char* what;
    int depth;
    ::helios::u32 values;
    ::helios::usize bytes;
    ::helios::u32 maxValues;
    ::helios::usize maxBytes;
};

[[noreturn, maybe_unused]] void badType(Ctx& c, int idx, const char* expected) {
    fail(c.L, std::format("{}: expected {}, got {}", c.what, expected, luaL_typename(c.L, idx)));
}

[[noreturn, maybe_unused]] void overValues(Ctx& c) {
    fail(c.L, std::format("{}: the arguments hold more than {} values (the glue's per-call limit)", c.what, c.maxValues));
}

[[maybe_unused]] void take(Ctx& c, ::helios::u32 n) {
    if (n > c.values) overValues(c);
    c.values -= n;
}

/// Fails at once when `n` more values cannot fit (a list's elements), before any is converted.
[[maybe_unused]] void reserve(Ctx& c, int n) {
    if (n > 0 && static_cast<::helios::u32>(n) > c.values) overValues(c);
}

/// Results: the C++ value is trusted, but a result nests tables at most kMaxDepth deep too.
[[maybe_unused]] void enter(lua_State* L, int depth) {
    if (depth >= kMaxDepth) fail(L, std::format("result nests more than {} tables deep", kMaxDepth));
    luaL_checkstack(L, 3, "schema glue");
}

struct Nest {
    Ctx& c;
    explicit Nest(Ctx& ctx) : c(ctx) {
        if (++c.depth > kMaxDepth) fail(c.L, std::format("{}: the value nests more than {} tables deep", c.what, kMaxDepth));
        luaL_checkstack(c.L, 3, "schema glue");
    }
    ~Nest() { --c.depth; }
    Nest(const Nest&) = delete;
    Nest& operator=(const Nest&) = delete;
};

/// @max on a string (bytes) or a list or set (elements), checked on the raw value before conversion.
[[maybe_unused]] void checkMax(Ctx& c, int idx, ::helios::u64 max, const char* name) {
    const int type = lua_type(c.L, idx);
    if (type != LUA_TSTRING && type != LUA_TTABLE) return; // (the conversion reports a wrong type)
    const auto n = static_cast<::helios::u64>(lua_objlen(c.L, idx));
    if (n > max) fail(c.L, std::format("{}: '{}' has {} {}; its @max is {}", c.what, name, n, type == LUA_TSTRING ? "bytes" : "elements", max));
}

[[maybe_unused]] ::helios::u64 saturatingMul(::helios::u64 a, ::helios::u64 b) {
    return b != 0 && a > std::numeric_limits<::helios::u64>::max() / b ? std::numeric_limits<::helios::u64>::max() : a * b;
}

[[maybe_unused]] std::string_view checkString(Ctx& c, int idx) {
    if (lua_type(c.L, idx) != LUA_TSTRING) badType(c, idx, "string");
    size_t n = 0;
    const char* s = lua_tolstring(c.L, idx, &n);
    if (n > c.bytes) fail(c.L, std::format("{}: the arguments hold more than {} string bytes (the glue's per-call limit)", c.what, c.maxBytes));
    c.bytes -= n;
    return std::string_view(s, n);
}

[[maybe_unused]] double checkNumber(Ctx& c, int idx) {
    if (lua_type(c.L, idx) != LUA_TNUMBER) badType(c, idx, "number");
    return lua_tonumber(c.L, idx);
}

/// A finite number within float's range (a double outside it cast to float is undefined behaviour).
[[maybe_unused]] ::helios::f32 checkF32(Ctx& c, int idx) {
    const double d = checkNumber(c, idx);
    if (!(std::fabs(d) <= static_cast<double>(FLT_MAX))) fail(c.L, std::format("{}: {} is not a finite f32", c.what, d));
    return static_cast<::helios::f32>(d);
}

/// A vector's components follow the f32 rule: finite (Luau vectors are floats already, so only NaN and inf).
[[maybe_unused]] void checkFiniteVector(Ctx& c, const float* p) {
    for (int i = 0; i < 3; ++i) {
        if (std::isfinite(p[i])) continue;
        const char* value = std::isnan(p[i]) ? "nan" : p[i] > 0 ? "inf" : "-inf"; // (a NaN's sign varies by platform)
        fail(c.L, std::format("{}: component {} is {}, not a finite f32", c.what, "xyz"[i], value));
    }
}

template <class T>
T checkInteger(Ctx& c, int idx, const char* type) {
    const double d = checkNumber(c, idx);
    constexpr double lo = std::is_signed_v<T> ? std::max(static_cast<double>(std::numeric_limits<T>::min()), -kMaxExactInteger) : 0.0;
    constexpr double hi = std::min(static_cast<double>(std::numeric_limits<T>::max()), kMaxExactInteger);
    if (!(d >= lo && d <= hi) || std::floor(d) != d) fail(c.L, std::format("{}: {} is not a {}", c.what, d, type));
    return static_cast<T>(d);
}

template <class T>
void pushInteger(lua_State* L, T v) {
    if constexpr (sizeof(T) == 8) {
        constexpr T kMax = static_cast<T>(9007199254740991ull);
        if constexpr (std::is_signed_v<T>) {
            if (v > kMax || v < -kMax) fail(L, std::format("result {} does not fit a Luau number exactly", v));
        } else if (v > kMax) {
            fail(L, std::format("result {} does not fit a Luau number exactly", v));
        }
    }
    lua_pushnumber(L, static_cast<double>(v));
}

[[maybe_unused]] ::helios::refl::Duration checkDuration(Ctx& c, int idx) {
    const double s = checkNumber(c, idx);
    if (!(std::fabs(s) <= 9.2e9)) fail(c.L, std::format("{}: {} seconds is not a valid duration", c.what, s));
    return ::helios::refl::Duration(static_cast<::helios::i64>(std::llround(s * 1e9)));
}

[[maybe_unused]] void pushId(lua_State* L, ::helios::u64 bits, int tag) {
    lua_pushlightuserdatatagged(L, reinterpret_cast<void*>(static_cast<uintptr_t>(bits)), tag);
}

[[maybe_unused]] ::helios::u64 checkId(Ctx& c, int idx, int tag, const char* type) {
    if (lua_type(c.L, idx) != LUA_TLIGHTUSERDATA || lua_lightuserdatatag(c.L, idx) != tag) badType(c, idx, type);
    return static_cast<::helios::u64>(reinterpret_cast<uintptr_t>(lua_tolightuserdatatagged(c.L, idx, tag)));
}

void nameIdTags(lua_State* L) {
    if (!lua_getlightuserdataname(L, kEntityIdTag)) lua_setlightuserdataname(L, kEntityIdTag, "EntityId");
    if (!lua_getlightuserdataname(L, kRecordRefTag)) lua_setlightuserdataname(L, kRecordRefTag, "RecordRef");
}

/// A realm the glue knows and the VM's host profile runs: Cell VMs run "server", Client "client", Editor "editor".
::helios::Result<void> checkRealm(::helios::script::Binder& binder, std::string_view realm) {
    using ::helios::script::HostProfile;
    const HostProfile profile = ::helios::script::vmFromState(binder.state()).config().profile;
    const std::string_view expected = profile == HostProfile::Cell ? "server" : profile == HostProfile::Client ? "client" : "editor";
    if (realm != "server" && realm != "client" && realm != "editor")
        return ::helios::Error(::helios::ErrorCode::InvalidArgument, std::format("unknown script realm '{}' (server, client or editor)", realm));
    if (realm != expected)
        return ::helios::Error(::helios::ErrorCode::InvalidArgument,
                               std::format("script realm '{}' does not match this VM's host profile, which runs '{}'", realm, expected));
    return {};
}
)glue";

    const Schema& S;
    const CompileOptions& O;
    DiagnosticEngine& D;
    std::map<std::string, NameUse> m_names;
    std::map<std::string, const Decl*> m_libs;
    std::vector<CodecKey> m_codecs;
    std::vector<u8> m_dirs;
    std::map<CodecKey, usize> m_codecIndex;
};

} // namespace

std::vector<OutputFile> generateLuau(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags) {
    LuauGenerator g(schema, options, diags);
    return g.run();
}

} // namespace helios::schemac
