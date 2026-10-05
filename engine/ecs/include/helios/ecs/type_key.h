#pragma once
// Type keys: how the ECS's typed API (World::id<T>(), get<T>(), set<T>(), CommandBuffer, SystemBuilder)
// finds a C++ type in a World's type -> ComponentId table (02 §1.4, "No per-image caches of global state";
// WP-0.6c). Two kinds of key, told apart by their low bit:
//
//   * A type declared in a named namespace (every engine and game component) is keyed by name: a
//     compile-time hash of its canonical name, its size and its alignment. Every image computes the same
//     key without keeping state, so a type that one image binds is found by every other, whichever of the
//     supported compilers built each image (MSVC, clang-cl, GCC, Clang). The canonical name drops the
//     class-keys MSVC prints ("struct ", "class ", "union ", "enum "), also inside template arguments, and
//     keeps a space only between two identifier characters. For a non-template type that is the qualified
//     name, which every compiler prints alike; keys of template specializations are guaranteed stable only
//     within one compiler (MSVC prints default template arguments and spells builtin types differently),
//     and so are keys of types in inline namespaces (Clang omits them).
//   * Every other type is keyed per image: a number drawn once per type from a process-wide counter in
//     helios_runtime. That covers what only one translation unit can name (types in unnamed namespaces,
//     local classes, closure types) and types in the global namespace. Clang prints a local class without
//     its enclosing function ("Local"), exactly like a global-namespace type, so the global namespace has
//     to be keyed per image too. Two such types never share a key, even when they print the same name; a
//     global-namespace type used from two images has two keys, so declare a component that crosses images
//     in a named namespace.
//
// What a key cannot tell apart: two distinct name-keyed types with one canonical name and one layout. In a
// correct program that is only a class nested in a local class (Clang prints "Local::Inner" for one in any
// function) or a 64-bit hash collision. Typed access through the second such type reaches the first type's
// component, which has the same size and alignment; registerComponent<T>() and bindType<T>() refuse to bind
// a key that is already bound to another component. A type with the same name but another layout (a game
// module built against a changed header, say) has another key: it resolves to no component until it is
// registered.
//
// Threading: keys are constants or are drawn once per type under the C++ static-initialization guard;
// every function here may be called from any thread.

#include <string_view>
#include <type_traits>

#include "helios/core/hash.h"
#include "helios/core/platform.h"
#include "helios/core/types.h"

