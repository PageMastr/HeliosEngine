// Importer registry (see importer.h).

#include "helios/assetpipe/importer.h"

#include <format>

namespace helios::assetpipe {

namespace {

char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool idChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

bool validId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64 || id.front() == '.' || id.front() == '_' || id.front() == '-') return false;
    for (const char c : id) {
        if (!idChar(c)) return false;
    }
    return true;
}

/// ".png": a dot, then 1-16 of [a-z0-9].
bool validExtension(std::string_view ext) noexcept {
    if (ext.size() < 2 || ext.size() > 17 || ext.front() != '.') return false;
    for (const char c : ext.substr(1)) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) return false;
    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (usize i = 0; i < suffix.size(); ++i) {
        if (lowerAscii(tail[i]) != suffix[i]) return false;
    }
    return true;
}

} // namespace

bool importsFile(const ImporterInfo& importer, std::string_view fileName) noexcept {
    // The extension is the text after the last '.', so "a.tar.png" is a ".png" and ".png" alone (a
    // dotfile with no stem) is nothing.
    const usize slash = fileName.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? fileName : fileName.substr(slash + 1);
    const usize dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return false;
    const std::string_view ext = name.substr(dot);
    for (const std::string& e : importer.extensions) {
        if (ext.size() == e.size() && endsWithIgnoringCase(ext, e)) return true;
    }
    return false;
}

Result<void> ImporterRegistry::add(ImporterInfo info) {
    if (!validId(info.id)) {
        return makeError(ErrorCode::InvalidArgument,
                         "importer id '{}' must be 1-64 characters of [a-z0-9._-] starting with a letter or digit",
                         info.id);
    }
    if (info.version == 0) return makeError(ErrorCode::InvalidArgument, "importer '{}': version 0 (versions start at 1)", info.id);
    if (info.settings && (info.settings->kind != refl::Kind::Struct || !info.settings->ops ||
                          !info.settings->ops->construct || !info.settings->ops->destruct)) {
        return makeError(ErrorCode::InvalidArgument, "importer '{}': settings type '{}' is not a struct", info.id,
                         info.settings->qualifiedName);
    }
    if (find(info.id)) return makeError(ErrorCode::AlreadyExists, "importer '{}' is already registered", info.id);
    for (const std::string& ext : info.extensions) {
        if (!validExtension(ext)) {
            return makeError(ErrorCode::InvalidArgument,
                             "importer '{}': extension '{}' must be a dot and 1-16 lower-case letters or digits", info.id,
                             ext);
        }
        for (const auto& other : m_importers) {
            for (const std::string& taken : other->extensions) {
                if (taken == ext) {
                    return makeError(ErrorCode::AlreadyExists, "importer '{}': extension '{}' is already imported by '{}'",
                                     info.id, ext, other->id);
                }
            }
        }
    }
    m_importers.push_back(std::make_unique<ImporterInfo>(std::move(info)));
    return {};
}

const ImporterInfo* ImporterRegistry::find(std::string_view id) const noexcept {
    for (const auto& i : m_importers) {
        if (i->id == id) return i.get();
    }
    return nullptr;
}

const ImporterInfo* ImporterRegistry::forFile(std::string_view fileName) const noexcept {
    for (const auto& i : m_importers) {
        if (importsFile(*i, fileName)) return i.get();
    }
    return nullptr;
}

} // namespace helios::assetpipe
