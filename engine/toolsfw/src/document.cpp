#include "helios/toolsfw/document.h"

#include <algorithm>
#include <string>

#include "helios/core/hash.h"
#include "helios/reflect/type_info.h"

#include "doc_access.h"

namespace helios::tf {

std::string_view docKindName(DocKind kind) noexcept {
    switch (kind) {
    case DocKind::Record: return "record";
    }
    return "unknown";
}

Document::Document(DocId id, const refl::TypeInfo& type, refl::RecordHeader header, fs::Path path)
    : m_id(id), m_type(&type), m_value(type), m_header(std::move(header)), m_path(std::move(path)) {}

Document::~Document() = default;

void DocAccess::setBase(Document& d, std::string text) {
    d.m_savedHash = hash64(text);
    d.m_baseText = std::move(text);
}

void Document::touch() noexcept {
    ++m_revision;
}

const std::string& Document::text() const {
    if (m_cacheRevision != m_revision) {
        m_textCache = recordText(*m_type, m_value.data(), m_header);
        m_hashCache = hash64(m_textCache);
        m_cacheRevision = m_revision;
    }
    return m_textCache;
}

u64 Document::contentHash() const {
    (void)text();
    return m_hashCache;
}

std::string recordText(const refl::TypeInfo& type, const void* object, const refl::RecordHeader& header) {
    return refl::writeRecord(type, object, header);
}

const refl::TypeInfo* recordTypeForPath(const refl::TypeRegistry& types, std::string_view relativePath) {
    // "records/<table>/..." -> <table>. Accept a path that starts at the table directory too.
    std::string_view rest = relativePath;
    if (rest.starts_with("records/")) rest.remove_prefix(8);
    const usize slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) return nullptr;
    const std::string_view table = rest.substr(0, slash);
    for (const refl::TypeInfo* t : types.types()) {
        if (t->decl != refl::DeclKind::Record) continue;
        if (const auto* tbl = t->attr<refl::attrs::Table>(); tbl && tbl->name == table) return t;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Workspace
// ---------------------------------------------------------------------------------------------
Workspace::Workspace(const refl::TypeRegistry& types) : m_types(&types) {}
Workspace::~Workspace() = default;

void Workspace::setRoot(fs::Path root) {
    std::error_code ec;
    fs::Path abs = std::filesystem::absolute(root, ec);
    m_root = (ec ? root : abs).lexically_normal();
}

Document* Workspace::find(const DocId& id) const noexcept {
    for (Document* d : m_order) {
        if (d->id() == id) return d;
    }
    return nullptr;
}

Document* Workspace::find(std::string_view key) const {
    if (key.empty()) return nullptr;
    for (Document* d : m_order) {
        if (!d->destroyed() && (d->name() == key || d->relativePath() == key)) return d;
    }
    if (auto g = Guid::parse(key)) {
        if (Document* d = find(*g)) return d;
    }
    return findByPath(fs::pathFromUtf8(key));
}

Document* Workspace::findByPath(const fs::Path& path) const {
    if (path.empty()) return nullptr;
    fs::Path abs = path.is_absolute() ? path : m_root / path;
    abs = abs.lexically_normal();
    for (Document* d : m_order) {
        if (!d->destroyed() && !d->path().empty() && d->path() == abs) return d;
    }
    return nullptr;
}

std::string Workspace::relativeTo(const fs::Path& path) const {
    if (m_root.empty()) return fs::pathToGenericUtf8(path);
    const fs::Path abs = (path.is_absolute() ? path : m_root / path).lexically_normal();
    const fs::Path rel = abs.lexically_relative(m_root);
    if (rel.empty()) return {};
    const std::string text = fs::pathToGenericUtf8(rel);
    if (text == ".." || text.starts_with("../")) return {};
    return text;
}

fs::Path Workspace::absolute(std::string_view relative) const {
    fs::Path p = fs::pathFromUtf8(relative);
    if (p.is_absolute()) return p.lexically_normal();
    return (m_root / p).lexically_normal();
}

} // namespace helios::tf
