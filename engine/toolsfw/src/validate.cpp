#include "helios/toolsfw/validate.h"

#include <cmath>
#include <format>
#include <set>

#include "helios/reflect/path.h"
#include "helios/toolsfw/framework.h"

#include "ops.h"

namespace helios::tf {

using refl::Kind;
using refl::PropertyPath;
using refl::TypeInfo;

std::string_view severityName(Issue::Severity severity) noexcept {
    return severity == Issue::Severity::Error ? "error" : "warning";
}

std::string formatIssue(const Issue& issue) {
    std::string out;
    if (!issue.file.empty()) out += issue.file + ": ";
    if (!issue.path.empty()) out += issue.path + ": ";
    out += std::format("{}: {}", severityName(issue.severity), issue.message);
    if (!issue.rule.empty()) out += " [" + issue.rule + "]";
    return out;
}

namespace {

bool numericValue(const TypeInfo& t, const void* v, f64& out) {
    switch (t.kind) {
    case Kind::I8:
    case Kind::I16:
    case Kind::I32:
    case Kind::I64: out = static_cast<f64>(refl::readIntegerBits(t, v)); return true;
    case Kind::U8:
    case Kind::U16:
    case Kind::U32:
    case Kind::U64: out = static_cast<f64>(static_cast<u64>(refl::readIntegerBits(t, v))); return true;
    case Kind::F32: out = static_cast<f64>(*static_cast<const f32*>(v)); return true;
    case Kind::F64: out = *static_cast<const f64*>(v); return true;
    default: return false;
    }
}

std::string numberText(f64 v) {
    char buf[40];
    const usize n = refl::formatJsonF64(v, buf);
    return std::string(buf, n);
}

void add(std::vector<Issue>& out, const PropertyPath& path, std::string rule, std::string message) {
    Issue i;
    i.path = path.toString();
    i.rule = std::move(rule);
    i.message = std::move(message);
    out.push_back(std::move(i));
}

void validateValue(const TypeInfo& t, const void* v, const refl::FieldInfo* field, const PropertyPath& path,
                   std::vector<Issue>& out);

void checkScalar(const TypeInfo& t, const void* v, const refl::FieldInfo* field, const PropertyPath& path,
                 std::vector<Issue>& out) {
    f64 x = 0;
    if (!numericValue(t, v, x)) return;
    if (refl::isFloatKind(t.kind) && !std::isfinite(x)) {
        add(out, path, "finite", std::format("{} is not a finite number", numberText(x)));
        return;
    }
    if (!field) return;
    if (const auto* range = field->attr<refl::attrs::Range>()) {
        if (x < range->min || x > range->max) {
            add(out, path, "range",
                std::format("{} is outside @range({}, {})", numberText(x), numberText(range->min), numberText(range->max)));
        }
    }
}

void validateSequence(const TypeInfo& t, const void* v, const refl::FieldInfo* field, const PropertyPath& path,
                      std::vector<Issue>& out) {
    const usize n = t.ops->size(v);
    if (field) {
        if (const auto* mx = field->attr<refl::attrs::Max>(); mx && n > mx->count) {
            add(out, path, "max", std::format("{} elements, @max({})", n, mx->count));
        }
    }
    std::string_view keyField;
    if (field) {
        if (const auto* k = field->attr<refl::attrs::Keyed>()) keyField = k->field;
    }
    std::set<std::string> keys;
    const TypeInfo& e = t.element();
    for (usize i = 0; i < n; ++i) {
        void* elem = t.ops->element(const_cast<void*>(v), i);
        PropertyPath child = path;
        if (t.kind == Kind::KeyedList) {
            const Guid key = t.ops->keyAt(v, i);
            if (key.isNil()) add(out, path.withIndex(i), "key", "keyed-list element with a nil key");
            const std::string text = refl::keyedKeyText(key);
            if (!keys.insert(text).second) add(out, path.withIndex(i), "key", std::format("duplicate key {}", text));
            child.keyed(text);
        } else if (!keyField.empty()) {
            const refl::FieldInfo* kf = e.field(keyField);
            std::string text;
            if (kf && kf->type().ops->keyToText) text = kf->type().ops->keyToText(kf->ptr(elem));
            if (!keys.insert(text).second) add(out, path.withIndex(i), "key", std::format("duplicate {} '{}'", keyField, text));
            child.keyed(text);
        } else {
            child.index(i);
        }
        // Range attributes of a numeric list apply to its elements.
        validateValue(e, elem, refl::isScalarKind(e.kind) ? field : nullptr, child, out);
    }
}

struct MapVisit {
    const TypeInfo* map;
    const PropertyPath* path;
    std::vector<Issue>* out;
};

void validateValue(const TypeInfo& t, const void* v, const refl::FieldInfo* field, const PropertyPath& path,
                   std::vector<Issue>& out) {
    if (refl::isScalarKind(t.kind)) {
        checkScalar(t, v, field, path, out);
        return;
    }
    switch (t.kind) {
    case Kind::Struct:
    case Kind::Builtin:
        for (const refl::FieldInfo& f : t.fields) validateValue(f.type(), f.ptr(v), &f, path.withField(f.name), out);
        break;
    case Kind::List:
    case Kind::KeyedList:
    case Kind::Array: validateSequence(t, v, field, path, out); break;
    case Kind::Optional:
        if (t.ops->has(v)) validateValue(t.element(), t.ops->get(const_cast<void*>(v)), field, path, out);
        break;
    case Kind::Variant: {
        const u32 index = t.ops->index(v);
        if (index < t.alternatives.size()) {
            const refl::VariantAlt& alt = t.alternatives[index];
            validateValue(alt.type(), t.ops->alt(const_cast<void*>(v)), nullptr, path.withField(alt.name), out);
        }
        break;
    }
    case Kind::Map: {
        MapVisit visit{&t, &path, &out};
        t.ops->forEach(v, &visit, [](void* user, const void* key, const void* value) {
            auto* mv = static_cast<MapVisit*>(user);
            const TypeInfo& kt = mv->map->key();
            const std::string text = kt.ops->keyToText ? kt.ops->keyToText(key) : std::string();
            validateValue(mv->map->element(), value, nullptr, mv->path->withKey(text), *mv->out);
        });
        break;
    }
    default: break;
    }
}

} // namespace

void validateObject(const TypeInfo& type, const void* object, std::vector<Issue>& out) {
    validateValue(type, object, nullptr, PropertyPath(), out);
}

void validateAt(const TypeInfo& type, const void* object, std::string_view path, std::vector<Issue>& out) {
    auto p = PropertyPath::parse(path);
    if (!p) {
        Issue i;
        i.path = std::string(path);
        i.rule = "path";
        i.message = p.error().message;
        out.push_back(std::move(i));
        return;
    }
    auto r = refl::resolve(type, object, *p);
    if (!r) return;  // removed values have nothing to validate
    validateValue(*r->type, r->ptr, r->field, *p, out);
}

void validateHeader(const refl::RecordHeader& header, std::vector<Issue>& out) {
    if (!refl::isValidRecordId(header.rid)) {
        Issue i;
        i.path = "$rid";
        i.rule = "header";
        i.message = std::format("invalid $rid {}", header.rid);
        out.push_back(std::move(i));
    }
    if (header.name.empty()) {
        Issue i;
        i.path = "$name";
        i.rule = "header";
        i.message = "empty $name";
        out.push_back(std::move(i));
    }
}

Result<void> validateTransaction(const Framework& framework, const Transaction& tx) {
    std::vector<Issue> issues;
    for (const Op& op : tx.ops) {
        const Document* d = framework.documents().find(op.doc);
        if (!d || d->destroyed()) continue;
        switch (op.kind) {
        case OpKind::Set:
            if (!op.after) break;
            if (!op.path.empty() && op.path.front() == '$') {
                validateHeader(d->header(), issues);
            } else if (!op.path.empty()) {
                // A whole-object set ("", the exact-diff fallback) is not range-checked: it would
                // also flag untouched fields that already violate the schema.
                validateAt(d->type(), d->object(), op.path, issues);
            }
            break;
        case OpKind::Insert:
        case OpKind::Move: {
            // The list itself (@max, key uniqueness) and the inserted element.
            validateAt(d->type(), d->object(), op.path, issues);
            break;
        }
        case OpKind::Create:
            // New records start from schema defaults, which need not satisfy @range yet (a new hull
            // has mass 0); `helios-tool validate` reports those. The header must be valid.
            validateHeader(d->header(), issues);
            break;
        default: break;
        }
    }
    std::string message;
    for (Issue& i : issues) {
        if (i.severity != Issue::Severity::Error) continue;
        if (const Document* d = framework.documents().find(tx.ops.front().doc)) i.file = d->relativePath();
        if (!message.empty()) message += "; ";
        message += formatIssue(i);
    }
    if (!message.empty()) return Error{ErrorCode::InvalidArgument, message};
    return {};
}

} // namespace helios::tf
