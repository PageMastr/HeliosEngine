#pragma once
// helios-schemac as a library: load sources (resolving imports), parse, analyze, apply the lock
// and generate outputs in memory. The command line (cli.cpp) and the tests both use it.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "diagnostics.h"
#include "model.h"

namespace helios::schemac {

/// File access used by the compiler (real file system or an in-memory map for tests).
class SourceProvider {
public:
    virtual ~SourceProvider() = default;
    virtual std::optional<std::string> read(const std::string& path) = 0;
};

class RealFileSystem final : public SourceProvider {
public:
    std::optional<std::string> read(const std::string& path) override;
};

class MemoryFileSystem final : public SourceProvider {
public:
    std::map<std::string, std::string> files;
    std::optional<std::string> read(const std::string& path) override;
};

struct CompileOptions {
    std::vector<std::string> files;       ///< schemas to generate code for
    std::vector<std::string> includeDirs; ///< import search roots (also define logical paths)
    std::string lockPath;                 ///< empty: ephemeral ids (warning)
    bool checkLock = false;               ///< fail if the lock would change
    bool allowDefaultChange = false;
    bool emitCpp = false;
    bool emitGo = false;
    bool emitJson = false;
    bool samples = false; ///< C++ sample-value header (tests / cross-language vectors)
    std::string cppOut = ".";
    std::string goOut = ".";
    std::string goPackage; ///< default: last component of the first file's package
    std::string jsonOut = "schema.json";
    bool warningsAsErrors = false;
    bool namingLints = true;
    bool warnWithoutLock = true; ///< warn that ids are not stable when lockPath is empty
};

struct OutputFile {
    std::string path;
    std::string content;
};

struct CompileResult {
    bool ok = false;
    std::vector<OutputFile> outputs;
    bool lockChanged = false;
    std::string lockText;                 ///< new lock content (when lockPath is set)
    std::vector<std::string> lockChanges; ///< human-readable lock changes
    std::vector<std::string> inputs;      ///< every file read (schemas, imports, lock) — for depfiles
    std::unique_ptr<Schema> schema;
};

CompileResult compile(const CompileOptions& options, SourceProvider& fs, DiagnosticEngine& diags);

/// Normalized generic path ("a/b/../c" -> "a/c").
std::string normalizePath(const std::string& path);

/// Output paths produced for `options` (used by the CLI to list outputs; mirrors compile()).
std::string cppHeaderPath(const std::string& logicalPath);
std::string cppSourcePath(const std::string& logicalPath);

} // namespace helios::schemac
