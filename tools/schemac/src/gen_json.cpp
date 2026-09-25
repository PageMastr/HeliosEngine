// `--emit json`: machine-readable schema description for editor tools and later generators
// (Luau bindings, replication descriptors, SQL). Deterministic: declaration order, canonical JSON.

#include <format>

#include "generators.h"
#include "json_out.h"
#include "text.h"

namespace helios::schemac {

namespace {

void writeAttrs(JsonOut& o, const std::vector<Attr>& attrs) {
    o.key("attrs");
    o.beginArray();
    for (const Attr& a : attrs) {
        o.beginObject(true);
        o.key("name");
        o.str(a.name);
        o.key("args");
        o.beginArray(true);
        for (const AttrArg& arg : a.args) {
            o.beginObject(true);
            if (!arg.key.empty()) {
                o.key("key");
                o.str(arg.key);
            }
            o.key("value");
            o.str(arg.value);
            if (arg.isString) {
                o.key("string");
                o.boolean(true);
            }
            o.endObject();
        }
        o.endArray();
        o.endObject();
    }
    o.endArray();
}

void writeFields(JsonOut& o, const Decl* d) {
    o.key("fields");
    o.beginArray();
    for (const Field& f : d->fields) {
        o.beginObject();
        o.key("name");
        o.str(f.name);
        o.key("id");
        o.unum(f.id);
        o.key("type");
        o.str(f.type ? f.type->signature : "?");
        if (f.defaultValue) {
            o.key("default");
            o.raw(f.defaultValue->json);
        }
        if (!f.doc.empty()) {
            o.key("doc");
            o.str(f.doc);
        }
        const char* audience = f.serverOnly ? "server" : f.clientOnly ? "client" : f.editorOnly ? "editor" : "shared";
        o.key("audience");
        o.str(audience);
        if (f.replicated) {
            o.key("repIndex");
            o.unum(f.repIndex);
        }
        if (f.predicted) {
            o.key("predicted");
            o.boolean(true);
        }
        if (!f.keyedBy.empty() || (f.type && f.type->kind == TypeKind::KeyedList)) {
            o.key("keyed");
            o.str(f.keyedBy.empty() ? "$key" : f.keyedBy);
        }
        if (!f.was.empty()) {
            o.key("was");
            o.beginArray(true);
            for (const std::string& w : f.was) o.str(w);
            o.endArray();
        }
        writeAttrs(o, f.attrs);
        o.endObject();
    }
    o.endArray();
}

void writeCommon(JsonOut& o, const Decl* d) {
    o.key("name");
    o.str(d->qualifiedName);
    o.key("kind");
    o.str(declKindName(d->kind));
    o.key("file");
    o.str(d->file ? d->file->logicalPath : "");
    if (d->outer) {
        o.key("outer");
        o.str(d->outer->qualifiedName);
    }
    if (!d->doc.empty()) {
        o.key("doc");
        o.str(d->doc);
    }
}

} // namespace

std::string generateSchemaJson(const Schema& schema) {
    JsonOut o;
    o.beginObject();
    o.key("format");
    o.str("helios-schema/1");
    o.key("files");
    o.beginArray();
    for (const auto& f : schema.files) {
        if (!f->generate) continue;
        o.beginObject();
        o.key("path");
        o.str(f->logicalPath);
        o.key("package");
        o.str(f->ast.package);
        o.key("imports");
        o.beginArray(true);
        for (const SourceFile* imp : f->imports) o.str(imp->logicalPath);
        o.endArray();
        o.endObject();
    }
    o.endArray();

    o.key("types");
    o.beginArray();
    for (const Decl* d : schema.decls) {
        if (!d->emitted || !d->isLockable()) continue;
        o.beginObject();
        writeCommon(o, d);
        o.key("id");
        o.unum(d->typeId);
        o.key("layoutHash");
        o.str(std::format("{:016x}", d->layoutHash));
        if (d->version != 0) {
            o.key("version");
            o.unum(d->version);
        }
        if (d->kind == DeclKind::Enum || d->kind == DeclKind::Flags) {
            o.key("underlying");
            o.str(primName(d->underlying));
            o.key("values");
            o.beginArray();
            for (const EnumVal& v : d->values) {
                o.beginObject(true);
                o.key("name");
                o.str(v.name);
                o.key("value");
                o.num(v.value);
                if (!v.doc.empty()) {
                    o.key("doc");
                    o.str(v.doc);
                }
                o.endObject();
            }
            o.endArray();
        } else if (d->kind == DeclKind::Variant) {
            o.key("alternatives");
            o.beginArray();
            for (const Alternative& a : d->alternatives) {
                o.beginObject(true);
                o.key("name");
                o.str(a.name);
                o.key("id");
                o.unum(a.id);
                o.key("type");
                o.str(a.type->qualifiedName);
                o.endObject();
            }
            o.endArray();
        } else {
            if (d->kind == DeclKind::Component) {
                o.key("replicate");
                o.str(repAudienceName(d->replicate));
                if (!d->lod.empty()) {
                    o.key("lod");
                    o.str(d->lod);
                }
                if (d->serverPart) {
                    o.key("serverPart");
                    o.str(d->serverPart->qualifiedName);
                }
                if (d->clientPart) {
                    o.key("clientPart");
                    o.str(d->clientPart->qualifiedName);
                }
            }
            if (d->kind == DeclKind::Rpc) {
                o.key("rpc");
                o.str(d->rpcName.empty() ? d->name : d->rpcName);
                if (d->service) {
                    o.key("service");
                    o.str(d->service->qualifiedName);
                }
                if (!d->direction.empty()) {
                    o.key("direction");
                    o.str(d->direction);
                }
                if (d->result) {
                    o.key("result");
                    o.str(d->result->signature);
                }
            }
            writeFields(o, d);
        }
        writeAttrs(o, d->attrs);
        o.endObject();
    }
    o.endArray();

    o.key("services");
    o.beginArray();
    for (const Decl* d : schema.decls) {
        if (!d->emitted || d->kind != DeclKind::Service) continue;
        o.beginObject();
        writeCommon(o, d);
        o.key("methods");
        o.beginArray();
        for (const Decl* m : d->methods) {
            o.beginObject(true);
            o.key("name");
            o.str(m->rpcName);
            o.key("request");
            o.str(m->qualifiedName);
            o.key("result");
            o.str(m->result ? m->result->signature : "");
            o.endObject();
        }
        o.endArray();
        writeAttrs(o, d->attrs);
        o.endObject();
    }
    o.endArray();

    o.key("constants");
    o.beginArray();
    for (const Decl* d : schema.decls) {
        if (!d->emitted || d->kind != DeclKind::Const || !d->constType) continue;
        o.beginObject(true);
        o.key("name");
        o.str(d->qualifiedName);
        o.key("type");
        o.str(d->constType->signature);
        o.key("value");
        o.raw(d->constValue.json.empty() ? "null" : d->constValue.json);
        o.endObject();
    }
    o.endArray();

    o.key("aliases");
    o.beginArray();
    for (const Decl* d : schema.decls) {
        if (!d->emitted || d->kind != DeclKind::Alias || !d->aliasTarget) continue;
        o.beginObject(true);
        o.key("name");
        o.str(d->qualifiedName);
        o.key("type");
        o.str(d->aliasTarget->signature);
        o.endObject();
    }
    o.endArray();

    o.key("formulas");
    o.beginArray();
    for (const Decl* d : schema.decls) {
        if (!d->emitted || d->kind != DeclKind::Formula) continue;
        o.beginObject();
        writeCommon(o, d);
        o.key("params");
        o.beginArray(true);
        for (const std::string& p : d->formulaParams) o.str(p);
        o.endArray();
        o.key("body");
        o.str(d->formulaBody);
        o.endObject();
    }
    o.endArray();

    // Script-callable C++ functions (02 §3.1, §7.4): signatures and fuel charges for the Luau
    // glue generator and luau-lsp (`--emit luau` is a later work package).
    o.key("scriptlibs");
    o.beginArray();
    for (const Decl* d : schema.decls) {
        if (!d->emitted || d->kind != DeclKind::ScriptLib) continue;
        o.beginObject();
        writeCommon(o, d);
        o.key("fns");
        o.beginArray();
        for (const Decl* fn : d->methods) {
            o.beginObject();
            o.key("name");
            o.str(fn->name);
            if (!fn->doc.empty()) {
                o.key("doc");
                o.str(fn->doc);
            }
            o.key("params");
            o.beginArray();
            for (const Field& p : fn->fields) {
                o.beginObject(true);
                o.key("name");
                o.str(p.name);
                o.key("type");
                o.str(p.type ? p.type->signature : "?");
                writeAttrs(o, p.attrs);
                o.endObject();
            }
            o.endArray();
            o.key("result");
            o.str(fn->result ? fn->result->signature : "");
            o.key("cost");
            o.beginObject(true);
            o.key("base");
            o.unum(fn->cost.base);
            o.key("each");
            o.unum(fn->cost.each);
            o.key("of");
            o.str(fn->cost.of);
            o.endObject();
            o.key("pure");
            o.boolean(fn->cost.pure);
            o.key("realms");
            o.beginArray(true);
            for (const std::string& r : fn->cost.realms) o.str(r);
            o.endArray();
            writeAttrs(o, fn->attrs);
            o.endObject();
        }
        o.endArray();
        writeAttrs(o, d->attrs);
        o.endObject();
    }
    o.endArray();
    o.endObject();
    return o.take();
}

} // namespace helios::schemac
