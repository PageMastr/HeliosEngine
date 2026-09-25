// helios-shaderc: Slang -> SPIR-V 1.6 + Helios reflection (.hsr), spirv-val, depfiles.

#include "cli.h"

#include <cstdlib>
#include <format>
#include <optional>
#include <span>
#include <string_view>

#include "helios/core/fs.h"
#include "helios/core/version.h"
#include "helios/render/shader_reflection.h"
#include "tool_process.h"
#include "slang_compiler.h"

namespace helios::shaderc {

namespace {

constexpr std::string_view kUsage = R"(usage: helios-shaderc <input.slang> -o <output.spv> [options]
       helios-shaderc --reflect-spirv <input.spv> [--hsr <file>] [--jsonc <file>]
       helios-shaderc --dump <file.hsr>

Compiles every [shader("...")] entry point of a Slang file into one SPIR-V 1.6 module (flags as in
cmake/HeliosShaders.cmake) and writes the Helios reflection blob (.hsr) next to it.

options:
  -o, --output <file>     SPIR-V output
  --hsr <file>            reflection blob (default: the output with extension .hsr)
  --no-hsr                do not write a reflection blob
  --jsonc <file>          human-readable JSONC rendering of the reflection
  --depfile <file>        Makefile-style dependency file (source + imported modules)
  -I <dir>                import/include root (repeatable)
  -D <NAME[=VALUE]>       preprocessor define (repeatable)
  --entry <name>          compile only this entry point (repeatable)
  -g                      debug information, no optimization (-g2 -O0)
  --validate <mode>       spirv-val: auto (run when found, default), on (required), off
  --spirv-val <path>      spirv-val executable (default: $HELIOS_SPIRV_VAL, else spirv-val on PATH)
  --slang-root <dir>      Slang release (default: $HELIOS_SLANG_ROOT, next to the tool, the pinned one)
  -q, --quiet             no progress output
  --version               print versions and exit
  -h, --help              this text
)";

struct Options {
    std::optional<std::filesystem::path> input;
    std::optional<std::filesystem::path> output;
    std::optional<std::filesystem::path> hsr;
    bool noHsr = false;
    std::optional<std::filesystem::path> jsonc;
    std::optional<std::filesystem::path> depfile;
    std::optional<std::filesystem::path> reflectSpirv;
    std::optional<std::filesystem::path> dump;
    std::vector<std::filesystem::path> includes;
    std::vector<std::pair<std::string, std::string>> defines;
    std::vector<std::string> entries;
    bool debug = false;
    std::string validate = "auto";
    std::string spirvVal;
    std::filesystem::path slangRoot;
    bool quiet = false;
    bool version = false;
    bool help = false;
};

/// Parses `args`; returns an error message on misuse.
std::optional<std::string> parse(const std::vector<std::string>& args, Options& o) {
    for (usize i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&](std::string& out) -> bool {
            if (i + 1 >= args.size()) return false;
            out = args[++i];
            return true;
        };
        auto path = [&](std::optional<std::filesystem::path>& out) -> bool {
            std::string v;
            if (!value(v)) return false;
            out = fs::pathFromUtf8(v);
            return true;
        };
        std::string v;
        if (a == "-o" || a == "--output") {
            if (!path(o.output)) return "missing value for " + a;
        } else if (a == "--hsr") {
            if (!path(o.hsr)) return "missing value for --hsr";
        } else if (a == "--no-hsr") {
            o.noHsr = true;
        } else if (a == "--jsonc") {
            if (!path(o.jsonc)) return "missing value for --jsonc";
        } else if (a == "--depfile") {
            if (!path(o.depfile)) return "missing value for --depfile";
        } else if (a == "--reflect-spirv") {
            if (!path(o.reflectSpirv)) return "missing value for --reflect-spirv";
        } else if (a == "--dump") {
            if (!path(o.dump)) return "missing value for --dump";
        } else if (a == "-I" || a.starts_with("-I")) {
            if (a == "-I") {
                if (!value(v)) return "missing value for -I";
            } else {
                v = a.substr(2);
            }
            o.includes.push_back(fs::pathFromUtf8(v));
        } else if (a == "-D" || a.starts_with("-D")) {
            if (a == "-D") {
                if (!value(v)) return "missing value for -D";
            } else {
                v = a.substr(2);
            }
            const usize eq = v.find('=');
            if (v.empty() || eq == 0) return "bad define '" + v + "'";
            o.defines.emplace_back(v.substr(0, eq), eq == std::string::npos ? std::string() : v.substr(eq + 1));
        } else if (a == "--entry") {
            if (!value(v)) return "missing value for --entry";
            o.entries.push_back(v);
        } else if (a == "-g") {
            o.debug = true;
        } else if (a == "--validate") {
            if (!value(o.validate)) return "missing value for --validate";
            if (o.validate != "auto" && o.validate != "on" && o.validate != "off") {
                return "--validate must be auto, on or off";
            }
        } else if (a == "--spirv-val") {
            if (!value(o.spirvVal)) return "missing value for --spirv-val";
        } else if (a == "--slang-root") {
            if (!value(v)) return "missing value for --slang-root";
            o.slangRoot = fs::pathFromUtf8(v);
        } else if (a == "-q" || a == "--quiet") {
            o.quiet = true;
        } else if (a == "--version") {
            o.version = true;
        } else if (a == "-h" || a == "--help") {
            o.help = true;
        } else if (!a.empty() && a[0] == '-') {
            return "unknown option '" + a + "'";
        } else {
            if (o.input) return "more than one input file";
            o.input = fs::pathFromUtf8(a);
        }
    }
    return std::nullopt;
}

