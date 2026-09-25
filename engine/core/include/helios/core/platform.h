#pragma once
// Platform, compiler and architecture detection plus portable attribute macros.
//
// Convention: detection macros are only ever defined to 1 (never to 0), so both
// `#if HELIOS_PLATFORM_WINDOWS` and `#ifdef HELIOS_PLATFORM_WINDOWS` are correct. In C++ code prefer
// the constexpr mirrors in helios::platform (kIsWindows, ...) together with `if constexpr`.
// Everything here is header-only, dependency-free and safe to include from any module.

// ---------------------------------------------------------------------------------------------
// Operating system
// ---------------------------------------------------------------------------------------------
#if defined(_WIN32)
#define HELIOS_PLATFORM_WINDOWS 1
#define HELIOS_PLATFORM_NAME "Windows"
#elif defined(__linux__)
#define HELIOS_PLATFORM_LINUX 1
#define HELIOS_PLATFORM_POSIX 1
#define HELIOS_PLATFORM_NAME "Linux"
#elif defined(__APPLE__)
#define HELIOS_PLATFORM_MACOS 1
#define HELIOS_PLATFORM_POSIX 1
#define HELIOS_PLATFORM_NAME "macOS"
#elif defined(__unix__)
#define HELIOS_PLATFORM_POSIX 1
#define HELIOS_PLATFORM_NAME "Unix"
#else
#error "Helios: unsupported platform"
#endif

// ---------------------------------------------------------------------------------------------
// Compiler. clang-cl defines both __clang__ and _MSC_VER: it reports CLANG (+ CLANG_CL), never MSVC,
// because its language extensions and builtins are Clang's.
// ---------------------------------------------------------------------------------------------
#if defined(__clang__)
#define HELIOS_COMPILER_CLANG 1
#if defined(_MSC_VER)
#define HELIOS_COMPILER_CLANG_CL 1
#endif
#define HELIOS_COMPILER_NAME "Clang"
#elif defined(_MSC_VER)
#define HELIOS_COMPILER_MSVC 1
#define HELIOS_COMPILER_NAME "MSVC"
#elif defined(__GNUC__)
#define HELIOS_COMPILER_GCC 1
#define HELIOS_COMPILER_NAME "GCC"
#else
#error "Helios: unsupported compiler"
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
#define HELIOS_TOOLCHAIN_MINGW 1
#endif

// ---------------------------------------------------------------------------------------------
// Architecture
// ---------------------------------------------------------------------------------------------
#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
#define HELIOS_ARCH_X64 1
#define HELIOS_ARCH_NAME "x64"
#elif defined(_M_ARM64) || defined(__aarch64__)
#define HELIOS_ARCH_ARM64 1
#define HELIOS_ARCH_NAME "arm64"
#else
#error "Helios: only 64-bit x64 and arm64 targets are supported"
#endif

// ---------------------------------------------------------------------------------------------
// Build flavour
// ---------------------------------------------------------------------------------------------
#if !defined(NDEBUG)
#define HELIOS_DEBUG 1
#endif

// ---------------------------------------------------------------------------------------------
// Attributes and hints
// ---------------------------------------------------------------------------------------------
#if defined(HELIOS_COMPILER_MSVC)
#define HELIOS_FORCEINLINE __forceinline
#define HELIOS_NOINLINE __declspec(noinline)
#define HELIOS_LIKELY(x) (x)
#define HELIOS_UNLIKELY(x) (x)
#define HELIOS_ASSUME(cond) __assume(cond)
#define HELIOS_UNREACHABLE_HINT() __assume(0)
#define HELIOS_FUNCTION_NAME __FUNCSIG__
#define HELIOS_RESTRICT __restrict
#define HELIOS_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define HELIOS_FORCEINLINE inline __attribute__((always_inline))
#define HELIOS_NOINLINE __attribute__((noinline))
#define HELIOS_LIKELY(x) __builtin_expect(!!(x), 1)
#define HELIOS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#if defined(HELIOS_COMPILER_CLANG)
#define HELIOS_ASSUME(cond) __builtin_assume(cond)
#else
// GCC has no side-effect-free assume before C++23 [[assume]]; `cond` must be side-effect free.
#define HELIOS_ASSUME(cond) \
    do {                    \
        if (!(cond))        \
            __builtin_unreachable(); \
    } while (false)
