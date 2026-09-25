#pragma once
// Resolved schema model produced by semantic analysis and consumed by the lock pass and the
// generators. All names are resolved, inline types are materialized as nested declarations,
// component client/server blocks are split into parts, and (after the lock pass) every type,
// field, enum value and variant alternative carries its stable id.

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast.h"
#include "diagnostics.h"

namespace helios::schemac {

enum class Prim : u8 { None, Bool, I8, I16, I32, I64, U8, U16, U32, U64, F32, F64, String, Name };

enum class Builtin : u8 {
    None,
    Guid,
    Vec2f,
    Vec3f,
    Vec4f,
    Vec3d,
    Quatf,
    Quatd,
    Color,
    WorldPos,
    EntityId,
    NetHandle,
    Duration,
    Tick,
    LocString,
    TagSet,
    TagQuery,
    HxlExpr,
};

enum class TypeKind : u8 { Prim, Builtin, Enum, Flags, Struct, Variant, List, KeyedList, Set, Map, Optional, Array, RecordRef, AssetRef };

struct Decl;

struct Type {
    TypeKind kind = TypeKind::Prim;
    Prim prim = Prim::None;
    Builtin builtin = Builtin::None;
    Decl* decl = nullptr;           ///< Enum/Flags/Struct/Variant, RecordRef target
    const Type* element = nullptr;  ///< containers; Map value
    const Type* key = nullptr;      ///< Map key
    u32 arraySize = 0;
    std::string assetKind;          ///< AssetRef<Kind>
    std::string signature;          ///< canonical text ("list<pkg.T>", "u8?", "keyed<pkg.T>")

    bool isContainer() const noexcept {
        return kind == TypeKind::List || kind == TypeKind::KeyedList || kind == TypeKind::Set || kind == TypeKind::Map ||
               kind == TypeKind::Array;
    }
};

bool isIntegerPrim(Prim p) noexcept;
bool isSignedPrim(Prim p) noexcept;
bool isFloatPrim(Prim p) noexcept;
std::string_view primName(Prim p) noexcept;
std::string_view builtinName(Builtin b) noexcept;
/// Float tuple builtins: component count and whether they are f64.
u32 tupleSize(Builtin b) noexcept;
bool tupleIsF64(Builtin b) noexcept;

struct AttrArg {
    std::string key;
    std::string value;
    bool isString = false;
    SourceLoc loc;
};

struct Attr {
    std::string name;
    std::vector<AttrArg> args;
    bool hasParens = false;
    SourceLoc loc;

    const AttrArg* arg(usize i) const noexcept { return i < args.size() ? &args[i] : nullptr; }
    const AttrArg* named(std::string_view key) const noexcept {
        for (const AttrArg& a : args) {
            if (a.key == key) return &a;
        }
        return nullptr;
    }
};

const Attr* findAttr(const std::vector<Attr>& attrs, std::string_view name) noexcept;

/// A resolved literal (defaults and constants).
struct Value {
    enum class Kind : u8 { None, Bool, Int, UInt, Float, String, Enum, Flags, List, Null, Duration, Guid };
    Kind kind = Kind::None;
    bool b = false;
    i64 i = 0;       ///< Int, Duration (nanoseconds)
    u64 u = 0;       ///< UInt, Flags bits, Guid high
    u64 u2 = 0;      ///< Guid low
    f64 f = 0;       ///< Float (already rounded to f32 for f32 targets)
    std::string s;   ///< String, Enum value name
    std::vector<Value> items;
    std::string json; ///< Canonical compact JSON text (matches helios::refl::JsonWriter)
};

enum class RepAudience : u8 { None, All, Owner, Server };

/// Fuel charge of a scriptlib fn (02 §7.4): `@script(cost=n[, each=m, of=result|<param>])`.
struct ScriptCost {
    u64 base = 0;
    u64 each = 0;
    std::string of; ///< "" (no per-element charge), "result" or a parameter name
    bool pure = false;
    std::vector<std::string> realms; ///< effective @realm (fn's own, else the scriptlib's)
};
std::string_view repAudienceName(RepAudience a) noexcept;

struct Field {
    std::string name;
    SourceLoc loc;
    const Type* type = nullptr;
    std::string doc;
    std::vector<Attr> attrs;
    Block block{};
    std::optional<Value> defaultValue;
    std::vector<std::string> was;
    std::string keyedBy; ///< @keyed(field)
    u32 id = 0;          ///< stable lock id
    bool replicated = false;
    u8 repIndex = 0xFF;
    bool predicted = false;
    bool serverOnly = false; ///< server {} block or @server_only
    bool clientOnly = false;
    bool editorOnly = false;
    const LiteralAst* defaultLiteral = nullptr; ///< sema-internal: unresolved default
    const Attr* attr(std::string_view n) const noexcept { return findAttr(attrs, n); }
};

struct EnumVal {
    std::string name;
    i64 value = 0;
    std::string doc;
    SourceLoc loc;
    std::vector<Attr> attrs;
};

struct Alternative {
    std::string name;
    u32 id = 0;
    Decl* type = nullptr;
    SourceLoc loc;
};

enum class DeclKind : u8 {
    Enum,
    Flags,
    Struct,
    Component,
    ComponentPart, ///< X.Server / X.Client of a component
    Record,
    Event,
    Message,
    ViewModel,
    Relation,
    Rpc,         ///< argument struct of an rpc
    Service,
    Formula,
    Const,
    Alias,
    Variant,
    Alternative, ///< struct of one variant alternative
    ScriptLib,   ///< `scriptlib X { fn ... }`: signatures of hand-written C++ functions Luau may call
    ScriptFn,    ///< one `fn` of a scriptlib (params in `fields`, return type in `result`)
};
std::string_view declKindName(DeclKind k) noexcept;

struct SourceFile;

struct Decl {
    DeclKind kind = DeclKind::Struct;
    std::string name;          ///< short name ("Handling")
    std::string qualifiedName; ///< "sample.ship.ShipHullDef.Handling"
    std::string package;       ///< "sample.ship"
    SourceFile* file = nullptr;
    SourceLoc loc;
    std::string doc;
    std::vector<Attr> attrs;
    std::vector<std::string> was; ///< type-level @was (qualified or short names)

