#pragma once
// Console variables and commands.
//
//   HELIOS_CVAR(bool, cvarVsync, "r.vsync", true, "Synchronize presentation to the display",
//               helios::CVarFlags::Saved);
//   if (cvarVsync.get()) ...
//   CVarRegistry::instance().execute("set r.vsync 0");     // or "r.vsync 0", "r.vsync" (query)
//
// * Typed CVars: bool, int (i32), float (f32), string. Names are case-insensitive.
// * Flags: Cheat (console changes need cheats enabled), Saved (persisted by serializeSaved()),
//   Replicated (server-authoritative: on a replication client only Code/Server may set it),
//   ReadOnly (only Code/Config may set it).
// * Console commands with arguments; execute() parses "cmd a "b c"; set x 1", comments (// or #).
// * Change callbacks run on the thread that changed the value, after the change, outside locks.
// * Values set from config for not-yet-registered names are kept and applied on registration.
//
// Threading: everything is thread-safe. get() for bool/int/float is a lock-free atomic load.
// CVar objects must have static storage duration or outlive their use (they unregister on
// destruction).

#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios {

enum class CVarFlags : u32 {
    None = 0,
    Cheat = 1u << 0,
    Saved = 1u << 1,
    Replicated = 1u << 2,
    ReadOnly = 1u << 3,
};
HELIOS_ENUM_FLAGS(CVarFlags)

enum class CVarType : u8 { Bool, Int, Float, String };

/// Who is changing a value; determines which flags are enforced.
enum class CVarSource : u8 { Code, Config, Console, Server };

class CVarRegistry;

/// Type-erased CVar interface (registry entries, console UI).
class CVarBase {
public:
    CVarBase(const CVarBase&) = delete;
    CVarBase& operator=(const CVarBase&) = delete;

    std::string_view name() const noexcept { return m_name; }
    std::string_view description() const noexcept { return m_description; }
    CVarFlags flags() const noexcept { return m_flags; }
    CVarType type() const noexcept { return m_type; }
    bool isRegistered() const noexcept { return m_registered; }

    virtual std::string valueString() const = 0;
    virtual std::string defaultString() const = 0;
    /// Parses and applies `text`, enforcing flags for `source`. Fires change callbacks.
    Result<void> setFromString(std::string_view text, CVarSource source = CVarSource::Code);
    void resetToDefault(CVarSource source = CVarSource::Code);

protected:
    CVarBase(std::string_view name, std::string_view description, CVarFlags flags, CVarType type);
    virtual ~CVarBase();
    /// Registers with the registry; called at the end of derived constructors.
    void registerSelf();
    /// Parses text and stores it; returns whether the value changed (no flag checks, no callbacks).
    virtual Result<bool> parseAndStore(std::string_view text) = 0;
    Result<void> checkWritable(CVarSource source) const;
    void notifyChanged();

private:
    friend class CVarRegistry;
    std::string m_name;
    std::string m_description;
    CVarFlags m_flags;
    CVarType m_type;
    bool m_registered = false;
};

template <class T>
class CVar final : public CVarBase {
    static_assert(std::is_same_v<T, bool> || std::is_same_v<T, i32> || std::is_same_v<T, f32> ||
                      std::is_same_v<T, std::string>,
                  "CVar<T> supports bool, i32, f32 and std::string");

    static constexpr CVarType typeOf() {
        if constexpr (std::is_same_v<T, bool>) return CVarType::Bool;
        else if constexpr (std::is_same_v<T, i32>) return CVarType::Int;
        else if constexpr (std::is_same_v<T, f32>) return CVarType::Float;
        else return CVarType::String;
    }

public:
    CVar(std::string_view name, T defaultValue, std::string_view description, CVarFlags flags = CVarFlags::None)
        : CVarBase(name, description, flags, typeOf()), m_default(defaultValue), m_value(defaultValue) {
        registerSelf();
    }
    ~CVar() override = default;

    /// Current value (lock-free for scalars; strings are copied under a lock).
    T get() const {
        if constexpr (std::is_same_v<T, std::string>) {
            std::lock_guard lock(m_stringMutex);
            return m_value;
        } else {
            return m_value.load(std::memory_order_relaxed);
        }
    }
    T operator*() const { return get(); }
    const T& defaultValue() const noexcept { return m_default; }

    /// Sets the value, enforcing flags for `source`. Fires change callbacks if it changed.
    Result<void> set(T value, CVarSource source = CVarSource::Code) {
        HELIOS_TRY(checkWritable(source));
        if (store(std::move(value))) notifyChanged();
        return {};
    }

    /// Registers a callback for this variable (see CVarRegistry::addChangeCallback).
    u64 onChange(std::function<void(CVarBase&)> callback);

    std::string valueString() const override { return format(get()); }
    std::string defaultString() const override { return format(m_default); }

protected:
    Result<bool> parseAndStore(std::string_view text) override;

private:
    bool store(T value) {
        if constexpr (std::is_same_v<T, std::string>) {
            std::lock_guard lock(m_stringMutex);
            if (m_value == value) return false;
            m_value = std::move(value);
            return true;
        } else {
            return m_value.exchange(value, std::memory_order_relaxed) != value;
        }
    }
    static std::string format(const T& v);

    using Storage = std::conditional_t<std::is_same_v<T, std::string>, std::string, std::atomic<T>>;
    T m_default;
    Storage m_value;
    mutable std::mutex m_stringMutex; // only used by CVar<std::string>
};

using CVarBool = CVar<bool>;
using CVarInt = CVar<i32>;
using CVarFloat = CVar<f32>;
using CVarString = CVar<std::string>;