namespace helios::ecs {

/// Key of a C++ type in a World's type -> ComponentId table (typeKey<T>()). Never 0. Odd keys are name
/// keys, even keys are per-image keys.
using TypeKey = u64;

namespace detail {

/// This function's signature, which names T: "... [with T = X]" (GCC), "... [T = X]" (Clang, clang-cl) or
/// "... signatureNaming<X>(void) noexcept" (MSVC). Constant-evaluated only.
template <class T>
constexpr const char* signatureNaming() noexcept {
#if defined(HELIOS_COMPILER_MSVC)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

/// T's name, cut out of a signatureNaming<T>() signature in any of the three formats.
constexpr std::string_view typeNameFromSignature(std::string_view sig) noexcept {
    constexpr std::string_view kGcc = "[with T = ";
    constexpr std::string_view kClang = "[T = ";
    constexpr std::string_view kMsvc = "signatureNaming<";
    usize begin = 0;
    usize end = std::string_view::npos;
    if (const usize at = sig.find(kGcc); at != std::string_view::npos) {
        begin = at + kGcc.size();
        end = sig.rfind(']');
        // GCC lists the signature's typedefs after the template arguments: "; X = ...".
        if (const usize semi = sig.find(';', begin); semi != std::string_view::npos && semi < end) end = semi;
    } else if (const usize at2 = sig.find(kClang); at2 != std::string_view::npos) {
        begin = at2 + kClang.size();
        end = sig.rfind(']');
    } else if (const usize at3 = sig.find(kMsvc); at3 != std::string_view::npos) {
        begin = at3 + kMsvc.size();
        end = sig.rfind(">(void)");
    }
    if (end == std::string_view::npos || end < begin) return sig.substr(begin);
    return sig.substr(begin, end - begin);
}

constexpr bool isIdentifierChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// The class-keys MSVC prints before every class, union and enum type name.
inline constexpr std::string_view kClassKeys[] = {"struct", "class", "union", "enum"};

/// Calls emit(c) for each character of the canonical spelling of `printed`, a type name as a compiler
/// prints it: a class-key followed by a space is dropped wherever it starts a word, and a space is kept
/// only between two identifier characters ("unsigned int"), never next to punctuation ("A, B", "> >",
/// "char *").
template <class Emit>
constexpr void forEachCanonicalChar(std::string_view printed, Emit&& emit) {
    bool spacePending = false;
    char last = '\0';
    usize i = 0;
    while (i < printed.size()) {
        const char c = printed[i];
        if (c == ' ') {
            spacePending = true;
            ++i;
            continue;
        }
        if (isIdentifierChar(c) && (i == 0 || !isIdentifierChar(printed[i - 1]))) {
            bool classKey = false;
            for (const std::string_view key : kClassKeys) {
                if (printed.substr(i, key.size()) == key && i + key.size() < printed.size() &&
                    printed[i + key.size()] == ' ') {
                    i += key.size(); // the space after it is read as an ordinary pending space
                    classKey = true;
                    break;
                }
            }
            if (classKey) continue;
        }
        if (spacePending && isIdentifierChar(c) && isIdentifierChar(last)) emit(' ');
        spacePending = false;
        emit(c);
        last = c;
        ++i;
    }
}

/// FNV-1a 64 of the canonical spelling of `printed` (equal to fnv1a64(canonical name)).
constexpr u64 canonicalNameHash(std::string_view printed) noexcept {
    u64 hash = kFnv1a64Offset;
    forEachCanonicalChar(printed, [&hash](char c) {
        hash ^= static_cast<u8>(c);
        hash *= kFnv1a64Prime;
    });
    return hash;
}

/// True if the canonical spelling of `printed` equals `canonical` (tests pin spellings with it).
constexpr bool canonicalNameEquals(std::string_view printed, std::string_view canonical) noexcept {
    usize at = 0;
    bool equal = true;
    forEachCanonicalChar(printed, [&](char c) {
        if (at >= canonical.size() || canonical[at] != c) equal = false;
        ++at;
    });
    return equal && at == canonical.size();
}

/// True if a type printed as `printed` is keyed per image (see the header comment): it is in an unnamed
/// namespace, a local class or a closure type as some compiler prints it, or its name has no "::" outside
/// template arguments (the global namespace, and Clang's spelling of a local class).
constexpr bool isPerImageTypeName(std::string_view printed) noexcept {
    // "(anonymous namespace)" Clang, "{anonymous}" GCC, "`anonymous namespace'" and every other scope MSVC
    // makes up ("`void __cdecl f(void)'::`2'::Local") start with a backtick; ")::" is GCC's "f()::Local".
    constexpr std::string_view kMarkers[] = {"(anonymous namespace)", "{anonymous}", "`", ")::",
                                             "<lambda", "(lambda", "<unnamed", "(unnamed"};
    for (const std::string_view marker : kMarkers) {
        if (printed.find(marker) != std::string_view::npos) return true;
    }
    int depth = 0;
    char previous = '\0';
    bool qualified = false;
    forEachCanonicalChar(printed, [&](char c) {
        if (c == '<') ++depth;
        if (c == '>') --depth;
        if (c == ':' && previous == ':' && depth == 0) qualified = true;
        previous = c;
    });
    return !qualified;
}

/// The name key of a type whose canonical name hashes to `nameHash`: odd, so never 0 and never a per-image
/// key. The layout is part of it, so two types that share a name but not a layout never share a key.
constexpr TypeKey nameTypeKey(u64 nameHash, usize size, usize alignment) noexcept {
    return hashCombine(hashCombine(nameHash, static_cast<u64>(size)), static_cast<u64>(alignment)) | 1u;
}

/// T's name as this compiler prints it.
template <class T>
inline constexpr std::string_view kPrintedTypeName = typeNameFromSignature(signatureNaming<T>());

/// FNV-1a 64 of T's canonical name.
template <class T>
inline constexpr u64 kCanonicalNameHash = canonicalNameHash(kPrintedTypeName<T>);

/// True if T is keyed by name, false if it is keyed per image.
template <class T>
inline constexpr bool kKeyedByName = !isPerImageTypeName(kPrintedTypeName<T>);

/// T's name key (meaningful when kKeyedByName<T>).
template <class T>
inline constexpr TypeKey kNameTypeKey = nameTypeKey(kCanonicalNameHash<T>, sizeof(T), alignof(T));

/// Draws the next per-image key from the process-wide counter (in helios_runtime): even, never 0.
/// Thread-safe.
TypeKey nextPerImageTypeKey() noexcept;

/// T's per-image key: drawn once per type in each image (a type that only one translation unit can name
/// is one type per translation unit). Thread-safe.
template <class T>
TypeKey perImageTypeKey() noexcept {
    static const TypeKey key = nextPerImageTypeKey();
    return key;
}

} // namespace detail

/// Key of C++ type T (cv-qualifiers ignored) in a World's type -> ComponentId table: a constant for a type
/// in a named namespace, the type's per-image key otherwise (see the header comment). T must be complete.
/// Callable from any thread.
template <class T>
TypeKey typeKey() noexcept {
    using U = std::remove_cv_t<T>;
    if constexpr (detail::kKeyedByName<U>) {
        return detail::kNameTypeKey<U>;
    } else {
        return detail::perImageTypeKey<U>();
    }
}

} // namespace helios::ecs