#endif
#define HELIOS_UNREACHABLE_HINT() __builtin_unreachable()
#define HELIOS_FUNCTION_NAME __PRETTY_FUNCTION__
#define HELIOS_RESTRICT __restrict__
#if defined(HELIOS_COMPILER_CLANG_CL)
#define HELIOS_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define HELIOS_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif
#endif

#define HELIOS_STRINGIFY_IMPL_(x) #x
/// Turns a macro argument into a string literal after expanding it.
#define HELIOS_STRINGIFY(x) HELIOS_STRINGIFY_IMPL_(x)
#define HELIOS_CONCAT_IMPL_(a, b) a##b
/// Token-pastes two macro arguments after expanding them.
#define HELIOS_CONCAT(a, b) HELIOS_CONCAT_IMPL_(a, b)

/// Size used to separate data written by different threads (false-sharing avoidance). A constant
/// rather than std::hardware_destructive_interference_size, which GCC warns about in headers.
#define HELIOS_CACHE_LINE_SIZE 64

// ---------------------------------------------------------------------------------------------
// Symbol visibility. Engine modules are static libraries, so HELIOS_CORE_API is empty unless a
// module is explicitly built shared. Plugins (hot-reloadable game DLLs) export C entry points with
// HELIOS_PLUGIN_EXPORT.
// ---------------------------------------------------------------------------------------------
#if defined(HELIOS_PLATFORM_WINDOWS)
#define HELIOS_API_EXPORT __declspec(dllexport)
#define HELIOS_API_IMPORT __declspec(dllimport)
#else
#define HELIOS_API_EXPORT __attribute__((visibility("default")))
#define HELIOS_API_IMPORT
#endif

#if defined(HELIOS_CORE_SHARED)
#if defined(HELIOS_CORE_BUILDING)
#define HELIOS_CORE_API HELIOS_API_EXPORT
#else
#define HELIOS_CORE_API HELIOS_API_IMPORT
#endif
#else
#define HELIOS_CORE_API
#endif

/// Declares an unmangled, exported function for DynamicLibrary::symbol() lookup.
#define HELIOS_PLUGIN_EXPORT extern "C" HELIOS_API_EXPORT

#if defined(HELIOS_COMPILER_MSVC)
#include <intrin.h>
#endif

namespace helios::platform {

#if defined(HELIOS_PLATFORM_WINDOWS)
inline constexpr bool kIsWindows = true;
#else
inline constexpr bool kIsWindows = false;
#endif
#if defined(HELIOS_PLATFORM_LINUX)
inline constexpr bool kIsLinux = true;
#else
inline constexpr bool kIsLinux = false;
#endif
#if defined(HELIOS_PLATFORM_POSIX)
inline constexpr bool kIsPosix = true;
#else
inline constexpr bool kIsPosix = false;
#endif
#if defined(HELIOS_COMPILER_MSVC)
inline constexpr bool kIsMsvc = true;
#else
inline constexpr bool kIsMsvc = false;
#endif
#if defined(HELIOS_COMPILER_CLANG)
inline constexpr bool kIsClang = true;
#else
inline constexpr bool kIsClang = false;
#endif
#if defined(HELIOS_COMPILER_GCC)
inline constexpr bool kIsGcc = true;
#else
inline constexpr bool kIsGcc = false;
#endif
#if defined(HELIOS_DEBUG)
inline constexpr bool kIsDebugBuild = true;
#else
inline constexpr bool kIsDebugBuild = false;
#endif

inline constexpr const char* kPlatformName = HELIOS_PLATFORM_NAME;
inline constexpr const char* kCompilerName = HELIOS_COMPILER_NAME;
inline constexpr const char* kArchName = HELIOS_ARCH_NAME;

/// Stops in an attached debugger at the call site (force-inlined). Without a debugger the process
/// receives a breakpoint trap, which normally terminates it. Callable from any thread.
HELIOS_FORCEINLINE void debugBreak() noexcept {
#if defined(HELIOS_COMPILER_MSVC)
    __debugbreak();
#elif defined(HELIOS_COMPILER_CLANG)
    __builtin_debugtrap();
#elif defined(HELIOS_ARCH_X64)
    __asm__ volatile("int $0x03");
#elif defined(HELIOS_ARCH_ARM64)
    __asm__ volatile(".inst 0xd4200000"); // brk #0
#else
    __builtin_trap();
#endif
}

} // namespace helios::platform

/// Breaks into the debugger at the call site. Usable as an expression.
#define HELIOS_DEBUG_BREAK() ::helios::platform::debugBreak()
