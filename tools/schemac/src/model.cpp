#include "model.h"

namespace helios::schemac {

bool isIntegerPrim(Prim p) noexcept { return p >= Prim::I8 && p <= Prim::U64; }
bool isSignedPrim(Prim p) noexcept { return p >= Prim::I8 && p <= Prim::I64; }
bool isFloatPrim(Prim p) noexcept { return p == Prim::F32 || p == Prim::F64; }

std::string_view primName(Prim p) noexcept {
    switch (p) {
    case Prim::None: return "?";
    case Prim::Bool: return "bool";
    case Prim::I8: return "i8";
    case Prim::I16: return "i16";
    case Prim::I32: return "i32";
    case Prim::I64: return "i64";
    case Prim::U8: return "u8";
    case Prim::U16: return "u16";
    case Prim::U32: return "u32";
    case Prim::U64: return "u64";
    case Prim::F32: return "f32";
    case Prim::F64: return "f64";
    case Prim::String: return "string";
    case Prim::Name: return "Name";
    }
    return "?";
}

std::string_view builtinName(Builtin b) noexcept {
    switch (b) {
    case Builtin::None: return "?";
    case Builtin::Guid: return "Guid";
    case Builtin::Vec2f: return "vec2f";
    case Builtin::Vec3f: return "vec3f";
    case Builtin::Vec4f: return "vec4f";
    case Builtin::Vec3d: return "vec3d";
    case Builtin::Quatf: return "quatf";
    case Builtin::Quatd: return "quatd";
    case Builtin::Color: return "color";
    case Builtin::WorldPos: return "WorldPos";
    case Builtin::EntityId: return "EntityId";
    case Builtin::NetHandle: return "NetHandle";
    case Builtin::Duration: return "Duration";
    case Builtin::Tick: return "Tick";
    case Builtin::LocString: return "LocString";
    case Builtin::TagSet: return "TagSet";
    case Builtin::TagQuery: return "TagQuery";
    case Builtin::HxlExpr: return "HxlExpr";
    }
    return "?";
}

u32 tupleSize(Builtin b) noexcept {
    switch (b) {
    case Builtin::Vec2f: return 2;
    case Builtin::Vec3f:
    case Builtin::Vec3d:
    case Builtin::WorldPos: return 3;
    case Builtin::Vec4f:
    case Builtin::Quatf:
    case Builtin::Quatd:
    case Builtin::Color: return 4;
    default: return 0;
    }
}

bool tupleIsF64(Builtin b) noexcept { return b == Builtin::Vec3d || b == Builtin::Quatd || b == Builtin::WorldPos; }

std::string_view repAudienceName(RepAudience a) noexcept {
    switch (a) {
    case RepAudience::None: return "none";
    case RepAudience::All: return "all";
    case RepAudience::Owner: return "owner";
    case RepAudience::Server: return "server";
    }
    return "none";
}

const Attr* findAttr(const std::vector<Attr>& attrs, std::string_view name) noexcept {
    for (const Attr& a : attrs) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

std::string_view declKindName(DeclKind k) noexcept {
    switch (k) {
    case DeclKind::Enum: return "enum";
    case DeclKind::Flags: return "flags";
    case DeclKind::Struct: return "struct";
    case DeclKind::Component: return "component";
    case DeclKind::ComponentPart: return "component part";
    case DeclKind::Record: return "record";
    case DeclKind::Event: return "event";
    case DeclKind::Message: return "message";
    case DeclKind::ViewModel: return "viewmodel";
    case DeclKind::Relation: return "relation";
    case DeclKind::Rpc: return "rpc";
    case DeclKind::Service: return "service";
    case DeclKind::Formula: return "formula";
    case DeclKind::Const: return "const";
    case DeclKind::Alias: return "alias";
    case DeclKind::Variant: return "variant";
    case DeclKind::Alternative: return "alternative";
    case DeclKind::ScriptLib: return "scriptlib";
    case DeclKind::ScriptFn: return "fn";
    }
    return "?";
}

bool Decl::isStructLike() const noexcept {
    switch (kind) {
    case DeclKind::Struct:
    case DeclKind::Component:
    case DeclKind::ComponentPart:
    case DeclKind::Record:
    case DeclKind::Event:
    case DeclKind::Message:
    case DeclKind::ViewModel:
    case DeclKind::Relation:
    case DeclKind::Rpc:
    case DeclKind::Alternative: return true;
    default: return false;
    }
}

const Type* Schema::intern(Type t) {
    auto it = typesBySignature.find(t.signature);
    if (it != typesBySignature.end()) return it->second;
    const Type* p = &typeStorage.emplace_back(std::move(t));
    typesBySignature.emplace(p->signature, p);
    return p;
}

std::vector<Decl*> Schema::declsOfFile(const SourceFile* f) const {
    std::vector<Decl*> out;
    for (Decl* d : decls) {
        if (d->file == f) out.push_back(d);
    }
    return out;
}

} // namespace helios::schemac
