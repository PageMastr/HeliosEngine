#pragma once
// Thread-safe structured logger.
//
//   HELIOS_LOG_INFO("Loaded {} assets in {:.2f} ms", count, ms);          // default channel
//   HELIOS_LOG_WARN(LogNet, "Client {} timed out", clientId);            // explicit channel
//
// * Levels Trace..Fatal; a global runtime level plus optional per-channel overrides.
// * Channels are constant-initialized objects declared with HELIOS_LOG_CHANNEL(Var, "Name").
// * Messages use std::format with compile-time checked format strings.
// * Levels below HELIOS_LOG_COMPILE_LEVEL compile to nothing (arguments are type-checked but never
//   evaluated).
// * Sinks are pluggable: colored console (enables VT processing on Windows), file, in-memory ring
//   buffer (editor console), debugger output, callback.
//
// Threading: every function here may be called from any thread. Formatting happens on the calling
// thread without locks; sinks receive records concurrently and synchronize internally.

#include <atomic>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/platform.h"
#include "helios/core/types.h"

#define HELIOS_LOG_LEVEL_TRACE 0
#define HELIOS_LOG_LEVEL_DEBUG 1
#define HELIOS_LOG_LEVEL_INFO 2
#define HELIOS_LOG_LEVEL_WARN 3
#define HELIOS_LOG_LEVEL_ERROR 4
#define HELIOS_LOG_LEVEL_FATAL 5

/// Messages below this level are stripped at compile time. Override per project with a definition.
#ifndef HELIOS_LOG_COMPILE_LEVEL
#if defined(NDEBUG)
#define HELIOS_LOG_COMPILE_LEVEL HELIOS_LOG_LEVEL_DEBUG
#else
#define HELIOS_LOG_COMPILE_LEVEL HELIOS_LOG_LEVEL_TRACE
#endif
#endif

namespace helios::log {

enum class Level : u8 { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Fatal = 5, Off = 6 };

/// "TRACE", "DEBUG", ... ("OFF" for Level::Off).
std::string_view levelName(Level level) noexcept;
/// Case-insensitive parse of a level name ("warn", "Warning", "error", ...).
std::optional<Level> parseLevel(std::string_view text) noexcept;

/// A named log category. Constant-initialized so it is usable from any static initializer; it
/// registers itself in the global channel list on first use (or via HELIOS_LOG_CHANNEL's registrar).
/// Channels must have static storage duration. Thread-safe.
class Channel {
public:
    explicit constexpr Channel(const char* name) noexcept : m_name(name) {}
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    const char* name() const noexcept { return m_name; }

    /// Level override for this channel; nullopt means "inherit the global level".
    std::optional<Level> levelOverride() const noexcept {
        const u8 raw = m_level.load(std::memory_order_relaxed);
        return raw == kInherit ? std::nullopt : std::optional<Level>(static_cast<Level>(raw));
    }
    void setLevel(Level level) noexcept { m_level.store(static_cast<u8>(level), std::memory_order_relaxed); }
    void clearLevel() noexcept { m_level.store(kInherit, std::memory_order_relaxed); }

    /// Registers the channel in the global list (idempotent, thread-safe).
    void ensureRegistered() noexcept {
        if (!m_registered.load(std::memory_order_acquire)) registerSlow();
    }

private:
    friend struct ChannelRegistryAccess;
    static constexpr u8 kInherit = 0xFF;
    void registerSlow() noexcept;

    const char* m_name;
    std::atomic<u8> m_level{kInherit};
    std::atomic<bool> m_registered{false};
    Channel* m_next = nullptr;
};

struct SourceLocation {
    const char* file = "";
    u32 line = 0;
    const char* function = "";
};

/// One log event as delivered to sinks. Views are only valid during Sink::write.
struct Record {
    Level level = Level::Info;
    const Channel* channel = nullptr;
    std::string_view message;
    SourceLocation location;
    i64 timestampNs = 0; ///< Wall clock, nanoseconds since the Unix epoch (UTC).
    u64 threadId = 0;
    std::string_view threadName;
};

/// Output destination. write() is called concurrently from any thread; implementations must be
/// thread-safe. Sinks must not log themselves (re-entrant log calls are dropped).
class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const Record& record) = 0;
    virtual void flush() {}

    /// Per-sink minimum level (applied after the global/channel filter).
    void setLevel(Level level) noexcept { m_level.store(level, std::memory_order_relaxed); }
    Level level() const noexcept { return m_level.load(std::memory_order_relaxed); }

