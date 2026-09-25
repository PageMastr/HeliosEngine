// Copy-on-write type registry with lock-free lookups.

#include "helios/reflect/registry.h"

#include <algorithm>
#include <format>

namespace helios::refl {

struct TypeRegistry::Snapshot {
    std::vector<const TypeInfo*> byName; // sorted by qualifiedName
    std::vector<const TypeInfo*> byId;   // sorted by id
};

TypeRegistry::TypeRegistry() {
    auto empty = std::make_unique<const Snapshot>();
    m_current.store(empty.get(), std::memory_order_release);
    m_snapshots.push_back(std::move(empty));
}

TypeRegistry::~TypeRegistry() = default;

TypeRegistry& TypeRegistry::global() {
    static TypeRegistry registry;
    return registry;
}

Result<void> TypeRegistry::add(std::span<const TypeInfo* const> types) { return publish(types, false); }

Result<void> TypeRegistry::replace(std::span<const TypeInfo* const> types) { return publish(types, true); }

Result<void> TypeRegistry::publish(std::span<const TypeInfo* const> types, bool allowReplace) {
    std::lock_guard lock(m_mutex);
    const Snapshot* current = m_current.load(std::memory_order_acquire);
    std::vector<const TypeInfo*> merged = current->byName;
    bool changed = false;
    for (const TypeInfo* t : types) {
        if (!t) return Error{ErrorCode::InvalidArgument, "null TypeInfo"};
        auto it = std::lower_bound(merged.begin(), merged.end(), t->qualifiedName,
                                   [](const TypeInfo* a, std::string_view n) { return a->qualifiedName < n; });
        if (it != merged.end() && (*it)->qualifiedName == t->qualifiedName) {
            if (*it == t) continue;
            if (!allowReplace) {
                return makeError(ErrorCode::AlreadyExists, "type '{}' is already registered (different TypeInfo)",
                                 t->qualifiedName);
            }
            *it = t;
            changed = true;
        } else {
            merged.insert(it, t);
            changed = true;
        }
    }
    if (!changed) return {};
    // Ids must be unique across names.
    std::vector<const TypeInfo*> byId = merged;
    std::sort(byId.begin(), byId.end(), [](const TypeInfo* a, const TypeInfo* b) { return a->id < b->id; });
    for (usize i = 1; i < byId.size(); ++i) {
        if (byId[i]->id == byId[i - 1]->id) {
            return makeError(ErrorCode::AlreadyExists, "type id {:#010x} used by both '{}' and '{}'", byId[i]->id,
                             byId[i - 1]->qualifiedName, byId[i]->qualifiedName);
        }
    }
    auto next = std::make_unique<Snapshot>();
    next->byName = std::move(merged);
    next->byId = std::move(byId);
    m_current.store(next.get(), std::memory_order_release);
    m_snapshots.push_back(std::move(next));
    return {};
}

const TypeInfo* TypeRegistry::find(TypeId id) const noexcept {
    const Snapshot* s = m_current.load(std::memory_order_acquire);
    auto it = std::lower_bound(s->byId.begin(), s->byId.end(), id, [](const TypeInfo* a, TypeId v) { return a->id < v; });
    return (it != s->byId.end() && (*it)->id == id) ? *it : nullptr;
}

const TypeInfo* TypeRegistry::find(std::string_view qualifiedName) const noexcept {
    const Snapshot* s = m_current.load(std::memory_order_acquire);
    auto it = std::lower_bound(s->byName.begin(), s->byName.end(), qualifiedName,
                               [](const TypeInfo* a, std::string_view n) { return a->qualifiedName < n; });
    return (it != s->byName.end() && (*it)->qualifiedName == qualifiedName) ? *it : nullptr;
}

std::span<const TypeInfo* const> TypeRegistry::types() const noexcept {
    const Snapshot* s = m_current.load(std::memory_order_acquire);
    return s->byName;
}

} // namespace helios::refl
