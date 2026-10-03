#include "ops.h"

#include <format>

#include "helios/reflect/patch.h"

namespace helios::tf::detail {

using refl::Kind;
using refl::PathSegment;
using refl::PropertyPath;
using refl::TypeInfo;

namespace {

Error conflict(std::string_view path, std::string_view what) {
    return Error{ErrorCode::InvalidState, std::format("conflict at '{}': {}", path, what)};
}

std::string describe(const std::optional<std::string>& v) {
    if (!v) return "<absent>";
    if (v->size() > 80) return v->substr(0, 77) + "...";
    return *v;
}

} // namespace

bool ListRef::keyed() const noexcept {
    return type->kind == Kind::KeyedList || !keyField.empty();
}

Result<ListRef> resolveList(const TypeInfo& root, void* object, std::string_view listPath) {
    HELIOS_TRY_ASSIGN(const PropertyPath p, PropertyPath::parse(listPath));
    HELIOS_TRY_ASSIGN(refl::Ref r, refl::resolve(root, object, p));
    // Optionals are transparent for lists too.
    while (r.type->kind == Kind::Optional) {
        if (!r.type->ops->has(r.ptr)) return Error{ErrorCode::NotFound, std::format("'{}': optional value is empty", listPath)};
        r.ptr = r.type->ops->get(r.ptr);
        r.type = &r.type->element();
    }
    if (r.type->kind != Kind::List && r.type->kind != Kind::KeyedList) {
        return Error{ErrorCode::InvalidArgument, std::format("'{}' is a {}, not a list", listPath, refl::kindName(r.type->kind))};
    }
    ListRef list;
    list.type = r.type;
    list.ptr = r.ptr;
    if (r.type->kind == Kind::List && r.field) {
        if (const auto* k = r.field->attr<refl::attrs::Keyed>()) list.keyField = k->field;
    }
    return list;
}

std::string elementKey(const ListRef& list, usize i) {
    if (list.type->kind == Kind::KeyedList) return refl::keyedKeyText(list.type->ops->keyAt(list.ptr, i));
    if (list.keyField.empty()) return {};
    const TypeInfo& e = list.type->element();
    const refl::FieldInfo* kf = e.field(list.keyField);
    if (!kf || !kf->type().ops->keyToText) return {};
    return kf->type().ops->keyToText(kf->ptr(list.type->ops->element(list.ptr, i)));
}

std::optional<usize> findElement(const ListRef& list, std::string_view key) {
    const usize n = list.type->ops->size(list.ptr);
    for (usize i = 0; i < n; ++i) {
        if (elementKey(list, i) == key) return i;
    }
    return std::nullopt;
}

std::string elementPath(std::string_view listPath, const ListRef& list, usize i) {
    PropertyPath p = PropertyPath::parse(listPath).valueOr(PropertyPath());
    if (list.keyed()) {
        p.keyed(elementKey(list, i));
    } else {
        p.index(i);
    }
    return p.toString();
}

std::string elementJson(const ListRef& list, usize i) {
    return refl::toJson(list.type->element(), list.type->ops->element(list.ptr, i), refl::JsonStyle::Compact);
}

Result<std::optional<std::string>> valueAt(const TypeInfo& root, const void* object, std::string_view path) {
    HELIOS_TRY_ASSIGN(const PropertyPath p, PropertyPath::parse(path));
    auto r = refl::resolve(root, object, p);
    if (!r) {
        if (r.errorCode() == ErrorCode::NotFound || r.errorCode() == ErrorCode::OutOfRange) {
            return std::optional<std::string>();
        }
        return std::move(r).error();
    }
    return std::optional<std::string>(refl::toJson(*r->type, r->ptr, refl::JsonStyle::Compact));
}

namespace {

Result<void> applySet(const TypeInfo& root, void* object, const Op& op) {
    HELIOS_TRY_ASSIGN(const std::optional<std::string> current, valueAt(root, object, op.path));
    if (current != op.before) {
        return conflict(op.path, std::format("expected {}, found {}", describe(op.before), describe(current)));
    }
    if (op.after) {
        HELIOS_TRY(refl::setJson(root, object, op.path, *op.after));
    } else if (current) {
        HELIOS_TRY(refl::apply(root, object, refl::PatchOp{refl::PatchOp::Kind::Remove, op.path, {}}));
    }
    // Ops carry canonical values, so the value read back must be byte-identical (ED-1).
    HELIOS_TRY_ASSIGN(const std::optional<std::string> now, valueAt(root, object, op.path));
    if (now != op.after) {
        // Restore the previous value before reporting (the op must not half-apply).
        if (op.before) {
            (void)refl::setJson(root, object, op.path, *op.before);
        } else if (now) {
            (void)refl::apply(root, object, refl::PatchOp{refl::PatchOp::Kind::Remove, op.path, {}});
        }
        return Error{ErrorCode::InvalidArgument,
                     std::format("'{}': value {} is not canonical (reads back as {})", op.path, describe(op.after), describe(now))};
    }
    return {};
}

Result<void> applyInsert(const TypeInfo& root, void* object, const Op& op) {
    HELIOS_TRY_ASSIGN(const ListRef list, resolveList(root, object, op.path));
    const usize n = list.type->ops->size(list.ptr);
    if (op.index > n) return conflict(op.path, std::format("insert index {} past the end ({} elements)", op.index, n));
    if (list.keyed() && findElement(list, op.key)) return conflict(op.path, std::format("key {} already present", op.key));
    if (!op.after) return Error{ErrorCode::InvalidArgument, "insert without a value"};
    Guid key;
    if (list.type->kind == Kind::KeyedList) {
        HELIOS_TRY_ASSIGN(key, Guid::parse(op.key));
        if (key.isNil()) return Error{ErrorCode::InvalidArgument, "keyed-list element with a nil key"};
    }
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(*op.after, op.path));
    const TypeInfo& e = list.type->element();
    // Read into a scratch value first so a bad element leaves the list untouched.
    refl::Value tmp(e);
    refl::ReadCtx ctx;
    HELIOS_TRY(refl::readJson(e, tmp.data(), doc.root(), ctx));
    const usize index = static_cast<usize>(op.index);
    void* slot = list.type->ops->insertAt(list.ptr, index);
    e.ops->copy(slot, tmp.data());
    if (list.type->kind == Kind::KeyedList) list.type->ops->setKeyAt(list.ptr, index, key);
    if (elementKey(list, index) != op.key || elementJson(list, index) != *op.after) {
        list.type->ops->eraseAt(list.ptr, index);
        return Error{ErrorCode::InvalidArgument, std::format("'{}': inserted element does not match its key or value", op.path)};
    }
    return {};
}

Result<void> checkElement(const ListRef& list, const Op& op, usize n) {
    if (op.index >= n) return conflict(op.path, std::format("index {} out of range ({} elements)", op.index, n));
    const std::string key = elementKey(list, static_cast<usize>(op.index));
    if (key != op.key) return conflict(op.path, std::format("element {} has key '{}', expected '{}'", op.index, key, op.key));
    return {};
}

Result<void> applyRemove(const TypeInfo& root, void* object, const Op& op) {
    HELIOS_TRY_ASSIGN(const ListRef list, resolveList(root, object, op.path));
    const usize n = list.type->ops->size(list.ptr);
    HELIOS_TRY(checkElement(list, op, n));
    const std::string current = elementJson(list, static_cast<usize>(op.index));
    if (!op.before || current != *op.before) {
        return conflict(op.path, std::format("element {} is {}, expected {}", op.index, describe(current), describe(op.before)));
    }
    list.type->ops->eraseAt(list.ptr, static_cast<usize>(op.index));
    return {};
}

Result<void> applyMove(const TypeInfo& root, void* object, const Op& op) {
    HELIOS_TRY_ASSIGN(const ListRef list, resolveList(root, object, op.path));
    const usize n = list.type->ops->size(list.ptr);
    HELIOS_TRY(checkElement(list, op, n));
    if (op.toIndex >= n) return conflict(op.path, std::format("move target {} out of range ({} elements)", op.toIndex, n));
    if (op.toIndex == op.index) return {};
    const TypeInfo& e = list.type->element();
    const usize from = static_cast<usize>(op.index);
    const usize to = static_cast<usize>(op.toIndex);
    refl::Value tmp(e);
    e.ops->copy(tmp.data(), list.type->ops->element(list.ptr, from));
    Guid key;
    if (list.type->kind == Kind::KeyedList) key = list.type->ops->keyAt(list.ptr, from);
    list.type->ops->eraseAt(list.ptr, from);
    void* slot = list.type->ops->insertAt(list.ptr, to);
    e.ops->copy(slot, tmp.data());
    if (list.type->kind == Kind::KeyedList) list.type->ops->setKeyAt(list.ptr, to, key);
    return {};
}

} // namespace

