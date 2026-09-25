#include "helios/core/log.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

#include "helios/core/fs.h"
#include "helios/core/thread.h"
#include "helios/core/time.h"
#include "platform/os.h"

namespace helios::log {

namespace detail {
constinit std::atomic<u8> g_globalLevel{static_cast<u8>(Level::Info)};
} // namespace detail

namespace {

char asciiLower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

// Leaked singletons: logging must keep working during static destruction and atexit handlers.
struct ChannelRegistry {
    std::mutex mutex;
    Channel* head = nullptr;
    std::vector<std::pair<std::string, Level>> overrides;
};

ChannelRegistry& channelRegistry() {
    static ChannelRegistry* registry = new ChannelRegistry();
    return *registry;
}

using SinkList = std::vector<std::shared_ptr<Sink>>;

struct LoggerState {
    std::mutex mutex;
    std::shared_ptr<const SinkList> sinks;
};

void flushAtExit() { flush(); }

LoggerState& loggerState() {
    static LoggerState* state = [] {
        auto* s = new LoggerState();
        s->sinks = std::make_shared<const SinkList>(SinkList{std::make_shared<ConsoleSink>()});
        std::atexit(flushAtExit);
        return s;
    }();
    return *state;
}

std::shared_ptr<const SinkList> currentSinks() {
    LoggerState& state = loggerState();
    std::lock_guard lock(state.mutex);
    return state.sinks;
}

// Re-entrancy guard: a sink or formatter that logs would otherwise recurse (or deadlock).
thread_local int t_logDepth = 0;

struct DepthGuard {
    DepthGuard() noexcept { ++t_logDepth; }
    ~DepthGuard() { --t_logDepth; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
};

std::string_view baseName(std::string_view path) noexcept {
    const usize slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
void civilFromDays(i64 z, i64& y, u32& m, u32& d) noexcept {
    z += 719468;
    const i64 era = (z >= 0 ? z : z - 146096) / 146097;
    const u64 doe = static_cast<u64>(z - era * 146097);
    const u64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<i64>(yoe) + era * 400;
    const u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u64 mp = (5 * doy + 2) / 153;
    d = static_cast<u32>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<u32>(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) ++y;
}

std::string_view levelTag(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    case Level::Off: return "OFF  ";
    }
    return "?????";
}

const char* levelColor(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "\x1b[90m";
    case Level::Debug: return "\x1b[36m";
    case Level::Info: return "\x1b[32m";
    case Level::Warn: return "\x1b[33m";
    case Level::Error: return "\x1b[31m";
    case Level::Fatal: return "\x1b[97;41m";
    case Level::Off: return "";
    }
    return "";
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------------------------

struct ChannelRegistryAccess {
    static Channel*& next(Channel& c) noexcept { return c.m_next; }
    static std::atomic<bool>& registered(Channel& c) noexcept { return c.m_registered; }
};

void Channel::registerSlow() noexcept {
    ChannelRegistry& registry = channelRegistry();
    std::lock_guard lock(registry.mutex);
    if (m_registered.load(std::memory_order_relaxed)) return;
    m_next = registry.head;
    registry.head = this;
    for (const auto& [name, level] : registry.overrides) {
        if (iequals(name, m_name)) setLevel(level);
    }
    m_registered.store(true, std::memory_order_release);
}

std::string_view levelName(Level level) noexcept {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    case Level::Off: return "OFF";
    }
    return "UNKNOWN";
}

std::optional<Level> parseLevel(std::string_view text) noexcept {
    if (iequals(text, "trace") || iequals(text, "verbose")) return Level::Trace;
    if (iequals(text, "debug")) return Level::Debug;
    if (iequals(text, "info")) return Level::Info;
    if (iequals(text, "warn") || iequals(text, "warning")) return Level::Warn;
    if (iequals(text, "error")) return Level::Error;
    if (iequals(text, "fatal") || iequals(text, "critical")) return Level::Fatal;
    if (iequals(text, "off") || iequals(text, "none")) return Level::Off;
    return std::nullopt;
}

void setChannelLevel(std::string_view channelName, Level level) {
    ChannelRegistry& registry = channelRegistry();
    std::lock_guard lock(registry.mutex);
    bool found = false;
    for (auto& [name, lvl] : registry.overrides) {
        if (iequals(name, channelName)) {
            lvl = level;
            found = true;
        }
    }
    if (!found) registry.overrides.emplace_back(std::string(channelName), level);
    for (Channel* c = registry.head; c; c = ChannelRegistryAccess::next(*c)) {
        if (iequals(c->name(), channelName)) c->setLevel(level);
    }
}

void clearChannelLevel(std::string_view channelName) {
    ChannelRegistry& registry = channelRegistry();
    std::lock_guard lock(registry.mutex);
    std::erase_if(registry.overrides, [&](const auto& entry) { return iequals(entry.first, channelName); });
    for (Channel* c = registry.head; c; c = ChannelRegistryAccess::next(*c)) {
        if (iequals(c->name(), channelName)) c->clearLevel();
    }
}

Channel* findChannel(std::string_view channelName) {
    ChannelRegistry& registry = channelRegistry();
    std::lock_guard lock(registry.mutex);
    for (Channel* c = registry.head; c; c = ChannelRegistryAccess::next(*c)) {
        if (iequals(c->name(), channelName)) return c;
    }
    return nullptr;
}

void forEachChannel(const std::function<void(Channel&)>& fn) {
    std::vector<Channel*> channels;
    {
        ChannelRegistry& registry = channelRegistry();
        std::lock_guard lock(registry.mutex);
        for (Channel* c = registry.head; c; c = ChannelRegistryAccess::next(*c)) channels.push_back(c);
    }
    std::sort(channels.begin(), channels.end(),
              [](const Channel* a, const Channel* b) { return std::string_view(a->name()) < b->name(); });
    for (Channel* c : channels) fn(*c);
}

// ---------------------------------------------------------------------------------------------
// Logger core
// ---------------------------------------------------------------------------------------------

void setLevel(Level level) noexcept { detail::g_globalLevel.store(static_cast<u8>(level), std::memory_order_relaxed); }

Level level() noexcept { return static_cast<Level>(detail::g_globalLevel.load(std::memory_order_relaxed)); }

void addSink(std::shared_ptr<Sink> sink) {
    if (!sink) return;
    LoggerState& state = loggerState();
    std::lock_guard lock(state.mutex);
    auto next = std::make_shared<SinkList>(*state.sinks);
    next->push_back(std::move(sink));
    state.sinks = std::move(next);
}

bool removeSink(const Sink* sink) {
    LoggerState& state = loggerState();
    std::lock_guard lock(state.mutex);
    auto next = std::make_shared<SinkList>(*state.sinks);
    const auto removed = std::erase_if(*next, [&](const std::shared_ptr<Sink>& s) { return s.get() == sink; });
    state.sinks = std::move(next);
    return removed > 0;
}

void clearSinks() {
    LoggerState& state = loggerState();
    std::lock_guard lock(state.mutex);
    state.sinks = std::make_shared<const SinkList>();
}

std::vector<std::shared_ptr<Sink>> sinks() { return *currentSinks(); }

void flush() {
    const auto list = currentSinks();
    for (const auto& sink : *list) sink->flush();
}

void writeMessage(Level lvl, Channel& channel, const SourceLocation& location, std::string_view message) {
    if (t_logDepth > 0) return; // log call from inside a sink: drop instead of recursing
    DepthGuard guard;
    channel.ensureRegistered();

    Record record;
    record.level = lvl;
    record.channel = &channel;
    record.message = message;
    record.location = location;
    record.timestampNs = unixTimeNanos();
    record.threadId = currentThreadId();
    record.threadName = currentThreadName();

    const auto list = currentSinks();
    for (const auto& sink : *list) {
        if (lvl >= sink->level()) sink->write(record);
    }
}

namespace detail {

void writeFormatted(Level lvl, Channel& channel, const SourceLocation& location, std::string_view fmt,
                    std::format_args args) {
    if (t_logDepth > 0) return;
    thread_local std::string t_buffer;
    {
        DepthGuard guard; // formatters that log must not clobber t_buffer
        t_buffer.clear();
        std::vformat_to(std::back_inserter(t_buffer), fmt, args);
    }
    writeMessage(lvl, channel, location, t_buffer);
    if (t_buffer.capacity() > 64 * 1024) t_buffer = std::string(); // don't pin huge one-off buffers
}

void fatalAbort() noexcept {
    flush();
    std::abort();
}

Channel& defaultChannel() noexcept { return LogGeneral; }

} // namespace detail

// ---------------------------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------------------------

std::string formatRecord(const Record& record, const FormatOptions& options) {
    constexpr i64 kNsPerSecond = 1'000'000'000;
    constexpr i64 kSecondsPerDay = 86'400;
    i64 seconds = record.timestampNs / kNsPerSecond;
    i64 nanos = record.timestampNs % kNsPerSecond;
    if (nanos < 0) {
        nanos += kNsPerSecond;
        --seconds;
    }
    i64 days = seconds / kSecondsPerDay;
    i64 secOfDay = seconds % kSecondsPerDay;
    if (secOfDay < 0) {
        secOfDay += kSecondsPerDay;
        --days;
    }
    const auto hh = static_cast<u32>(secOfDay / 3600);
    const auto mm = static_cast<u32>((secOfDay % 3600) / 60);
    const auto ss = static_cast<u32>(secOfDay % 60);
    const auto ms = static_cast<u32>(nanos / 1'000'000);

    std::string out;
    out.reserve(96 + record.message.size());
    auto it = std::back_inserter(out);
    if (options.date) {
        i64 y;
        u32 m, d;
        civilFromDays(days, y, m, d);
        std::format_to(it, "{:04}-{:02}-{:02} ", y, m, d);
    }
    std::format_to(it, "{:02}:{:02}:{:02}.{:03} {} [{}] ", hh, mm, ss, ms, levelTag(record.level),
                   record.channel ? record.channel->name() : "?");
    if (options.thread) {
        if (record.threadName.empty()) {
            std::format_to(it, "<{}> ", record.threadId);
        } else {
            std::format_to(it, "<{}:{}> ", record.threadId, record.threadName);
        }
    }
    out.append(record.message);
    if (options.sourceForWarnings && record.level >= Level::Warn && record.location.file && *record.location.file) {
        std::format_to(it, "  ({}:{})", baseName(record.location.file), record.location.line);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Sinks
// ---------------------------------------------------------------------------------------------

ConsoleSink::ConsoleSink(bool allowColors) {
    if (allowColors) {
        const bool out = os::consoleEnableColors(os::ConsoleStream::Out);
        const bool err = os::consoleEnableColors(os::ConsoleStream::Err);
        m_colors = out && err;
    }
}

void ConsoleSink::write(const Record& record) {
    FormatOptions options;
    options.date = false;
    options.thread = false;
    std::string line = formatRecord(record, options);
    if (m_colors) {
        const bool wholeLine = record.level >= Level::Warn || record.level <= Level::Debug;
        if (wholeLine) {
            line.insert(0, levelColor(record.level));
            line.append("\x1b[0m");
        } else {
            // Color only the level tag, which follows "HH:MM:SS.mmm ".
            constexpr usize kTagPos = 13;
            line.insert(kTagPos + 5, "\x1b[0m");
            line.insert(kTagPos, levelColor(record.level));
        }
    }
    line.push_back('\n');
    const auto stream = record.level >= Level::Error ? os::ConsoleStream::Err : os::ConsoleStream::Out;
    std::lock_guard lock(m_mutex);
    os::consoleWrite(stream, line);
}

struct FileSink::Impl {
    std::mutex mutex;
    fs::File file;
    std::string buffer;
    Level flushLevel = Level::Warn;
};

FileSink::FileSink(const std::filesystem::path& path, bool append, Level flushLevel) : m_impl(std::make_unique<Impl>()) {
    m_impl->flushLevel = flushLevel;
    auto file = fs::File::open(path, append ? fs::OpenMode::Append : fs::OpenMode::Write);
    if (file) m_impl->file = std::move(file).value();
}

FileSink::~FileSink() { flush(); }

bool FileSink::isOpen() const noexcept { return m_impl->file.isOpen(); }

void FileSink::write(const Record& record) {
    std::string line = formatRecord(record);
    line.push_back('\n');
    std::lock_guard lock(m_impl->mutex);
    if (!m_impl->file.isOpen()) return;
    m_impl->buffer.append(line);
    if (record.level >= m_impl->flushLevel || m_impl->buffer.size() >= 16 * 1024) {
        (void)m_impl->file.write(m_impl->buffer.data(), m_impl->buffer.size());
        m_impl->buffer.clear();
    }
}

void FileSink::flush() {
    std::lock_guard lock(m_impl->mutex);
    if (m_impl->file.isOpen() && !m_impl->buffer.empty()) {
        (void)m_impl->file.write(m_impl->buffer.data(), m_impl->buffer.size());
        m_impl->buffer.clear();
    }
}

RingBufferSink::RingBufferSink(usize capacity) : m_capacity(capacity == 0 ? 1 : capacity) {
    m_entries.reserve(m_capacity);
}

void RingBufferSink::write(const Record& record) {
    Entry entry;
    entry.level = record.level;
    entry.channel = record.channel ? record.channel->name() : "";
    entry.message.assign(record.message);
    entry.timestampNs = record.timestampNs;
    entry.threadId = record.threadId;
    std::lock_guard lock(m_mutex);
    entry.sequence = m_sequence.load(std::memory_order_relaxed) + 1;
    m_sequence.store(entry.sequence, std::memory_order_release);
    if (m_entries.size() < m_capacity) {
        m_entries.push_back(std::move(entry));
    } else {
        m_entries[m_head] = std::move(entry);
        m_head = (m_head + 1) % m_capacity;
    }
}

std::vector<RingBufferSink::Entry> RingBufferSink::snapshot(u64 afterSequence) const {
    std::lock_guard lock(m_mutex);
    std::vector<Entry> out;
    const usize n = m_entries.size();
    out.reserve(n);
    for (usize i = 0; i < n; ++i) {
        const Entry& e = m_entries[(m_head + i) % n];
        if (e.sequence > afterSequence) out.push_back(e);
    }
    return out;
}

u64 RingBufferSink::lastSequence() const noexcept { return m_sequence.load(std::memory_order_acquire); }

usize RingBufferSink::size() const {
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

void RingBufferSink::clear() {
    std::lock_guard lock(m_mutex);
    m_entries.clear();
    m_head = 0;
}

void DebuggerSink::write(const Record& record) {
    if (!os::isDebuggerPresent()) return;
    std::string line = formatRecord(record);
    line.push_back('\n');
    os::debugOutput(line);
}

} // namespace helios::log
