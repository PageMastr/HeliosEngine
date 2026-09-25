#include "sema.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_set>

#include "parser.h"
#include "text.h"

namespace helios::schemac {

namespace {

struct BuiltinEntry {
    std::string_view name;
    Prim prim;
    Builtin builtin;
};
constexpr BuiltinEntry kBuiltins[] = {
    {"bool", Prim::Bool, Builtin::None},        {"i8", Prim::I8, Builtin::None},
    {"i16", Prim::I16, Builtin::None},          {"i32", Prim::I32, Builtin::None},
    {"i64", Prim::I64, Builtin::None},          {"u8", Prim::U8, Builtin::None},
    {"u16", Prim::U16, Builtin::None},          {"u32", Prim::U32, Builtin::None},
    {"u64", Prim::U64, Builtin::None},          {"f32", Prim::F32, Builtin::None},
    {"f64", Prim::F64, Builtin::None},          {"string", Prim::String, Builtin::None},
    {"Name", Prim::Name, Builtin::None},        {"Guid", Prim::None, Builtin::Guid},
    {"vec2f", Prim::None, Builtin::Vec2f},      {"vec3f", Prim::None, Builtin::Vec3f},
    {"vec4f", Prim::None, Builtin::Vec4f},      {"vec3d", Prim::None, Builtin::Vec3d},
    {"quatf", Prim::None, Builtin::Quatf},      {"quatd", Prim::None, Builtin::Quatd},
    {"color", Prim::None, Builtin::Color},      {"WorldPos", Prim::None, Builtin::WorldPos},
    {"EntityId", Prim::None, Builtin::EntityId}, {"NetHandle", Prim::None, Builtin::NetHandle},
    {"Duration", Prim::None, Builtin::Duration}, {"Tick", Prim::None, Builtin::Tick},
    {"LocString", Prim::None, Builtin::LocString}, {"TagSet", Prim::None, Builtin::TagSet},
    {"TagQuery", Prim::None, Builtin::TagQuery}, {"HxlExpr", Prim::None, Builtin::HxlExpr},
};
constexpr std::string_view kGenericNames[] = {"list", "set", "map", "AssetRef", "Ref"};

enum Target : u8 { TField = 1, TType = 2, TRpc = 4, TEnumValue = 8, TFn = 16 };
struct AttrSpec {
    std::string_view name;
    u8 targets;
    int minArgs;
    int maxArgs; ///< -1 = unlimited
};
constexpr AttrSpec kAttrSpecs[] = {
    // Replication (04)
    {"replicate", TType, 1, 1},
    {"lod", TType | TField, 1, 1},
    {"quant", TField, 0, -1},
    {"rate", TType | TField | TRpc, 1, 1},
    {"priority", TType | TField, 1, 1},
    {"predicted", TField, 0, 0},
    {"interp", TField, 1, 1},
    // Messages
    {"reliable", TRpc | TType, 0, 0},
    {"unreliable", TRpc | TType, 0, 0},
    {"ratelimit", TRpc, 1, 1},
    {"intent", TRpc, 1, 1},
    {"audience", TType | TRpc, 1, 1},
    {"idempotent", TRpc, 0, 0},
    {"timeout", TRpc, 1, 1},
    {"scope", TType | TRpc, 1, 1},
    // Persistence (05)
    {"persist", TType | TField, 0, 0},
    {"store", TType, 1, 1},
    {"table", TType, 1, 1},
    {"key", TType | TField, 0, -1},
    {"sql", TType | TField, 1, -1},
    {"lifecycle", TType, 1, -1},
    {"ledger_policy", TType, 1, 1},
    {"reason_required", TRpc, 0, 0},
    // Client/server split
    {"server_only", TType | TField, 0, 0},
    {"client_only", TType | TField, 0, 0},
    {"authoring", TType, 0, 0},
    {"opaque", TField, 0, 0},
    {"client", TType, 0, 0},
    {"server", TType, 0, 0},
    // ECS traits
    {"tag", TType, 0, 0},
    {"shared", TType, 0, 0},
    {"sparse", TType, 0, 0},
    {"singleton", TType, 0, 0},
    {"exclusive", TType, 0, 0},
    {"acyclic", TType, 0, 0},
    {"target", TType, 1, 1},
    // Editor (07)
    {"doc", TType | TField | TRpc | TEnumValue | TFn, 1, 1},
    {"editor", TType | TField | TEnumValue, 1, -1},
    {"range", TField, 2, 2},
    {"step", TField, 1, 1},
    {"unit", TField, 1, 1},
    {"asset", TField, 1, 1},
    {"hidden", TType | TField | TEnumValue, 0, 0},
    {"readonly", TField, 0, 0},
    {"validate", TType | TField, 1, 1},
    {"keyed", TField, 0, 1},
    {"normalized", TField, 0, 0},
    {"max", TField | TFn, 1, 1},
    {"deprecated", TType | TField | TRpc | TEnumValue | TFn, 0, 1},
    // Scripting and versioning
    {"script", TType | TField | TFn, 1, 3}, // fields: @script(read|write|none); fns: @script(cost=n[, each=m, of=x])
    {"pure", TFn, 0, 0},
    {"realm", TType | TFn, 1, 3},
    {"was", TType | TField, 1, -1},
    {"version", TType, 1, 1},
    {"merge", TField, 1, 1},
};

const AttrSpec* findSpec(std::string_view name) {
    for (const AttrSpec& s : kAttrSpecs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::string_view targetName(Target t) {
    switch (t) {
    case TField: return "a field";
    case TType: return "a type";
    case TRpc: return "an rpc";
    case TEnumValue: return "an enum value";
    case TFn: return "a scriptlib fn";
    }
    return "?";
}

bool parseNumber(std::string_view text, f64& out) { return parseSchemaNumber(text, out); }
bool parseUnsigned(std::string_view text, u64& out) { return parseSchemaUnsigned(text, out); }

/// Schema identifier ("maxForce"); with `dotted`, also qualified names ("game.ship.Hull").
bool isIdentifier(std::string_view s, bool dotted) {
    bool start = true;
    for (const char c : s) {
        if (dotted && c == '.' && !start) {
            start = true;
            continue;
        }
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!alpha && (start || c < '0' || c > '9')) return false;
        start = false;
    }
    return !start;
}

bool primRange(Prim p, i64& lo, u64& hi) {
    switch (p) {
    case Prim::I8: lo = std::numeric_limits<i8>::min(); hi = static_cast<u64>(std::numeric_limits<i8>::max()); return true;
    case Prim::I16: lo = std::numeric_limits<i16>::min(); hi = static_cast<u64>(std::numeric_limits<i16>::max()); return true;
    case Prim::I32: lo = std::numeric_limits<i32>::min(); hi = static_cast<u64>(std::numeric_limits<i32>::max()); return true;
    case Prim::I64: lo = std::numeric_limits<i64>::min(); hi = static_cast<u64>(std::numeric_limits<i64>::max()); return true;
    case Prim::U8: lo = 0; hi = std::numeric_limits<u8>::max(); return true;
    case Prim::U16: lo = 0; hi = std::numeric_limits<u16>::max(); return true;
    case Prim::U32: lo = 0; hi = std::numeric_limits<u32>::max(); return true;
    case Prim::U64: lo = 0; hi = std::numeric_limits<u64>::max(); return true;
    default: return false;
    }
}

bool isKeyable(const Type* t) {
    switch (t->kind) {
    case TypeKind::Prim: return isIntegerPrim(t->prim) || t->prim == Prim::String || t->prim == Prim::Name;
    case TypeKind::Builtin: return t->builtin == Builtin::Guid || t->builtin == Builtin::EntityId;
    case TypeKind::Enum:
    case TypeKind::RecordRef: return true;
    default: return false;
    }
}

bool isNumeric(const Type* t) {
    if (t->kind == TypeKind::Prim) return isIntegerPrim(t->prim) || isFloatPrim(t->prim);
    if (t->kind == TypeKind::Builtin) return tupleSize(t->builtin) != 0;
    if (t->kind == TypeKind::List || t->kind == TypeKind::Array || t->kind == TypeKind::Optional) return isNumeric(t->element);
    return false;
}

class Sema {
public:
    Sema(Schema& schema, DiagnosticEngine& diags, const SemaOptions& options)
        : S(schema), D(diags), m_options(options) {}

    bool run() {
        const usize before = D.errorCount();
        declareAll();
        for (auto& f : S.files) {
            for (Decl* d : std::vector<Decl*>(f->decls)) resolveTopLevel(d);
        }
        for (Decl* d : std::vector<Decl*>(S.decls)) {
            if (d->kind == DeclKind::Component) splitComponent(d);
        }
        for (Decl* d : S.decls) validateDecl(d);
        checkRecursion();
        // Every type declaration is addressable by signature (generators look them up).
        for (Decl* d : S.decls) {
            if (d->isLockable()) declType(d);
        }
        return D.errorCount() == before;
    }

private:
    // --- declaration ---------------------------------------------------------------------------
    static DeclKind mapKind(DeclKindAst k) {
        switch (k) {
        case DeclKindAst::Enum: return DeclKind::Enum;
        case DeclKindAst::Flags: return DeclKind::Flags;
        case DeclKindAst::Struct: return DeclKind::Struct;
        case DeclKindAst::Component: return DeclKind::Component;
        case DeclKindAst::Relation: return DeclKind::Relation;
        case DeclKindAst::Record: return DeclKind::Record;
        case DeclKindAst::Event: return DeclKind::Event;
        case DeclKindAst::Rpc: return DeclKind::Rpc;
        case DeclKindAst::Message: return DeclKind::Message;
        case DeclKindAst::Service: return DeclKind::Service;
        case DeclKindAst::ViewModel: return DeclKind::ViewModel;
        case DeclKindAst::Formula: return DeclKind::Formula;
        case DeclKindAst::Const: return DeclKind::Const;
        case DeclKindAst::Alias: return DeclKind::Alias;
        case DeclKindAst::ScriptLib: return DeclKind::ScriptLib;
        }
        return DeclKind::Struct;
    }

    std::string qualify(const std::string& pkg, std::string_view name) {
        return pkg.empty() ? std::string(name) : pkg + "." + std::string(name);
    }

    bool registerName(Decl* d) {
        auto [it, inserted] = S.declsByName.emplace(d->qualifiedName, d);
        if (!inserted) {
            D.error(d->loc, std::format("duplicate declaration '{}'", d->qualifiedName));
            D.note(it->second->loc, "previous declaration is here");
            return false;
        }
        return true;
    }

    std::vector<Attr> convertAttrs(const std::vector<AttrAst>& in) {
        std::vector<Attr> out;
        for (const AttrAst& a : in) {
            Attr attr;
            attr.name = a.name;
            attr.hasParens = a.hasParens;
            attr.loc = a.loc;
            for (const AttrArgAst& arg : a.args) attr.args.push_back(AttrArg{arg.key, arg.value, arg.isString, arg.loc});
            out.push_back(std::move(attr));
        }
        return out;
    }

    void declareAll() {
        for (auto& fp : S.files) {
            SourceFile* f = fp.get();
            for (const std::string& part : splitDots(f->ast.package)) {
                if (isCppKeyword(part))
                    D.error(f->ast.packageLoc, std::format("package component '{}' is a C++ keyword (packages become C++ namespaces)", part));
            }
            for (DeclAst& a : f->ast.decls) {
                Decl* d = S.newDecl();
                d->kind = mapKind(a.kind);
                d->name = a.name;
                d->package = f->ast.package;
                d->qualifiedName = qualify(d->package, a.name);
                d->file = f;
                d->loc = a.loc;
                d->doc = a.doc;
                d->attrs = convertAttrs(a.attrs);
                d->cppPath = {a.name};
                d->goName = a.name;
                d->emitted = f->generate;
                m_ast[d] = &a;
                Prim p;
                Builtin b;
                if (lookupBuiltinType(a.name, p, b) || std::find(std::begin(kGenericNames), std::end(kGenericNames), a.name) != std::end(kGenericNames)) {
                    D.error(a.loc, std::format("'{}' is a built-in type name and cannot be declared", a.name));
                    continue;
                }
                if (!registerName(d)) continue;
                f->decls.push_back(d);
                S.decls.push_back(d);
                if (d->kind == DeclKind::Record) {
                    const std::string base = a.name.size() > 3 && a.name.ends_with("Def") ? a.name.substr(0, a.name.size() - 3) : a.name;
                    const std::string refName = qualify(d->package, base + "Ref");
                    if (!m_refs.emplace(refName, d).second) {
                        D.error(a.loc, std::format("record '{}' declares '{}', which is already declared", a.name, refName));
                    }
                }
            }
        }
        // A record's generated Ref name must not collide with a declared type.
        for (const auto& [refName, rec] : m_refs) {
            auto it = S.declsByName.find(refName);
            if (it != S.declsByName.end()) {
                D.error(it->second->loc, std::format("'{}' collides with the reference type declared by record '{}'",
                                                     refName, rec->qualifiedName));
                D.note(rec->loc, "record declared here");
            }
        }
    }

    // --- name lookup ---------------------------------------------------------------------------
    /// Declared types visible from `ctx` (for suggestions).
    std::vector<std::string> visibleNames(const Decl* ctx) {
        std::vector<std::string> out;
        for (const BuiltinEntry& b : kBuiltins) out.emplace_back(b.name);
        auto addPkg = [&](const std::string& pkg) {
            for (const auto& [q, d] : S.declsByName) {
                if (d->package == pkg && q.size() > pkg.size() + 1) out.push_back(q.substr(pkg.empty() ? 0 : pkg.size() + 1));
            }
            for (const auto& [q, d] : m_refs) {
                if (d->package == pkg) out.push_back(q.substr(pkg.empty() ? 0 : pkg.size() + 1));
            }
        };
        addPkg(ctx->package);
        for (const SourceFile* imp : ctx->file->imports) addPkg(imp->ast.package);
        return out;
    }

    struct Lookup {
        Decl* decl = nullptr;   ///< type or alias declaration
        Decl* refOf = nullptr;  ///< FooRef -> record
    };

    /// Resolves a (possibly dotted) name as seen from `ctx`: enclosing declarations first, then the
    /// package and its parent packages (so `common.Point` finds `game.common.Point` from
    /// `game.main`, and fully qualified names work), then unqualified names of directly imported
    /// packages. Only declarations of the same file or of directly imported files are visible
    /// (the generated code includes exactly those headers). Sets `reported` when an error was
    /// already emitted (ambiguous or not imported).
    Lookup findName(const std::string& name, const Decl* ctx, SourceLoc loc, bool& reported) {
        reported = false;
        auto tryName = [&](const std::string& q) -> Lookup {
            if (auto it = S.declsByName.find(q); it != S.declsByName.end()) return Lookup{it->second, nullptr};
            if (auto it = m_refs.find(q); it != m_refs.end()) return Lookup{nullptr, it->second};
            return {};
        };
        const SourceFile* here = ctx->file;
        auto visible = [&](const Lookup& l) {
            const Decl* d = l.decl ? l.decl : l.refOf;
            return !here || d->file == here || std::find(here->imports.begin(), here->imports.end(), d->file) != here->imports.end();
        };
        Lookup hidden;
        auto consider = [&](const std::string& q) -> Lookup {
            const Lookup l = tryName(q);
            if (!l.decl && !l.refOf) return {};
            if (visible(l)) return l;
            if (!hidden.decl && !hidden.refOf) hidden = l;
            return {};
        };
        for (const Decl* c = ctx; c; c = c->outer) {
            if (Lookup l = consider(c->qualifiedName + "." + name); l.decl || l.refOf) return l;
        }
        for (std::string pkg = ctx->package;;) {
            if (Lookup l = consider(qualify(pkg, name)); l.decl || l.refOf) return l;
            if (pkg.empty()) break;
            const usize dot = pkg.rfind('.');
            pkg = dot == std::string::npos ? std::string() : pkg.substr(0, dot);
        }
        // Unqualified names of directly imported packages.
        Lookup found;
        std::string foundPkg;
        std::set<std::string> seen;
        if (here) {
            for (const SourceFile* imp : here->imports) {
                const std::string& pkg = imp->ast.package;
                if (pkg == ctx->package || !seen.insert(pkg).second) continue;
                const Lookup l = consider(qualify(pkg, name));
                if (!l.decl && !l.refOf) continue;
                if (found.decl || found.refOf) {
                    reported = true;
                    D.error(loc, std::format("'{}' is ambiguous: it is declared in packages '{}' and '{}' (qualify the name)", name,
                                             foundPkg, pkg));
                    return {};
                }
                found = l;
                foundPkg = pkg;
            }
        }
        if (!found.decl && !found.refOf && (hidden.decl || hidden.refOf)) {
            const Decl* d = hidden.decl ? hidden.decl : hidden.refOf;
            reported = true;
            D.error(loc, std::format("'{}' is declared in '{}', which is not imported by '{}' (add import \"{}\";)", name,
                                     d->file->path, here->path, d->file->logicalPath));
        }
        return found;
    }

    // --- types ---------------------------------------------------------------------------------
    const Type* primType(Prim p) {
        Type t;
        t.kind = TypeKind::Prim;
        t.prim = p;
        t.signature = std::string(primName(p));
        return S.intern(std::move(t));
    }
    const Type* builtinType(Builtin b) {
        Type t;
        t.kind = TypeKind::Builtin;
        t.builtin = b;
        t.signature = std::string(builtinName(b));
        return S.intern(std::move(t));
    }
    const Type* declType(Decl* d) {
        Type t;
        switch (d->kind) {
        case DeclKind::Enum: t.kind = TypeKind::Enum; break;
        case DeclKind::Flags: t.kind = TypeKind::Flags; break;
        case DeclKind::Variant: t.kind = TypeKind::Variant; break;
        default: t.kind = TypeKind::Struct; break;
        }
        t.decl = d;
        t.signature = d->qualifiedName;
        return S.intern(std::move(t));
    }
    const Type* wrapType(TypeKind kind, const Type* element, const Type* key = nullptr, u32 arraySize = 0) {
        Type t;
        t.kind = kind;
        t.element = element;
        t.key = key;
        t.arraySize = arraySize;
        switch (kind) {
        case TypeKind::List: t.signature = "list<" + element->signature + ">"; break;
        case TypeKind::KeyedList: t.signature = "keyed<" + element->signature + ">"; break;
        case TypeKind::Set: t.signature = "set<" + element->signature + ">"; break;
        case TypeKind::Map: t.signature = "map<" + key->signature + "," + element->signature + ">"; break;
        case TypeKind::Optional: t.signature = element->signature + "?"; break;
        case TypeKind::Array: t.signature = element->signature + "[" + std::to_string(arraySize) + "]"; break;
        default: break;
        }
        return S.intern(std::move(t));
    }
    const Type* recordRefType(Decl* rec) {
        Type t;
        t.kind = TypeKind::RecordRef;
        t.decl = rec;
        t.signature = "Ref<" + rec->qualifiedName + ">";
        return S.intern(std::move(t));
    }
    const Type* assetRefType(const std::string& kind) {
        Type t;
        t.kind = TypeKind::AssetRef;
        t.assetKind = kind;
        t.signature = "AssetRef<" + kind + ">";
        return S.intern(std::move(t));
    }

    /// New nested declaration for an inline type of field `fieldName` in `owner`.
    Decl* newNested(Decl* owner, DeclKind kind, const std::string& name, SourceLoc loc) {
        Decl* d = S.newDecl();
        d->kind = kind;
        d->name = name;
        d->package = owner->package;
        d->qualifiedName = owner->qualifiedName + "." + name;
        d->file = owner->file;
        d->loc = loc;
        d->outer = owner;
        d->emitted = owner->emitted;
        d->cppPath = owner->cppPath;
        d->cppPath.push_back(name);
        d->goName = owner->goName + name;
        for (Decl* n : owner->nested) {
            if (n->name == name) {
                D.error(loc, std::format("'{}' already has a nested type named '{}'", owner->qualifiedName, name));
                D.note(n->loc, "the other nested type is here");
            }
        }
        owner->nested.push_back(d);
        S.decls.push_back(d);
        S.declsByName.emplace(d->qualifiedName, d);
        return d;
    }

    const Type* resolveType(const TypeExpr& e, Decl* owner, const std::string& fieldName, std::string* inlineName = nullptr) {
        switch (e.kind) {
        case TypeExpr::Kind::Named: {
            Prim p;
            Builtin b;
            if (lookupBuiltinType(e.name, p, b)) return p != Prim::None ? primType(p) : builtinType(b);
            if (std::find(std::begin(kGenericNames), std::end(kGenericNames), e.name) != std::end(kGenericNames)) {
                D.error(e.loc, std::format("'{}' needs type arguments, e.g. {}<T>", e.name, e.name));
                return nullptr;
            }
            bool ambiguous = false;
            const Lookup l = findName(e.name, owner, e.loc, ambiguous);
            if (ambiguous) return nullptr;
            if (l.refOf) return recordRefType(l.refOf);
            if (!l.decl) {
                const std::string hint = suggest(e.name, visibleNames(owner));
                D.error(e.loc, hint.empty() ? std::format("unknown type '{}'", e.name)
                                            : std::format("unknown type '{}'; did you mean '{}'?", e.name, hint));
                return nullptr;
            }
            return typeOfDecl(l.decl, e.loc);
        }
        case TypeExpr::Kind::Generic: {
            const auto arity = [&](usize n) {
                if (e.args.size() == n) return true;
                D.error(e.loc, std::format("'{}' takes {} type argument{}, got {}", e.name, n, n == 1 ? "" : "s", e.args.size()));
                return false;
            };
            if (e.name == "list" || e.name == "set") {
                if (!arity(1)) return nullptr;
                const Type* el = resolveType(*e.args[0], owner, fieldName, inlineName);
                if (!el) return nullptr;
                if (e.name == "list") {
                    if (el->kind == TypeKind::Prim && el->prim == Prim::Bool) {
                        D.error(e.loc, "list<bool> is not supported; use flags or list<u8>");
                        return nullptr;
                    }
                    return wrapType(TypeKind::List, el);
                }
                if (!isKeyable(el)) {
                    D.error(e.args[0]->loc, std::format("set elements must be integers, enums, strings, Names, Guids, EntityIds or record refs, not '{}'", el->signature));
                    return nullptr;
                }
                return wrapType(TypeKind::Set, el);
            }
            if (e.name == "map") {
                if (!arity(2)) return nullptr;
                const Type* k = resolveType(*e.args[0], owner, fieldName, inlineName);
                const Type* v = resolveType(*e.args[1], owner, fieldName, inlineName);
                if (!k || !v) return nullptr;
                if (!isKeyable(k)) {
                    D.error(e.args[0]->loc, std::format("map keys must be integers, enums, strings, Names, Guids, EntityIds or record refs, not '{}'", k->signature));
                    return nullptr;
                }
                return wrapType(TypeKind::Map, v, k);
            }
            if (e.name == "AssetRef") {
                if (!arity(1)) return nullptr;
                if (e.args[0]->kind != TypeExpr::Kind::Named) {
                    D.error(e.args[0]->loc, "AssetRef<Kind> takes an asset kind name");
                    return nullptr;
                }
                return assetRefType(e.args[0]->name);
            }
            if (e.name == "Ref") {
                if (!arity(1)) return nullptr;
                const Type* t = resolveType(*e.args[0], owner, fieldName, inlineName);
                if (!t) return nullptr;
                if (t->kind != TypeKind::Struct || t->decl->kind != DeclKind::Record) {
                    D.error(e.args[0]->loc, std::format("Ref<T> needs a record type, '{}' is not a record", t->signature));
                    return nullptr;
                }
                return recordRefType(t->decl);
            }
            D.error(e.loc, std::format("unknown generic type '{}' (expected list, set, map, AssetRef or Ref)", e.name));
            return nullptr;
        }
        case TypeExpr::Kind::Optional: {
            const Type* inner = resolveType(*e.args[0], owner, fieldName, inlineName);
            if (!inner) return nullptr;
            if (inner->kind == TypeKind::Optional) {
                D.error(e.loc, "nested optionals (T? ?) are not supported");
                return nullptr;
            }
            if (inner->kind == TypeKind::List || inner->kind == TypeKind::KeyedList || inner->kind == TypeKind::Set ||
                inner->kind == TypeKind::Map) {
                D.error(e.loc, std::format("optional containers ('{}?') are not supported: an empty container already means 'none'",
                                           inner->signature));
                return nullptr;
            }
            return wrapType(TypeKind::Optional, inner);
        }
        case TypeExpr::Kind::Array: {
            const Type* inner = resolveType(*e.args[0], owner, fieldName, inlineName);
            if (!inner) return nullptr;
            if (inner->kind == TypeKind::Prim && inner->prim == Prim::Bool) {
                // std::array<bool, N> is fine in C++; allowed.
            }
            return wrapType(TypeKind::Array, inner, nullptr, static_cast<u32>(e.arraySize));
        }
        case TypeExpr::Kind::InlineStruct: {
            const std::string name = inlineName ? *inlineName : pascalCase(fieldName);
            Decl* d = newNested(owner, owner->kind == DeclKind::Variant ? DeclKind::Alternative : DeclKind::Struct, name, e.loc);
            resolveMembers(d, *e.members);
            return declType(d);
        }
        case TypeExpr::Kind::InlineEnum: {
            const std::string name = inlineName ? *inlineName : pascalCase(fieldName);
            Decl* d = newNested(owner, DeclKind::Enum, name, e.loc);
            resolveEnumValues(d, e.enumValues, nullptr);
            return declType(d);
        }
        case TypeExpr::Kind::InlineVariant: {
            const std::string name = inlineName ? *inlineName : pascalCase(fieldName);
            Decl* v = newNested(owner, DeclKind::Variant, name, e.loc);
            resolveVariant(v, e.alternatives);
            return declType(v);
        }
        }
        return nullptr;
    }

    const Type* typeOfDecl(Decl* d, SourceLoc loc) {
        switch (d->kind) {
        case DeclKind::Alias:
            resolveAlias(d);
            return d->aliasTarget;
        case DeclKind::Service:
        case DeclKind::Formula:
        case DeclKind::Const:
        case DeclKind::ScriptLib:
        case DeclKind::ScriptFn:
            D.error(loc, std::format("'{}' is a {}, not a type", d->qualifiedName, declKindName(d->kind)));
            return nullptr;
        default: return declType(d);
        }
    }

    void resolveVariant(Decl* v, const std::vector<AltAst>& alts) {
        std::set<std::string> names;
        if (alts.empty()) D.error(v->loc, "a variant needs at least one alternative");
        for (const AltAst& a : alts) {
            if (!names.insert(a.name).second) {
                D.error(a.loc, std::format("duplicate alternative '{}'", a.name));
                continue;
            }
            // Alternatives are sibling structs of the variant in C++ ("<Variant><Alt>").
            Decl* alt = S.newDecl();
            alt->kind = DeclKind::Alternative;
            alt->name = a.name;
            alt->package = v->package;
            alt->qualifiedName = v->qualifiedName + "." + a.name;
            alt->file = v->file;
            alt->loc = a.loc;
            alt->doc = a.doc;
            alt->outer = v;
            alt->emitted = v->emitted;
            alt->cppPath = std::vector<std::string>(v->cppPath.begin(), v->cppPath.end() - 1);
            alt->cppPath.push_back(v->name + a.name);
            alt->goName = v->goName + a.name;
            v->nested.push_back(alt);
            S.decls.push_back(alt);
            S.declsByName.emplace(alt->qualifiedName, alt);
            if (a.members) resolveMembers(alt, *a.members);
            declType(alt); // alternatives are addressable struct types (generators look them up)
            v->alternatives.push_back(Alternative{a.name, 0, alt, a.loc});
        }
    }

    void resolveAlias(Decl* d) {
        if (d->aliasTarget || m_resolvingAlias.contains(d)) {
            if (!d->aliasTarget && m_resolvingAlias.contains(d)) {
                D.error(d->loc, std::format("alias '{}' refers to itself", d->qualifiedName));
                m_resolvingAlias.erase(d);
                m_brokenAlias.insert(d);
            }
            return;
        }
        if (m_brokenAlias.contains(d)) return;
        if (m_resolvingAlias.size() >= kMaxAliasChain) {
            D.error(d->loc, std::format("alias chain through '{}' is longer than {} aliases", d->qualifiedName, kMaxAliasChain));
            m_brokenAlias.insert(d);
            return;
        }
        m_resolvingAlias.insert(d);
        const DeclAst* a = m_ast.at(d);
        const TypeExpr& e = *a->aliasTarget;
        if (e.kind == TypeExpr::Kind::InlineStruct || e.kind == TypeExpr::Kind::InlineEnum || e.kind == TypeExpr::Kind::InlineVariant) {
            // `alias X = { ... }` names the inline type X (it replaces the alias in C++ and Go).
            Decl* named = S.newDecl();
            named->kind = e.kind == TypeExpr::Kind::InlineStruct ? DeclKind::Struct
                          : e.kind == TypeExpr::Kind::InlineEnum ? DeclKind::Enum
                                                                  : DeclKind::Variant;
            named->name = d->name;
            named->package = d->package;
            named->qualifiedName = d->qualifiedName;
            named->file = d->file;
            named->loc = d->loc;
            named->doc = d->doc;
            named->attrs = d->attrs;
            named->emitted = d->emitted;
            named->cppPath = {d->name};
            named->goName = d->name;
            S.decls.push_back(named);
            d->aliasTarget = declType(named); // set before resolving members (self references)
            if (e.kind == TypeExpr::Kind::InlineStruct) {
                resolveMembers(named, *e.members);
            } else if (e.kind == TypeExpr::Kind::InlineEnum) {
                resolveEnumValues(named, e.enumValues, nullptr);
            } else {
                resolveVariant(named, e.alternatives);
            }
        } else {
            d->aliasTarget = resolveType(e, d, d->name);
        }
        m_resolvingAlias.erase(d);
    }

    void resolveEnumValues(Decl* d, const std::vector<EnumValueAst>& values, const TypeExpr* base) {
        const bool flags = d->kind == DeclKind::Flags;
        d->underlying = Prim::U32;
        if (base) {
            Prim p;
            Builtin b;
            if (base->kind != TypeExpr::Kind::Named || !lookupBuiltinType(base->name, p, b) || !isIntegerPrim(p)) {
                D.error(base->loc, std::format("the underlying type of {} '{}' must be an integer type (u8, i16, ...)",
                                               declKindName(d->kind), d->name));
            } else if (flags && isSignedPrim(p)) {
                D.error(base->loc, "flags need an unsigned underlying type");
            } else {
                d->underlying = p;
            }
        }
        if (values.empty()) D.error(d->loc, std::format("{} '{}' needs at least one value", declKindName(d->kind), d->name));
        i64 lo = 0;
        u64 hi = 0;
        primRange(d->underlying, lo, hi);
        i64 next = flags ? 1 : 0;
        std::map<std::string, SourceLoc> names;
        std::map<i64, std::string> used;
        for (const EnumValueAst& va : values) {
            EnumVal v;
            v.name = va.name;
            v.loc = va.loc;
            v.doc = va.doc;
            v.attrs = convertAttrs(va.attrs);
            if (va.value) {
                const LiteralAst& lit = *va.value;
                u64 mag = 0;
                if (lit.kind != LiteralAst::Kind::Int || !parseUnsigned(lit.text, mag)) {
                    D.error(lit.loc, std::format("the value of '{}' must be an integer", va.name));
                    continue;
                }
                if (lit.negative) {
                    const u64 limit = isSignedPrim(d->underlying) ? static_cast<u64>(-(lo + 1)) + 1 : 0;
                    if (!isSignedPrim(d->underlying) || mag > limit) {
                        D.error(lit.loc, std::format("value -{} of '{}' does not fit {}", lit.text, va.name, primName(d->underlying)));
                        continue;
                    }
                    v.value = mag == limit ? lo : -static_cast<i64>(mag);
                } else {
                    if (mag > hi) {
                        D.error(lit.loc, std::format("value {} of '{}' does not fit {}", lit.text, va.name, primName(d->underlying)));
                        continue;
                    }
                    v.value = static_cast<i64>(mag);
                }
            } else {
                if (static_cast<u64>(next) > hi || (flags && next == 0)) {
                    D.error(va.loc, std::format("implicit value of '{}' does not fit {}", va.name, primName(d->underlying)));
                    continue;
                }
                v.value = next;
            }
            if (flags) {
                const u64 bits = static_cast<u64>(v.value);
                if (bits != 0 && (bits & (bits - 1)) == 0) next = std::max<i64>(next, static_cast<i64>(bits << 1));
                if (!va.value && bits != 0) next = static_cast<i64>(bits << 1);
            } else {
                next = v.value + 1;
            }
            if (auto [it, ok] = names.emplace(v.name, v.loc); !ok) {
                D.error(v.loc, std::format("duplicate value name '{}'", v.name));
                D.note(it->second, "previous value is here");
                continue;
            }
            if (auto [it, ok] = used.emplace(v.value, v.name); !ok) {
                D.error(v.loc, std::format("'{}' has the same value ({}) as '{}'", v.name, v.value, it->second));
                continue;
            }
            d->values.push_back(std::move(v));
        }
    }

    void resolveMembers(Decl* d, const MembersAst& members) { resolveFields(d, members.fields); }

    void resolveFields(Decl* d, const std::vector<FieldAst>& fields) {
        std::map<std::string, SourceLoc> seen;
        for (const FieldAst& fa : fields) {
            Field f;
            f.name = fa.name;
            f.loc = fa.loc;
            f.doc = fa.doc;
            f.block = fa.block;
            f.attrs = convertAttrs(fa.attrs);
            f.serverOnly = fa.block == Block::Server;
            f.clientOnly = fa.block == Block::Client;
            f.editorOnly = fa.block == Block::Editor;
            if (auto [it, ok] = seen.emplace(fa.name, fa.loc); !ok) {
                D.error(fa.loc, std::format("duplicate field '{}' in '{}'", fa.name, d->qualifiedName));
                D.note(it->second, "previous field is here");
                continue;
            }
            f.type = resolveType(*fa.type, d, fa.name);
            if (!f.type) continue;
            if (const Attr* keyed = f.attr("keyed")) {
                if (f.type->kind != TypeKind::List || f.type->element->kind != TypeKind::Struct) {
                    D.error(keyed->loc, std::format("@keyed needs a list of structs, '{}' is '{}'", f.name, f.type->signature));
                } else if (keyed->args.empty()) {
                    f.type = wrapType(TypeKind::KeyedList, f.type->element);
                } else {
                    f.keyedBy = keyed->args[0].value;
                }
            }
            f.defaultLiteral = fa.defaultValue ? &*fa.defaultValue : nullptr;
            d->fields.push_back(std::move(f));
        }
    }

    void resolveTopLevel(Decl* d) {
        const DeclAst& a = *m_ast.at(d);
        switch (d->kind) {
        case DeclKind::Enum:
        case DeclKind::Flags: resolveEnumValues(d, a.enumValues, a.base.get()); break;
        case DeclKind::Alias: resolveAlias(d); break;
        case DeclKind::Const: {
            d->constType = a.base ? resolveType(*a.base, d, d->name) : nullptr;
            if (d->constType) {
                const Type* t = d->constType;
                const bool ok = (t->kind == TypeKind::Prim) || (t->kind == TypeKind::Builtin && t->builtin == Builtin::Duration);
                if (!ok) {
                    D.error(a.base->loc, std::format("const '{}' must have a scalar, string or Duration type", d->name));
                    d->constType = nullptr;
                }
            }
            break;
        }
        case DeclKind::Formula:
            d->formulaParams = a.formulaParams;
            d->formulaBody = a.exprText;
            break;
        case DeclKind::Service:
            for (const RpcAst& r : a.members.rpcs) {
                Decl* m = S.newDecl();
                m->kind = DeclKind::Rpc;
                m->name = d->name + r.name + "Request";
                m->rpcName = r.name;
                m->service = d;
                m->package = d->package;
                m->qualifiedName = qualify(d->package, m->name);
                m->file = d->file;
                m->loc = r.loc;
                m->doc = r.doc;
                m->attrs = convertAttrs(r.attrs);
                m->direction = r.direction;
                m->emitted = d->emitted;
                m->cppPath = {m->name};
                m->goName = m->name;
                if (!registerName(m)) continue;
                S.decls.push_back(m);
                d->file->decls.push_back(m); // request structs are top-level types of the file
                d->methods.push_back(m);
                resolveParams(m, r.params);
                if (r.result) m->result = resolveType(*r.result, m, r.name + "Result");
            }
            if (!a.members.fields.empty()) D.error(a.members.fields[0].loc, "a service contains only rpc declarations");
            break;
        case DeclKind::ScriptLib: resolveScriptLib(d, a); break;
        case DeclKind::Rpc:
            d->direction = a.direction;
            if (d->direction.empty())
                D.error(d->loc, std::format("rpc '{}' needs a direction (client->server, server->client or server->server)", d->name));
            resolveParams(d, a.params);
            if (a.result) d->result = resolveType(*a.result, d, d->name + "Result");
            break;
        default:
            if (a.base) {
                D.error(a.base->loc, std::format("base types (': Base') are only allowed on enum and flags; embed '{}' as a field instead",
                                                 a.base->name));
            }
            resolveMembers(d, a.members);
            if (!a.members.rpcs.empty()) D.error(a.members.rpcs[0].loc, "rpc declarations are only allowed inside a service");
            break;
        }
    }

    void resolveParams(Decl* d, const std::vector<FieldAst>& params) { resolveFields(d, params); }

    /// True if `e` contains an inline struct/enum/variant (not allowed in scriptlib signatures:
    /// they would become nested types of a declaration that generates no C++ or Go type).
    static const TypeExpr* findInlineType(const TypeExpr& e) {
        if (e.kind == TypeExpr::Kind::InlineStruct || e.kind == TypeExpr::Kind::InlineEnum || e.kind == TypeExpr::Kind::InlineVariant)
            return &e;
        for (const auto& arg : e.args) {
            if (const TypeExpr* x = findInlineType(*arg)) return x;
        }
        return nullptr;
    }

    // --- scriptlibs (02 §3.1, §7.4) ------------------------------------------------------------
    void resolveScriptLib(Decl* lib, const DeclAst& a) {
        std::map<std::string, SourceLoc> seen;
        for (const FnAst& fa : a.members.fns) {
            if (auto [it, ok] = seen.emplace(fa.name, fa.loc); !ok) {
                D.error(fa.loc, std::format("duplicate fn '{}' in scriptlib '{}'", fa.name, lib->name));
                D.note(it->second, "previous fn is here");
                continue;
            }
            Decl* fn = S.newDecl();
            fn->kind = DeclKind::ScriptFn;
            fn->name = fa.name;
            fn->package = lib->package;
            fn->qualifiedName = lib->qualifiedName + "." + fa.name;
            fn->file = lib->file;
            fn->loc = fa.loc;
            fn->doc = fa.doc;
            fn->attrs = convertAttrs(fa.attrs);
            fn->outer = lib;
            fn->emitted = lib->emitted;
            fn->cppPath = {lib->name, fa.name};
            fn->goName = lib->goName + pascalCase(fa.name);
            S.decls.push_back(fn);
            lib->methods.push_back(fn);
            bool inlineError = false;
            for (const FieldAst& p : fa.params) {
                if (const TypeExpr* x = findInlineType(*p.type)) {
                    D.error(x->loc, std::format("inline types are not allowed in the signature of fn '{}'; declare a named type", fa.name));
                    inlineError = true;
                }
            }
            if (fa.result) {
                if (const TypeExpr* x = findInlineType(*fa.result)) {
                    D.error(x->loc, std::format("inline types are not allowed in the signature of fn '{}'; declare a named type", fa.name));
                    inlineError = true;
                }
            }
            if (inlineError) continue;
            resolveParams(fn, fa.params);
            if (fa.result) fn->result = resolveType(*fa.result, fn, fa.name + "Result");
        }
    }

    /// `@realm(server|client|editor)` values; false (with a diagnostic) on bad values.
    bool readRealms(const Attr& a, std::vector<std::string>& out) {
        bool ok = true;
        for (const AttrArg& arg : a.args) {
            if (!arg.key.empty() || (arg.value != "server" && arg.value != "client" && arg.value != "editor")) {
                D.error(arg.loc, std::format("invalid @realm({}): expected server, client or editor", arg.value));
                ok = false;
                continue;
            }
            if (std::find(out.begin(), out.end(), arg.value) != out.end()) {
                D.error(arg.loc, std::format("@realm lists '{}' twice", arg.value));
                ok = false;
                continue;
            }
            out.push_back(arg.value);
        }
        return ok;
    }

    void validateScriptLib(Decl* lib) {
        checkAttrs(lib->attrs, TType);
        if (m_options.namingLints && !isPascalCase(lib->name)) D.warning(lib->loc, std::format("scriptlib name '{}' should be PascalCase", lib->name));
        for (const Attr& a : lib->attrs) {
            const AttrSpec* spec = findSpec(a.name); // unknown / non-type attributes were reported by checkAttrs
            if (spec && (spec->targets & TType) != 0 && a.name != "realm" && a.name != "doc" && a.name != "deprecated")
                D.error(a.loc, std::format("@{} is not valid on a scriptlib (use @realm, @doc or @deprecated)", a.name));
        }
        std::vector<std::string> libRealms;
        if (const Attr* r = lib->attr("realm")) readRealms(*r, libRealms);
        applyTypeAttrs(lib);
        if (lib->methods.empty()) D.warning(lib->loc, std::format("scriptlib '{}' declares no fn", lib->name));
        for (Decl* fn : lib->methods) validateScriptFn(fn, libRealms);
    }

    /// Lint (02 §3.2, §7.4): every fn needs @script(cost=n); `each` needs `of` (and vice versa);
    /// `of=result` needs @pure and a container result; `of=<param>` must name a parameter.
    void validateScriptFn(Decl* fn, const std::vector<std::string>& libRealms) {
        checkAttrs(fn->attrs, TFn);
        if (m_options.namingLints && !isCamelCase(fn->name)) D.warning(fn->loc, std::format("fn name '{}' should be camelCase", fn->name));
        applyTypeAttrs(fn);
        for (Field& p : fn->fields) {
            checkAttrs(p.attrs, TField);
            if (m_options.namingLints && !isCamelCase(p.name)) D.warning(p.loc, std::format("parameter name '{}' should be camelCase", p.name));
            bool forbidden = false;
            for (const Attr& a : p.attrs) {
                if (a.name == "quant" || a.name == "predicted" || a.name == "interp" || a.name == "lod" || a.name == "was" ||
                    a.name == "keyed" || a.name == "merge" || a.name == "server_only" || a.name == "client_only" || a.name == "opaque") {
                    D.error(a.loc, std::format("@{} is not valid on a fn parameter", a.name));
                    forbidden = true;
                }
            }
            if (!forbidden) validateField(fn, p, false); // (its field-context messages would only repeat the error)
        }
        ScriptCost& cost = fn->cost;
        cost.realms = libRealms;
        if (const Attr* r = fn->attr("realm")) {
            cost.realms.clear();
            readRealms(*r, cost.realms);
        }
        if (cost.realms.empty()) D.warning(fn->loc, std::format("fn '{}' has no @realm (on it or its scriptlib): no script realm can call it", fn->name));
        cost.pure = fn->attr("pure") != nullptr;
        if (const Attr* m = fn->attr("max"); m && !m->args.empty()) {
            u64 n = 0;
            const Type* t = fn->result && fn->result->kind == TypeKind::Optional ? fn->result->element : fn->result;
            if (!parseUnsigned(m->args[0].value, n) || n == 0) {
                D.error(m->loc, "@max needs a positive integer");
            } else if (!t || !(t->isContainer() || (t->kind == TypeKind::Prim && (t->prim == Prim::String || t->prim == Prim::Name)))) {
                D.error(m->loc, std::format("@max on fn '{}' needs a list, set, map or string result", fn->name));
            }
        }
        const Attr* script = fn->attr("script");
        if (!script) {
            D.error(fn->loc, std::format("fn '{}' needs @script(cost=n): every script-callable function charges fuel (02 §7.4)", fn->name));
            return;
        }
        bool hasCost = false;
        bool hasEach = false;
        const AttrArg* ofArg = nullptr;
        for (const AttrArg& arg : script->args) {
            if (arg.key == "cost" || arg.key == "each") {
                u64 n = 0;
                if (!parseUnsigned(arg.value, n) || n > 0xFFFFFFFFull) {
                    D.error(arg.loc, std::format("@script({}=) needs a non-negative integer, got '{}'", arg.key, arg.value));
                    continue;
                }
                (arg.key == "cost" ? cost.base : cost.each) = n;
                (arg.key == "cost" ? hasCost : hasEach) = true;
            } else if (arg.key == "of") {
                ofArg = &arg;
                cost.of = arg.value;
            } else {
                D.error(arg.loc, arg.key.empty() ? std::string("@script on a fn takes named arguments: cost=, each=, of=")
                                                 : std::format("unknown @script argument '{}' (cost, each, of)", arg.key));
            }
        }
        if (!hasCost) D.error(script->loc, std::format("fn '{}' needs @script(cost=n) (02 §7.4)", fn->name));
        if (hasEach != (ofArg != nullptr)) {
            D.error(script->loc, std::format("@script on fn '{}': each= and of= go together (per-element charge of a parameter or the result)", fn->name));
        }
        if (!ofArg) return;
        if (cost.of == "result") {
            if (!cost.pure) D.error(ofArg->loc, std::format("fn '{}' charges per result element (of=result) and must be @pure (02 §3.2)", fn->name));
            const Type* t = fn->result && fn->result->kind == TypeKind::Optional ? fn->result->element : fn->result;
            if (!t || !t->isContainer()) D.error(ofArg->loc, std::format("of=result needs a list, set or map result, fn '{}' returns {}", fn->name,
                                                                         fn->result ? fn->result->signature : std::string("nothing")));
            return;
        }
        const bool isParam = std::any_of(fn->fields.begin(), fn->fields.end(), [&](const Field& p) { return p.name == cost.of; });
        if (!isParam) D.error(ofArg->loc, std::format("@script(of={}): fn '{}' has no parameter '{}' (use a parameter name or 'result')", cost.of, fn->name, cost.of));
    }

    // --- components ----------------------------------------------------------------------------
    void splitComponent(Decl* d) {
        std::vector<Field> shared;
        std::vector<Field> server;
        std::vector<Field> client;
        for (Field& f : d->fields) {
            switch (f.block) {
            case Block::Server:
                f.serverOnly = false; // the part itself is server-only
                server.push_back(std::move(f));
                break;
            case Block::Client:
                f.clientOnly = false;
                client.push_back(std::move(f));
                break;
            case Block::Editor:
                D.error(f.loc, "components have no editor {} block; put editor-only data in an @authoring component");
                break;
            default: shared.push_back(std::move(f)); break;
            }
        }
        if (!server.empty()) {
            d->serverPart = newNested(d, DeclKind::ComponentPart, "Server", d->loc);
            d->serverPart->fields = std::move(server);
        }
        if (!client.empty()) {
            d->clientPart = newNested(d, DeclKind::ComponentPart, "Client", d->loc);
            d->clientPart->fields = std::move(client);
        }
        d->fields = std::move(shared);
    }

    // --- validation ----------------------------------------------------------------------------
    void checkAttrs(const std::vector<Attr>& attrs, Target target, std::set<std::string>* seenOut = nullptr) {
        std::set<std::string> seen;
        for (const Attr& a : attrs) {
            const AttrSpec* spec = findSpec(a.name);
            if (!spec) {
                std::vector<std::string> names;
                for (const AttrSpec& s : kAttrSpecs) names.emplace_back(s.name);
                const std::string hint = suggest(a.name, names);
                D.warning(a.loc, hint.empty() ? std::format("unknown attribute '@{}'", a.name)
                                              : std::format("unknown attribute '@{}'; did you mean '@{}'?", a.name, hint));
                continue;
            }
            if ((spec->targets & target) == 0) {
                D.error(a.loc, std::format("@{} is not valid on {}", a.name, targetName(target)));
                continue;
            }
            const int n = static_cast<int>(a.args.size());
            if (n < spec->minArgs || (spec->maxArgs >= 0 && n > spec->maxArgs)) {
                if (spec->minArgs == spec->maxArgs) {
                    D.error(a.loc, std::format("@{} takes {} argument{}, got {}", a.name, spec->minArgs, spec->minArgs == 1 ? "" : "s", n));
                } else if (spec->maxArgs < 0) {
                    D.error(a.loc, std::format("@{} takes at least {} argument{}, got {}", a.name, spec->minArgs,
                                               spec->minArgs == 1 ? "" : "s", n));
                } else {
                    D.error(a.loc, std::format("@{} takes {} to {} arguments, got {}", a.name, spec->minArgs, spec->maxArgs, n));
                }
                continue;
            }
            if (!seen.insert(a.name).second) D.error(a.loc, std::format("duplicate attribute @{}", a.name));
        }
        if (seenOut) *seenOut = std::move(seen);
    }

    bool argIn(const Attr& a, std::initializer_list<std::string_view> allowed) {
        const std::string& v = a.args[0].value;
        for (const std::string_view s : allowed) {
            if (v == s) return true;
        }
        std::string list;
        for (const std::string_view s : allowed) list += (list.empty() ? "" : ", ") + std::string(s);
        D.error(a.args[0].loc, std::format("invalid @{}({}): expected one of {}", a.name, v, list));
        return false;
    }

    bool isServerOnlyType(const Decl* d) const {
        for (const Decl* c = d; c; c = c->outer) {
            if (c->attr("server_only") || (c->kind == DeclKind::ComponentPart && c->name == "Server")) return true;
        }
        return false;
    }

    /// First server-only type reachable from `t` (by value or reference), or null.
    const Decl* serverOnlyReference(const Type* t) const {
        if (!t) return nullptr;
        switch (t->kind) {
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Flags:
        case TypeKind::Variant:
        case TypeKind::RecordRef: return isServerOnlyType(t->decl) ? t->decl : nullptr;
        case TypeKind::Map:
            if (const Decl* k = serverOnlyReference(t->key)) return k;
            return serverOnlyReference(t->element);
        case TypeKind::List:
        case TypeKind::KeyedList:
        case TypeKind::Set:
        case TypeKind::Optional:
        case TypeKind::Array: return serverOnlyReference(t->element);
        default: return nullptr;
        }
    }

    void validateDecl(Decl* d) {
        // Names become C++ identifiers verbatim (fields get a '_' suffix instead, see cppFieldName).
        if (isCppKeyword(d->name) && d->kind != DeclKind::ComponentPart && d->kind != DeclKind::ScriptFn)
            D.error(d->loc, std::format("'{}' is a C++ keyword and cannot name a {}", d->name, declKindName(d->kind)));
        for (const EnumVal& v : d->values) {
            if (isCppKeyword(v.name)) D.error(v.loc, std::format("'{}' is a C++ keyword and cannot name an enum value", v.name));
        }
        switch (d->kind) {
        case DeclKind::Formula:
            checkAttrs(d->attrs, TType);
            return;
        case DeclKind::Const:
            checkAttrs(d->attrs, TType);
            if (d->constType) {
                const DeclAst& a = *m_ast.at(d);
                if (auto v = resolveValue(*a.constValue, d->constType, std::format("const '{}'", d->name))) d->constValue = *v;
            }
            return;
        case DeclKind::Alias:
            checkAttrs(d->attrs, TType);
            return;
        case DeclKind::Service:
            checkAttrs(d->attrs, TType);
            return;
        case DeclKind::ScriptLib: validateScriptLib(d); return;
        case DeclKind::ScriptFn: return; // validated with its scriptlib
        case DeclKind::Enum:
        case DeclKind::Flags:
            checkAttrs(d->attrs, TType);
            for (const EnumVal& v : d->values) {
                checkAttrs(v.attrs, TEnumValue);
                if (m_options.namingLints && !isPascalCase(v.name))
                    D.warning(v.loc, std::format("enum value '{}' should be PascalCase", v.name));
            }
            if (m_options.namingLints && !isPascalCase(d->name)) D.warning(d->loc, std::format("type name '{}' should be PascalCase", d->name));
            applyTypeAttrs(d);
            return;
        case DeclKind::Variant:
            applyTypeAttrs(d);
            return;
        default: break;
        }
        if (!d->isStructLike()) return;
        const Target target = d->kind == DeclKind::Rpc ? TRpc : TType;
        checkAttrs(d->attrs, target);
        if (m_options.namingLints && !isPascalCase(d->name) && d->kind != DeclKind::Rpc)
            D.warning(d->loc, std::format("type name '{}' should be PascalCase", d->name));
        applyTypeAttrs(d);

        // Component replication.
        if (d->kind == DeclKind::Component) {
            if (const Attr* r = d->attr("replicate"); r && !r->args.empty() && argIn(*r, {"all", "owner", "server", "none"})) {
                const std::string& v = r->args[0].value;
                d->replicate = v == "all" ? RepAudience::All : v == "owner" ? RepAudience::Owner : v == "server" ? RepAudience::Server : RepAudience::None;
            }
            if (const Attr* l = d->attr("lod"); l && !l->args.empty()) d->lod = l->args[0].value;
            if (d->isReplicatedComponent()) {
                if (d->fields.size() > 64) {
                    D.error(d->loc, std::format("component '{}' has {} replicated fields; at most 64 fit the dirty mask (move some into a server {{}} block or split the component)",
                                                d->name, d->fields.size()));
                }
                u8 index = 0;
                for (Field& f : d->fields) {
                    f.replicated = true;
                    f.repIndex = index < 64 ? index : 0xFF;
                    ++index;
                }
            }
        } else {
            for (std::string_view only : {"replicate", "tag", "shared", "sparse", "singleton"}) {
                if (const Attr* a = d->attr(only)) D.error(a->loc, std::format("@{} is only valid on components", only));
            }
        }
        for (std::string_view only : {"exclusive", "acyclic", "target"}) {
            if (const Attr* a = d->attr(only); a && d->kind != DeclKind::Relation)
                D.error(a->loc, std::format("@{} is only valid on relations", only));
        }
        if (const Attr* t = d->attr("table"); t && d->kind != DeclKind::Record) D.error(t->loc, "@table is only valid on records");
        if (const Attr* r = d->attr("realm")) D.error(r->loc, "@realm is only valid on scriptlibs and their fns");
        for (std::string_view only : {"client", "server"}) {
            if (const Attr* a = d->attr(only); a && d->kind != DeclKind::ViewModel)
                D.error(a->loc, std::format("@{} is only valid on viewmodels (use a {} {{}} block for fields)", only, only));
        }

        // Persistence lints (05, ADR-008).
        const Attr* store = d->attr("store");
        if (store && !store->args.empty()) argIn(*store, {"checkpoint", "ledger", "character", "activity", "config"});
        const bool ledger = store && !store->args.empty() && store->args[0].value == "ledger";
        if (ledger && d->attr("persist"))
            D.error(d->attr("persist")->loc, std::format("'{}' is @store(ledger): ledger data is never @persist write-behind (ADR-008)", d->name));
        if (const Attr* lp = d->attr("ledger_policy"); lp && !ledger)
            D.error(lp->loc, "@ledger_policy requires @store(ledger)");

        // Messages (AAA-SEC-1) and reason codes (06 §4).
        if (d->kind == DeclKind::Rpc) validateRpc(d);

        const bool replicatedComponent = d->isReplicatedComponent();
        const bool declServerOnly = isServerOnlyType(d);
        for (usize i = 0; i < d->fields.size(); ++i) {
            Field& f = d->fields[i];
            checkAttrs(f.attrs, TField);
            // C++: a member cannot share the name of its class or of a nested type.
            if (f.name == d->cppPath.back())
                D.error(f.loc, std::format("field '{}' has the name of its type '{}' (not valid in C++); rename it", f.name, d->name));
            for (const Decl* n : d->nested) {
                if (n->cppPath.back() == f.name && n->kind != DeclKind::Alternative)
                    D.error(f.loc, std::format("field '{}' has the name of the nested type '{}' (not valid in C++); rename it", f.name,
                                               n->qualifiedName));
            }
            if (replicatedComponent && pascalCase(f.name) == "AllFields")
                D.error(f.loc, std::format("field '{}' clashes with Mut<{}>::kAllFields; rename it", f.name, d->name));
            if (m_options.namingLints && !isCamelCase(f.name) && d->kind != DeclKind::Relation)
                D.warning(f.loc, std::format("field name '{}' should be camelCase", f.name));
            validateField(d, f, replicatedComponent);
            if (f.attr("server_only")) f.serverOnly = true;
            if (f.attr("client_only")) f.clientOnly = true;
            if (f.attr("predicted")) f.predicted = true;
            // AAA-SEC-4: shared data must not reference server-only types.
            if (!declServerOnly && !f.serverOnly) {
                if (const Decl* so = serverOnlyReference(f.type); so && !f.attr("opaque")) {
                    D.error(f.loc, std::format("shared field '{}' references server-only type '{}'; mark it @opaque or move it into a server {{}} block (AAA-SEC-4)",
                                               f.name, so->qualifiedName));
                }
            }
            // Default value.
            if (f.defaultLiteral) {
                f.defaultValue = resolveValue(*f.defaultLiteral, f.type, std::format("field '{}'", f.name));
                if (f.defaultValue) checkRangeDefault(f);
            }
        }
    }

    void applyTypeAttrs(Decl* d) {
        if (const Attr* v = d->attr("version"); v && !v->args.empty()) {
            u64 n = 0;
            if (!parseUnsigned(v->args[0].value, n) || n == 0 || n > 0xFFFFFFFFull) {
                D.error(v->args[0].loc, "@version needs a positive integer");
            } else {
                d->version = static_cast<u32>(n);
            }
        }
        if (const Attr* w = d->attr("was")) {
            for (const AttrArg& a : w->args) {
                if (!isIdentifier(a.value, true)) {
                    D.error(a.loc, std::format("@was(\"{}\") must name the old type (a name or qualified name)", a.value));
                    continue;
                }
                d->was.push_back(a.value);
            }
        }
        if (const Attr* doc = d->attr("doc"); doc && !doc->args.empty()) d->doc += (d->doc.empty() ? "" : "\n") + doc->args[0].value;
    }

    void validateRpc(Decl* d) {
        for (const Attr& a : d->attrs) {
            if (a.name == "timeout" && !a.args.empty() && !parseDuration(a.args[0].value))
                D.error(a.args[0].loc, std::format("invalid @timeout({}): expected a duration like 500ms", a.args[0].value));
            if ((a.name == "ratelimit" || a.name == "rate") && !a.args.empty()) {
                const std::string& v = a.args[0].value;
                const usize slash = v.find('/');
                f64 n = 0;
                const std::string unit = slash == std::string::npos ? "" : v.substr(slash + 1);
                if (slash == std::string::npos || !parseNumber(v.substr(0, slash), n) || n <= 0 ||
                    (unit != "s" && unit != "m" && unit != "h"))
                    D.error(a.args[0].loc, std::format("invalid @{}({}): expected a rate like 2/s", a.name, v));
            }
        }
        // AAA-SEC-1 applies to every client->server call, including rpcs of a service.
        if (d->direction == "client->server") {
            const bool rate = d->attr("ratelimit") || d->attr("rate");
            const bool intent = d->attr("intent") != nullptr;
            if (!rate || !intent) {
                const std::string rpcName = d->service ? d->service->name + "." + d->rpcName : d->name;
                D.error(d->loc, std::format("client->server rpc '{}' needs {}{}{} (AAA-SEC-1)", rpcName, rate ? "" : "@ratelimit(n/s)",
                                            !rate && !intent ? " and " : "", intent ? "" : "@intent(...)"));
            }
        }
        if (const Attr* rr = d->attr("reason_required")) {
            auto isReason = [](const Type* t) {
                if (t->kind == TypeKind::Optional) t = t->element;
                return t->kind == TypeKind::RecordRef && t->decl->name == "ReasonCodeDef";
            };
            bool found = false;
            for (const Field& f : d->fields) {
                if (!f.type) continue;
                if (isReason(f.type)) found = true;
                if (f.type->kind == TypeKind::Struct) {
                    for (const Field& inner : f.type->decl->fields) {
                        if (inner.type && isReason(inner.type)) found = true;
                    }
                }
            }
            if (!found) {
                D.error(rr->loc, std::format("rpc '{}' is @reason_required but carries no ReasonCodeRef (directly or in a parameter struct, 06 §4)",
                                             d->rpcName.empty() ? d->name : d->rpcName));
            }
        }
    }

    void validateField(Decl* d, Field& f, bool replicatedComponent) {
        if (!f.type) return;
        for (const Attr& a : f.attrs) {
            if (a.name == "quant" || a.name == "predicted" || a.name == "interp") {
                if (!replicatedComponent) D.error(a.loc, std::format("@{} is only valid on fields of replicated components", a.name));
            } else if (a.name == "lod") {
                if (!replicatedComponent) D.error(a.loc, "@lod is only valid on replicated components and their fields");
            } else if (a.name == "range") {
                f64 lo = 0, hi = 0;
                if (a.args.size() == 2 && (!parseNumber(a.args[0].value, lo) || !parseNumber(a.args[1].value, hi))) {
                    D.error(a.loc, "@range needs two numbers: @range(min, max)");
                } else if (lo > hi) {
                    D.error(a.loc, std::format("@range min {} is greater than max {}", a.args[0].value, a.args[1].value));
                } else if (!isNumeric(f.type)) {
                    D.error(a.loc, std::format("@range needs a numeric field, '{}' is '{}'", f.name, f.type->signature));
                }
            } else if (a.name == "step") {
                f64 s = 0;
                if (!parseNumber(a.args[0].value, s) || s <= 0) D.error(a.loc, "@step needs a positive number");
            } else if (a.name == "max") {
                u64 n = 0;
                const Type* t = f.type->kind == TypeKind::Optional ? f.type->element : f.type;
                const bool sized = t->isContainer() || (t->kind == TypeKind::Prim && (t->prim == Prim::String || t->prim == Prim::Name)) ||
                                   (t->kind == TypeKind::Builtin && t->builtin == Builtin::TagSet);
                if (!parseUnsigned(a.args[0].value, n) || n == 0) {
                    D.error(a.loc, "@max needs a positive integer");
                } else if (!sized) {
                    D.error(a.loc, std::format("@max needs a list, set, map or string field, '{}' is '{}'", f.name, f.type->signature));
                }
            } else if (a.name == "editor") {
                for (const AttrArg& arg : a.args) {
                    if (arg.key.empty()) {
                        D.error(arg.loc, "@editor takes named arguments: category=, widget=, order=");
                    } else if (arg.key != "category" && arg.key != "widget" && arg.key != "order" && arg.key != "customizer") {
                        D.error(arg.loc, std::format("unknown @editor argument '{}' (category, widget, order, customizer)", arg.key));
                    } else if (arg.key == "order") {
                        f64 n = 0;
                        if (!parseNumber(arg.value, n) || n != std::floor(n) || n < static_cast<f64>(std::numeric_limits<i32>::min()) ||
                            n > static_cast<f64>(std::numeric_limits<i32>::max()))
                            D.error(arg.loc, "@editor(order=) needs a 32-bit integer");
                    }
                }
            } else if (a.name == "was") {
                for (const AttrArg& arg : a.args) {
                    if (!isIdentifier(arg.value, false)) {
                        D.error(arg.loc, std::format("@was(\"{}\") must name the old field (an identifier)", arg.value));
                        continue;
                    }
                    if (arg.value == f.name) {
                        D.error(arg.loc, std::format("@was(\"{}\") names the field itself", arg.value));
                        continue;
                    }
                    for (const Field& other : d->fields) {
                        if (&other != &f && other.name == arg.value)
                            D.error(arg.loc, std::format("@was(\"{}\"): '{}' is still a field of '{}'", arg.value, arg.value, d->name));
                    }
                    f.was.push_back(arg.value);
                }
            } else if (a.name == "keyed" && !a.args.empty() && f.type->kind == TypeKind::List) {
                const Decl* el = f.type->element->decl;
                const Field* key = nullptr;
                for (const Field& ef : el->fields) {
                    if (ef.name == f.keyedBy) key = &ef;
                }
                if (!key) {
                    D.error(a.args[0].loc, std::format("@keyed({}): '{}' has no field '{}'", f.keyedBy, el->qualifiedName, f.keyedBy));
                } else if (key->type && !isKeyable(key->type)) {
                    D.error(a.args[0].loc, std::format("@keyed({}): key field type '{}' is not a valid key", f.keyedBy, key->type->signature));
                }
            } else if (a.name == "merge") {
                if (a.args[0].value != "append") D.error(a.args[0].loc, "@merge supports only 'append'");
                if (f.type->kind != TypeKind::List) D.error(a.loc, "@merge(append) is only valid on lists");
            } else if (a.name == "script") {
                if (a.args.size() != 1) {
                    D.error(a.loc, "@script on a field takes one argument: read, write or none");
                } else {
                    argIn(a, {"read", "write", "none"});
                }
            } else if (a.name == "normalized") {
                if (f.type->kind != TypeKind::Builtin || tupleSize(f.type->builtin) == 0)
                    D.error(a.loc, "@normalized needs a vector or quaternion field");
            } else if (a.name == "asset") {
                const Type* t = f.type;
                while (t->kind == TypeKind::List || t->kind == TypeKind::Optional || t->kind == TypeKind::Array) t = t->element;
                if (t->kind != TypeKind::AssetRef) D.error(a.loc, "@asset needs an AssetRef field");
            } else if (a.name == "opaque") {
                if (!serverOnlyReference(f.type)) D.warning(a.loc, std::format("@opaque on '{}' has no effect (it references no server-only data)", f.name));
            }
        }
    }

    void checkRangeDefault(const Field& f) {
        const Attr* r = f.attr("range");
        if (!r || r->args.size() != 2 || !f.defaultValue) return;
        f64 lo = 0, hi = 0;
        if (!parseNumber(r->args[0].value, lo) || !parseNumber(r->args[1].value, hi)) return;
        const Value& v = *f.defaultValue;
        f64 x = 0;
        switch (v.kind) {
        case Value::Kind::Int: x = static_cast<f64>(v.i); break;
        case Value::Kind::UInt: x = static_cast<f64>(v.u); break;
        case Value::Kind::Float: x = v.f; break;
        default: return;
        }
        if (x < lo || x > hi) D.error(f.loc, std::format("default {} of '{}' is outside @range({}, {})", v.json, f.name, r->args[0].value, r->args[1].value));
    }

    // --- values --------------------------------------------------------------------------------
    std::optional<Value> resolveValue(const LiteralAst& lit, const Type* t, const std::string& what) {
        auto fail = [&](std::string msg) -> std::optional<Value> {
            D.error(lit.loc, std::format("invalid default for {}: {}", what, msg));
            return std::nullopt;
        };
        Value v;
        switch (t->kind) {
        case TypeKind::Prim:
            switch (t->prim) {
            case Prim::Bool:
                if (lit.kind != LiteralAst::Kind::Bool) return fail("expected true or false");
                v.kind = Value::Kind::Bool;
                v.b = lit.boolValue;
                v.json = v.b ? "true" : "false";
                return v;
            case Prim::String:
            case Prim::Name:
                if (lit.kind != LiteralAst::Kind::String) return fail("expected a string");
                v.kind = Value::Kind::String;
                v.s = lit.text;
                v.json = jsonQuote(v.s);
                return v;
            case Prim::F32:
            case Prim::F64: {
                if (lit.kind != LiteralAst::Kind::Int && lit.kind != LiteralAst::Kind::Float) return fail("expected a number");
                f64 x = 0;
                if (!parseNumber(lit.text, x)) return fail(std::format("'{}' is not a number", lit.text));
                if (lit.negative) x = -x;
                v.kind = Value::Kind::Float;
                if (t->prim == Prim::F32) {
                    if (std::fabs(x) > static_cast<f64>(std::numeric_limits<f32>::max())) return fail("value does not fit f32");
                    const f32 x32 = static_cast<f32>(x);
                    v.f = static_cast<f64>(x32);
                    v.json = formatF32(x32);
                } else {
                    if (!std::isfinite(x)) return fail("value does not fit f64");
                    v.f = x;
                    v.json = formatF64(x);
                }
                return v;
            }
            default: {
                if (lit.kind != LiteralAst::Kind::Int) return fail(std::format("expected an integer for {}", primName(t->prim)));
                u64 mag = 0;
                if (!parseUnsigned(lit.text, mag)) return fail(std::format("'{}' is not an integer", lit.text));
                i64 lo = 0;
                u64 hi = 0;
                primRange(t->prim, lo, hi);
                if (lit.negative) {
                    if (!isSignedPrim(t->prim)) return fail(std::format("{} is unsigned", primName(t->prim)));
                    const u64 limit = static_cast<u64>(-(lo + 1)) + 1; // |min|
                    if (mag > limit) return fail(std::format("-{} does not fit {}", lit.text, primName(t->prim)));
                    v.kind = Value::Kind::Int;
                    v.i = mag == limit ? lo : -static_cast<i64>(mag);
                    v.json = std::to_string(v.i);
                } else {
                    if (mag > hi) return fail(std::format("{} does not fit {}", lit.text, primName(t->prim)));
                    if (isSignedPrim(t->prim)) {
                        v.kind = Value::Kind::Int;
                        v.i = static_cast<i64>(mag);
                        v.json = std::to_string(v.i);
                    } else {
                        v.kind = Value::Kind::UInt;
                        v.u = mag;
                        v.json = std::to_string(v.u);
                    }
                }
                return v;
            }
            }
        case TypeKind::Builtin:
            switch (t->builtin) {
            case Builtin::LocString:
            case Builtin::TagQuery:
            case Builtin::HxlExpr:
                if (lit.kind != LiteralAst::Kind::String) return fail("expected a string");
                v.kind = Value::Kind::String;
                v.s = lit.text;
                if (t->builtin == Builtin::LocString) {
                    // Canonical JSONC writes loc keys as "loc:<key>" (02 §3.7); the prefix is optional here.
                    if (v.s.starts_with("loc:")) v.s.erase(0, 4);
                    v.json = jsonQuote("loc:" + v.s);
                } else {
                    v.json = jsonQuote(v.s);
                }
                return v;
            case Builtin::Guid: {
                if (lit.kind != LiteralAst::Kind::String || !parseGuid(lit.text, v.u, v.u2)) return fail("expected a GUID string");
                v.kind = Value::Kind::Guid;
                v.json = jsonQuote(formatGuid(v.u, v.u2));
                return v;
            }
            case Builtin::Duration: {
                if (lit.kind != LiteralAst::Kind::Unit) return fail("expected a duration like 500ms or 30d");
                auto ns = parseDuration((lit.negative ? "-" : "") + lit.text);
                if (!ns) return fail(std::format("'{}' is not a duration (units d, h, m, s, ms, us, ns)", lit.text));
                v.kind = Value::Kind::Duration;
                v.i = *ns;
                v.json = jsonQuote(formatDuration(v.i));
                return v;
            }
            case Builtin::EntityId:
            case Builtin::NetHandle:
            case Builtin::Tick: {
                u64 n = 0;
                if (lit.kind != LiteralAst::Kind::Int || lit.negative || !parseUnsigned(lit.text, n)) return fail("expected a non-negative integer");
                if (t->builtin == Builtin::NetHandle && n > 0xFFFFFFFFull) return fail("value does not fit a NetHandle (u32)");
                v.kind = Value::Kind::UInt;
                v.u = n;
                v.json = t->builtin == Builtin::EntityId ? jsonQuote("ent:" + std::to_string(n)) : std::to_string(n);
                return v;
            }
            default: {
                const u32 n = tupleSize(t->builtin);
                if (n == 0) return fail(std::format("default values are not supported for {}", t->signature));
                if (lit.kind != LiteralAst::Kind::List || lit.items.size() != n)
                    return fail(std::format("expected a list of {} numbers, e.g. [0, 0, 1]", n));
                v.kind = Value::Kind::List;
                v.json = "[";
                const Type* comp = primType(tupleIsF64(t->builtin) ? Prim::F64 : Prim::F32);
                for (usize i = 0; i < lit.items.size(); ++i) {
                    auto item = resolveValue(lit.items[i], comp, what);
                    if (!item) return std::nullopt;
                    v.json += (i ? "," : "") + item->json;
                    v.items.push_back(std::move(*item));
                }
                v.json += "]";
                return v;
            }
            }
        case TypeKind::Enum: {
            if (lit.kind != LiteralAst::Kind::Ident) return fail(std::format("expected a value of {}", t->decl->name));
            const std::vector<std::string> parts = splitDots(lit.text);
            const std::string& name = parts.back();
            for (const EnumVal& ev : t->decl->values) {
                if (ev.name == name) {
                    v.kind = Value::Kind::Enum;
                    v.s = name;
                    v.i = ev.value;
                    v.json = jsonQuote(name);
                    return v;
                }
            }
            std::vector<std::string> names;
            for (const EnumVal& ev : t->decl->values) names.push_back(ev.name);
            const std::string hint = suggest(name, names);
            return fail(hint.empty() ? std::format("'{}' is not a value of {}", name, t->decl->name)
                                     : std::format("'{}' is not a value of {}; did you mean '{}'?", name, t->decl->name, hint));
        }
        case TypeKind::Flags: {
            u64 bits = 0;
            auto addName = [&](const LiteralAst& item) -> bool {
                if (item.kind == LiteralAst::Kind::Int) {
                    u64 n = 0;
                    if (!parseUnsigned(item.text, n)) return false;
                    bits |= n;
                    return true;
                }
                if (item.kind != LiteralAst::Kind::Ident) return false;
                const std::string name = splitDots(item.text).back();
                for (const EnumVal& ev : t->decl->values) {
                    if (ev.name == name) {
                        bits |= static_cast<u64>(ev.value);
                        return true;
                    }
                }
                return false;
            };
            if (lit.kind == LiteralAst::Kind::List) {
                for (const LiteralAst& item : lit.items) {
                    if (!addName(item)) {
                        if (item.kind == LiteralAst::Kind::Ident)
                            return fail(std::format("'{}' is not a flag of {}", item.text, t->decl->name));
                        return fail(std::format("expected flag names of {}", t->decl->name));
                    }
                }
            } else if (!addName(lit)) {
                return fail(std::format("expected a flag name of {} or a list of them", t->decl->name));
            }
            v.kind = Value::Kind::Flags;
            v.u = bits;
            // Same layout as refl::detail::writeEnumJson: single-bit names in declaration order, then leftovers.
            v.json = "[";
            u64 remaining = bits;
            bool first = true;
            for (const EnumVal& ev : t->decl->values) {
                const u64 bit = static_cast<u64>(ev.value);
                if (bit != 0 && (bit & (bit - 1)) == 0 && (remaining & bit) != 0) {
                    v.json += (first ? "" : ",") + jsonQuote(ev.name);
                    remaining &= ~bit;
                    first = false;
                }
            }
            if (remaining != 0) v.json += (first ? "" : ",") + std::to_string(remaining);
            v.json += "]";
            return v;
        }
        case TypeKind::Optional:
            if (lit.kind == LiteralAst::Kind::Null) return std::nullopt; // same as the implicit default
            return fail("optional fields default to none; only 'null' is accepted");
        case TypeKind::List:
        case TypeKind::KeyedList:
        case TypeKind::Set:
        case TypeKind::Map:
            if (lit.kind == LiteralAst::Kind::List && lit.items.empty()) return std::nullopt;
            return fail("container defaults must be empty ([])");
        case TypeKind::Array: {
            const Type* el = t->element;
            const bool scalar = el->kind == TypeKind::Prim && (isIntegerPrim(el->prim) || isFloatPrim(el->prim) || el->prim == Prim::Bool);
            if (!scalar) return fail("array defaults are only supported for numeric and bool elements");
            if (lit.kind != LiteralAst::Kind::List || lit.items.size() > t->arraySize)
                return fail(std::format("expected a list of at most {} values", t->arraySize));
            v.kind = Value::Kind::List;
            v.json = "[";
            for (usize i = 0; i < t->arraySize; ++i) {
                Value item;
                if (i < lit.items.size()) {
                    auto r = resolveValue(lit.items[i], el, what);
                    if (!r) return std::nullopt;
                    item = std::move(*r);
                } else if (el->prim == Prim::Bool) {
                    item.kind = Value::Kind::Bool;
                    item.json = "false";
                } else if (isFloatPrim(el->prim)) {
                    item.kind = Value::Kind::Float;
                    item.json = "0";
                } else {
                    item.kind = isSignedPrim(el->prim) ? Value::Kind::Int : Value::Kind::UInt;
                    item.json = "0";
                }
                v.json += (i ? "," : "") + item.json;
                v.items.push_back(std::move(item));
            }
            v.json += "]";
            return v;
        }
        default: return fail(std::format("default values are not supported for {}", t->signature));
        }
    }

    // --- recursion -----------------------------------------------------------------------------
    /// Types embedded by value (their size depends on these types).
    void byValueDeps(const Type* t, std::vector<const Decl*>& out) {
        if (!t) return;
        switch (t->kind) {
        case TypeKind::Struct: out.push_back(t->decl); break;
        case TypeKind::Variant:
            for (const Alternative& a : t->decl->alternatives) out.push_back(a.type);
            break;
        case TypeKind::Optional:
        case TypeKind::Array: byValueDeps(t->element, out); break;
        default: break;
        }
    }

    void checkRecursion() {
        std::map<const Decl*, int> state; // 1 = visiting, 2 = done
        bool depthReported = false;
        std::function<bool(const Decl*, std::vector<const Decl*>&)> visit = [&](const Decl* d, std::vector<const Decl*>& stack) {
            int& s = state[d];
            if (s == 2) return true;
            if (s == 1) {
                std::string chain;
                bool on = false;
                for (const Decl* x : stack) {
                    if (x == d) on = true;
                    if (on) chain += x->qualifiedName + " -> ";
                }
                chain += d->qualifiedName;
                D.error(d->loc, std::format("'{}' contains itself by value ({}); use list<T>, map<K, T> or Ref<T> to break the cycle",
                                            d->qualifiedName, chain));
                return false;
            }
            if (stack.size() >= kMaxValueNesting) {
                if (!depthReported) {
                    D.error(d->loc, std::format("'{}' is nested more than {} levels deep by value; use list<T>, map<K, T> or Ref<T>",
                                                d->qualifiedName, kMaxValueNesting));
                }
                depthReported = true;
                return false;
            }
            s = 1;
            stack.push_back(d);
            std::vector<const Decl*> deps;
            for (const Field& f : d->fields) byValueDeps(f.type, deps);
            for (const Decl* dep : deps) {
                if (!visit(dep, stack)) {
                    stack.pop_back();
                    s = 2;
                    return false;
                }
            }
            stack.pop_back();
            s = 2;
            return true;
        };
        for (const Decl* d : S.decls) {
            if (!d->isStructLike()) continue;
            std::vector<const Decl*> stack;
            visit(d, stack);
        }
    }

    Schema& S;
    DiagnosticEngine& D;
    SemaOptions m_options;
    std::unordered_map<const Decl*, const DeclAst*> m_ast;
    std::unordered_map<std::string, Decl*> m_refs;
    std::unordered_set<const Decl*> m_resolvingAlias;
    std::unordered_set<const Decl*> m_brokenAlias;
    static constexpr usize kMaxAliasChain = 64;
    static constexpr usize kMaxValueNesting = 64;
};

} // namespace

bool lookupBuiltinType(std::string_view name, Prim& prim, Builtin& builtin) noexcept {
    for (const BuiltinEntry& b : kBuiltins) {
        if (b.name == name) {
            prim = b.prim;
            builtin = b.builtin;
            return true;
        }
    }
    return false;
}

bool analyze(Schema& schema, DiagnosticEngine& diags, const SemaOptions& options) {
    Sema s(schema, diags, options);
    return s.run();
}

} // namespace helios::schemac
