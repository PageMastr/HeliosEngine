#pragma once
// Documents (07 §1.2): an editable unit with a GUID, a source file, a revision and a dirty flag,
// holding one reflected object. Phase 0 has record documents (`.hrec`, canonical JSONC, 02 §3.3);
// worlds, graphs, `.meta` and the other kinds reuse the same shape later.
//
// Documents are read through this class and mutated only by transactions (TxBuilder / Framework):
// there is no public mutable access to the object, so every change reaches undo, the journal and
// the remote-control clients (07 §0 rule 3).
//
// Threading: documents belong to the Framework's owner thread (see framework.h).

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/result.h"
#include "helios/reflect/record.h"
#include "helios/reflect/registry.h"
#include "helios/reflect/serialize.h"
#include "helios/toolsfw/types.h"

namespace helios::tf {

enum class DocKind : u8 { Record };

std::string_view docKindName(DocKind kind) noexcept;

class Document {
public:
    Document(DocId id, const refl::TypeInfo& type, refl::RecordHeader header, fs::Path path);
    ~Document();
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    DocId id() const noexcept { return m_id; }
    DocKind kind() const noexcept { return DocKind::Record; }
    const refl::TypeInfo& type() const noexcept { return *m_type; }
    /// The reflected object (read-only; edit through transactions).
    const void* object() const noexcept { return m_value.data(); }
    const refl::RecordHeader& header() const noexcept { return m_header; }
    /// Absolute source file ('.hrec'); empty for a document that was never saved.
    const fs::Path& path() const noexcept { return m_path; }
    /// Project-relative, '/'-separated source path ("records/hull/frigate.hrec"), or "" if unsaved.
    const std::string& relativePath() const noexcept { return m_relativePath; }
    /// Display and lookup name: the record's `$name` ("hull/frigate").
    const std::string& name() const noexcept { return m_header.name; }

    /// Incremented by every applied transaction (including undo and redo) that touches it.
    u64 revision() const noexcept { return m_revision; }
    /// Canonical JSONC of the whole record file (header + fields), cached per revision.
    const std::string& text() const;
    /// XXH3-64 of text().
    u64 contentHash() const;
    /// Hash of the canonical text last loaded from or saved to disk (0 when the file does not
    /// exist: a never-saved document, or a destroyed one whose file was removed).
    u64 savedHash() const noexcept { return m_savedHash; }
    /// Canonical text last loaded from or saved to disk (the base of three-way merges).
    const std::string& baseText() const noexcept { return m_baseText; }
    /// True when the content differs from the last load or save (undoing back to it clears it).
    /// A destroyed document is dirty while its file still exists.
    bool dirty() const { return m_destroyed ? m_savedHash != 0 : contentHash() != m_savedHash; }
    /// True when the document is scheduled for removal by a committed Destroy op.
    bool destroyed() const noexcept { return m_destroyed; }

private:
    friend struct DocAccess;
    void touch() noexcept;

    DocId m_id;
    const refl::TypeInfo* m_type;
    refl::Value m_value;
    refl::RecordHeader m_header;
    fs::Path m_path;
    std::string m_relativePath;
    u64 m_revision = 0;
    u64 m_savedHash = 0;
    std::string m_baseText;
    bool m_destroyed = false;
    mutable std::string m_textCache;
    mutable u64 m_hashCache = 0;
    mutable u64 m_cacheRevision = ~0ull;
};

/// Resolves a record file's type: an explicit type name, else the type whose `@table("<t>")` matches
/// the first directory below `records/` ("records/hull/frigate.hrec" -> `@table("hull")`). On
/// Windows the comparison ignores ASCII case, like the file system.
const refl::TypeInfo* recordTypeForPath(const refl::TypeRegistry& types, std::string_view relativePath);

/// Canonical record text of a type-erased object (refl::writeRecord).
std::string recordText(const refl::TypeInfo& type, const void* object, const refl::RecordHeader& header);

/// The documents of one project. Owned by the Framework; exposed read-only.
class Workspace {
public:
    explicit Workspace(const refl::TypeRegistry& types);
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    const refl::TypeRegistry& types() const noexcept { return *m_types; }
    /// Project root; relative paths resolve against it.
    const fs::Path& root() const noexcept { return m_root; }
    void setRoot(fs::Path root);

    /// Documents in open order.
    std::span<Document* const> documents() const noexcept { return m_order; }
    usize size() const noexcept { return m_order.size(); }
    /// By id, including destroyed documents (whose Destroy op may still be undone).
    Document* find(const DocId& id) const noexcept;
    /// A live document by `$name` ("hull/frigate"), GUID text, or project-relative / absolute path.
    Document* find(std::string_view nameOrIdOrPath) const;
    /// The live document of a file. On Windows paths compare without ASCII case, as NTFS does, so
    /// a differently cased path finds the open document instead of opening the file twice.
    Document* findByPath(const fs::Path& path) const;

    /// Project-relative, '/'-separated path of `path` ("" when it is outside the root).
    std::string relativeTo(const fs::Path& path) const;
    /// Absolute path of a project-relative one.
    fs::Path absolute(std::string_view relative) const;

private:
    friend struct DocAccess;
    const refl::TypeRegistry* m_types;
    fs::Path m_root;
    std::vector<std::unique_ptr<Document>> m_docs;
    std::vector<Document*> m_order;
};

} // namespace helios::tf