Result<void> applyValueOp(const TypeInfo& root, void* object, const Op& op) {
    switch (op.kind) {
    case OpKind::Set: return applySet(root, object, op);
    case OpKind::Insert: return applyInsert(root, object, op);
    case OpKind::Remove: return applyRemove(root, object, op);
    case OpKind::Move: return applyMove(root, object, op);
    case OpKind::Create:
    case OpKind::Destroy: break;
    }
    return Error{ErrorCode::InvalidArgument, "document ops are applied by the Framework"};
}

// ---------------------------------------------------------------------------------------------
// diff -> ops
// ---------------------------------------------------------------------------------------------
namespace {

class OpCollector final : public refl::PatchWriter {
public:
    OpCollector(const TypeInfo& root, void* working, const DocId& doc, std::vector<Op>& out)
        : m_root(root), m_working(working), m_doc(doc), m_out(out) {}

    void set(const PropertyPath& path, const TypeInfo& type, const void* value) override {
        if (!m_error.ok()) return;
        const std::string afterJson = refl::toJson(type, value, refl::JsonStyle::Compact);
        if (!path.empty() && path.back().kind == PathSegment::Kind::Keyed) {
            const std::string listPath = path.parent().toString();
            auto list = resolveList(m_root, m_working, listPath);
            if (list && list->keyed() && !findElement(*list, path.back().text)) {
                Op op;
                op.kind = OpKind::Insert;
                op.doc = m_doc;
                op.path = listPath;
                op.index = list->type->ops->size(list->ptr);
                op.key = path.back().text;
                // Canonical element text: read it into a scratch element, write it back.
                refl::Value tmp(list->type->element());
                if (auto doc = refl::JsonDocument::parse(afterJson)) {
                    refl::ReadCtx ctx;
                    if (!refl::readJson(list->type->element(), tmp.data(), doc->root(), ctx)) {
                        fail(Error{ErrorCode::InvalidArgument, "diff: unreadable element"});
                        return;
                    }
                }
                if (!list->keyField.empty()) {
                    // The path's key names the element; make sure the element carries it.
                    const refl::FieldInfo* kf = list->type->element().field(list->keyField);
                    if (kf) kf->type().ops->keyFromText(op.key, kf->ptr(tmp.data()));
                }
                op.after = refl::toJson(list->type->element(), tmp.data(), refl::JsonStyle::Compact);
                push(std::move(op));
                return;
            }
        }
        const std::string pathText = path.toString();
        auto before = valueAt(m_root, m_working, pathText);
        if (!before) {
            fail(std::move(before).error());
            return;
        }
        Op op;
        op.kind = OpKind::Set;
        op.doc = m_doc;
        op.path = pathText;
        op.before = std::move(*before);
        // Canonicalize through the path: write, read back.
        if (auto r = refl::setJson(m_root, m_working, pathText, afterJson); !r) {
            fail(std::move(r).error());
            return;
        }
        auto after = valueAt(m_root, m_working, pathText);
        if (!after || !*after) {
            fail(after ? Error{ErrorCode::InvalidState, "diff: value vanished"} : std::move(after).error());
            return;
        }
        op.after = std::move(*after);
        m_out.push_back(std::move(op));
    }

