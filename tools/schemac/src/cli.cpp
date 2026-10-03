#include "cli.h"

#include <filesystem>
#include <format>
#include <map>
#include <set>

#include "compiler.h"
#include "helios/core/fs.h"
#include "lock_mutex.h"
#include "text.h"

namespace helios::schemac {

namespace {

constexpr std::string_view kVersion = "helios-schemac 0.1.0 (schema language v1, lock format 1)";

constexpr std::string_view kUsage = R"(usage: helios-schemac [options] <file.hschema>...

Compiles Helios schema files (docs/plan/02-engine-runtime.md §3) into code.

options:
  -I <dir>, --include <dir>   import search root; also defines output paths (repeatable)
  --lock <file>               append-only schema lock (stable type/field ids); created if missing
  --check-lock                fail instead of updating an out-of-date lock (CI)
  --allow-default-change      accept changed field defaults (they are part of the wire contract)
  --emit <list>               comma-separated generators: cpp, go, json, luau, sql, repl, lint (default: cpp);
                              repl writes <file>.repl.gen.h/.cpp to --cpp-out
  --cpp-out <dir>             C++ output root (default: .)
  --go-out <dir>              Go package directory (default: .)
  --go-package <name>         Go package name (default: last component of the schema package)
  --json-out <file>           schema description for --emit json (default: schema.json)
  --luau-out <dir>            schema.d.luau and fuel_costs.defaults.json for --emit luau (default: .);
                              the scriptlib glue (<file>.luau.gen.h/.cpp) goes to --cpp-out
  --sql-out <dir>             <schema>/schema.sql and <schema>/migration.sql for --emit sql (default: .)
  --sql-baseline <lock>       lock the migration stub starts from (default: --lock as it was before this run)
  --lint-out <file>           lint report for --emit lint (default: schema.lint.json)
  --samples                   also emit <file>.samples.gen.h (test values shared with the Go test)
  --depfile <file>            write a Makefile-style dependency file (for build systems)
  --depfile-target <path>     target named in the depfile (default: first output)
  --Werror                    treat warnings as errors
  --no-naming-lints           do not warn about naming conventions
  --quiet                     only print diagnostics
  --version, --help

planned generators (not yet implemented): proto, editor, records, docs
)";

struct PlannedEmitter {
    std::string_view name;
    std::string_view what;
};
constexpr PlannedEmitter kPlanned[] = {
    {"proto", ".proto files for connect-go (05 §2.1)"},
    {"editor", "schema.editor.json (07); use --emit json meanwhile"},
    {"records", "record cooking to .hrdb (02 §3.3)"},
    {"docs", "JSON doc model for helios-docs (09 §2.7.1); use --emit json meanwhile"},
};

std::string escapeMake(const std::string& path) {
    std::string out;
    for (const char c : path) {
        if (c == ' ') {
            out += "\\ ";
        } else if (c == '#') {
            out += "\\#";
        } else if (c == '$') {
            out += "$$";
        } else {
            out += c;
        }
    }
    return out;
}

/// Writes `content` unless the file already holds it (keeps timestamps, avoids rebuilds).
bool writeIfChanged(const std::string& path, const std::string& content, std::string& err, bool& written) {
    written = false;
    const fs::Path p = fs::pathFromUtf8(path);
    if (auto existing = fs::readTextFile(p); existing && *existing == content) return true;
    const fs::Path parent = p.parent_path();
    if (!parent.empty() && !fs::isDirectory(parent)) {
        if (auto r = fs::createDirectories(parent); !r) {
            err += std::format("helios-schemac: error: cannot create directory '{}': {}\n", fs::pathToUtf8(parent), r.error().message);
            return false;
        }
    }
    if (auto r = fs::writeTextFile(p, content); !r) {
        err += std::format("helios-schemac: error: cannot write '{}': {}\n", path, r.error().message);
        return false;
    }
    written = true;
    return true;
}

} // namespace

