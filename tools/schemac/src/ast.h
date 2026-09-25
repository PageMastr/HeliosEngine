#pragma once
// Syntax tree of one .hschema file, as parsed (names unresolved).

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "diagnostics.h"

namespace helios::schemac {

struct AttrArgAst {
    std::string key;   ///< "" for positional arguments.
    std::string value; ///< Decoded string contents, or the verbatim source text of the argument.
    bool isString = false;
    SourceLoc loc;
};

struct AttrAst {
    std::string name;
    std::vector<AttrArgAst> args;
    bool hasParens = false;
    SourceLoc loc;
};

struct LiteralAst {
    enum class Kind : u8 { Int, Float, String, Bool, Ident, Null, List, Unit };
    Kind kind = Kind::Int;
    std::string text; ///< Int/Float: digits (without sign); Ident: name; String: decoded; Unit: "500ms"
    bool negative = false;
    bool boolValue = false;
    std::vector<LiteralAst> items; ///< List
    SourceLoc loc;
};

struct TypeExpr;
struct MembersAst;

enum class Block : u8 { None, Client, Server, Editor };

struct FieldAst {
    std::string name;
    SourceLoc loc;
    std::unique_ptr<TypeExpr> type;
    std::optional<LiteralAst> defaultValue;
    std::vector<AttrAst> attrs;
    std::string doc;
    Block block = Block::None;
};

struct RpcAst {
    std::string name;
    SourceLoc loc;
    std::vector<FieldAst> params;
    std::unique_ptr<TypeExpr> result;
    std::vector<AttrAst> attrs;
    std::string direction; ///< "client->server", "server->client", "server->server" or "".
    SourceLoc directionLoc;
    std::string doc;
};

/// `fn name(params) -> result @attrs;` inside a scriptlib (02 §3.1).
struct FnAst {
    std::string name;
    SourceLoc loc;
    std::vector<FieldAst> params;
    std::unique_ptr<TypeExpr> result;
    std::vector<AttrAst> attrs;
    std::string doc;
};

struct MembersAst {
    std::vector<FieldAst> fields;
    std::vector<RpcAst> rpcs;
    std::vector<FnAst> fns;
};

struct EnumValueAst {
    std::string name;
    SourceLoc loc;
    std::optional<LiteralAst> value;
    std::vector<AttrAst> attrs;
    std::string doc;
};

struct AltAst {
    std::string name;
    SourceLoc loc;
    std::unique_ptr<MembersAst> members; ///< null for unit alternatives
    std::string doc;
};

struct TypeExpr {
    enum class Kind : u8 { Named, Generic, Optional, Array, InlineStruct, InlineEnum, InlineVariant };
    Kind kind = Kind::Named;
    SourceLoc loc;
    std::string name; ///< Named/Generic: dotted name as written
    std::vector<std::unique_ptr<TypeExpr>> args; ///< Generic arguments; Optional/Array: [inner]
    u64 arraySize = 0;
    std::unique_ptr<MembersAst> members;         ///< InlineStruct
    std::vector<EnumValueAst> enumValues;        ///< InlineEnum
    std::vector<AltAst> alternatives;            ///< InlineVariant
};

enum class DeclKindAst : u8 {
    Enum,
    Flags,
    Struct,
    Component,
    Relation,
    Record,
    Event,
    Rpc,
    Message,
    Service,
    ViewModel,
    Formula,
    Const,
    Alias,
    ScriptLib,
};

struct DeclAst {
    DeclKindAst kind = DeclKindAst::Struct;
    std::string name;
    SourceLoc loc;
    SourceLoc kindLoc;
    std::unique_ptr<TypeExpr> base; ///< `: Base`
    std::vector<AttrAst> attrs;     ///< header attributes (with or without '@')
    std::string doc;
    bool hasBody = false;
    MembersAst members;                 ///< struct-like bodies, services and scriptlibs
    std::vector<EnumValueAst> enumValues; ///< enum / flags
    // rpc
    std::vector<FieldAst> params;
    std::unique_ptr<TypeExpr> result;
    std::string direction;
    SourceLoc directionLoc;
    // formula
    std::vector<std::string> formulaParams;
    std::string exprText; ///< formula body (verbatim)
    SourceLoc exprLoc;
    // const / alias
    std::optional<LiteralAst> constValue;
    std::unique_ptr<TypeExpr> aliasTarget;
};

struct ImportAst {
    std::string path;
    SourceLoc loc;
};

struct FileAst {
    std::string package;
    SourceLoc packageLoc;
    std::vector<ImportAst> imports;
    std::vector<DeclAst> decls;
};

} // namespace helios::schemac