    void remove(const PropertyPath& path) override {
        if (!m_error.ok()) return;
        if (!path.empty() && path.back().kind == PathSegment::Kind::Keyed) {
            const std::string listPath = path.parent().toString();
            auto list = resolveList(m_root, m_working, listPath);
            if (list && list->keyed()) {
                const auto index = findElement(*list, path.back().text);
                if (!index) {
                    fail(Error{ErrorCode::InvalidState, "diff: removed element not found"});
                    return;
                }
                Op op;
                op.kind = OpKind::Remove;
                op.doc = m_doc;
                op.path = listPath;
                op.index = *index;
                op.key = path.back().text;
                op.before = elementJson(*list, *index);
                push(std::move(op));
                return;
            }
        }
        const std::string pathText = path.toString();
        auto before = valueAt(m_root, m_working, pathText);
        if (!before) {
            fail(std::move(before).error());
            return;
        }
        Op op;
        op.kind = OpKind::Set;
        op.doc = m_doc;
        op.path = pathText;
        op.before = std::move(*before);
        push(std::move(op));
    }

    Result<void> error() const { return m_error; }

private:
    void push(Op op) {
        if (auto r = applyValueOp(m_root, m_working, op); !r) {
            fail(std::move(r).error());
            return;
        }
        m_out.push_back(std::move(op));
    }
    void fail(Error e) {
        if (m_error.ok()) m_error = std::move(e);
    }

    const TypeInfo& m_root;
    void* m_working;
    DocId m_doc;
    std::vector<Op>& m_out;
    Result<void> m_error;
};

} // namespace

Result<std::vector<Op>> diffOps(const TypeInfo& type, const void* before, const void* after, const DocId& doc) {
    std::vector<Op> ops;
    refl::Value working(type);
    type.ops->copy(working.data(), before);
    OpCollector collector(type, working.data(), doc, ops);
    refl::diff(type, before, after, collector);
    const bool exact = collector.error().ok() && refl::equals(type, working.data(), after) &&
                       refl::toJson(type, working.data()) == refl::toJson(type, after);
    if (exact) return ops;
    // The diff could not be expressed exactly (or failed): fall back to one whole-object Set,
    // which is always exact.
    Op op;
    op.kind = OpKind::Set;
    op.doc = doc;
    op.path = "";
    op.before = refl::toJson(type, before, refl::JsonStyle::Compact);
    op.after = refl::toJson(type, after, refl::JsonStyle::Compact);
    ops.clear();
    if (*op.before != *op.after) ops.push_back(std::move(op));
    return ops;
}

} // namespace helios::tf::detail
