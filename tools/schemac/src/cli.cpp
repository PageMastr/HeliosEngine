#include "cli.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <thread>

#include "compiler.h"
#include "helios/core/fs.h"
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
  --emit <list>               comma-separated generators: cpp, go, json (default: cpp)
  --cpp-out <dir>             C++ output root (default: .)
  --go-out <dir>              Go package directory (default: .)
  --go-package <name>         Go package name (default: last component of the schema package)
  --json-out <file>           schema description for --emit json (default: schema.json)
  --samples                   also emit <file>.samples.gen.h (test values shared with the Go test)
  --depfile <file>            write a Makefile-style dependency file (for build systems)
  --depfile-target <path>     target named in the depfile (default: first output)
  --Werror                    treat warnings as errors
  --no-naming-lints           do not warn about naming conventions
  --quiet                     only print diagnostics
  --version, --help

planned generators (not yet implemented): luau, repl, sql, proto, editor, records, lint, docs
)";

struct PlannedEmitter {
    std::string_view name;
    std::string_view what;
};
constexpr PlannedEmitter kPlanned[] = {
    {"luau", "Luau bindings + .d.luau (script host, 02 §7.4)"},
    {"repl", "replication descriptors / ComponentRepDesc (04 §4.1)"},
    {"sql", "goose migration stubs (05 §3)"},
    {"proto", ".proto files for connect-go (05 §2.1)"},
    {"editor", "schema.editor.json (07); use --emit json meanwhile"},
    {"records", "record cooking to .hrdb (02 §3.3)"},
    {"lint", "standalone lint pass (the lints already run on every compile)"},
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

/// Cross-process mutex for a schema lock file: several helios_schema() calls (possibly running in
/// parallel in one build) may share one lock, and each run reads, extends and rewrites it — without
/// mutual exclusion a concurrent run would overwrite another's new ids, tombstones and renames.
/// Portable: creating a directory is atomic on every OS. A directory older than kStaleAfter is left
/// over from a killed run and is taken over.
class LockFileMutex {
public:
    static constexpr auto kStaleAfter = std::chrono::seconds(120);
    static constexpr auto kGiveUpAfter = std::chrono::seconds(300);

    ~LockFileMutex() {
        if (!m_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove(m_dir, ec);
        }
    }

    bool acquire(const std::string& lockPath, std::string& err) {
        const std::filesystem::path dir = fs::pathFromUtf8(lockPath + ".writing");
        if (dir.has_parent_path()) {
            std::error_code ignored; // a new lock may live in a directory that does not exist yet
            std::filesystem::create_directories(dir.parent_path(), ignored);
        }
        const auto start = std::chrono::steady_clock::now();
        auto delay = std::chrono::milliseconds(2);
        for (;;) {
            std::error_code ec;
            if (std::filesystem::create_directory(dir, ec)) {
                m_dir = dir;
                return true;
            }
            if (ec) {
                err += std::format("helios-schemac: error: cannot create '{}': {}\n", fs::pathToUtf8(dir), ec.message());
                return false;
            }
            const auto modified = std::filesystem::last_write_time(dir, ec);
            if (!ec && std::filesystem::file_time_type::clock::now() - modified > kStaleAfter) {
                std::filesystem::remove(dir, ec); // stale: a killed run left it behind
                continue;
            }
            if (std::chrono::steady_clock::now() - start > kGiveUpAfter) {
                err += std::format("helios-schemac: error: timed out waiting for '{}' (another helios-schemac is updating the lock; "
                                   "remove the directory if none is running)\n",
                                   fs::pathToUtf8(dir));
                return false;
            }
            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, std::chrono::milliseconds(100));
        }
    }

private:
    std::filesystem::path m_dir;
};

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
        } else {
            for (const PlannedEmitter& p : kPlanned) {
                if (p.name == e) {
                    err += std::format("helios-schemac: error: --emit {} is not yet implemented ({}; planned for a later work package)\n", e,
                                       p.what);
                    return 2;
                }
            }
            err += std::format("helios-schemac: error: unknown generator '{}' (available: cpp, go, json)\n", e);
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
    if (!result.ok) return 1;

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