std::string spirvValCommand(const Options& o) {
    if (!o.spirvVal.empty()) return o.spirvVal;
    if (const char* env = std::getenv("HELIOS_SPIRV_VAL"); env && *env) return env;
    return "spirv-val";
}

/// Runs spirv-val on the module `bytes` (read from `spv`) per the --validate mode. The module goes
/// through stdin, so the path never passes through the validator's (possibly ANSI) argv. Returns
/// false (with `err` filled) on failure.
bool validate(const Options& o, const std::filesystem::path& spv, std::span<const u8> bytes, std::string& out,
              std::string& err) {
    if (o.validate == "off") return true;
    const std::string tool = spirvValCommand(o);
    auto probe = runProcess({tool, "--version"});
    if (!probe.ok() || !probe->ok()) {
        if (o.validate == "on") {
            err += std::format("helios-shaderc: spirv-val ('{}') is not available but --validate on was given\n", tool);
            return false;
        }
        if (!o.quiet) out += std::format("helios-shaderc: spirv-val not found ('{}'); validation skipped\n", tool);
        return true;
    }
    auto run = runProcess({tool, "--target-env", "vulkan1.3", "--scalar-block-layout", "-"},
                          std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!run.ok()) {
        err += "helios-shaderc: could not run spirv-val: " + run.error().toString() + "\n";
        return false;
    }
    if (!run->ok()) {
        err += std::format("helios-shaderc: spirv-val rejected {}:\n{}", fs::pathToUtf8(spv), run->output);
        return false;
    }
    return true;
}

bool writeReflection(const Options& o, const render::ShaderReflection& r, const std::filesystem::path& defaultHsr,
                     std::string& err) {
    if (!o.noHsr) {
        const std::filesystem::path hsr = o.hsr ? *o.hsr : defaultHsr;
        if (auto w = fs::writeFile(hsr, render::serializeReflection(r)); !w) {
            err += std::format("helios-shaderc: cannot write {}: {}\n", fs::pathToUtf8(hsr), w.error().toString());
            return false;
        }
    }
    if (o.jsonc) {
        if (auto w = fs::writeTextFile(*o.jsonc, render::reflectionToJsonc(r)); !w) {
            err += std::format("helios-shaderc: cannot write {}: {}\n", fs::pathToUtf8(*o.jsonc), w.error().toString());
            return false;
        }
    }
    return true;
}

std::filesystem::path withExtension(std::filesystem::path p, std::string_view ext) {
    p.replace_extension(ext);
    return p;
}

} // namespace

