#pragma once
// Command-line parsing.
//
//   game.exe -windowed --width=1920 -map=/content/maps/station.json save1
//
// * "-key", "--key"          flag (no value)
// * "-key=value", "--key=v"  option with value (split at the first '=')
// * anything else            positional (including "-" alone and negative numbers like "-5");
//                            "-key value" is a flag followed by a positional, not an option
// * "--"                     ends option parsing; everything after is positional
// Keys are matched ASCII case-insensitively (Windows users type -Windowed and -windowed alike).
// Repeated keys: value() returns the last occurrence, values() all of them.
//
// On Windows use fromProcess() (GetCommandLineW, full Unicode) rather than main()'s argv, whose
// narrow strings are in the ANSI code page. parseWindows() implements the MSVC CRT splitting rules
// portably, so it is testable on every platform.
//
// Threading: a parsed CommandLine is an immutable value; concurrent const access is safe.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/types.h"

namespace helios {

class CommandLine {
public:
    struct Option {
        std::string key;
        std::optional<std::string> value;
    };

    CommandLine() = default;

    /// Parses main()'s arguments (argv[0] is the program). Arguments are taken as UTF-8.
    static CommandLine parse(int argc, const char* const* argv);
    /// Parses already split arguments; `hasProgram` says whether args[0] is the program path.
    static CommandLine parse(std::span<const std::string> args, bool hasProgram = true);
    /// Splits and parses a Windows command line string (first token is the program).
    static CommandLine parseWindows(std::wstring_view commandLine);
    /// The current process's command line (GetCommandLineW / /proc/self/cmdline).
    static CommandLine fromProcess();

    /// Splits a Windows command line into UTF-8 arguments following the MSVC CRT rules:
    /// whitespace separates; "..." groups; 2n backslashes + quote -> n backslashes + toggle;
    /// 2n+1 backslashes + quote -> n backslashes + literal quote; "" inside quotes -> literal quote;
    /// the program name (first token) takes no backslash escapes.
    static std::vector<std::string> splitWindows(std::wstring_view commandLine);

    const std::string& program() const noexcept { return m_program; }
    /// All arguments after the program, as given.
    const std::vector<std::string>& arguments() const noexcept { return m_arguments; }
    const std::vector<Option>& options() const noexcept { return m_options; }
    const std::vector<std::string>& positional() const noexcept { return m_positional; }

    /// True if the key appeared as a flag or option.
    bool has(std::string_view key) const noexcept;
    /// Value of the last "key=value" occurrence.
    std::optional<std::string_view> value(std::string_view key) const noexcept;
    std::vector<std::string_view> values(std::string_view key) const;

    std::string_view getString(std::string_view key, std::string_view fallback = {}) const noexcept;
    /// Parsed integer value, or `fallback` if missing or malformed.
    i64 getInt(std::string_view key, i64 fallback = 0) const noexcept;
    f64 getFloat(std::string_view key, f64 fallback = 0.0) const noexcept;
    /// A bare flag counts as true; values 1/true/yes/on and 0/false/no/off are recognized.
    bool getBool(std::string_view key, bool fallback = false) const noexcept;

private:
    void addArgument(std::string arg, bool& optionsEnded);

    std::string m_program;
    std::vector<std::string> m_arguments;
    std::vector<Option> m_options;
    std::vector<std::string> m_positional;
};

} // namespace helios
