// Record file (.hrec) reading and writing.

#include "helios/reflect/record.h"

#include <format>

namespace helios::refl {

Result<void> readRecord(const TypeInfo& type, void* object, std::string_view text, RecordHeader& header, ReadCtx& ctx) {
    HELIOS_TRY_ASSIGN(const JsonDocument doc, JsonDocument::parse(text, "<record>"));
    const JsonValue root = doc.root();
    if (!root.isObject()) return ctx.typeError("record object", root);
    header = RecordHeader{};
    const JsonValue rid = root.get("$rid");
    if (!rid.isValid()) return ctx.error("record has no $rid (mint one once with mintRecordId() and keep it)");
    {
        ReadCtx::Scope s(ctx, "$rid");
        if (!rid.getU64(header.rid) || !isValidRecordId(header.rid)) return ctx.error("$rid must be a non-zero 63-bit integer");
    }
    auto readString = [&](std::string_view key, std::string& out) -> Result<void> {
        const JsonValue v = root.get(key);
        if (!v.isValid()) return {};
        ReadCtx::Scope s(ctx, key);
        if (!v.isString()) return ctx.typeError("string", v);
        out.assign(v.asString());
        return {};
    };
    HELIOS_TRY(readString("$name", header.name));
    HELIOS_TRY(readString("$parent", header.parent));
    HELIOS_TRY(readString("$comment", header.comment));
    return readJson(type, object, root, ctx);
}

std::string writeRecord(const TypeInfo& type, const void* object, const RecordHeader& header) {
    // Render the fields canonically, then splice the header keys in front ($-keys first).
    const std::string body = toJson(type, object, JsonStyle::Pretty);
    JsonWriter head(JsonStyle::Pretty);
    head.beginObject();
    head.key("$rid");
    head.unsignedInteger(header.rid);
    if (!header.name.empty()) {
        head.key("$name");
        head.string(header.name);
    }
    if (!header.parent.empty()) {
        head.key("$parent");
        head.string(header.parent);
    }
    if (!header.comment.empty()) {
        head.key("$comment");
        head.string(header.comment);
    }
    head.endObject();
    std::string out = head.take(); // "{\n  ...\n}\n"
    // body is "{}\n" or "{\n  fields...\n}\n": merge the two objects textually.
    if (body.size() > 3) {
        out.resize(out.size() - 3); // drop "\n}\n"
        out += ',';
        out.append(body, 1, std::string::npos); // skip '{'
    }
    return out;
}

} // namespace helios::refl
