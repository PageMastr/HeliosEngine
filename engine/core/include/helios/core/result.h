#pragma once
// Result<T>: value-or-error return type (std::expected-like, usable in C++20), the engine's
// replacement for exceptions across module boundaries.
//
//   Result<Config> loadConfig(const Path& p) {
//       HELIOS_TRY_ASSIGN(auto text, fs::readTextFile(p));       // propagates the error
//       if (text.empty()) return Error{ErrorCode::ParseError, "empty config"};
//       return parse(text);
//   }
//   Result<void> save() { HELIOS_TRY(fs::writeFile(...)); return {}; }
//
// Accessing value() of an error result is a fatal error (logged, then abort) in every build;
// operator* / operator-> are unchecked in shipping builds (asserted in development builds).
// Threading: a Result is a plain value type with no shared state.

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "helios/core/assert.h"
#include "helios/core/platform.h"
#include "helios/core/types.h"

namespace helios {

enum class ErrorCode : u32 {
    Ok = 0,
    Unknown,
    InvalidArgument,
    OutOfRange,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    IoError,
    EndOfFile,
    Timeout,
    Unsupported,
    OutOfMemory,
    ParseError,
    InvalidState,
    Cancelled,
    Busy,
    Corrupt,
    VersionMismatch,
    LimitExceeded,
};

/// Stable, human-readable name ("NotFound").
std::string_view errorCodeName(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg = {}) : code(c), message(std::move(msg)) {}

    /// "NotFound: <message>" (or just the code name without a message).
    std::string toString() const;
    friend bool operator==(const Error&, const Error&) = default;
};

/// Builds an Error with a std::format message.
template <class... Args>
Error makeError(ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
    return Error{code, std::format(fmt, std::forward<Args>(args)...)};
}

namespace detail {
[[noreturn]] void resultValueAccessFailed(const Error& error) noexcept;
[[noreturn]] void resultErrorAccessFailed() noexcept;
} // namespace detail

template <class T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported; use a pointer");
    static_assert(!std::is_same_v<std::remove_cv_t<T>, Error>, "Result<Error> is ambiguous");

public:
    using ValueType = T;

    Result()
        requires std::is_default_constructible_v<T>
        : m_storage(std::in_place_index<0>) {}

    template <class U = T>
        requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, Result> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Error> &&
                 !std::is_same_v<std::remove_cvref_t<U>, std::in_place_t>)
    Result(U&& value) : m_storage(std::in_place_index<0>, std::forward<U>(value)) {}

    Result(Error error) : m_storage(std::in_place_index<1>, std::move(error)) {}

    template <class... Args>
    explicit Result(std::in_place_t, Args&&... args) : m_storage(std::in_place_index<0>, std::forward<Args>(args)...) {}

    bool hasValue() const noexcept { return m_storage.index() == 0; }
    bool ok() const noexcept { return hasValue(); }
    explicit operator bool() const noexcept { return hasValue(); }

    T& value() & {
        if (HELIOS_UNLIKELY(!hasValue())) detail::resultValueAccessFailed(*std::get_if<1>(&m_storage));
        return *std::get_if<0>(&m_storage);
    }
    const T& value() const& {
        if (HELIOS_UNLIKELY(!hasValue())) detail::resultValueAccessFailed(*std::get_if<1>(&m_storage));
        return *std::get_if<0>(&m_storage);
    }
    T&& value() && { return std::move(value()); }

    T& operator*() & noexcept {
        HELIOS_ASSERT(hasValue(), "Result accessed with an error");
        return *std::get_if<0>(&m_storage);
    }
    const T& operator*() const& noexcept {
        HELIOS_ASSERT(hasValue(), "Result accessed with an error");
        return *std::get_if<0>(&m_storage);
    }
    T&& operator*() && noexcept { return std::move(**this); }
    T* operator->() noexcept { return &**this; }
    const T* operator->() const noexcept { return &**this; }

    const Error& error() const& {
        if (HELIOS_UNLIKELY(hasValue())) detail::resultErrorAccessFailed();
        return *std::get_if<1>(&m_storage);
    }
    Error&& error() && {
        if (HELIOS_UNLIKELY(hasValue())) detail::resultErrorAccessFailed();
        return std::move(*std::get_if<1>(&m_storage));
    }
    ErrorCode errorCode() const noexcept { return hasValue() ? ErrorCode::Ok : std::get_if<1>(&m_storage)->code; }

    template <class U>
    T valueOr(U&& fallback) const& {
        return hasValue() ? *std::get_if<0>(&m_storage) : static_cast<T>(std::forward<U>(fallback));
    }
    template <class U>
    T valueOr(U&& fallback) && {
        return hasValue() ? std::move(*std::get_if<0>(&m_storage)) : static_cast<T>(std::forward<U>(fallback));
    }

    /// Result<U> with fn(value) applied, or the error unchanged.
    template <class F>
    auto map(F&& fn) const& -> Result<std::invoke_result_t<F, const T&>> {
        if (!hasValue()) return error();
        if constexpr (std::is_void_v<std::invoke_result_t<F, const T&>>) {
            std::forward<F>(fn)(**this);
            return {};
        } else {
            return std::forward<F>(fn)(**this);
        }
    }

    /// fn(value) must itself return a Result; errors short-circuit.
    template <class F>
    auto andThen(F&& fn) const& -> std::invoke_result_t<F, const T&> {
        if (!hasValue()) return error();
        return std::forward<F>(fn)(**this);
    }

