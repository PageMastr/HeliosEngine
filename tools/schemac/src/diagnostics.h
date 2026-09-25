#pragma once
// Diagnostics for helios-schemac: "file:line:col: error: message" with optional notes.
//
// Threading: a DiagnosticEngine belongs to one compilation (single-threaded).

#include <string>
#include <string_view>
#include <vector>

#include "helios/core/types.h"

namespace helios::schemac {

struct SourceLoc {
    u32 file = 0; ///< Index into DiagnosticEngine::files() (0 = no file).
    u32 line = 0; ///< 1-based; 0 = unknown.
    u32 col = 0;  ///< 1-based byte column.
    bool valid() const noexcept { return line != 0; }
};

enum class Severity : u8 { Note, Warning, Error };

struct Diagnostic {
    Severity severity = Severity::Error;
    SourceLoc loc;
    std::string message;
    std::vector<Diagnostic> notes;
};

class DiagnosticEngine {
public:
    /// Registers a source file for location printing; returns its index (>= 1).
    u32 addFile(std::string displayPath, std::string text);
    const std::string& filePath(u32 file) const;
    const std::string& fileText(u32 file) const;

    Diagnostic& error(SourceLoc loc, std::string message);
    Diagnostic& warning(SourceLoc loc, std::string message);
    /// Adds a note to the most recent diagnostic.
    void note(SourceLoc loc, std::string message);

    bool hasErrors() const noexcept { return m_errors != 0; }
    usize errorCount() const noexcept { return m_errors; }
    usize warningCount() const noexcept { return m_warnings; }
    const std::vector<Diagnostic>& diagnostics() const noexcept { return m_diags; }
    /// Treat warnings as errors from now on.
    void setWarningsAsErrors(bool on) noexcept { m_werror = on; }

    /// "path:line:col: error: message" (no source excerpt).
    std::string format(const Diagnostic& d) const;
    /// All diagnostics with source excerpts and carets (for the command line).
    std::string formatAll(bool withExcerpts = true) const;

private:
    struct File {
        std::string path;
        std::string text;
    };
    std::vector<File> m_files{File{}}; // index 0 = no file
    std::vector<Diagnostic> m_diags;
    usize m_errors = 0;
    usize m_warnings = 0;
    bool m_werror = false;
};

} // namespace helios::schemac