/// Console command handler: receives the arguments after the command name; returns output text.
using ConsoleCommandFn = std::function<Result<std::string>(std::span<const std::string> args)>;

class CVarRegistry {
public:
    /// The process-wide registry (never destroyed, usable during static init/exit).
    static CVarRegistry& instance();

    CVarBase* find(std::string_view name) const;
    bool hasCommand(std::string_view name) const;

    /// Sets a variable from text. A Config-sourced set of an unknown name is remembered and
    /// applied when the variable registers.
    Result<void> set(std::string_view name, std::string_view value, CVarSource source = CVarSource::Console);
    Result<std::string> get(std::string_view name) const;

    Result<void> registerCommand(std::string_view name, std::string_view description, ConsoleCommandFn fn,
                                 CVarFlags flags = CVarFlags::None);
    bool unregisterCommand(std::string_view name);

    /// Executes one or more statements separated by ';' or newlines. Built-ins: set, get, reset,
    /// toggle, list [prefix], help [name]. "<cvar>" queries, "<cvar> <value>" sets,
    /// "<command> args..." runs a command. Stops at the first error. Returns accumulated output.
    Result<std::string> execute(std::string_view text, CVarSource source = CVarSource::Console);
    /// Executes a config file body line by line, continuing past errors (which are logged).
    /// Returns the number of failed statements.
    usize executeConfig(std::string_view text);

    /// "name value" lines for Saved variables whose value differs from the default.
    std::string serializeSaved() const;
    /// (name, value) for all variables with any of `flags` (e.g. Replicated for server->client sync).
    std::vector<std::pair<std::string, std::string>> snapshot(CVarFlags flags) const;

    /// Callback for one variable (by name) or for all variables (empty name). Returns an id.
    u64 addChangeCallback(std::string_view name, std::function<void(CVarBase&)> callback);
    void removeChangeCallback(u64 id);

    void setCheatsEnabled(bool enabled) noexcept { m_cheats.store(enabled, std::memory_order_relaxed); }
    bool cheatsEnabled() const noexcept { return m_cheats.load(std::memory_order_relaxed); }
    /// When true (clients), Replicated variables only accept Code and Server sources.
    void setReplicationClient(bool isClient) noexcept { m_replicationClient.store(isClient, std::memory_order_relaxed); }
    bool isReplicationClient() const noexcept { return m_replicationClient.load(std::memory_order_relaxed); }

    /// Calls fn for every registered variable, sorted by name. Do not register/unregister from fn.
    void forEachCVar(const std::function<void(CVarBase&)>& fn) const;
    /// Names of variables and commands starting with `prefix` (case-insensitive), sorted.
    std::vector<std::string> complete(std::string_view prefix) const;

    struct Impl;

private:
    friend class CVarBase;
    CVarRegistry();
    bool registerCVar(CVarBase& cvar);
    void unregisterCVar(CVarBase& cvar) noexcept;
    void fireCallbacks(CVarBase& cvar);
    Result<std::string> executeStatement(std::span<const std::string> tokens, CVarSource source);

    Impl* m_impl;
    std::atomic<bool> m_cheats{false};
    std::atomic<bool> m_replicationClient{false};
};

/// RAII console command registration (static storage duration recommended).
class ConsoleCommand {
public:
    ConsoleCommand(std::string_view name, std::string_view description, ConsoleCommandFn fn,
                   CVarFlags flags = CVarFlags::None);
    ~ConsoleCommand();
    ConsoleCommand(const ConsoleCommand&) = delete;
    ConsoleCommand& operator=(const ConsoleCommand&) = delete;

private:
    std::string m_name;
    bool m_registered = false;
};

template <class T>
u64 CVar<T>::onChange(std::function<void(CVarBase&)> callback) {
    return CVarRegistry::instance().addChangeCallback(name(), std::move(callback));
}

namespace detail {
Result<bool> parseCVarBool(std::string_view text);
Result<i32> parseCVarInt(std::string_view text);
Result<f32> parseCVarFloat(std::string_view text);
std::string formatCVarFloat(f32 v);
} // namespace detail

template <class T>
Result<bool> CVar<T>::parseAndStore(std::string_view text) {
    if constexpr (std::is_same_v<T, bool>) {
        HELIOS_TRY_ASSIGN(bool v, detail::parseCVarBool(text));
        return store(v);
    } else if constexpr (std::is_same_v<T, i32>) {
        HELIOS_TRY_ASSIGN(i32 v, detail::parseCVarInt(text));
        return store(v);
    } else if constexpr (std::is_same_v<T, f32>) {
        HELIOS_TRY_ASSIGN(f32 v, detail::parseCVarFloat(text));
        return store(v);
    } else {
        return store(std::string(text));
    }
}

template <class T>
std::string CVar<T>::format(const T& v) {
    if constexpr (std::is_same_v<T, bool>) return v ? "1" : "0";
    else if constexpr (std::is_same_v<T, i32>) return std::to_string(v);
    else if constexpr (std::is_same_v<T, f32>) return detail::formatCVarFloat(v);
    else return v;
}

} // namespace helios

/// Defines a CVar with internal linkage: HELIOS_CVAR(i32, cvarMaxFps, "r.maxFps", 144, "Frame cap");
/// Use it in exactly one .cpp per name (a header would create one duplicate per translation unit);
/// other code reads the value through CVarRegistry::instance().find()/get() or an accessor.
#define HELIOS_CVAR(type, var, ...) static ::helios::CVar<type> var{__VA_ARGS__}

/// Defines a console command: HELIOS_CONSOLE_COMMAND(cmdQuit, "quit", "Exit the game", fn);
#define HELIOS_CONSOLE_COMMAND(var, ...) static ::helios::ConsoleCommand var{__VA_ARGS__}