    Decl* outer = nullptr;              ///< enclosing declaration for nested types
    std::vector<Decl*> nested;          ///< nested types in declaration order
    std::vector<std::string> cppPath;   ///< C++ name relative to the package namespace
    std::string goName;                 ///< flattened Go name

    // Struct-like (struct, component, parts, record, event, message, viewmodel, relation, rpc, alternative)
    std::vector<Field> fields;
    RepAudience replicate = RepAudience::None; ///< components
    std::string lod;
    Decl* serverPart = nullptr;
    Decl* clientPart = nullptr;
    u32 version = 0;

    // Enum / flags
    Prim underlying = Prim::U32;
    std::vector<EnumVal> values;

    // Variant
    std::vector<Alternative> alternatives;

    // Rpc (argument struct) / service / scriptlib fn
    std::string direction;
    const Type* result = nullptr;
    Decl* service = nullptr;
    std::string rpcName;
    std::vector<Decl*> methods; ///< service: request structs; scriptlib: its fns
    ScriptCost cost;            ///< ScriptFn: @script(cost=, each=, of=)

    // Const / alias / formula
    const Type* constType = nullptr;
    Value constValue;
    const Type* aliasTarget = nullptr;
    std::vector<std::string> formulaParams;
    std::string formulaBody;

    // Identity
    u32 typeId = 0;
    u64 layoutHash = 0;
    bool emitted = true; ///< declared in a file being generated (not only imported)

    const Attr* attr(std::string_view n) const noexcept { return findAttr(attrs, n); }
    bool isStructLike() const noexcept;
    /// Types with a lock entry and a TypeInfo (structs, enums, variants).
    bool isLockable() const noexcept { return isStructLike() || kind == DeclKind::Enum || kind == DeclKind::Flags || kind == DeclKind::Variant; }
    bool isReplicatedComponent() const noexcept {
        return kind == DeclKind::Component && replicate != RepAudience::None;
    }
};

struct SourceFile {
    std::string path;        ///< as given / resolved (for messages)
    std::string logicalPath; ///< include-root relative ("sample/ship.hschema")
    std::string stem;        ///< "ship"
    u32 diagIndex = 0;
    bool generate = false;   ///< listed on the command line (not only imported)
    FileAst ast;
    std::vector<SourceFile*> imports;
    std::vector<Decl*> decls; ///< top-level declarations in order
};

struct Schema {
    std::vector<std::unique_ptr<SourceFile>> files;
    std::deque<Decl> declStorage;
    std::deque<Type> typeStorage;
    std::unordered_map<std::string, const Type*> typesBySignature;
    std::unordered_map<std::string, Decl*> declsByName; ///< qualified name -> decl (types, consts, services...)
    std::vector<Decl*> decls;                           ///< every declaration incl. nested, in order

    Decl* newDecl() { return &declStorage.emplace_back(); }
    /// Interns a type by signature (identical types share one object).
    const Type* intern(Type t);
    std::vector<Decl*> declsOfFile(const SourceFile* f) const;
};

} // namespace helios::schemac
