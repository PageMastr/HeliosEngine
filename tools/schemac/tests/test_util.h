#pragma once
// Helpers for compiling in-memory schemas in tests.

#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

#include "compiler.h"
#include "diagnostics.h"
#include "model.h"

namespace schemac_test {

using namespace helios;
using namespace helios::schemac;

struct Compiled {
    CompileResult result;
    DiagnosticEngine diags;
    std::string messages; ///< "file:line:col: severity: message" per line (no excerpts)

    bool ok() const { return result.ok; }
    const Schema& schema() const { return *result.schema; }
    const Decl* decl(const std::string& qualified) const {
        auto it = result.schema->declsByName.find(qualified);
        return it == result.schema->declsByName.end() ? nullptr : it->second;
    }
    const std::string* output(const std::string& suffix) const {
        for (const OutputFile& o : result.outputs) {
            if (o.path.ends_with(suffix)) return &o.content;
        }
        return nullptr;
    }
};

inline std::string formatDiags(const DiagnosticEngine& diags) {
    std::string out;
    for (const Diagnostic& d : diags.diagnostics()) {
        out += diags.format(d) + "\n";
        for (const Diagnostic& n : d.notes) out += diags.format(n) + "\n";
    }
    return out;
}

/// Compiles `files` (path -> text); the first file (or `mainFiles`) is generated.
inline std::unique_ptr<Compiled> compileFiles(const std::map<std::string, std::string>& files, CompileOptions options = {},
                                              MemoryFileSystem* fsOverride = nullptr) {
    auto c = std::make_unique<Compiled>();
    MemoryFileSystem local;
    MemoryFileSystem& fs = fsOverride ? *fsOverride : local;
    for (const auto& [path, text] : files) fs.files[normalizePath(path)] = text;
    if (options.files.empty()) options.files.push_back(files.begin()->first);
    if (options.includeDirs.empty()) options.includeDirs.push_back("schemas");
    if (options.lockPath.empty()) options.warnWithoutLock = false;
    c->result = compile(options, fs, c->diags);
    c->messages = formatDiags(c->diags);
    return c;
}

/// Compiles one schema file "schemas/test/t.hschema" with the given text.
inline std::unique_ptr<Compiled> compileText(const std::string& text, CompileOptions options = {}) {
    return compileFiles({{"schemas/test/t.hschema", text}}, std::move(options));
}

/// Compiles `body` inside "package test;" and returns the diagnostics text.
inline std::string diagnosticsOf(const std::string& body, CompileOptions options = {}) {
    options.namingLints = false;
    return compileText("package test;\n" + body, std::move(options))->messages;
}

} // namespace schemac_test
