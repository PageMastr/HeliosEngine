#include "diagnostics.h"

#include <algorithm>
#include <format>

namespace helios::schemac {

u32 DiagnosticEngine::addFile(std::string displayPath, std::string text) {
    m_files.push_back(File{std::move(displayPath), std::move(text)});
    return static_cast<u32>(m_files.size() - 1);
}

const std::string& DiagnosticEngine::filePath(u32 file) const { return m_files.at(file).path; }
const std::string& DiagnosticEngine::fileText(u32 file) const { return m_files.at(file).text; }

Diagnostic& DiagnosticEngine::error(SourceLoc loc, std::string message) {
    ++m_errors;
    m_diags.push_back(Diagnostic{Severity::Error, loc, std::move(message), {}});
    return m_diags.back();
}

Diagnostic& DiagnosticEngine::warning(SourceLoc loc, std::string message) {
    if (m_werror) return error(loc, std::move(message));
    ++m_warnings;
    m_diags.push_back(Diagnostic{Severity::Warning, loc, std::move(message), {}});
    return m_diags.back();
}

void DiagnosticEngine::note(SourceLoc loc, std::string message) {
    if (m_diags.empty()) return;
    m_diags.back().notes.push_back(Diagnostic{Severity::Note, loc, std::move(message), {}});
}

namespace {
std::string_view severityName(Severity s) {
    switch (s) {
    case Severity::Note: return "note";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
    }
    return "error";
}
} // namespace

std::string DiagnosticEngine::format(const Diagnostic& d) const {
    std::string out;
    if (d.loc.file != 0 && d.loc.file < m_files.size()) {
        out += m_files[d.loc.file].path;
        if (d.loc.valid()) out += std::format(":{}:{}", d.loc.line, d.loc.col);
        out += ": ";
    }
    out += severityName(d.severity);
    out += ": ";
    out += d.message;
    return out;
}

std::string DiagnosticEngine::formatAll(bool withExcerpts) const {
    std::string out;
    auto excerpt = [&](const Diagnostic& d) {
        if (!withExcerpts || d.loc.file == 0 || d.loc.file >= m_files.size() || !d.loc.valid()) return;
        const std::string& text = m_files[d.loc.file].text;
        usize start = 0;
        for (u32 line = 1; line < d.loc.line && start < text.size(); ++line) {
            const usize nl = text.find('\n', start);
            if (nl == std::string::npos) return;
            start = nl + 1;
        }
        usize end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string lineText = text.substr(start, end - start);
        if (!lineText.empty() && lineText.back() == '\r') lineText.pop_back();
        // Long (e.g. generated or minified) lines: show a window around the column.
        constexpr usize kWindow = 120;
        usize col = d.loc.col;
        if (lineText.size() > kWindow) {
            const usize at = col == 0 ? 0 : col - 1;
            usize first = at > kWindow / 2 ? at - kWindow / 2 : 0;
            first = std::min(first, lineText.size() - kWindow);
            const bool cutLeft = first > 0;
            const bool cutRight = first + kWindow < lineText.size();
            lineText = (cutLeft ? "..." : "") + lineText.substr(first, kWindow) + (cutRight ? "..." : "");
            col = col - first + (cutLeft ? 3 : 0);
        }
        out += "    " + lineText + "\n    ";
        for (usize c = 1; c < col && c - 1 < lineText.size(); ++c) out += lineText[c - 1] == '\t' ? '\t' : ' ';
        out += "^\n";
    };
    for (const Diagnostic& d : m_diags) {
        out += format(d);
        out += '\n';
        excerpt(d);
        for (const Diagnostic& n : d.notes) {
            out += format(n);
            out += '\n';
            excerpt(n);
        }
    }
    return out;
}

} // namespace helios::schemac
