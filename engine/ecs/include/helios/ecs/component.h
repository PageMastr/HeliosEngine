#pragma once
// Component registration descriptors (02 §4.1).
//
// Two registration paths feed one runtime path:
//   * Runtime: ComponentDesc {name, size, alignment, hooks, flags, replication layout}. This is
//     what the schema compiler's generated registerComponents(World&) and a reflection TypeInfo
//     (02 §3.6: size/align + TypeOps) will fill in.
//   * Template: World::registerComponent<T>() builds the same descriptor from T with
//     componentDescOf<T>() (value-initializing ctor, copy/move/dtor hooks when T is not trivial,
//     replication layout from T::kReplicatedFields).
//
// Schema attributes map to flags: @shared -> Shared ((OnInstantiate, Inherit)), @sparse -> Sparse,
// @tag -> size 0, @singleton -> Singleton. DontFragment is the flecs 4.1 non-fragmenting storage
// (implies sparse) for high-churn components (SPIKES.md §2).
//
// Replicated components (Iris-style push, 02 §4.4) carry a hidden `FieldMask _dirty` member and a
// constexpr tuple of member pointers naming the replicated fields in wire order:
//
//   struct Health {
//       f32 hp = 100, maxHp = 100;
//       helios::ecs::FieldMask _dirty = 0;
//       static constexpr auto kReplicatedFields = std::make_tuple(&Health::hp, &Health::maxHp);
//   };
//
// Mut<Health>::set<&Health::hp>(v) then marks field 0 (see dirty.h).
//
// Threading: descriptors are plain values.

#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeinfo>

#include "helios/core/types.h"
#include "helios/ecs/types.h"

namespace helios::ecs {

enum class ComponentFlags : u32 {
    None = 0,
    Shared = 1u << 0,       ///< Prefab instances inherit (share) the prefab's value until overridden.
    Sparse = 1u << 1,       ///< Sparse-set storage (stable address, no table move of the data).
    DontFragment = 1u << 2, ///< Adding/removing does not change the entity's table (implies Sparse).
    Singleton = 1u << 3,    ///< Only valid on its own component entity (World::setSingleton).
    Replicated = 1u << 4,   ///< Participates in dirty tracking; requires a dirty-mask member.
    DontInherit = 1u << 5,  ///< Never copied from prefabs to instances (identity-like data).
};
constexpr ComponentFlags operator|(ComponentFlags a, ComponentFlags b) noexcept {
    return static_cast<ComponentFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr ComponentFlags operator&(ComponentFlags a, ComponentFlags b) noexcept {
    return static_cast<ComponentFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
constexpr bool hasFlag(ComponentFlags set, ComponentFlags flag) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(flag)) != 0;
}

/// Lifecycle hooks over arrays of `count` elements (flecs calls them per batch). Any may be null:
/// null construct = zero-fill, null copy/move = memcpy, null destruct = nothing. A component with
/// only `construct` is "plain": construct runs once at registration to capture its default value
/// (see ComponentInfo::defaultValue) and flecs sees a hook-free, memcpy-relocatable type.
struct ComponentHooks {
    void (*construct)(void* dst, i32 count) = nullptr;
    void (*destruct)(void* dst, i32 count) = nullptr;
    void (*copy)(void* dst, const void* src, i32 count) = nullptr; ///< copy-assign into constructed dst
    void (*move)(void* dst, void* src, i32 count) = nullptr;       ///< move-assign into constructed dst
};

struct ComponentDesc {
    std::string name;       ///< Unique, e.g. "Health" or "game.ShipMotion" (dots become scopes).
    u32 size = 0;           ///< 0 = tag.
    u32 alignment = 1;
    ComponentHooks hooks;
    ComponentFlags flags = ComponentFlags::None;
    /// Replicated only: byte offset of the FieldMask dirty member and number of fields (<= 64).
    u32 dirtyOffset = ~0u;
    u32 replicatedFieldCount = 0;
};

/// What the World knows about a registered component.
struct ComponentInfo {
    ComponentId id = 0;
    std::string name;
    u32 size = 0;
    u32 alignment = 1;
    ComponentFlags flags = ComponentFlags::None;
    u32 dirtyOffset = ~0u;
    u32 replicatedFieldCount = 0;
    u8 replIndex = 0xFF; ///< Bit in RepDirty::componentMask (Replicated only).
    /// Default value bytes for "plain" components (no destruct/copy/move hooks). Such components
    /// register no flecs lifecycle hooks at all — hooks would make every table containing them a
    /// flecs "complex" table and disable its fast append/move/delete paths — and the World writes
    /// this value wherever it adds one without an explicit value. nullptr for tags and for
    /// components with lifecycle hooks (flecs constructs those).
    const std::byte* defaultValue = nullptr;
    /// The hooks it was registered with (owned by the World, stable for its lifetime).
    const ComponentHooks* hooks = nullptr;