int runCli(std::span<const std::string> args, std::string& out, std::string& err) {
    CompileOptions options;
    std::string depfile;
    std::string depTarget;
    bool quiet = false;
    std::set<std::string> emit;

    for (usize i = 0; i < args.size(); ++i) {
        std::string arg = args[i];
        std::string value;
        bool hasValue = false;
        if (arg.starts_with("--") && arg.find('=') != std::string::npos) {
            value = arg.substr(arg.find('=') + 1);
            arg = arg.substr(0, arg.find('='));
            hasValue = true;
        } else if (arg.starts_with("-I") && arg.size() > 2) {
            value = arg.substr(2);
            arg = "-I";
            hasValue = true;
        }
        auto need = [&](std::string& target) -> bool {
            if (hasValue) {
                target = value;
                return true;
            }
            if (i + 1 >= args.size()) {
                err += std::format("helios-schemac: error: {} needs a value\n", arg);
                return false;
            }
            target = args[++i];
            return true;
        };
        if (arg == "--help" || arg == "-h") {
            out += kUsage;
            return 0;
        }
        if (arg == "--version") {
            out += std::string(kVersion) + "\n";
            return 0;
        }
        if (arg == "-I" || arg == "--include") {
            std::string dir;
            if (!need(dir)) return 2;
            options.includeDirs.push_back(dir);
        } else if (arg == "--lock") {
            if (!need(options.lockPath)) return 2;
        } else if (arg == "--check-lock") {
            options.checkLock = true;
        } else if (arg == "--allow-default-change") {
            options.allowDefaultChange = true;
        } else if (arg == "--emit") {
            std::string list;
            if (!need(list)) return 2;
            usize start = 0;
            while (start <= list.size()) {
                const usize comma = list.find(',', start);
                const std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!item.empty()) emit.insert(item);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (arg == "--cpp-out") {
            if (!need(options.cppOut)) return 2;
        } else if (arg == "--go-out") {
            if (!need(options.goOut)) return 2;
        } else if (arg == "--go-package") {
            if (!need(options.goPackage)) return 2;
        } else if (arg == "--json-out") {
            if (!need(options.jsonOut)) return 2;
        } else if (arg == "--luau-out") {
            if (!need(options.luauOut)) return 2;
        } else if (arg == "--sql-out") {
            if (!need(options.sqlOut)) return 2;
        } else if (arg == "--sql-baseline") {
            if (!need(options.sqlBaseline)) return 2;
        } else if (arg == "--lint-out") {
            if (!need(options.lintOut)) return 2;
        } else if (arg == "--samples") {
            options.samples = true;
        } else if (arg == "--depfile") {
            if (!need(depfile)) return 2;
        } else if (arg == "--depfile-target") {
            if (!need(depTarget)) return 2;
        } else if (arg == "--Werror") {
            options.warningsAsErrors = true;
        } else if (arg == "--no-naming-lints") {
            options.namingLints = false;
        } else if (arg == "--quiet") {
            quiet = true;
        } else if (arg.starts_with("-") && arg != "-") {
            err += std::format("helios-schemac: error: unknown option '{}' (see --help)\n", arg);
            return 2;
        } else {
            options.files.push_back(arg);
        }
    }

    if (emit.empty()) emit.insert("cpp");
    for (const std::string& e : emit) {
        if (e == "cpp") {
            options.emitCpp = true;
        } else if (e == "go") {
            options.emitGo = true;
        } else if (e == "json") {
            options.emitJson = true;
        } else if (e == "luau") {
            options.emitLuau = true;
        } else if (e == "sql") {
            options.emitSql = true;
        } else if (e == "repl") {
            options.emitRepl = true;
        } else if (e == "lint") {
            options.emitLint = true;
        } else {
            for (const PlannedEmitter& p : kPlanned) {
                if (p.name == e) {
                    err += std::format("helios-schemac: error: --emit {} is not yet implemented ({}; planned for a later work package)\n", e,
                                       p.what);
                    return 2;
                }
            }
            err += std::format("helios-schemac: error: unknown generator '{}' (available: cpp, go, json, luau, sql, repl, lint)\n", e);
            return 2;
        }
    }
    if (options.files.empty()) {
        err += "helios-schemac: error: no input files (see --help)\n";
        return 2;
    }

    // Runs that may rewrite the lock hold its mutex from reading it to writing it.
    LockFileMutex lockMutex;
    if (!options.lockPath.empty() && !options.checkLock && !lockMutex.acquire(options.lockPath, err)) return 3;

    RealFileSystem fsys;
    DiagnosticEngine diags;
    CompileResult result = compile(options, fsys, diags);
    err += diags.formatAll();
    if (!result.ok) {
        // The lint report is what CI keeps of a failing gate (--emit lint --Werror), so it is written
        // whenever the lint pass ran; the other outputs of a failed run are not.
        for (const OutputFile& o : result.outputs) {
            bool w = false;
            if (options.emitLint && o.path == options.lintOut) writeIfChanged(o.path, o.content, err, w);
        }
        lockMutex.release(err);
        return 1;
    }

    int status = 0;
    usize written = 0;
    for (const OutputFile& o : result.outputs) {
        bool w = false;
        if (!writeIfChanged(o.path, o.content, err, w)) status = 3;
        written += w ? 1 : 0;
    }
    if (!options.lockPath.empty() && result.lockChanged && !options.checkLock) {
        bool w = false;
        if (!writeIfChanged(options.lockPath, result.lockText, err, w)) status = 3;
        // Always reported (even with --quiet): the build just modified a committed file.
        out += std::format("helios-schemac: updated schema lock {} ({} change{}); commit it\n", options.lockPath, result.lockChanges.size(),
                           result.lockChanges.size() == 1 ? "" : "s");
        if (!quiet) {
            constexpr usize kMaxListed = 20;
            for (usize i = 0; i < result.lockChanges.size() && i < kMaxListed; ++i) out += "  " + result.lockChanges[i] + "\n";
            if (result.lockChanges.size() > kMaxListed) out += std::format("  ... and {} more\n", result.lockChanges.size() - kMaxListed);
        }
    }
    lockMutex.release(err); // the lock is written; the depfile is this run's own
    if (!depfile.empty()) {
        if (depTarget.empty() && !result.outputs.empty()) depTarget = result.outputs.front().path;
        std::string text = escapeMake(depTarget) + ":";
        for (const std::string& in : result.inputs) text += " \\\n  " + escapeMake(in);
        text += "\n";
        bool w = false;
        if (!writeIfChanged(depfile, text, err, w)) status = 3;
    }
    if (!quiet && status == 0) {
        out += std::format("helios-schemac: {} file{} compiled, {} output{} ({} updated)\n", options.files.size(), options.files.size() == 1 ? "" : "s",
                           result.outputs.size(), result.outputs.size() == 1 ? "" : "s", written);
    }
    return status;
}

} // namespace helios::schemac
