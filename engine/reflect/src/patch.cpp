// Structural diff and patch application.

#include "helios/reflect/patch.h"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>

#include "helios/reflect/serialize.h"
#include "path_internal.h"

namespace helios::refl {

namespace {

void* mut(const void* p) noexcept { return const_cast<void*>(p); }

struct KV {
    const void* key;
    const void* value;
};
std::vector<KV> collectEntries(const TypeInfo& t, const void* container) {
    std::vector<KV> out;
    t.ops->forEach(container, &out, [](void* user, const void* k, const void* v) {
        static_cast<std::vector<KV>*>(user)->push_back({k, v});
    });
    return out;
}

std::string_view keyedFieldOf(const FieldInfo* field) noexcept {
    if (!field) return {};
    const auto* k = field->attr<attrs::Keyed>();
    return k ? k->field : std::string_view();
}

struct KeyedElem {
    std::string key;
    const void* ptr;
};

/// Element keys of a keyed list (GUID keys or @keyed(field) values). False if some element has
/// no usable key or keys repeat — the caller then falls back to replacing the whole list.
bool keyedElements(const TypeInfo& t, const void* list, std::string_view byField, std::vector<KeyedElem>& out) {
    const usize n = t.ops->size(list);
    out.clear();
    out.reserve(n);
    std::unordered_set<std::string> seen;
    for (usize i = 0; i < n; ++i) {
        const void* e = t.ops->element(mut(list), i);
        std::string key;
        if (t.kind == Kind::KeyedList) {
            key = keyedKeyText(t.ops->keyAt(list, i));
        } else {
            const FieldInfo* kf = t.element().field(byField);
            if (!kf || !kf->type().ops || !kf->type().ops->keyToText) return false;
            key = kf->type().ops->keyToText(kf->ptr(e));
        }
        if (!seen.insert(key).second) return false;
        out.push_back({std::move(key), e});
    }
    return true;
}

void diffImpl(const TypeInfo& t, const void* a, const void* b, const PropertyPath& path, const FieldInfo* field,
              PatchWriter& out);

void diffKeyed(const TypeInfo& t, const void* a, const void* b, const PropertyPath& path, std::string_view byField,
               PatchWriter& out) {
    std::vector<KeyedElem> ea;
    std::vector<KeyedElem> eb;
    if (!keyedElements(t, a, byField, ea) || !keyedElements(t, b, byField, eb)) {
        out.set(path, t, b);
        return;
    }
    std::unordered_map<std::string, const void*> inA;
    for (const KeyedElem& e : ea) inA.emplace(e.key, e.ptr);
    std::unordered_set<std::string> inB;
    for (const KeyedElem& e : eb) inB.insert(e.key);

    // Element-wise ops reproduce `b` only if b == (a's surviving elements, in a's order) + additions.
    std::vector<const KeyedElem*> commonA;
    for (const KeyedElem& e : ea) {
        if (inB.contains(e.key)) commonA.push_back(&e);
    }
    usize ci = 0;
    bool additionsStarted = false;
    for (const KeyedElem& e : eb) {
        if (inA.contains(e.key)) {
            if (additionsStarted || ci >= commonA.size() || commonA[ci]->key != e.key) {
                out.set(path, t, b); // reordered: replace the list
                return;
            }
            ++ci;
        } else {
            additionsStarted = true;
        }
    }
    for (const KeyedElem& e : ea) {
        if (!inB.contains(e.key)) out.remove(path.withKeyed(e.key));
    }
    const TypeInfo& et = t.element();
    for (const KeyedElem& e : eb) {
        const auto it = inA.find(e.key);
        if (it != inA.end()) {
            diffImpl(et, it->second, e.ptr, path.withKeyed(e.key), nullptr, out);
        } else {
            out.set(path.withKeyed(e.key), et, e.ptr);
        }
    }
}

void diffImpl(const TypeInfo& t, const void* a, const void* b, const PropertyPath& path, const FieldInfo* field,
              PatchWriter& out) {
    if (equals(t, a, b)) return;
    switch (t.kind) {
    case Kind::Struct:
        for (const FieldInfo& f : t.fields) diffImpl(f.type(), f.ptr(a), f.ptr(b), path.withField(f.name), &f, out);
        return;
    case Kind::Optional:
        if (t.ops->has(a) && t.ops->has(b)) {
            diffImpl(t.element(), t.ops->get(mut(a)), t.ops->get(mut(b)), path, field, out);
        } else {
            out.set(path, t, b);
        }
        return;
    case Kind::KeyedList: diffKeyed(t, a, b, path, {}, out); return;
    case Kind::List:
    case Kind::Array: {
        const std::string_view byField = t.kind == Kind::List ? keyedFieldOf(field) : std::string_view();
        if (!byField.empty()) {
            diffKeyed(t, a, b, path, byField, out);
            return;
        }
        const usize n = t.ops->size(a);
        if (n != t.ops->size(b)) {
            out.set(path, t, b);
            return;
        }
        for (usize i = 0; i < n; ++i)
            diffImpl(t.element(), t.ops->element(mut(a), i), t.ops->element(mut(b), i), path.withIndex(i), nullptr, out);
        return;
    }
    case Kind::Map: {
        const TypeInfo& kt = t.key();
        const auto ea = collectEntries(t, a);
        const auto eb = collectEntries(t, b);
        for (const KV& kv : ea) {
            if (!t.ops->find(mut(b), kv.key)) out.remove(path.withKey(kt.ops->keyToText(kv.key)));
        }
        for (const KV& kv : eb) {
            const void* old = t.ops->find(mut(a), kv.key);
            const PropertyPath p = path.withKey(kt.ops->keyToText(kv.key));
            if (old) {
                diffImpl(t.element(), old, kv.value, p, nullptr, out);
            } else {
                out.set(p, t.element(), kv.value);
            }
        }
        return;
    }
    case Kind::Variant: {
        const u32 ia = t.ops->index(a);
        if (ia != t.ops->index(b)) {
            out.set(path, t, b);
            return;
        }
        const VariantAlt& alt = t.alternatives[ia];
        diffImpl(alt.type(), t.ops->alt(mut(a)), t.ops->alt(mut(b)), path.withField(alt.name), nullptr, out);
        return;
    }
    default: out.set(path, t, b); return;
    }
}

} // namespace

void PatchBuilder::set(const PropertyPath& path, const TypeInfo& type, const void* value) {
    m_patch.ops.push_back(PatchOp{PatchOp::Kind::Set, path.toString(), toJson(type, value, JsonStyle::Compact)});
}

void PatchBuilder::remove(const PropertyPath& path) {
    m_patch.ops.push_back(PatchOp{PatchOp::Kind::Remove, path.toString(), {}});
}

void diff(const TypeInfo& type, const void* before, const void* after, PatchWriter& out) {
    diffImpl(type, before, after, PropertyPath(), nullptr, out);
}

Patch diff(const TypeInfo& type, const void* before, const void* after) {
    PatchBuilder builder;
    diff(type, before, after, builder);
    return builder.take();
}

Result<void> apply(const TypeInfo& type, void* object, const PatchOp& op) {
    HELIOS_TRY_ASSIGN(const PropertyPath path, PropertyPath::parse(op.path));
    if (op.kind == PatchOp::Kind::Remove) return detail::removeAtPath(type, object, path);
    return detail::setAtPath(type, object, path, op.value);
}

Result<void> apply(const TypeInfo& type, void* object, const Patch& patch) {
    for (const PatchOp& op : patch.ops) HELIOS_TRY(apply(type, object, op));
    return {};
}

std::string Patch::toJson(JsonStyle style) const {
    JsonWriter w(style);
    w.beginArray(false);
    for (const PatchOp& op : ops) {
        w.beginObject();
        w.key("op");
        w.string(op.kind == PatchOp::Kind::Set ? "set" : "remove");
        w.key("path");
        w.string(op.path);
        if (op.kind == PatchOp::Kind::Set) {
            w.key("value");
            w.raw(op.value);
        }
        w.endObject();
    }
    w.endArray();
    return w.take();
}

Result<Patch> Patch::fromJson(std::string_view text) {
    HELIOS_TRY_ASSIGN(const JsonDocument doc, JsonDocument::parse(text, "<patch>"));
    const JsonValue root = doc.root();
    if (!root.isArray()) return Error{ErrorCode::ParseError, "patch: expected an array of operations"};
    Patch patch;
    usize index = 0;
    for (JsonValue e : root.elements()) {
        const JsonValue opv = e.get("op");
        const JsonValue pathv = e.get("path");
        if (!e.isObject() || !opv.isString() || !pathv.isString())
            return makeError(ErrorCode::ParseError, "patch[{}]: expected {{\"op\", \"path\"[, \"value\"]}}", index);
        PatchOp op;
        op.path = std::string(pathv.asString());
        if (opv.asString() == "set") {
            const JsonValue value = e.get("value");
            if (!value.isValid()) return makeError(ErrorCode::ParseError, "patch[{}]: set without value", index);
            JsonWriter w(JsonStyle::Compact);
            w.copy(value);
            op.value = w.take();
        } else if (opv.asString() == "remove") {
            op.kind = PatchOp::Kind::Remove;
        } else {
            return makeError(ErrorCode::ParseError, "patch[{}]: unknown op '{}'", index, opv.asString());
        }
        patch.ops.push_back(std::move(op));
        ++index;
    }
    return patch;
}

} // namespace helios::refl