private:
    std::variant<T, Error> m_storage;
};

template <>
class [[nodiscard]] Result<void> {
public:
    using ValueType = void;

    Result() noexcept = default;
    Result(Error error) : m_error(std::move(error)) {}

    bool hasValue() const noexcept { return !m_error.has_value(); }
    bool ok() const noexcept { return hasValue(); }
    explicit operator bool() const noexcept { return hasValue(); }

    /// Fatal if this holds an error (mirrors Result<T>::value()).
    void value() const {
        if (HELIOS_UNLIKELY(m_error.has_value())) detail::resultValueAccessFailed(*m_error);
    }
    const Error& error() const& {
        if (HELIOS_UNLIKELY(!m_error.has_value())) detail::resultErrorAccessFailed();
        return *m_error;
    }
    Error&& error() && {
        if (HELIOS_UNLIKELY(!m_error.has_value())) detail::resultErrorAccessFailed();
        return std::move(*m_error);
    }
    ErrorCode errorCode() const noexcept { return m_error ? m_error->code : ErrorCode::Ok; }

private:
    std::optional<Error> m_error;
};

namespace detail {
template <class R>
Error forwardError(R&& result) {
    return std::forward<R>(result).error();
}
} // namespace detail

} // namespace helios

template <>
struct std::formatter<helios::ErrorCode> : std::formatter<std::string_view> {
    template <class Ctx>
    auto format(helios::ErrorCode code, Ctx& ctx) const {
        return std::formatter<std::string_view>::format(helios::errorCodeName(code), ctx);
    }
};

template <>
struct std::formatter<helios::Error> : std::formatter<std::string_view> {
    template <class Ctx>
    auto format(const helios::Error& error, Ctx& ctx) const {
        return std::formatter<std::string_view>::format(error.toString(), ctx);
    }
};

/// Evaluates a Result-returning expression and returns its Error from the enclosing function
/// (which must return a Result) on failure. The value, if any, is discarded.
#define HELIOS_TRY(expr)                                                                          \
    do {                                                                                          \
        auto&& heliosTryResult_ = (expr);                                                         \
        if (HELIOS_UNLIKELY(!heliosTryResult_.hasValue()))                                        \
            return ::helios::detail::forwardError(std::forward<decltype(heliosTryResult_)>(heliosTryResult_)); \
    } while (false)

#define HELIOS_TRY_ASSIGN_IMPL_(tmp, decl, expr)                                                  \
    auto&& tmp = (expr);                                                                          \
    if (HELIOS_UNLIKELY(!tmp.hasValue())) return ::helios::detail::forwardError(std::forward<decltype(tmp)>(tmp)); \
    decl = std::forward<decltype(tmp)>(tmp).value()

/// `HELIOS_TRY_ASSIGN(auto file, File::open(path));` declares `file` from the value or propagates
/// the error. Wrap `expr` in parentheses if it contains top-level commas.
#define HELIOS_TRY_ASSIGN(decl, expr) HELIOS_TRY_ASSIGN_IMPL_(HELIOS_CONCAT(heliosTry_, __LINE__), decl, expr)