int runCli(const std::vector<std::string>& args, std::string& out, std::string& err) {
    Options o;
    if (auto problem = parse(args, o)) {
        err += "helios-shaderc: " + *problem + "\n" + std::string(kUsage);
        return 2;
    }
    if (o.help) {
        out += kUsage;
        return 0;
    }
    if (o.version) {
        out += std::format("helios-shaderc {}\n", version::kString);
        auto compiler = SlangCompiler::load(o.slangRoot);
        if (compiler.ok()) {
            out += std::format("Slang {} ({})\n", compiler.value()->version(), fs::pathToUtf8(compiler.value()->libraryPath()));
        } else {
            out += "Slang: " + compiler.error().message + "\n";
        }
        return 0;
    }
    if (o.dump) {
        auto bytes = fs::readFile(*o.dump);
        if (!bytes) {
            err += "helios-shaderc: " + bytes.error().toString() + "\n";
            return 1;
        }
        auto r = render::parseReflection(bytes.value());
        if (!r) {
            err += std::format("helios-shaderc: {}: {}\n", fs::pathToUtf8(*o.dump), r.error().toString());
            return 1;
        }
        out += render::reflectionToJsonc(r.value());
        return 0;
    }
    if (o.reflectSpirv) {
        auto bytes = fs::readFile(*o.reflectSpirv);
        if (!bytes) {
            err += "helios-shaderc: " + bytes.error().toString() + "\n";
            return 1;
        }
        auto r = render::reflectSpirvBytes(bytes.value());
        if (!r) {
            err += std::format("helios-shaderc: {}: {}\n", fs::pathToUtf8(*o.reflectSpirv), r.error().toString());
            return 1;
        }
        if (!validate(o, *o.reflectSpirv, bytes.value(), out, err)) return 1;
        return writeReflection(o, r.value(), withExtension(*o.reflectSpirv, ".hsr"), err) ? 0 : 1;
    }
    if (!o.input || !o.output) {
        err += "helios-shaderc: an input file and -o <output.spv> are required\n" + std::string(kUsage);
        return 2;
    }

    auto compiler = SlangCompiler::load(o.slangRoot);
    if (!compiler) {
        err += "helios-shaderc: " + compiler.error().message + "\n";
        return 1;
    }
    CompileRequest request;
    request.source = *o.input;
    request.includeDirs = o.includes;
    request.defines = o.defines;
    request.entryPoints = o.entries;
    request.debugInfo = o.debug;
    auto compiled = compiler.value()->compile(request);
    if (!compiled) {
        err += compiled.error().message;
        if (!err.empty() && err.back() != '\n') err += '\n';
        err += std::format("helios-shaderc: {} failed to compile\n", fs::pathToUtf8(*o.input));
        return 1;
    }
    if (!compiled->diagnostics.empty() && !o.quiet) err += compiled->diagnostics;

    auto reflection = render::reflectSpirv(compiled->spirv);
    if (!reflection) {
        err += "helios-shaderc: cannot reflect the generated SPIR-V: " + reflection.error().toString() + "\n";
        return 1;
    }
    if (reflection->pushConstants.size > 128) {
        err += std::format("helios-shaderc: {}: push constants are {} bytes; Helios pipelines allow 128 (03 §1.1)\n",
                           fs::pathToUtf8(*o.input), reflection->pushConstants.size);
        return 1;
    }
    const std::span<const u8> spirvBytes(reinterpret_cast<const u8*>(compiled->spirv.data()), compiled->spirv.size() * 4);
    if (auto w = fs::writeFile(*o.output, spirvBytes); !w) {
        err += std::format("helios-shaderc: cannot write {}: {}\n", fs::pathToUtf8(*o.output), w.error().toString());
        return 1;
    }
    if (!validate(o, *o.output, spirvBytes, out, err)) {
        std::error_code ec;
        std::filesystem::remove(*o.output, ec);  // never leave an invalid module that looks up to date
        return 1;
    }
    if (!writeReflection(o, reflection.value(), withExtension(*o.output, ".hsr"), err)) return 1;
    if (o.depfile) {
        if (auto w = fs::writeTextFile(*o.depfile, makeDepfile(*o.output, compiled->dependencies)); !w) {
            err += std::format("helios-shaderc: cannot write {}: {}\n", fs::pathToUtf8(*o.depfile), w.error().toString());
            return 1;
        }
    }
    if (!o.quiet) {
        std::string names;
        for (const render::ShaderEntryPoint& e : reflection->entryPoints) {
            names += std::format("{}{} ({})", names.empty() ? "" : ", ", e.name, render::shaderStageName(e.stage));
        }
        out += std::format("helios-shaderc: {} -> {} ({} words; {})\n", fs::pathToUtf8(*o.input),
                           fs::pathToUtf8(*o.output), compiled->spirv.size(), names);
    }
    return 0;
}

} // namespace helios::shaderc