    bool isTag() const noexcept { return size == 0; }
    bool isReplicated() const noexcept { return hasFlag(flags, ComponentFlags::Replicated); }
};

// ---------------------------------------------------------------------------------------------
// Replicated-field helpers
// ---------------------------------------------------------------------------------------------

/// A component with a `FieldMask _dirty` member and `kReplicatedFields` (tuple of member pointers).
template <class T>
concept ReplicatedComponent = requires(T& t) {
    { t._dirty } -> std::same_as<FieldMask&>;
    std::tuple_size<std::remove_cvref_t<decltype(T::kReplicatedFields)>>::value;
};

template <ReplicatedComponent T>
inline constexpr u32 kReplicatedFieldCount =
    static_cast<u32>(std::tuple_size_v<std::remove_cvref_t<decltype(T::kReplicatedFields)>>);

inline constexpr u32 kNoField = ~0u;

namespace detail {
template <class T, auto Member, usize I>
consteval u32 fieldIndexImpl() {
    constexpr auto fields = T::kReplicatedFields;
    if constexpr (I >= std::tuple_size_v<std::remove_cvref_t<decltype(fields)>>) {
        return kNoField;
    } else {
        using F = std::remove_cvref_t<decltype(std::get<I>(fields))>;
        if constexpr (std::is_same_v<F, decltype(Member)>) {
            if (std::get<I>(fields) == Member) return static_cast<u32>(I);
        }
        return fieldIndexImpl<T, Member, I + 1>();
    }
}
} // namespace detail

/// Index of `Member` in T::kReplicatedFields, or kNoField.
template <ReplicatedComponent T, auto Member>
inline constexpr u32 kFieldIndex = detail::fieldIndexImpl<T, Member, 0>();

/// Mask with the low `count` bits set.
constexpr FieldMask allFieldsMask(u32 count) noexcept {
    return count >= 64 ? ~FieldMask(0) : ((FieldMask(1) << count) - 1);
}

// ---------------------------------------------------------------------------------------------
// Template -> descriptor
// ---------------------------------------------------------------------------------------------

namespace detail {
template <class T>
void constructN(void* dst, i32 count) {
    T* p = static_cast<T*>(dst);
    for (i32 i = 0; i < count; ++i) ::new (static_cast<void*>(p + i)) T{};
}
template <class T>
void destructN(void* dst, i32 count) {
    T* p = static_cast<T*>(dst);
    for (i32 i = 0; i < count; ++i) p[i].~T();
}
template <class T>
void copyN(void* dst, const void* src, i32 count) {
    T* d = static_cast<T*>(dst);
    const T* s = static_cast<const T*>(src);
    for (i32 i = 0; i < count; ++i) d[i] = s[i];
}
template <class T>
void moveN(void* dst, void* src, i32 count) {
    T* d = static_cast<T*>(dst);
    T* s = static_cast<T*>(src);
    for (i32 i = 0; i < count; ++i) d[i] = std::move(s[i]);
}

/// Unqualified type name for T (compiler-specific, used only as a default component name).
template <class T>
constexpr std::string_view rawTypeName() noexcept {
#if defined(HELIOS_COMPILER_MSVC)
    constexpr std::string_view fn = __FUNCSIG__; // "... rawTypeName<struct ns::T>(void) noexcept"
    constexpr std::string_view prefix = "rawTypeName<";
    constexpr std::string_view suffix = ">(void)";
#else
    constexpr std::string_view fn = __PRETTY_FUNCTION__;
    constexpr std::string_view prefix = "T = ";
    constexpr std::string_view suffix = "]";
#endif
    const usize at = fn.find(prefix);
    const usize begin = at == std::string_view::npos ? 0 : at + prefix.size();
    usize end = fn.rfind(suffix);
    // GCC appends "; std::string_view = ..." inside the brackets.
    if (const usize semi = fn.find(';', begin); semi != std::string_view::npos && semi < end) end = semi;
    std::string_view name = fn.substr(begin, end == std::string_view::npos || end < begin ? fn.npos : end - begin);
    for (std::string_view kw : {std::string_view("struct "), std::string_view("class ")}) {
        if (name.starts_with(kw)) name.remove_prefix(kw.size());
    }
    return name;
}
} // namespace detail

/// Default component name of T: its qualified C++ name with "::" replaced by "." (flecs scopes).
template <class T>
std::string defaultComponentName() {
    std::string name(detail::rawTypeName<T>());
    std::string out;
    out.reserve(name.size());
    for (usize i = 0; i < name.size(); ++i) {
        if (name[i] == ':' && i + 1 < name.size() && name[i + 1] == ':') {
            out.push_back('.');
            ++i;
        } else {
            out.push_back(name[i]);
        }
    }
    return out;
}

/// Builds the runtime descriptor for T (name defaults to defaultComponentName<T>()).
template <class T>
ComponentDesc componentDescOf(std::string_view name = {}, ComponentFlags flags = ComponentFlags::None) {
    static_assert(std::is_default_constructible_v<T>, "components must be default constructible");
    static_assert(std::is_copy_assignable_v<T> || std::is_trivially_copyable_v<T>, "components must be copyable");
    ComponentDesc desc;
    desc.name = name.empty() ? defaultComponentName<T>() : std::string(name);
    desc.flags = flags;
    if constexpr (std::is_empty_v<T>) {
        desc.size = 0;
        desc.alignment = 1;
    } else {
        desc.size = static_cast<u32>(sizeof(T));
        desc.alignment = static_cast<u32>(alignof(T));
        desc.hooks.construct = &detail::constructN<T>;
        if constexpr (!std::is_trivially_destructible_v<T>) desc.hooks.destruct = &detail::destructN<T>;
        if constexpr (!std::is_trivially_copyable_v<T>) {
            desc.hooks.copy = &detail::copyN<T>;
            desc.hooks.move = &detail::moveN<T>;
        }
        if constexpr (ReplicatedComponent<T>) {
            static_assert(std::is_standard_layout_v<T>, "replicated components must be standard layout");
            static_assert(kReplicatedFieldCount<T> <= kMaxReplicatedFields);
            desc.flags = desc.flags | ComponentFlags::Replicated;
            desc.dirtyOffset = static_cast<u32>(offsetof(T, _dirty));
            desc.replicatedFieldCount = kReplicatedFieldCount<T>;
        }
    }
    return desc;
}

} // namespace helios::ecs
