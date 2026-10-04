#include "helios/toolsfw/json_util.h"

namespace helios::tf::json {

std::string quote(std::string_view text) {
    refl::JsonWriter w(refl::JsonStyle::Compact);
    w.string(text);
    return w.take();
}

std::string compact(refl::JsonValue value) {
    refl::JsonWriter w(refl::JsonStyle::Compact);
    w.copy(value);
    return w.take();
}

Result<std::string> normalize(std::string_view text) {
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(text));
    return compact(doc.root());
}

std::optional<std::string_view> getString(refl::JsonValue object, std::string_view key) {
    if (!object.isObject()) return std::nullopt;
    const refl::JsonValue v = object.get(key);
    if (!v.isString()) return std::nullopt;
    return v.asString();
}

std::optional<f64> getNumber(refl::JsonValue object, std::string_view key) {
    if (!object.isObject()) return std::nullopt;
    f64 out = 0;
    const refl::JsonValue v = object.get(key);
    if (!v.isNumber() || !v.getF64(out)) return std::nullopt;
    return out;
}

std::optional<i64> getInteger(refl::JsonValue object, std::string_view key) {
    if (!object.isObject()) return std::nullopt;
    i64 out = 0;
    const refl::JsonValue v = object.get(key);
    if (!v.isNumber() || !v.getI64(out)) return std::nullopt;
    return out;
}

std::optional<bool> getBool(refl::JsonValue object, std::string_view key) {
    if (!object.isObject()) return std::nullopt;
    const refl::JsonValue v = object.get(key);
    if (!v.isBool()) return std::nullopt;
    return v.asBool();
}

Result<refl::JsonDocument> parseObject(std::string_view text, std::string_view sourceName) {
    HELIOS_TRY_ASSIGN(refl::JsonDocument doc, refl::JsonDocument::parse(text.empty() ? std::string_view("{}") : text,
                                                                     sourceName));
    if (!doc.root().isObject()) {
        return Error{ErrorCode::InvalidArgument, std::string(sourceName) + ": expected a JSON object"};
    }
    return doc;
}

} // namespace helios::tf::json
