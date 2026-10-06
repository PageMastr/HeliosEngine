#pragma once
// Type keys: how the ECS's typed API (World::id<T>(), get<T>(), set<T>(), CommandBuffer, SystemBuilder)
// finds a C++ type in a World's type -> ComponentId table (02 §1.4, "No per-image caches of global state";
// WP-0.6c). Two kinds of key, told apart by their low bit:
//
//   * A type declared in a named namespace (every engine and game component) is keyed by name: a
//     compile-time hash of its canonical name, its size and its alignment. Every image computes the same
//     key without keeping state, so a type that one image binds is found by every other image that the same
//     compiler built. The canonical name drops the class-keys MSVC prints ("struct ", "class ", "union ",
//     "enum "), also inside template arguments, and keeps a space only between two identifier characters.
//     For a class, union or enum declared with a name, whose qualified name has neither template arguments
//     nor an inline namespace, that is the qualified name, which MSVC, clang-cl, GCC and Clang print alike,
//     so its key is the same whichever of them built each image. Two kinds of name hold only within one
//     compiler:
//       - names with template arguments, of template specializations and of the types nested in them
//         ("ns::Box<unsigned long>::Inner"): MSVC prints default template arguments, and the compilers
//         spell builtin types differently ("long unsigned int", "unsigned long", "unsigned __int64");
//       - names in an inline namespace: GCC and MSVC print it ("ns::v1::T"); Clang and clang-cl leave it
//         out ("ns::T") wherever the name is unambiguous without it. A Clang translation unit that also
//         sees a "T" declared in "ns" itself prints "ns::v1::T", so on Clang the key also depends on the
//         declarations each translation unit sees.
//     An unnamed class that a typedef names ("typedef struct { ... } T;") prints as "ns::T" on GCC and
//     Clang; no test pins MSVC's spelling of it, so it is not known to cross compilers.
//   * Every other type is keyed per image: a number drawn once per type from a process-wide counter in
//     helios_runtime. That covers what only one translation unit can name (types in unnamed namespaces,
//     local classes, closure types), types in the global namespace, and every specialization with such a
//     type among its template arguments. Clang and clang-cl print a local class without its enclosing
//     function ("Local"), exactly like a global-namespace type, and inside template arguments too
//     ("ns::Box<Local>" for every function's Local). So a class name without a scope makes a type per-image
//     wherever it appears: as the type itself or in a template argument. Two such types never share a key,
//     even when they print the same name; a global-namespace type used from two images has two keys, so
//     declare a component that crosses images, and its template arguments, in named namespaces.
//
// What a key cannot tell apart:
//   * Two distinct name-keyed types with one canonical name and one layout. In a correct program that
//     happens only on Clang and clang-cl, which print a local class without its enclosing function:
//       - a type named through a local class: a class or enum nested in one ("Local::Inner", a local class
//         too) or a pointer to a member of one ("int Local::*"), the same text for a local class "Local" of
//         any function and for a global-namespace "Local", as the type itself or in a template argument
//         ("ns::Box<Local::Inner>", "ns::Box<int Local::*>");
//       - a type in an inline namespace and a type of the same name declared in the enclosing namespace
//         itself, used by translation units that never see both (each prints "ns::T").
//     Typed access through the second such type reaches the first type's component, which has the same
//     size and alignment.
//   * A 64-bit collision of two name keys (the name, size and alignment hashed together), which can join
//     two types of different layouts: typed access through the second type would then reach a component
//     of another size.
//   In both cases registerComponent<T>() and bindType<T>() refuse to bind a key that is already bound to
//   another component. A type with the same name but another layout (a game module built against a changed
//   header, say) has another key: it resolves to no component until it is registered.
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

/// Words a compiler prints inside template arguments that do not name a class: fundamental types and their
/// modifiers (MSVC's __int64 included), cv-qualifiers, the class-keys, literal keywords, and the calling
/// conventions and pointer modifiers MSVC prints in function and pointer types.
inline constexpr std::string_view kNonClassWords[] = {
    "void", "bool", "char", "signed", "unsigned", "short", "int", "long", "float", "double", "wchar_t",
    "char8_t", "char16_t", "char32_t", "__int8", "__int16", "__int32", "__int64", "__int128", "__float128",
    "const", "volatile", "struct", "class", "union", "enum", "true", "false", "nullptr", "decltype",
    "noexcept", "__restrict", "__restrict__", "__cdecl", "__stdcall", "__fastcall", "__vectorcall",
    "__thiscall", "__clrcall", "__ptr32", "__ptr64", "__unaligned"};

/// True if `printed` has "::" right before position `at` (spaces skipped).
constexpr bool scopeEndsAt(std::string_view printed, usize at) noexcept {
    while (at > 0 && printed[at - 1] == ' ') --at;
    return at >= 2 && printed[at - 1] == ':' && printed[at - 2] == ':';
}

/// True if `printed` has "::" at position `at` (spaces skipped).
constexpr bool scopeStartsAt(std::string_view printed, usize at) noexcept {
    while (at < printed.size() && printed[at] == ' ') ++at;
    return at + 1 < printed.size() && printed[at] == ':' && printed[at + 1] == ':';
}

/// True if a type printed as `printed` is keyed per image (see the header comment): it is in an unnamed
/// namespace, a local class or a closure type as some compiler prints it, its name has no "::" outside
/// template arguments (the global namespace, and Clang's spelling of a local class), or a template argument
/// names a class without a scope (the same two, as an argument: "ns::Box<Local>"). A word inside template
/// arguments counts as such a class name unless "::" precedes or follows it, it starts with a digit, or it
/// is one of kNonClassWords.
constexpr bool isPerImageTypeName(std::string_view printed) noexcept {
    // "(anonymous namespace)" Clang, "{anonymous}" GCC, "`anonymous namespace'" and every other scope MSVC
    // makes up ("`void __cdecl f(void)'::`2'::Local") start with a backtick; ")::" is GCC's "f()::Local".
    constexpr std::string_view kMarkers[] = {"(anonymous namespace)", "{anonymous}", "`", ")::",
                                             "<lambda", "(lambda", "<unnamed", "(unnamed"};
    for (const std::string_view marker : kMarkers) {
        if (printed.find(marker) != std::string_view::npos) return true;
    }
    int depth = 0;
    bool qualified = false;
    usize i = 0;
    while (i < printed.size()) {
        const char c = printed[i];
        if (c == '<') {
            ++depth;
        } else if (c == '>') {
            --depth;
        } else if (c == ':' && i + 1 < printed.size() && printed[i + 1] == ':') {
            if (depth == 0) qualified = true;
            ++i;
        } else if (isIdentifierChar(c)) {
            usize end = i;
            while (end < printed.size() && isIdentifierChar(printed[end])) ++end;
            const bool scoped = scopeEndsAt(printed, i) || scopeStartsAt(printed, end);
            if (depth > 0 && !scoped && !(c >= '0' && c <= '9')) {
                const std::string_view word = printed.substr(i, end - i);
                bool nonClass = false;
                for (const std::string_view known : kNonClassWords) {
                    if (word == known) nonClass = true;
                }
                if (!nonClass) return true;
            }
            i = end;
            continue;
        }
        ++i;
    }
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
