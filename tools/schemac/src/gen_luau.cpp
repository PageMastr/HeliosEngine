// `--emit luau` (02 §3.5, §7.4): what the Luau script host needs from the schema.
//   * <file>.luau.gen.h / .luau.gen.cpp, next to the C++ output: per scriptlib, the abstract C++
//     interface its hand-written functions implement and bind<Lib>(), which registers them on an
//     engine/script Binder. The glue charges each fn's fuel (cost and `each` per element of an `of`
//     argument before the call, `each` per result element right after it) and converts arguments
//     and results without ever running Luau code (raw table access only);
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
    /// value, where a WorldPos would lose its frame (only signature-level positions carry one).
    std::string unsupported(const Type* t, bool inStruct, std::set<const Decl*>& seen) {
        switch (t->kind) {
        case TypeKind::Prim: return {};
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
                std::string why = unsupported(f.type, true, seen);
                if (!why.empty()) return why + std::format(" in field '{}' of '{}'", f.name, t->decl->qualifiedName);
            }
            return {};
        }
        case TypeKind::List:
        case TypeKind::Set:
        case TypeKind::Array:
        case TypeKind::Optional: return unsupported(t->element, inStruct, seen);
        default: return std::format("'{}'", t->signature);
        }
    }

    void checkType(const Decl* fn, const Type* t, SourceLoc loc, const std::string& what) {
        std::set<const Decl*> seen;
        const std::string why = unsupported(t, false, seen);
        if (!why.empty()) {
            D.error(loc, std::format("{} of fn '{}' cannot cross the Luau boundary: {} is not supported by --emit luau (supported: bool, "
                                     "integers, floats, string, Name, vec3f, WorldPos, EntityId, Duration, Tick, LocString, TagQuery, "
                                     "HxlExpr, enums, record refs, structs of those, and list, set, T[N] and T? of them)",
                                     what, fn->name, why));
            return;
        }
        collectNames(t, loc);
    }

    /// Registers the Luau type names `t` needs and reports collisions.
    void collectNames(const Type* t, SourceLoc loc) {
        const Decl* d = nullptr;
        if (t->kind == TypeKind::Enum || t->kind == TypeKind::Struct || t->kind == TypeKind::RecordRef) d = t->decl;
        if (t->element) collectNames(t->element, loc);
        if (!d) return;
        const std::string name = luauDeclName(d);
        auto [it, fresh] = m_names.emplace(name, NameUse{d, t->kind == TypeKind::RecordRef});
        if (!fresh) {
            if (it->second.decl != d || it->second.ref != (t->kind == TypeKind::RecordRef))
                D.error(loc, std::format("'{}' and '{}' both become the Luau type '{}'", it->second.decl->qualifiedName, d->qualifiedName, name));
            return;
        }
        if (isReserved(name) || name == "EntityId")
            D.error(loc, std::format("'{}' becomes the Luau type '{}', which the script host already declares", d->qualifiedName, name));
        if (t->kind == TypeKind::Struct) {
            for (const Field& f : d->fields) {
                if (f.type) collectNames(f.type, loc);
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
                if (p.type) checkType(fn, p.type, p.loc, std::format("parameter '{}'", p.name));
            }
            if (fn->result) checkType(fn, fn->result, fn->loc, "the result");
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

    /// C++ type of a signature-level value: WorldPos is the script host's frame-carrying FramePos.
    static std::string sigCpp(const Type* t) {
        switch (t->kind) {
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
    static bool byValue(const Type* t) {
        return t->kind == TypeKind::Prim ? t->prim != Prim::String
                                         : t->kind == TypeKind::Enum || t->kind == TypeKind::RecordRef ||
                                               (t->kind == TypeKind::Builtin &&
                                                (t->builtin == Builtin::EntityId || t->builtin == Builtin::Duration || t->builtin == Builtin::Tick ||
                                                 t->builtin == Builtin::Vec3f));
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
            w.line(std::format("/// Registers the {} functions whose @realm includes `realm` (\"server\", \"client\" or \"editor\") as",
                               lib->name));
            w.line(std::format("/// the Luau library `{}` (call inside the ScriptVm::create registrar). `impl` must outlive the VM.",
                               lib->name));
            w.line("/// Returns how many functions were registered.");
            w.line(std::format("::helios::u32 bind{}(::helios::script::Binder& binder, {}& impl, std::string_view realm);", lib->name,
                               lib->name));
        }
        w.line();
        w.line(std::format("}} // namespace {}", cppNamespace(f->ast.package)));
        return w.take();
    }

    // --- C++ source ----------------------------------------------------------------------------
    static constexpr u8 kPush = 1; ///< C++ -> Luau (results)
    static constexpr u8 kCheck = 2; ///< Luau -> C++ (arguments)

    /// Index of the codec of `t`, noting the directions `dirs` it needs (and those of its parts).
    usize codecOf(const Type* t, u8 dirs = 0) {
        auto it = m_codecIndex.find(t);
        if (it == m_codecIndex.end()) {
            it = m_codecIndex.emplace(t, m_codecs.size()).first;
            m_codecs.push_back(t);
            m_dirs.push_back(0);
        }
        const usize index = it->second;
        if ((m_dirs[index] & dirs) == dirs) return index;
        m_dirs[index] |= dirs;
        if (t->element) codecOf(t->element, dirs);
        if (t->kind == TypeKind::Struct) {
            for (const Field& f : t->decl->fields) {
                if (f.type) codecOf(f.type, dirs);
            }
        }
        return index;
    }

    static std::string typeLabel(const Type* t) {
        switch (t->kind) {
        case TypeKind::Enum:
        case TypeKind::Struct:
        case TypeKind::RecordRef: return luauDeclName(t->decl);
        case TypeKind::Optional: return typeLabel(t->element) + "?";
        case TypeKind::List:
        case TypeKind::Set:
        case TypeKind::Array: return "{" + typeLabel(t->element) + "}";
        case TypeKind::Builtin:
            if (t->builtin == Builtin::Vec3f) return "vector";
            if (t->builtin == Builtin::WorldPos || t->builtin == Builtin::EntityId) return std::string(builtinName(t->builtin));
            if (t->builtin == Builtin::Duration || t->builtin == Builtin::Tick) return "number";
            return "string";
        case TypeKind::Prim: return t->prim == Prim::Bool ? "boolean" : (t->prim == Prim::String || t->prim == Prim::Name) ? "string" : "number";
        default: return t->signature;
        }
    }

    void emitCodec(CodeWriter& w, const Type* t, usize i) {
        const std::string C = sigCpp(t);
        const std::string label = cppQuote(typeLabel(t));
        w.line(std::format("// {}", t->signature));
        if (m_dirs[i] & kPush) emitPush(w, t, i, C);
        if (m_dirs[i] & kCheck) emitCheck(w, t, i, C, label);
        w.line();
    }

    void emitPush(CodeWriter& w, const Type* t, usize i, const std::string& C) {
        w.open(std::format("void push{}(lua_State* L, const {}& v, int depth) {{", i, C));
        switch (t->kind) {
        case TypeKind::Prim:
            w.line("(void)depth;");
            if (t->prim == Prim::Bool) {
                w.line("lua_pushboolean(L, v ? 1 : 0);");
            } else if (t->prim == Prim::String) {
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
                w.line(std::format("push{}(L, v.{}, depth + 1);", codecOf(f.type, kPush), cppFieldName(f.name)));
                w.line(std::format("lua_rawsetfield(L, -2, {});", cppQuote(f.name)));
            }
            break;
        case TypeKind::Optional:
            w.open("if (!v) {");
            w.line("lua_pushnil(L);");
            w.line("return;");
            w.close();
            w.line(std::format("push{}(L, *v, depth);", codecOf(t->element, kPush)));
            break;
        default: // list, set, array
            w.line("enter(L, depth);");
            w.line("lua_createtable(L, static_cast<int>(v.size()), 0);");
            w.line("int n = 0;");
            w.open("for (const auto& e : v) {");
            w.line(std::format("push{}(L, e, depth + 1);", codecOf(t->element, kPush)));
            w.line("lua_rawseti(L, -2, ++n);");
            w.close();
            break;
        }
        w.close();
    }

    void emitCheck(CodeWriter& w, const Type* t, usize i, const std::string& C, const std::string& label) {
        w.open(std::format("{} check{}(lua_State* L, int idx, const char* what, int depth) {{", C, i));
        switch (t->kind) {
        case TypeKind::Prim:
            w.line("(void)depth;");
            if (t->prim == Prim::Bool) {
                w.line("if (lua_type(L, idx) != LUA_TBOOLEAN) badType(L, idx, what, \"boolean\");");
                w.line("return lua_toboolean(L, idx) != 0;");
            } else if (t->prim == Prim::String || t->prim == Prim::Name) {
                w.line(std::format("return {}(checkString(L, idx, what));", t->prim == Prim::Name ? "::helios::Name" : "std::string"));
            } else if (isFloatPrim(t->prim)) {
                w.line(std::format("return static_cast<{}>(checkNumber(L, idx, what));", C));
            } else {
                w.line(std::format("return checkInteger<{}>(L, idx, what, {});", C, cppQuote(primName(t->prim))));
            }
            break;
        case TypeKind::Builtin:
            w.line("(void)depth;");
            switch (t->builtin) {
            case Builtin::Vec3f:
                w.line("const float* p = lua_tovector(L, idx);");
                w.line("if (!p) badType(L, idx, what, \"vector\");");
                w.line("return ::helios::Vec3(p[0], p[1], p[2]);");
                break;
            case Builtin::WorldPos:
                w.line("if (!::helios::script::toWorldPos(L, idx)) badType(L, idx, what, \"WorldPos\");");
                w.line("return ::helios::script::checkWorldPos(L, idx);");
                break;
            case Builtin::EntityId: w.line("return ::helios::refl::EntityId(checkId(L, idx, what, kEntityIdTag, \"EntityId\"));"); break;
            case Builtin::Duration: w.line("return checkDuration(L, idx, what);"); break;
            case Builtin::Tick: w.line("return checkInteger<::helios::u64>(L, idx, what, \"Tick\");"); break;
            case Builtin::LocString: w.line("return ::helios::refl::LocString{std::string(checkString(L, idx, what))};"); break;
            default: w.line(std::format("return {}{{std::string(checkString(L, idx, what))}};", C)); break;
            }
            break;
        case TypeKind::RecordRef:
            w.line("(void)depth;");
            w.line(std::format("return {}(checkId(L, idx, what, kRecordRefTag, {}));", C, label));
            break;
        case TypeKind::Enum: {
            w.line("(void)depth;");
            w.line("const std::string_view s = checkString(L, idx, what);");
            std::string names;
            for (const EnumVal& e : t->decl->values) {
                w.line(std::format("if (s == {}) return {}::{};", cppQuote(e.name), C, cppFieldName(e.name)));
                names += (names.empty() ? "" : ", ") + e.name;
            }
            w.line(std::format("luaL_errorL(L, \"%s: '%s' is not a {} (expected one of {})\", what, std::string(s).c_str());", t->decl->name,
                               names));
            break;
        }
        case TypeKind::Struct:
            w.line("enter(L, depth);");
            w.line(std::format("if (lua_type(L, idx) != LUA_TTABLE) badType(L, idx, what, {});", label));
            w.line("idx = lua_absindex(L, idx);");
            w.line(std::format("{} out{{}};", C));
            for (const Field& f : t->decl->fields) {
                if (!f.type) continue;
                w.line(std::format("if (lua_rawgetfield(L, idx, {}) != LUA_TNIL) out.{} = check{}(L, -1, what, depth + 1);", cppQuote(f.name),
                                   cppFieldName(f.name), codecOf(f.type, kCheck)));
                w.line("lua_pop(L, 1);");
            }
            w.line("return out;");
            break;
        case TypeKind::Optional:
            w.line(std::format("if (lua_isnoneornil(L, idx)) return std::nullopt;"));
            w.line(std::format("return check{}(L, idx, what, depth);", codecOf(t->element, kCheck)));
            break;
        default: { // list, set, array
            w.line("enter(L, depth);");
            w.line(std::format("if (lua_type(L, idx) != LUA_TTABLE) badType(L, idx, what, {});", label));
            w.line("idx = lua_absindex(L, idx);");
            w.line("const int n = lua_objlen(L, idx);");
            const std::string limit = t->kind == TypeKind::Array ? std::to_string(t->arraySize) : "kMaxListElements";
            w.line(std::format("if (n > {}) fail(L, std::format(\"{{}}: {{}} elements, at most {{}} allowed\", what, n, {}));", limit, limit));
            w.line(std::format("{} out{{}};", C));
            if (t->kind == TypeKind::List) w.line("out.reserve(static_cast<usize>(n));");
            w.open("for (int i = 1; i <= n; ++i) {");
            w.line("lua_rawgeti(L, idx, i);");
            const std::string elem = std::format("check{}(L, -1, what, depth + 1)", codecOf(t->element, kCheck));
            if (t->kind == TypeKind::List) w.line(std::format("out.push_back({});", elem));
            if (t->kind == TypeKind::Set) w.line(std::format("out.insert({});", elem));
            if (t->kind == TypeKind::Array) w.line(std::format("out[static_cast<usize>(i - 1)] = {};", elem));
            w.line("lua_pop(L, 1);");
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
                    if (p.type) codecOf(p.type, kCheck);
                }
                if (fn->result) codecOf(fn->result, kPush);
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
        w.line("#include <array>");
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
        w.line("#include \"lualib.h\"");
        w.line();
        w.line("namespace {");
        w.line();
        w.line("using ::helios::usize;");
        w.line();
        w.raw(kGlueRuntime);
        w.line();
        for (usize i = 0; i < m_codecs.size(); ++i) {
            if (m_dirs[i] & kPush) w.line(std::format("void push{}(lua_State* L, const {}& v, int depth);", i, sigCpp(m_codecs[i])));
            if (m_dirs[i] & kCheck) w.line(std::format("{} check{}(lua_State* L, int idx, const char* what, int depth);", sigCpp(m_codecs[i]), i));
        }
        w.line();
        for (usize i = 0; i < m_codecs.size(); ++i) emitCodec(w, m_codecs[i], i);
        for (const Decl* lib : libs) {
            const std::string implType = "::" + cppNamespace(lib->package) + "::" + lib->name;
            for (const Decl* fn : lib->methods) emitGlue(w, lib, fn, implType);
        }
        w.line("} // namespace");
        w.line();
        w.line(std::format("namespace {} {{", cppNamespace(f->ast.package)));
        for (const Decl* lib : libs) {
            w.line();
            w.open(std::format("::helios::u32 bind{}(::helios::script::Binder& binder, {}& impl, std::string_view realm) {{", lib->name, lib->name));
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
        for (usize i = 0; i < fn->fields.size(); ++i) {
            const Field& p = fn->fields[i];
            const std::string what = cppQuote(std::format("{}.{} argument '{}'", lib->name, fn->name, p.name));
            u64 max = 0;
            if (const Attr* m = p.attr("max"); m && !m->args.empty() && parseSchemaUnsigned(m->args[0].value, max) &&
                (p.type->kind == TypeKind::List || p.type->kind == TypeKind::Set)) {
                // @max bounds the argument before any element is converted.
                w.line(std::format("if (lua_type(L, {0}) == LUA_TTABLE && lua_objlen(L, {0}) > {1}) luaL_errorL(L, \"%s: more than {1} elements\", {2});",
                                   i + 1, max, what));
            }
            w.line(std::format("const auto a{} = check{}(L, {}, {}, 0);", i, codecOf(p.type, kCheck), i + 1, what));
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
            const Type* r = fn->result->kind == TypeKind::Optional ? fn->result->element : fn->result;
            const std::string dot = fn->result->kind == TypeKind::Optional ? "->" : ".";
            const std::string one = std::format("result{}{}", dot, r->kind == TypeKind::Prim && r->prim == Prim::Name ? "view().size()" : "size()");
            const std::string size = fn->result->kind == TypeKind::Optional ? std::format("(result ? {} : 0)", one) : one;
            w.line(std::format("if ({0} > {1}u) fail(L, std::format(\"{2}.{3} returned {{}} elements; its @max is {1}\", {0}));", size, max,
                               lib->name, fn->name));
        }
        w.line(std::format("push{}(L, result, 0);", codecOf(fn->result, kPush)));
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
            w.line(std::format("-- {} (a table; missing fields take their schema defaults, other keys are ignored).", d->qualifiedName));
            w.open(std::format("export type {} = {{", name));
            for (const Field& f : d->fields) {
                if (!f.type) continue;
                docLines(w, f.doc, "-- ");
                w.line(std::format("{}: {},", f.name, typeLabel(f.type)));
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
                for (const Field& p : fn->fields) params += (params.empty() ? "" : ", ") + p.name + ": " + typeLabel(p.type);
                w.line(std::format("{}: ({}) -> {},", fn->name, params, fn->result ? typeLabel(fn->result) : "()"));
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
    };

    // Helpers compiled into every glue file (internal linkage). The light-userdata tags are the Phase 0
    // contract with the script host (tools/schemac/README.md "Generated Luau").
    static constexpr std::string_view kGlueRuntime = R"([[maybe_unused]] constexpr int kEntityIdTag = 1;  // light-userdata tag of EntityId values
[[maybe_unused]] constexpr int kRecordRefTag = 2; // light-userdata tag of record references
[[maybe_unused]] constexpr int kMaxDepth = 32;    // nesting of tables in one argument or result (cyclic tables stop here)
[[maybe_unused]] constexpr int kMaxListElements = 1 << 16; // elements of one list or set argument
[[maybe_unused]] constexpr double kMaxExactInteger = 9007199254740992.0; // 2^53: larger 64-bit values do not fit a Luau number
static_assert(sizeof(void*) == 8, "ids travel as 64-bit light userdata");

// Messages are formatted here, not by Luau's printf-style formatter, so no format depends on the C runtime.
[[noreturn, maybe_unused]] void fail(lua_State* L, const std::string& message) { luaL_errorL(L, "%s", message.c_str()); }

[[noreturn, maybe_unused]] void badType(lua_State* L, int idx, const char* what, const char* expected) {
    fail(L, std::format("{}: expected {}, got {}", what, expected, luaL_typename(L, idx)));
}

[[maybe_unused]] void enter(lua_State* L, int depth) {
    if (depth >= kMaxDepth) fail(L, std::format("value nests more than {} tables deep", kMaxDepth));
    luaL_checkstack(L, 3, "schema glue");
}

[[maybe_unused]] ::helios::u64 saturatingMul(::helios::u64 a, ::helios::u64 b) {
    return b != 0 && a > std::numeric_limits<::helios::u64>::max() / b ? std::numeric_limits<::helios::u64>::max() : a * b;
}

[[maybe_unused]] std::string_view checkString(lua_State* L, int idx, const char* what) {
    if (lua_type(L, idx) != LUA_TSTRING) badType(L, idx, what, "string");
    size_t n = 0;
    const char* s = lua_tolstring(L, idx, &n);
    return std::string_view(s, n);
}

[[maybe_unused]] double checkNumber(lua_State* L, int idx, const char* what) {
    if (lua_type(L, idx) != LUA_TNUMBER) badType(L, idx, what, "number");
    return lua_tonumber(L, idx);
}

template <class T>
T checkInteger(lua_State* L, int idx, const char* what, const char* type) {
    const double d = checkNumber(L, idx, what);
    constexpr double lo = std::is_signed_v<T> ? std::max(static_cast<double>(std::numeric_limits<T>::min()), -kMaxExactInteger) : 0.0;
    constexpr double hi = std::min(static_cast<double>(std::numeric_limits<T>::max()), kMaxExactInteger);
    if (!(d >= lo && d <= hi) || std::floor(d) != d) fail(L, std::format("{}: {} is not a {}", what, d, type));
    return static_cast<T>(d);
}

template <class T>
void pushInteger(lua_State* L, T v) {
    if constexpr (sizeof(T) == 8) {
        if constexpr (std::is_signed_v<T>) {
            if (v > static_cast<T>(kMaxExactInteger) || v < -static_cast<T>(kMaxExactInteger))
                fail(L, std::format("result {} does not fit a Luau number exactly", v));
        } else if (v > static_cast<T>(kMaxExactInteger)) {
            fail(L, std::format("result {} does not fit a Luau number exactly", v));
        }
    }
    lua_pushnumber(L, static_cast<double>(v));
}

[[maybe_unused]] ::helios::refl::Duration checkDuration(lua_State* L, int idx, const char* what) {
    const double s = checkNumber(L, idx, what);
    if (!(std::fabs(s) <= 9.2e9)) fail(L, std::format("{}: {} seconds is not a valid duration", what, s));
    return ::helios::refl::Duration(static_cast<::helios::i64>(std::llround(s * 1e9)));
}

[[maybe_unused]] void pushId(lua_State* L, ::helios::u64 bits, int tag) {
    lua_pushlightuserdatatagged(L, reinterpret_cast<void*>(static_cast<uintptr_t>(bits)), tag);
}

[[maybe_unused]] ::helios::u64 checkId(lua_State* L, int idx, const char* what, int tag, const char* type) {
    if (lua_type(L, idx) != LUA_TLIGHTUSERDATA || lua_lightuserdatatag(L, idx) != tag) badType(L, idx, what, type);
    return static_cast<::helios::u64>(reinterpret_cast<uintptr_t>(lua_tolightuserdatatagged(L, idx, tag)));
}

void nameIdTags(lua_State* L) {
    if (!lua_getlightuserdataname(L, kEntityIdTag)) lua_setlightuserdataname(L, kEntityIdTag, "EntityId");
    if (!lua_getlightuserdataname(L, kRecordRefTag)) lua_setlightuserdataname(L, kRecordRefTag, "RecordRef");
}
)";

    const Schema& S;
    const CompileOptions& O;
    DiagnosticEngine& D;
    std::map<std::string, NameUse> m_names;
    std::map<std::string, const Decl*> m_libs;
    std::vector<const Type*> m_codecs;
    std::vector<u8> m_dirs;
    std::map<const Type*, usize> m_codecIndex;
};

} // namespace

std::vector<OutputFile> generateLuau(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags) {
    LuauGenerator g(schema, options, diags);
    return g.run();
}

} // namespace helios::schemac
