#pragma once
// Assertions.
//
//   HELIOS_ASSERT(ptr != nullptr);                          // development builds only
//   HELIOS_ASSERT(index < size, "index {} >= {}", index, size);
//   HELIOS_VERIFY(file.close());                            // expression evaluated in every build
//   HELIOS_UNREACHABLE();                                   // never returns
//
// HELIOS_ASSERT is compiled in when HELIOS_ENABLE_ASSERTS is 1 (CMake enables it for Debug and
// RelWithDebInfo); when disabled the condition is not evaluated. HELIOS_VERIFY always evaluates and
// always checks its condition, so it is safe for side effects. Failures go to the assert handler,
// which may be overridden (e.g. by tests or the editor) and decides to break, continue or abort.
//
// Threading: failures may be reported from any thread concurrently; the handler must be
// thread-safe. setAssertHandler is atomic.

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "helios/core/platform.h"
#include "helios/core/types.h"

#ifndef HELIOS_ENABLE_ASSERTS
#if defined(NDEBUG)
#define HELIOS_ENABLE_ASSERTS 0
#else
#define HELIOS_ENABLE_ASSERTS 1
#endif
#endif

namespace helios {

struct AssertInfo {
    const char* kind = "ASSERT"; ///< "ASSERT", "VERIFY" or "UNREACHABLE".
    const char* expression = "";
    const char* file = "";
    u32 line = 0;
    const char* function = "";
    std::string_view message;
};

enum class AssertAction : u8 {
    Break,    ///< Trigger a debugger break at the failing site, then continue.
    Continue, ///< Ignore the failure and continue.
    Abort,    ///< Flush logs and terminate the process.
};

using AssertHandler = AssertAction (*)(const AssertInfo& info);

/// Installs a handler; nullptr restores the default. Returns the previous handler.
AssertHandler setAssertHandler(AssertHandler handler) noexcept;
AssertHandler assertHandler() noexcept;

/// Default policy: log the failure (Error/Fatal) and flush; Break when a debugger is attached;
/// otherwise Abort, except a failed VERIFY in a build without asserts continues.
AssertAction defaultAssertHandler(const AssertInfo& info);

/// Number of assert/verify failures reported so far (all threads).
u64 assertFailureCount() noexcept;

namespace detail {
AssertAction reportAssertFailure(const char* kind, const char* expression, const char* file, u32 line,
                                 const char* function, std::string_view message);
[[noreturn]] void assertAbort() noexcept;

template <class... Args>
HELIOS_NOINLINE AssertAction assertFailed(const char* kind, const char* expression, const char* file, u32 line,
                                          const char* function, std::format_string<Args...> fmt, Args&&... args) {
    // Always format, also without arguments, so "{{" / "}}" escapes print as braces.
    const std::string message = std::format(fmt, std::forward<Args>(args)...);
    return reportAssertFailure(kind, expression, file, line, function, message);
}
} // namespace detail
} // namespace helios

// `"" __VA_ARGS__` makes the message optional without __VA_OPT__: it concatenates with a leading
// string literal format, or becomes "" when no message is given. Messages must be literals.
#define HELIOS_ASSERT_CHECK_(kind, cond, ...)                                                            \
    do {                                                                                                 \
        if (HELIOS_UNLIKELY(!(cond))) {                                                                  \
            const ::helios::AssertAction heliosAssertAction_ = ::helios::detail::assertFailed(           \
                kind, #cond, __FILE__, static_cast<::helios::u32>(__LINE__), HELIOS_FUNCTION_NAME, "" __VA_ARGS__); \
            if (heliosAssertAction_ == ::helios::AssertAction::Break) {                                  \
                HELIOS_DEBUG_BREAK();                                                                    \
            } else if (heliosAssertAction_ == ::helios::AssertAction::Abort) {                           \
                ::helios::detail::assertAbort();                                                         \
            }                                                                                            \
        }                                                                                                \
    } while (false)

#if HELIOS_ENABLE_ASSERTS
#define HELIOS_ASSERT(cond, ...) HELIOS_ASSERT_CHECK_("ASSERT", cond, __VA_ARGS__)
#else
// Not evaluated, but still parsed so variables used only in asserts do not warn as unused.
#define HELIOS_ASSERT(cond, ...) ((void)sizeof(!(cond)))
#endif

/// Evaluates `cond` in every build and reports failure through the assert handler.
#define HELIOS_VERIFY(cond, ...) HELIOS_ASSERT_CHECK_("VERIFY", cond, __VA_ARGS__)

#if HELIOS_ENABLE_ASSERTS
/// Marks code that must never execute. Reports and aborts in development builds; an optimizer hint
/// in shipping builds.
#define HELIOS_UNREACHABLE(...)                                                                          \
    do {                                                                                                 \
        if (::helios::detail::assertFailed("UNREACHABLE", "unreachable code", __FILE__,                  \
                                           static_cast<::helios::u32>(__LINE__), HELIOS_FUNCTION_NAME,   \
                                           "" __VA_ARGS__) == ::helios::AssertAction::Break) {           \
            HELIOS_DEBUG_BREAK();                                                                        \
        }                                                                                                \
        ::helios::detail::assertAbort();                                                                 \
    } while (false)
#else
#define HELIOS_UNREACHABLE(...) HELIOS_UNREACHABLE_HINT()
#endif
