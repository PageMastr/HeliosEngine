#include "helios/core/cvar.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <mutex>

#include "helios/core/log.h"

namespace helios {

namespace {

char asciiLower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowerKey(std::string_view name) {
    std::string key(name);
    for (char& c : key) c = asciiLower(c);
    return key;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

/// Quotes a value for console/config output if it needs it.
std::string quoteIfNeeded(std::string_view value) {
    const bool needs = value.empty() || value.find_first_of(" \t;\"/#") != std::string_view::npos;
    if (!needs) return std::string(value);
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/// Splits text into statements (';' or newline, outside quotes) of whitespace-separated tokens.
/// Double quotes group; \" and \\ escape inside quotes; // and # start a comment to end of line.
Result<std::vector<std::vector<std::string>>> tokenize(std::string_view text) {
    std::vector<std::vector<std::string>> statements(1);
    std::string token;
    bool inToken = false;
    bool inQuotes = false;
    auto endToken = [&] {
        if (inToken) statements.back().push_back(std::move(token));
        token.clear();
        inToken = false;
    };
    auto endStatement = [&] {
        endToken();
        if (!statements.back().empty()) statements.emplace_back();
    };
    for (usize i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inQuotes) {
            if (c == '\\' && i + 1 < text.size() && (text[i + 1] == '"' || text[i + 1] == '\\')) {
                token.push_back(text[++i]);
            } else if (c == '"') {
                inQuotes = false;
            } else if (c == '\n') {
                return Error{ErrorCode::ParseError, "unterminated quoted string"};
            } else {
                token.push_back(c);
            }
            continue;
        }
        if (c == '"') {
            inQuotes = true;
            inToken = true;
        } else if (c == ';' || c == '\n') {
            endStatement();
        } else if (c == ' ' || c == '\t' || c == '\r') {
            endToken();
        } else if ((c == '/' && i + 1 < text.size() && text[i + 1] == '/') || (c == '#' && !inToken)) {
            while (i < text.size() && text[i] != '\n') ++i;
            endStatement();
        } else {
            token.push_back(c);
            inToken = true;
        }
    }
    if (inQuotes) return Error{ErrorCode::ParseError, "unterminated quoted string"};
    endToken();
    if (statements.back().empty()) statements.pop_back();
    return statements;
}

std::string_view typeName(CVarType type) noexcept {
    switch (type) {
    case CVarType::Bool: return "bool";
    case CVarType::Int: return "int";
    case CVarType::Float: return "float";
    case CVarType::String: return "string";
    }
    return "?";
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Registry storage
// ---------------------------------------------------------------------------------------------

struct CVarRegistry::Impl {
    struct Command {
        std::string name;
        std::string description;
        ConsoleCommandFn fn;
        CVarFlags flags = CVarFlags::None;
    };
    struct Callback {
        u64 id;
        std::string key; // lowercase name, empty = all variables
        std::function<void(CVarBase&)> fn;
    };

    mutable std::recursive_mutex mutex;
    std::map<std::string, CVarBase*> cvars; // lowercase name -> variable
    std::map<std::string, Command> commands;
    std::map<std::string, std::string> pending; // config values for not-yet-registered names
    std::vector<Callback> callbacks;
    u64 nextCallbackId = 1;
};

CVarRegistry::CVarRegistry() : m_impl(new Impl()) {}

CVarRegistry& CVarRegistry::instance() {
    static CVarRegistry* registry = new CVarRegistry(); // leaked: CVars unregister during exit
    return *registry;
}

bool CVarRegistry::registerCVar(CVarBase& cvar) {
    std::string pendingValue;
    bool hasPending = false;
    {
        std::lock_guard lock(m_impl->mutex);
        const std::string key = lowerKey(cvar.name());
        if (m_impl->cvars.contains(key) || m_impl->commands.contains(key)) {
            HELIOS_LOG_ERROR(LogCore, "CVar '{}' is already registered; the duplicate stays unregistered", cvar.name());
            return false;
        }
        m_impl->cvars.emplace(key, &cvar);
        if (auto it = m_impl->pending.find(key); it != m_impl->pending.end()) {
            pendingValue = std::move(it->second);
            hasPending = true;
            m_impl->pending.erase(it);
        }
    }
    if (hasPending) {
        auto parsed = cvar.parseAndStore(pendingValue);
        if (!parsed) {
            HELIOS_LOG_WARN(LogCore, "CVar '{}': ignoring configured value '{}': {}", cvar.name(), pendingValue,
                            parsed.error().toString());
        }
    }
    return true;
}

void CVarRegistry::unregisterCVar(CVarBase& cvar) noexcept {
    std::lock_guard lock(m_impl->mutex);
    auto it = m_impl->cvars.find(lowerKey(cvar.name()));
    if (it != m_impl->cvars.end() && it->second == &cvar) m_impl->cvars.erase(it);
}

void CVarRegistry::fireCallbacks(CVarBase& cvar) {
    std::vector<std::function<void(CVarBase&)>> toCall;
    {
        std::lock_guard lock(m_impl->mutex);
        const std::string key = lowerKey(cvar.name());
        for (const auto& cb : m_impl->callbacks) {
            if (cb.key.empty() || cb.key == key) toCall.push_back(cb.fn);
        }
    }
    for (auto& fn : toCall) fn(cvar);
}

CVarBase* CVarRegistry::find(std::string_view name) const {
    std::lock_guard lock(m_impl->mutex);
    auto it = m_impl->cvars.find(lowerKey(name));
    return it == m_impl->cvars.end() ? nullptr : it->second;
}

bool CVarRegistry::hasCommand(std::string_view name) const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->commands.contains(lowerKey(name));
}

Result<void> CVarRegistry::set(std::string_view name, std::string_view value, CVarSource source) {
    // The lock is released before setting so change callbacks never run under the registry lock.
    // (CVars have static storage duration, so the pointer stays valid.)
    std::unique_lock lock(m_impl->mutex);
    auto it = m_impl->cvars.find(lowerKey(name));
    if (it == m_impl->cvars.end()) {
        if (source == CVarSource::Config) {
            m_impl->pending[lowerKey(name)] = std::string(value);
            return {};
        }
        return makeError(ErrorCode::NotFound, "unknown variable '{}'", name);
    }
    CVarBase* cvar = it->second;
    lock.unlock();
    return cvar->setFromString(value, source);
}

Result<std::string> CVarRegistry::get(std::string_view name) const {
    std::lock_guard lock(m_impl->mutex);
    auto it = m_impl->cvars.find(lowerKey(name));
    if (it == m_impl->cvars.end()) return makeError(ErrorCode::NotFound, "unknown variable '{}'", name);
    return it->second->valueString();
}

Result<void> CVarRegistry::registerCommand(std::string_view name, std::string_view description, ConsoleCommandFn fn,
                                           CVarFlags flags) {
    if (name.empty() || !fn) return Error{ErrorCode::InvalidArgument, "command needs a name and a handler"};
    std::lock_guard lock(m_impl->mutex);
    const std::string key = lowerKey(name);
    if (m_impl->commands.contains(key) || m_impl->cvars.contains(key)) {
        return makeError(ErrorCode::AlreadyExists, "console name '{}' is already registered", name);
    }
    m_impl->commands.emplace(key, Impl::Command{std::string(name), std::string(description), std::move(fn), flags});
    return {};
}

bool CVarRegistry::unregisterCommand(std::string_view name) {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->commands.erase(lowerKey(name)) > 0;
}

u64 CVarRegistry::addChangeCallback(std::string_view name, std::function<void(CVarBase&)> callback) {
    std::lock_guard lock(m_impl->mutex);
    const u64 id = m_impl->nextCallbackId++;
    m_impl->callbacks.push_back(Impl::Callback{id, lowerKey(name), std::move(callback)});
    return id;
}

void CVarRegistry::removeChangeCallback(u64 id) {
    std::lock_guard lock(m_impl->mutex);
    std::erase_if(m_impl->callbacks, [id](const Impl::Callback& cb) { return cb.id == id; });
}

void CVarRegistry::forEachCVar(const std::function<void(CVarBase&)>& fn) const {
    std::lock_guard lock(m_impl->mutex);
    for (auto& [key, cvar] : m_impl->cvars) fn(*cvar);
}

std::vector<std::string> CVarRegistry::complete(std::string_view prefix) const {
    const std::string lower = lowerKey(prefix);
    std::vector<std::string> out;
    std::lock_guard lock(m_impl->mutex);
    for (const auto& [key, cvar] : m_impl->cvars) {
        if (key.starts_with(lower)) out.emplace_back(cvar->name());
    }
    for (const auto& [key, cmd] : m_impl->commands) {
        if (key.starts_with(lower)) out.push_back(cmd.name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return lowerKey(a) < lowerKey(b); });
    return out;
}

std::string CVarRegistry::serializeSaved() const {
    std::string out;
    std::lock_guard lock(m_impl->mutex);
    for (const auto& [key, cvar] : m_impl->cvars) {
        if (!hasFlag(cvar->flags(), CVarFlags::Saved)) continue;
        const std::string value = cvar->valueString();
        if (value == cvar->defaultString()) continue;
        out.append(cvar->name());
        out.push_back(' ');
        out.append(quoteIfNeeded(value));
        out.push_back('\n');
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> CVarRegistry::snapshot(CVarFlags flags) const {
    std::vector<std::pair<std::string, std::string>> out;
    std::lock_guard lock(m_impl->mutex);
    for (const auto& [key, cvar] : m_impl->cvars) {
        if (hasAnyFlag(cvar->flags(), flags)) out.emplace_back(std::string(cvar->name()), cvar->valueString());
    }
    return out;
}

Result<std::string> CVarRegistry::execute(std::string_view text, CVarSource source) {
    HELIOS_TRY_ASSIGN(const auto statements, tokenize(text));
    std::string output;
    for (const auto& tokens : statements) {
        HELIOS_TRY_ASSIGN(std::string result, executeStatement(tokens, source));
        if (!result.empty()) {
            if (!output.empty()) output.push_back('\n');
            output.append(result);
        }
    }
    return output;
}

usize CVarRegistry::executeConfig(std::string_view text) {
    usize failures = 0;
    usize lineNumber = 0;
    usize start = 0;
    while (start <= text.size()) {
        const usize end = std::min(text.find('\n', start), text.size());
        const std::string_view line = trim(text.substr(start, end - start));
        ++lineNumber;
        start = end + 1;
        if (line.empty()) continue;
        auto result = execute(line, CVarSource::Config);
        if (!result) {
            ++failures;
            HELIOS_LOG_WARN(LogCore, "config line {}: '{}': {}", lineNumber, line, result.error().toString());
        }
    }
    return failures;
}

Result<std::string> CVarRegistry::executeStatement(std::span<const std::string> tokens, CVarSource source) {
    if (tokens.empty()) return std::string();
    const std::string& verb = tokens[0];

    auto describe = [](const CVarBase& cvar) {
        return std::format("{} = {} (default {}, {}) - {}", cvar.name(), quoteIfNeeded(cvar.valueString()),
                           quoteIfNeeded(cvar.defaultString()), typeName(cvar.type()), cvar.description());
    };

    if (iequals(verb, "set")) {
        if (tokens.size() != 3) return Error{ErrorCode::InvalidArgument, "usage: set <name> <value>"};
        HELIOS_TRY(set(tokens[1], tokens[2], source));
        return std::string();
    }
    if (iequals(verb, "get")) {
        if (tokens.size() != 2) return Error{ErrorCode::InvalidArgument, "usage: get <name>"};
        return get(tokens[1]);
    }
    if (iequals(verb, "reset")) {
        if (tokens.size() != 2) return Error{ErrorCode::InvalidArgument, "usage: reset <name>"};
        CVarBase* cvar = find(tokens[1]);
        if (!cvar) return makeError(ErrorCode::NotFound, "unknown variable '{}'", tokens[1]);
        HELIOS_TRY(cvar->setFromString(cvar->defaultString(), source));
        return std::string();
    }
    if (iequals(verb, "toggle")) {
        if (tokens.size() != 2) return Error{ErrorCode::InvalidArgument, "usage: toggle <name>"};
        CVarBase* cvar = find(tokens[1]);
        if (!cvar) return makeError(ErrorCode::NotFound, "unknown variable '{}'", tokens[1]);
        if (cvar->type() != CVarType::Bool) return makeError(ErrorCode::InvalidArgument, "'{}' is not a bool", tokens[1]);
        HELIOS_TRY(cvar->setFromString(cvar->valueString() == "1" ? "0" : "1", source));
        return std::string();
    }
    if (iequals(verb, "list") || iequals(verb, "help")) {
        const std::string prefix = tokens.size() > 1 ? lowerKey(tokens[1]) : std::string();
        std::string out;
        std::lock_guard lock(m_impl->mutex);
        for (const auto& [key, cvar] : m_impl->cvars) {
            if (!key.starts_with(prefix)) continue;
            if (!out.empty()) out.push_back('\n');
            out.append(describe(*cvar));
        }
        for (const auto& [key, cmd] : m_impl->commands) {
            if (!key.starts_with(prefix)) continue;
            if (!out.empty()) out.push_back('\n');
            out.append(std::format("{} (command) - {}", cmd.name, cmd.description));
        }
        return out;
    }

    if (CVarBase* cvar = find(verb)) {
        if (tokens.size() == 1) return describe(*cvar);
        if (tokens.size() == 2) {
            HELIOS_TRY(cvar->setFromString(tokens[1], source));
            return std::string();
        }
        return makeError(ErrorCode::InvalidArgument, "too many arguments for '{}' (quote values with spaces)", verb);
    }

    ConsoleCommandFn fn;
    CVarFlags flags = CVarFlags::None;
    {
        std::lock_guard lock(m_impl->mutex);
        auto it = m_impl->commands.find(lowerKey(verb));
        if (it != m_impl->commands.end()) {
            fn = it->second.fn;
            flags = it->second.flags;
        }
    }
    if (fn) {
        if (hasFlag(flags, CVarFlags::Cheat) && source == CVarSource::Console && !cheatsEnabled()) {
            return makeError(ErrorCode::PermissionDenied, "'{}' requires cheats", verb);
        }
        return fn(tokens.subspan(1));
    }
    if (source == CVarSource::Config && tokens.size() == 2) {
        // "<name> <value>" in a config file for a variable that registers later (plugins).
        HELIOS_TRY(set(verb, tokens[1], source));
        return std::string();
    }
    return makeError(ErrorCode::NotFound, "unknown command or variable '{}'", verb);
}

// ---------------------------------------------------------------------------------------------
// CVarBase
// ---------------------------------------------------------------------------------------------

CVarBase::CVarBase(std::string_view name, std::string_view description, CVarFlags flags, CVarType type)
    : m_name(name), m_description(description), m_flags(flags), m_type(type) {}

CVarBase::~CVarBase() {
    if (m_registered) CVarRegistry::instance().unregisterCVar(*this);
}

void CVarBase::registerSelf() { m_registered = CVarRegistry::instance().registerCVar(*this); }

Result<void> CVarBase::checkWritable(CVarSource source) const {
    const CVarRegistry& registry = CVarRegistry::instance();
    if (hasFlag(m_flags, CVarFlags::ReadOnly) && source != CVarSource::Code && source != CVarSource::Config) {
        return makeError(ErrorCode::PermissionDenied, "'{}' is read-only", m_name);
    }
    if (hasFlag(m_flags, CVarFlags::Cheat) && source == CVarSource::Console && !registry.cheatsEnabled()) {
        return makeError(ErrorCode::PermissionDenied, "'{}' is cheat-protected", m_name);
    }
    if (hasFlag(m_flags, CVarFlags::Replicated) && registry.isReplicationClient() && source != CVarSource::Code &&
        source != CVarSource::Server) {
        return makeError(ErrorCode::PermissionDenied, "'{}' is replicated from the server", m_name);
    }
    return {};
}

void CVarBase::notifyChanged() {
    if (m_registered) CVarRegistry::instance().fireCallbacks(*this);
}

Result<void> CVarBase::setFromString(std::string_view text, CVarSource source) {
    HELIOS_TRY(checkWritable(source));
    HELIOS_TRY_ASSIGN(const bool changed, parseAndStore(m_type == CVarType::String ? text : trim(text)));
    if (changed) notifyChanged();
    return {};
}

void CVarBase::resetToDefault(CVarSource source) {
    auto result = setFromString(defaultString(), source);
    if (!result) HELIOS_LOG_WARN(LogCore, "reset of '{}' failed: {}", m_name, result.error().toString());
}

// ---------------------------------------------------------------------------------------------
// ConsoleCommand
// ---------------------------------------------------------------------------------------------

ConsoleCommand::ConsoleCommand(std::string_view name, std::string_view description, ConsoleCommandFn fn,
                               CVarFlags flags)
    : m_name(name) {
    auto result = CVarRegistry::instance().registerCommand(name, description, std::move(fn), flags);
    m_registered = result.ok();
    if (!result) HELIOS_LOG_ERROR(LogCore, "console command '{}': {}", name, result.error().toString());
}

ConsoleCommand::~ConsoleCommand() {
    if (m_registered) CVarRegistry::instance().unregisterCommand(m_name);
}

// ---------------------------------------------------------------------------------------------
// Value parsing
// ---------------------------------------------------------------------------------------------

namespace detail {

Result<bool> parseCVarBool(std::string_view text) {
    if (text == "1" || iequals(text, "true") || iequals(text, "on") || iequals(text, "yes")) return true;
    if (text == "0" || iequals(text, "false") || iequals(text, "off") || iequals(text, "no")) return false;
    return makeError(ErrorCode::ParseError, "'{}' is not a boolean (use 0/1, true/false, on/off)", text);
}

Result<i32> parseCVarInt(std::string_view text) {
    i32 value = 0;
    std::string_view digits = text;
    int base = 10;
    bool negative = false;
    if (digits.starts_with('+') || digits.starts_with('-')) { // exactly one optional sign
        negative = digits.front() == '-';
        digits.remove_prefix(1);
    }
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        base = 16;
        digits.remove_prefix(2);
    }
    // from_chars accepts its own '-', which would let "--5" or "0x-5" through.
    if (digits.empty() || digits.front() == '-' || digits.front() == '+') {
        return makeError(ErrorCode::ParseError, "'{}' is not an integer", text);
    }
    i64 wide = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), wide, base);
    if (ec != std::errc() || ptr != digits.data() + digits.size()) {
        return makeError(ErrorCode::ParseError, "'{}' is not an integer", text);
    }
    if (negative) wide = -wide;
    if (wide < INT32_MIN || wide > INT32_MAX) return makeError(ErrorCode::OutOfRange, "'{}' does not fit in 32 bits", text);
    value = static_cast<i32>(wide);
    return value;
}

Result<f32> parseCVarFloat(std::string_view text) {
    std::string_view s = text;
    if (s.starts_with('+')) {
        s.remove_prefix(1);
        if (s.starts_with('-')) return makeError(ErrorCode::ParseError, "'{}' is not a number", text);
    }
    f32 value = 0.0f;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || ec != std::errc() || ptr != s.data() + s.size()) {
        return makeError(ErrorCode::ParseError, "'{}' is not a number", text);
    }
    // from_chars accepts "nan"/"inf"; a console typo must not poison e.g. a FOV or tick rate.
    if (!std::isfinite(value)) return makeError(ErrorCode::ParseError, "'{}' is not a finite number", text);
    return value;
}

std::string formatCVarFloat(f32 v) { return std::format("{}", v); }

} // namespace detail
} // namespace helios
