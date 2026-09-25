#include "helios/core/cmdline.h"

#include <charconv>

#include "helios/core/utf.h"
#include "platform/os.h"

namespace helios {

namespace {

char asciiLower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
}

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

} // namespace

CommandLine CommandLine::parse(int argc, const char* const* argv) {
    std::vector<std::string> args;
    args.reserve(argc > 0 ? static_cast<usize>(argc) : 0);
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i] ? argv[i] : "");
    return parse(args, true);
}

CommandLine CommandLine::parse(std::span<const std::string> args, bool hasProgram) {
    CommandLine cl;
    usize i = 0;
    if (hasProgram && !args.empty()) {
        cl.m_program = args[0];
        i = 1;
    }
    bool optionsEnded = false;
    for (; i < args.size(); ++i) cl.addArgument(args[i], optionsEnded);
    return cl;
}

CommandLine CommandLine::parseWindows(std::wstring_view commandLine) {
    const std::vector<std::string> args = splitWindows(commandLine);
    return parse(args, true);
}

CommandLine CommandLine::fromProcess() {
    const std::vector<std::string> args = os::processArguments();
    return parse(args, true);
}

void CommandLine::addArgument(std::string arg, bool& optionsEnded) {
    m_arguments.push_back(arg);
    if (optionsEnded) {
        m_positional.push_back(std::move(arg));
        return;
    }
    if (arg == "--") {
        optionsEnded = true;
        return;
    }
    const bool dashed = arg.size() >= 2 && arg[0] == '-';
    const bool negativeNumber = dashed && arg[1] != '-' && (isDigit(arg[1]) || arg[1] == '.');
    if (!dashed || negativeNumber) {
        m_positional.push_back(std::move(arg));
        return;
    }
    std::string_view body(arg);
    body.remove_prefix(arg[1] == '-' ? 2 : 1);
    const usize eq = body.find('=');
    Option option;
    option.key = std::string(body.substr(0, eq));
    if (option.key.empty()) {
        m_positional.push_back(std::move(arg));
        return;
    }
    if (eq != std::string_view::npos) option.value = std::string(body.substr(eq + 1));
    m_options.push_back(std::move(option));
}

std::vector<std::string> CommandLine::splitWindows(std::wstring_view cmd) {
    std::vector<std::string> out;
    const usize n = cmd.size();
    usize i = 0;
    auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t'; };

    // Program name: quotes toggle, no backslash processing, ends at the first unquoted blank.
    std::wstring program;
    bool inQuotes = false;
    while (i < n) {
        const wchar_t c = cmd[i];
        if (c == L'"') {
            inQuotes = !inQuotes;
            ++i;
            continue;
        }
        if (!inQuotes && isSpace(c)) break;
        program.push_back(c);
        ++i;
    }
    out.push_back(wideToUtf8(program));

    for (;;) {
        while (i < n && isSpace(cmd[i])) ++i;
        if (i >= n) break;
        std::wstring arg;
        inQuotes = false;
        while (i < n) {
            const wchar_t c = cmd[i];
            if (!inQuotes && isSpace(c)) break;
            if (c == L'\\') {
                usize count = 0;
                while (i < n && cmd[i] == L'\\') {
                    ++count;
                    ++i;
                }
                if (i < n && cmd[i] == L'"') {
                    arg.append(count / 2, L'\\');
                    if (count % 2 == 1) { // escaped quote
                        arg.push_back(L'"');
                        ++i;
                    }
                } else {
                    arg.append(count, L'\\');
                }
                continue;
            }
            if (c == L'"') {
                if (inQuotes && i + 1 < n && cmd[i + 1] == L'"') { // "" inside quotes -> literal quote
                    arg.push_back(L'"');
                    i += 2;
                    continue;
                }
                inQuotes = !inQuotes;
                ++i;
                continue;
            }
            arg.push_back(c);
            ++i;
        }
        out.push_back(wideToUtf8(arg));
    }
    return out;
}

bool CommandLine::has(std::string_view key) const noexcept {
    for (const Option& o : m_options) {
        if (iequals(o.key, key)) return true;
    }
    return false;
}

std::optional<std::string_view> CommandLine::value(std::string_view key) const noexcept {
    for (auto it = m_options.rbegin(); it != m_options.rend(); ++it) {
        if (it->value && iequals(it->key, key)) return std::string_view(*it->value);
    }
    return std::nullopt;
}

std::vector<std::string_view> CommandLine::values(std::string_view key) const {
    std::vector<std::string_view> out;
    for (const Option& o : m_options) {
        if (o.value && iequals(o.key, key)) out.emplace_back(*o.value);
    }
    return out;
}

std::string_view CommandLine::getString(std::string_view key, std::string_view fallback) const noexcept {
    const auto v = value(key);
    return v ? *v : fallback;
}

i64 CommandLine::getInt(std::string_view key, i64 fallback) const noexcept {
    const auto v = value(key);
    if (!v) return fallback;
    std::string_view s = *v;
    if (s.starts_with('+')) s.remove_prefix(1);
    if (s.starts_with('+') || (v->starts_with('+') && s.starts_with('-'))) return fallback; // "++5", "+-5"
    i64 out = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return (ec == std::errc() && ptr == s.data() + s.size() && !s.empty()) ? out : fallback;
}

f64 CommandLine::getFloat(std::string_view key, f64 fallback) const noexcept {
    const auto v = value(key);
    if (!v) return fallback;
    std::string_view s = *v;
    if (s.starts_with('+')) s.remove_prefix(1);
    if (v->starts_with('+') && s.starts_with('-')) return fallback; // "+-1.5"
    f64 out = 0.0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return (ec == std::errc() && ptr == s.data() + s.size() && !s.empty()) ? out : fallback;
}

bool CommandLine::getBool(std::string_view key, bool fallback) const noexcept {
    bool present = false;
    std::optional<std::string_view> last;
    for (const Option& o : m_options) {
        if (iequals(o.key, key)) {
            present = true;
            last = o.value ? std::optional<std::string_view>(*o.value) : std::nullopt;
        }
    }
    if (!present) return fallback;
    if (!last) return true;
    if (*last == "1" || iequals(*last, "true") || iequals(*last, "yes") || iequals(*last, "on")) return true;
    if (*last == "0" || iequals(*last, "false") || iequals(*last, "no") || iequals(*last, "off")) return false;
    return fallback;
}

} // namespace helios
