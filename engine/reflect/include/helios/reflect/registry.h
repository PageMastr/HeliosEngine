#pragma once
// TypeRegistry: lookup of TypeInfos by stable id or qualified name (02 §3.6).
//
// Registration publishes an immutable snapshot (copy-on-write), so lookups are lock-free (one
// atomic load) and may run concurrently with registration. Snapshots are kept alive until the
// registry is destroyed, so pointers returned by find() stay valid. Generated code registers a
// whole schema file per call (registerTypes(span)), which keeps the copying linear in practice.
// A hot reload swaps in new TypeInfos with replace() at a safe point.
//
// Threading: find()/types() are lock-free and thread-safe; add()/replace() serialize on a mutex.

#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/reflect/type_info.h"

namespace helios::refl {

class TypeRegistry {
public:
    TypeRegistry();
    ~TypeRegistry();
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    /// Process-wide registry used by generated registerTypes() helpers by default.
    static TypeRegistry& global();

    /// Registers types. Re-registering the same TypeInfo object is a no-op; a different TypeInfo
    /// with the same id or qualified name fails with AlreadyExists (nothing is registered then).
    Result<void> add(std::span<const TypeInfo* const> types);
    Result<void> add(const TypeInfo& type) {
        const TypeInfo* one = &type;
        return add(std::span<const TypeInfo* const>(&one, 1));
    }
    /// Registers or replaces types by qualified name (hot reload). Fails if a new type's id is
    /// already used by a type with a different name.
    Result<void> replace(std::span<const TypeInfo* const> types);

    const TypeInfo* find(TypeId id) const noexcept;
    const TypeInfo* find(std::string_view qualifiedName) const noexcept;
    /// All registered types sorted by qualified name (stable for tools and tests).
    std::span<const TypeInfo* const> types() const noexcept;
    usize size() const noexcept { return types().size(); }

private:
    struct Snapshot;
    Result<void> publish(std::span<const TypeInfo* const> types, bool allowReplace);

    std::atomic<const Snapshot*> m_current{nullptr};
    std::mutex m_mutex;
    std::vector<std::unique_ptr<const Snapshot>> m_snapshots; // every published snapshot (never freed early)
};

/// 02 §3.6 lookups in the global registry.
inline const TypeInfo* find(TypeId id) noexcept { return TypeRegistry::global().find(id); }
inline const TypeInfo* find(std::string_view qualifiedName) noexcept { return TypeRegistry::global().find(qualifiedName); }

} // namespace helios::refl