private:
    std::atomic<Level> m_level{Level::Trace};
};

struct FormatOptions {
    bool date = true;         ///< Include YYYY-MM-DD.
    bool thread = true;       ///< Include thread id / name.
    bool sourceForWarnings = true; ///< Append file:line for Warn and above.
};

/// Formats a record as a single line of text (no trailing newline). Timestamps are UTC.
std::string formatRecord(const Record& record, const FormatOptions& options = {});

/// ANSI-colored console output (stdout; Error/Fatal go to stderr). Colors are enabled only when
/// the stream is a terminal (Windows: VT processing is switched on) and NO_COLOR is unset.
class ConsoleSink final : public Sink {
public:
    explicit ConsoleSink(bool allowColors = true);
    void write(const Record& record) override;
    bool colorsEnabled() const noexcept { return m_colors; }

private:
    std::mutex m_mutex;
    bool m_colors = false;
};

/// Appends formatted lines to a file. Flushes to the OS after every record at or above
/// `flushLevel` (default Warn) and on flush().
class FileSink final : public Sink {
public:
    explicit FileSink(const std::filesystem::path& path, bool append = false, Level flushLevel = Level::Warn);
    ~FileSink() override;
    bool isOpen() const noexcept;
    void write(const Record& record) override;
    void flush() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Keeps the last N records in memory for the editor/in-game console.
class RingBufferSink final : public Sink {
public:
    struct Entry {
        u64 sequence = 0; ///< Monotonic, starts at 1.
        Level level = Level::Info;
        std::string channel;
        std::string message;
        i64 timestampNs = 0;
        u64 threadId = 0;
    };

    explicit RingBufferSink(usize capacity = 2048);
    void write(const Record& record) override;

    /// Copies all retained entries with sequence > afterSequence, oldest first.
    std::vector<Entry> snapshot(u64 afterSequence = 0) const;
    u64 lastSequence() const noexcept;
    usize size() const;
    usize capacity() const noexcept { return m_capacity; }
    void clear();

private:
    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;
    usize m_capacity;
    usize m_head = 0; // index of the oldest entry once full
    std::atomic<u64> m_sequence{0};
};

/// Sends lines to the debugger output window (OutputDebugStringW on Windows; no-op elsewhere).
class DebuggerSink final : public Sink {
public:
    void write(const Record& record) override;
};

/// Invokes a callback for each record (tests, telemetry forwarding). The callback must be
/// thread-safe.
class CallbackSink final : public Sink {
public:
    explicit CallbackSink(std::function<void(const Record&)> fn) : m_fn(std::move(fn)) {}
    void write(const Record& record) override { m_fn(record); }

private:
    std::function<void(const Record&)> m_fn;
};

/// Sink management. A ConsoleSink is installed by default.
void addSink(std::shared_ptr<Sink> sink);
bool removeSink(const Sink* sink);
void clearSinks();
std::vector<std::shared_ptr<Sink>> sinks();

/// Global runtime level (default Info). Channels without an override use it.
void setLevel(Level level) noexcept;
Level level() noexcept;

/// Sets a channel's level by name (case-insensitive). The setting is remembered and applied to a
/// channel that registers later, so it can be issued from config before the channel is used.
void setChannelLevel(std::string_view channelName, Level level);
void clearChannelLevel(std::string_view channelName);
Channel* findChannel(std::string_view channelName);
void forEachChannel(const std::function<void(Channel&)>& fn);

/// Flushes all sinks.
void flush();

/// True if a message at `level` on `channel` would currently be emitted (ignores sink levels).
inline bool isEnabled(Level lvl, const Channel& channel) noexcept;

/// Writes an already formatted message (used by bindings, e.g. scripts).
void writeMessage(Level level, Channel& channel, const SourceLocation& location, std::string_view message);

namespace detail {
extern std::atomic<u8> g_globalLevel;

void writeFormatted(Level level, Channel& channel, const SourceLocation& location, std::string_view fmt,
                    std::format_args args);
[[noreturn]] void fatalAbort() noexcept;

Channel& defaultChannel() noexcept;

template <class... Args>
inline void dispatch(Level lvl, const SourceLocation& loc, Channel& channel, std::format_string<Args...> fmt,
                     Args&&... args) {
    channel.ensureRegistered(); // applies name-based level overrides before the first filter check
    if (!isEnabled(lvl, channel)) return;
    writeFormatted(lvl, channel, loc, fmt.get(), std::make_format_args(args...));
}

template <class... Args>
inline void dispatch(Level lvl, const SourceLocation& loc, std::format_string<Args...> fmt, Args&&... args) {
    Channel& channel = defaultChannel();
    if (!isEnabled(lvl, channel)) return;
    writeFormatted(lvl, channel, loc, fmt.get(), std::make_format_args(args...));
}
} // namespace detail

inline bool isEnabled(Level lvl, const Channel& channel) noexcept {
    const std::optional<Level> overrideLevel = channel.levelOverride();
    const Level threshold =
        overrideLevel ? *overrideLevel : static_cast<Level>(detail::g_globalLevel.load(std::memory_order_relaxed));
    return lvl >= threshold && lvl != Level::Off;
}

} // namespace helios::log

/// Declares (in a header or .cpp) a log channel variable usable as the first HELIOS_LOG_* argument.
#define HELIOS_LOG_CHANNEL(var, name)                                                   \
    inline constinit ::helios::log::Channel var{name};                                  \
    [[maybe_unused]] static const bool HELIOS_CONCAT(heliosLogChannelReg_, var) =       \
        (var.ensureRegistered(), true)

namespace helios {
// Core channels. LogGeneral is used when a HELIOS_LOG_* call names no channel.
HELIOS_LOG_CHANNEL(LogGeneral, "General");
HELIOS_LOG_CHANNEL(LogCore, "Core");
HELIOS_LOG_CHANNEL(LogJobs, "Jobs");
HELIOS_LOG_CHANNEL(LogFs, "FileSystem");
HELIOS_LOG_CHANNEL(LogMemory, "Memory");
} // namespace helios

#define HELIOS_LOG_SOURCE_LOCATION_ \
    ::helios::log::SourceLocation { __FILE__, static_cast<::helios::u32>(__LINE__), __func__ }

#define HELIOS_LOG_EMIT_(lvl, ...) \
    ::helios::log::detail::dispatch(::helios::log::Level::lvl, HELIOS_LOG_SOURCE_LOCATION_, __VA_ARGS__)

// Stripped levels keep the call type-checked (format strings still validated) without evaluating it.
#define HELIOS_LOG_DISCARD_(lvl, ...)            \
    do {                                         \
        if constexpr (false) {                   \
            HELIOS_LOG_EMIT_(lvl, __VA_ARGS__);  \
        }                                        \
    } while (false)

#if HELIOS_LOG_COMPILE_LEVEL <= HELIOS_LOG_LEVEL_TRACE
#define HELIOS_LOG_TRACE(...) HELIOS_LOG_EMIT_(Trace, __VA_ARGS__)
#else
#define HELIOS_LOG_TRACE(...) HELIOS_LOG_DISCARD_(Trace, __VA_ARGS__)
#endif
#if HELIOS_LOG_COMPILE_LEVEL <= HELIOS_LOG_LEVEL_DEBUG
#define HELIOS_LOG_DEBUG(...) HELIOS_LOG_EMIT_(Debug, __VA_ARGS__)
#else
#define HELIOS_LOG_DEBUG(...) HELIOS_LOG_DISCARD_(Debug, __VA_ARGS__)
#endif
#if HELIOS_LOG_COMPILE_LEVEL <= HELIOS_LOG_LEVEL_INFO
#define HELIOS_LOG_INFO(...) HELIOS_LOG_EMIT_(Info, __VA_ARGS__)
#else
#define HELIOS_LOG_INFO(...) HELIOS_LOG_DISCARD_(Info, __VA_ARGS__)
#endif
#if HELIOS_LOG_COMPILE_LEVEL <= HELIOS_LOG_LEVEL_WARN
#define HELIOS_LOG_WARN(...) HELIOS_LOG_EMIT_(Warn, __VA_ARGS__)
#else
#define HELIOS_LOG_WARN(...) HELIOS_LOG_DISCARD_(Warn, __VA_ARGS__)
#endif
#if HELIOS_LOG_COMPILE_LEVEL <= HELIOS_LOG_LEVEL_ERROR
#define HELIOS_LOG_ERROR(...) HELIOS_LOG_EMIT_(Error, __VA_ARGS__)
#else
#define HELIOS_LOG_ERROR(...) HELIOS_LOG_DISCARD_(Error, __VA_ARGS__)
#endif
/// Logs at Fatal level, flushes all sinks and aborts the process. Never stripped.
#define HELIOS_LOG_FATAL(...)                        \
    do {                                             \
        HELIOS_LOG_EMIT_(Fatal, __VA_ARGS__);        \
        ::helios::log::detail::fatalAbort();         \
    } while (false)
