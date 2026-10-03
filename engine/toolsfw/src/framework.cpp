#include "helios/toolsfw/framework.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>

#include "helios/core/hash.h"
#include "helios/reflect/patch.h"
#include "helios/reflect/path.h"
#include "helios/toolsfw/json_util.h"
#include "helios/toolsfw/validate.h"

#include "doc_access.h"
#include "framework_internal.h"
#include "ops.h"
#include "platform/tf_os.h"

namespace helios::tf {

namespace {

bool isHeaderPath(std::string_view path) noexcept {
    return !path.empty() && path.front() == '$';
}

std::string* headerField(refl::RecordHeader& h, std::string_view path) noexcept {
    if (path == "$name") return &h.name;
    if (path == "$parent") return &h.parent;
    if (path == "$comment") return &h.comment;
    return nullptr;
}

Result<std::string> headerValue(const Document& d, std::string_view path) {
    auto& h = const_cast<refl::RecordHeader&>(d.header());
    const std::string* field = headerField(h, path);
    if (!field) return Error{ErrorCode::InvalidArgument, std::format("'{}': only $name, $parent and $comment can be edited", path)};
    return json::quote(*field);
}

Result<void> applyHeaderSet(Document& d, const Op& op) {
    HELIOS_TRY_ASSIGN(const std::string current, headerValue(d, op.path));
    if (!op.before || *op.before != current) {
        return Error{ErrorCode::InvalidState, std::format("conflict at '{}': expected {}, found {}", op.path,
                                                          op.before.value_or("<absent>"), current)};
    }
    if (!op.after) return Error{ErrorCode::InvalidArgument, std::format("'{}' cannot be removed", op.path)};
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(*op.after, op.path));
    if (!doc.root().isString()) return Error{ErrorCode::InvalidArgument, std::format("'{}' must be a string", op.path)};
    const std::string value(doc.root().asString());
    if (op.path == "$name" && value.empty()) return Error{ErrorCode::InvalidArgument, "$name must not be empty"};
    if (json::quote(value) != *op.after) return Error{ErrorCode::InvalidArgument, std::format("'{}': value is not canonical", op.path)};
    *headerField(DocAccess::header(d), op.path) = value;
    return {};
}

/// The element a list path segment names: (list path, index) for `list[#key]` / `list[i]`.
struct ElementRef {
    std::string listPath;
    detail::ListRef list;
    usize index = 0;
};

Result<std::optional<ElementRef>> resolveElement(const refl::TypeInfo& root, void* object, std::string_view path) {
    HELIOS_TRY_ASSIGN(const refl::PropertyPath p, refl::PropertyPath::parse(path));
    if (p.empty()) return std::optional<ElementRef>();
    const auto kind = p.back().kind;
    if (kind != refl::PathSegment::Kind::Keyed && kind != refl::PathSegment::Kind::Index) return std::optional<ElementRef>();
    ElementRef e;
    e.listPath = p.parent().toString();
    auto list = detail::resolveList(root, object, e.listPath);
    if (!list) {
        // An index into a map, or a keyed segment of something else: not a list element.
        if (list.errorCode() == ErrorCode::InvalidArgument) return std::optional<ElementRef>();
        return std::move(list).error();
    }
    e.list = *list;
    HELIOS_TRY_ASSIGN(const refl::Ref elem, refl::resolve(root, object, p));
    const usize n = e.list.type->ops->size(e.list.ptr);
    for (usize i = 0; i < n; ++i) {
        if (e.list.type->ops->element(e.list.ptr, i) == elem.ptr) {
            e.index = i;
            return std::optional<ElementRef>(std::move(e));
        }
    }
    return Error{ErrorCode::NotFound, std::format("'{}': element not found", path)};
}

std::string sanitizeLabel(std::string label, std::string_view fallback) {
    if (label.empty()) label = std::string(fallback);
    return label;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// TxBuilder
// ---------------------------------------------------------------------------------------------
TxBuilder::TxBuilder(Framework& fw, Origin origin, std::string label) noexcept
    : m_fw(&fw), m_origin(origin), m_label(std::move(label)) {}

TxBuilder::~TxBuilder() {
    if (!m_done) abort();
}

Result<void> TxBuilder::push(Op op) {
    if (m_done) return Error{ErrorCode::InvalidState, "transaction already committed or aborted"};
    const Document* d = m_fw->m_workspace->find(op.doc);
    const bool known = std::any_of(m_baseRev.begin(), m_baseRev.end(), [&](const auto& p) { return p.first == op.doc; });
    const u64 rev = d ? d->revision() : 0;
    HELIOS_TRY(m_fw->applyOp(op));
    if (!known) m_baseRev.emplace_back(op.doc, rev);
    m_ops.push_back(std::move(op));
    return {};
}

namespace {
Document* liveDocument(const Workspace& ws, const DocId& id) {
    Document* d = ws.find(id);
    return d && !d->destroyed() ? d : nullptr;
}
Error noDocument(const DocId& id) {
    return Error{ErrorCode::NotFound, std::format("no open document {}", id)};
}
} // namespace

Result<void> TxBuilder::set(const DocId& doc, std::string_view path, std::string_view json) {
    Document* d = liveDocument(*m_fw->m_workspace, doc);
    if (!d) return noDocument(doc);
    if (isHeaderPath(path)) {
        HELIOS_TRY_ASSIGN(const std::string before, headerValue(*d, path));
        HELIOS_TRY_ASSIGN(const std::string after, json::normalize(json));
        if (before == after) return {};
        Op op;
        op.kind = OpKind::Set;
        op.doc = doc;
        op.path = std::string(path);
        op.before = before;
        op.after = after;
        return push(std::move(op));
    }
    refl::Value target(d->type());
    d->type().ops->copy(target.data(), d->object());
    HELIOS_TRY(refl::setJson(d->type(), target.data(), path, json));
    return detail::FwAccess::applyDiff(*this, *d, target.data());
}

Result<void> TxBuilder::remove(const DocId& doc, std::string_view path) {
    Document* d = liveDocument(*m_fw->m_workspace, doc);
    if (!d) return noDocument(doc);
    if (isHeaderPath(path)) return Error{ErrorCode::InvalidArgument, std::format("'{}' cannot be removed", path)};
    HELIOS_TRY_ASSIGN(const auto element, resolveElement(d->type(), DocAccess::object(*d), path));
    if (element) {
        Op op;
        op.kind = OpKind::Remove;
        op.doc = doc;
        op.path = element->listPath;
        op.index = element->index;
        op.key = detail::elementKey(element->list, element->index);
        op.before = detail::elementJson(element->list, element->index);
        return push(std::move(op));
    }
    refl::Value target(d->type());
    d->type().ops->copy(target.data(), d->object());
    HELIOS_TRY(refl::apply(d->type(), target.data(), refl::PatchOp{refl::PatchOp::Kind::Remove, std::string(path), {}}));
    return detail::FwAccess::applyDiff(*this, *d, target.data());
}

Result<void> TxBuilder::insert(const DocId& doc, std::string_view listPath, u64 index, std::string_view elementJson,
                               std::optional<Guid> key) {
    Document* d = liveDocument(*m_fw->m_workspace, doc);
    if (!d) return noDocument(doc);
    HELIOS_TRY_ASSIGN(const detail::ListRef list, detail::resolveList(d->type(), DocAccess::object(*d), listPath));
    const usize n = list.type->ops->size(list.ptr);
    if (index > n) return Error{ErrorCode::OutOfRange, std::format("'{}': insert index {} past the end ({})", listPath, index, n)};
    const refl::TypeInfo& e = list.type->element();
    refl::Value tmp(e);
    if (!elementJson.empty()) {
        HELIOS_TRY_ASSIGN(const refl::JsonDocument jdoc, refl::JsonDocument::parse(elementJson, listPath));
        refl::ReadCtx ctx;
        HELIOS_TRY(refl::readJson(e, tmp.data(), jdoc.root(), ctx));
    }
    Op op;
    op.kind = OpKind::Insert;
    op.doc = doc;
    op.path = std::string(listPath);
    op.index = index;
    if (list.type->kind == refl::Kind::KeyedList) {
        const Guid k = key ? *key : m_fw->newKey();
        if (k.isNil()) return Error{ErrorCode::InvalidArgument, "keyed-list elements need a non-nil key"};
        op.key = refl::keyedKeyText(k);
    } else if (!list.keyField.empty()) {
        const refl::FieldInfo* kf = e.field(list.keyField);
        if (!kf || !kf->type().ops->keyToText) return Error{ErrorCode::InvalidArgument, "unsupported @keyed field"};
        op.key = kf->type().ops->keyToText(kf->ptr(tmp.data()));
    }
    op.after = refl::toJson(e, tmp.data(), refl::JsonStyle::Compact);
    return push(std::move(op));
}

Result<void> TxBuilder::move(const DocId& doc, std::string_view elementPath, u64 toIndex) {
    Document* d = liveDocument(*m_fw->m_workspace, doc);
    if (!d) return noDocument(doc);
    HELIOS_TRY_ASSIGN(const auto element, resolveElement(d->type(), DocAccess::object(*d), elementPath));
    if (!element) return Error{ErrorCode::InvalidArgument, std::format("'{}' is not a list element", elementPath)};
    const usize n = element->list.type->ops->size(element->list.ptr);
    if (toIndex >= n) return Error{ErrorCode::OutOfRange, std::format("'{}': move target {} out of range ({})", elementPath, toIndex, n)};
    if (toIndex == element->index) return {};
    Op op;
    op.kind = OpKind::Move;
    op.doc = doc;
    op.path = element->listPath;
    op.index = element->index;
    op.toIndex = toIndex;
    op.key = detail::elementKey(element->list, element->index);
    return push(std::move(op));
}

Result<void> TxBuilder::edit(const DocId& doc, const std::function<void(void* object)>& mutate) {
    Document* d = liveDocument(*m_fw->m_workspace, doc);
    if (!d) return noDocument(doc);
    refl::Value target(d->type());
    d->type().ops->copy(target.data(), d->object());
    mutate(target.data());
    return detail::FwAccess::applyDiff(*this, *d, target.data());
}

Result<DocId> TxBuilder::createRecord(const refl::TypeInfo& type, std::string_view file, const refl::RecordHeader& header,
                                      std::string_view json) {
    if (type.decl != refl::DeclKind::Record) {
        return Error{ErrorCode::InvalidArgument, std::format("{} is not a record type", type.qualifiedName)};
    }
    const Workspace& ws = *m_fw->m_workspace;
    std::string rel = ws.relativeTo(ws.absolute(file));
    if (rel.empty()) return Error{ErrorCode::InvalidArgument, std::format("'{}' is outside the project", file)};
    if (!rel.ends_with(".hrec")) return Error{ErrorCode::InvalidArgument, std::format("'{}': record files end in .hrec", file)};
    refl::RecordHeader h = header;
    if (h.rid == 0) h.rid = refl::mintRecordId();
    if (!refl::isValidRecordId(h.rid)) return Error{ErrorCode::InvalidArgument, "invalid $rid"};
    if (h.name.empty()) return Error{ErrorCode::InvalidArgument, "a record needs a $name"};
    refl::Value value(type);
    if (!json.empty()) {
        HELIOS_TRY_ASSIGN(const refl::JsonDocument jdoc, refl::JsonDocument::parse(json, file));
        refl::ReadCtx ctx;
        HELIOS_TRY(refl::readJson(type, value.data(), jdoc.root(), ctx));
    }
    Op op;
    op.kind = OpKind::Create;
    op.doc = m_fw->newKey();
    op.typeName = std::string(type.qualifiedName);
    op.file = std::move(rel);
    op.after = recordText(type, value.data(), h);
    const DocId id = op.doc;
    HELIOS_TRY(push(std::move(op)));
    return id;
}

Result<void> TxBuilder::destroy(const DocId& doc) {
    Document* d = liveDocument(*m_fw->m_workspace, doc);
    if (!d) return noDocument(doc);
    Op op;
    op.kind = OpKind::Destroy;
    op.doc = doc;
    op.typeName = std::string(d->type().qualifiedName);
    op.file = d->relativePath();
    op.before = d->text();
    return push(std::move(op));
}

Result<void> TxBuilder::apply(const Op& op) {
    return push(op);
}

Result<TxId> TxBuilder::commit() {
    return m_fw->commit(*this);
}

void TxBuilder::abort() {
    if (m_done) return;
    m_fw->rollback(m_ops);
    m_ops.clear();
    m_baseRev.clear();
    m_done = true;
}

namespace detail {

Result<void> FwAccess::applyDiff(TxBuilder& tx, Document& d, const void* target) {
    HELIOS_TRY_ASSIGN(std::vector<Op> ops, diffOps(d.type(), d.object(), target, d.id()));
    const usize mark = tx.m_ops.size();
    for (Op& op : ops) {
        if (auto r = tx.push(std::move(op)); !r) {
            truncate(tx, mark);
            return r;
        }
    }
    return {};
}

void FwAccess::truncate(TxBuilder& tx, usize count) {
    if (tx.m_ops.size() <= count) return;
    std::vector<Op> tail(std::make_move_iterator(tx.m_ops.begin() + static_cast<isize>(count)),
                         std::make_move_iterator(tx.m_ops.end()));
    tx.m_ops.resize(count);
    tx.m_fw->rollback(tail);
    // Forget base revisions of documents no remaining op touches.
    std::erase_if(tx.m_baseRev, [&](const auto& p) {
        return std::none_of(tx.m_ops.begin(), tx.m_ops.end(), [&](const Op& o) { return o.doc == p.first; });
    });
}

} // namespace detail

// ---------------------------------------------------------------------------------------------
// Framework
// ---------------------------------------------------------------------------------------------
Framework::Framework(const FrameworkConfig& config)
    : m_config(config),
      m_invokers{CommandInvoker(*this, Origin::Ui),     CommandInvoker(*this, Origin::UiScripted),
                 CommandInvoker(*this, Origin::Luau),   CommandInvoker(*this, Origin::Rpc),
                 CommandInvoker(*this, Origin::Cli),    CommandInvoker(*this, Origin::Import),
                 CommandInvoker(*this, Origin::Collab)} {}

Result<std::unique_ptr<Framework>> Framework::create(const FrameworkConfig& config) {
    std::unique_ptr<Framework> fw(new Framework(config));
    HELIOS_TRY(fw->init());
    return fw;
}

Result<void> Framework::init() {
    const refl::TypeRegistry& types = m_config.types ? *m_config.types : refl::TypeRegistry::global();
    m_workspace = std::make_unique<Workspace>(types);
    if (!m_config.projectRoot.empty()) m_workspace->setRoot(m_config.projectRoot);
    if (m_config.user.empty()) m_config.user = "local";
    if (m_config.journal) {
        const fs::Path root = m_config.journalRoot.empty() ? defaultJournalRoot() : m_config.journalRoot;
        if (m_config.session.empty()) m_config.session = newSessionName();
        JournalHeader header;
        header.project = m_config.project;
        header.session = m_config.session;
        header.user = m_config.user;
        header.host = os::hostName();
        header.pid = os::currentProcessId();
        header.created = now();
        const fs::Path path = journalDirectory(root, m_config.project) / fs::pathFromUtf8(m_config.session + ".hjl");
        HELIOS_TRY_ASSIGN(m_journal, JournalWriter::create(path, header, m_config.journalOptions));
    }
    addPreCommitHook(&validateTransaction);
    if (m_config.builtinCommands) HELIOS_TRY(detail::registerBuiltinCommands(*this));
    return {};
}

Framework::~Framework() {
    if (m_group) cancelGroup();
    if (m_journal) {
        if (auto r = m_journal->close(true); !r) HELIOS_LOG_ERROR(LogTools, "journal close failed: {}", r.error());
    }
}

i64 Framework::now() const {
    if (m_config.clock) return m_config.clock();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

Guid Framework::newKey() {
    return m_config.newKey ? m_config.newKey() : Guid::generate();
}

void Framework::emit(const FrameworkEvent& event) {
    ++m_changes;
    for (const auto& l : m_listeners) l(event);
}

Result<void> Framework::journalRecord(const JournalRecord& record) {
    if (!m_journal) return {};
    return m_journal->append(record);
}

// ---- documents --------------------------------------------------------------------------------
Result<Document*> Framework::open(const fs::Path& file, const refl::TypeInfo* type) {
    const fs::Path abs = file.is_absolute() ? file.lexically_normal() : m_workspace->absolute(fs::pathToUtf8(file));
    if (Document* d = m_workspace->findByPath(abs)) return d;
    return openInternal(abs, type, true);
}

Result<Document*> Framework::openInternal(const fs::Path& abs, const refl::TypeInfo* type, bool journal) {
    return detail::FwAccess::openDocument(*this, abs, type, journal, newKey());
}

namespace detail {

Result<Document*> FwAccess::openDocument(Framework& fw, const fs::Path& abs, const refl::TypeInfo* type, bool journal,
                                         DocId id) {
    Workspace& ws = *fw.m_workspace;
    HELIOS_TRY_ASSIGN(const std::string text, fs::readTextFile(abs));
    const std::string rel = ws.relativeTo(abs);
    const refl::TypeInfo* t = type ? type : recordTypeForPath(ws.types(), rel);
    if (!t) {
        return Error{ErrorCode::NotFound,
                     std::format("{}: no record type (expected records/<table>/... with a matching @table)", fs::pathToGenericUtf8(abs))};
    }
    auto doc = std::make_unique<Document>(id, *t, refl::RecordHeader{}, abs);
    refl::ReadCtx ctx;
    if (auto r = refl::readRecord(*t, DocAccess::object(*doc), text, DocAccess::header(*doc), ctx); !r) {
        return Error{r.error().code, std::format("{}: {}", fs::pathToGenericUtf8(abs), r.error().message)};
    }
    for (const std::string& w : ctx.warnings()) HELIOS_LOG_WARN(LogTools, "{}: {}", rel.empty() ? fs::pathToGenericUtf8(abs) : rel, w);
    DocAccess::setPath(*doc, abs, rel);
    DocAccess::setBase(*doc, std::string(doc->text()));
    Document* raw = DocAccess::add(ws, std::move(doc));
    if (journal) {
        JournalRecord rec;
        rec.kind = JournalRecordKind::Open;
        rec.doc = raw->id();
        rec.file = rel;
        rec.typeName = std::string(t->qualifiedName);
        rec.hash = hash64(text);
        if (auto r = fw.journalRecord(rec); !r) {
            DocAccess::remove(ws, raw->id());
            return Error{r.error().code, "journal: " + r.error().message};
        }
    }
    fw.emit({FrameworkEvent::Kind::Opened, raw->id(), {}});
    return raw;
}

} // namespace detail

Result<usize> Framework::openAll() {
    const fs::Path dir = m_workspace->root() / "records";
    if (!fs::isDirectory(dir)) return usize{0};
    fs::ListOptions opts;
    opts.recursive = true;
    opts.includeDirectories = false;
    opts.extension = ".hrec";
    HELIOS_TRY_ASSIGN(const std::vector<fs::DirEntry> entries, fs::listDirectory(dir, opts));
    usize opened = 0;
    std::string failures;
    for (const fs::DirEntry& e : entries) {
        auto r = open(e.path);
        if (r) {
            ++opened;
        } else {
            if (!failures.empty()) failures += "\n";
            failures += r.error().toString();
        }
    }
    if (!failures.empty()) return Error{ErrorCode::ParseError, failures};
    return opened;
}

Result<void> Framework::save(const DocId& id) {
    Document* d = m_workspace->find(id);
    if (!d) return noDocument(id);
    JournalRecord rec;
    rec.kind = JournalRecordKind::Save;
    rec.doc = id;
    rec.file = d->relativePath();
    if (d->destroyed()) {
        if (!d->path().empty() && fs::exists(d->path())) HELIOS_TRY(fs::remove(d->path()));
        DocAccess::clearBase(*d);
        rec.hash = 0;
    } else {
        if (d->path().empty()) return Error{ErrorCode::InvalidState, std::format("{} has no file", d->name())};
        const std::string text = d->text();
        HELIOS_TRY(fs::createDirectories(d->path().parent_path()));
        HELIOS_TRY(fs::writeTextFile(d->path(), text, fs::WriteMode::Atomic));
        DocAccess::setBase(*d, text);
        rec.hash = hash64(text);
    }
    if (auto r = journalRecord(rec); !r) HELIOS_LOG_ERROR(LogTools, "journal: {}", r.error());
    emit({FrameworkEvent::Kind::Saved, id, {}});
    return {};
}

Result<usize> Framework::saveAll() {
    usize saved = 0;
    std::vector<DocId> ids;
    for (Document* d : m_workspace->documents()) {
        if (d->dirty()) ids.push_back(d->id());
    }
    for (const DocId& id : ids) {
        HELIOS_TRY(save(id));
        ++saved;
    }
    return saved;
}

Result<void> Framework::close(const DocId& id, bool discard) {
    Document* d = m_workspace->find(id);
    if (!d) return noDocument(id);
    if (d->dirty() && !discard) return Error{ErrorCode::InvalidState, std::format("{} has unsaved changes", d->name())};
    if (m_group) {
        const bool touched = std::any_of(m_group->ops().begin(), m_group->ops().end(), [&](const Op& o) { return o.doc == id; });
        if (touched) return Error{ErrorCode::InvalidState, "the document is part of an open transaction group"};
    }
    // History entries that touch the document can no longer be undone.
    std::vector<usize> keep;
    std::vector<HistoryEntry> history;
    std::vector<usize> remap(m_history.size(), ~usize{0});
    for (usize i = 0; i < m_history.size(); ++i) {
        const auto docs = m_history[i].tx.documents();
        if (std::find(docs.begin(), docs.end(), id) != docs.end()) {
            m_historyBytes -= m_history[i].tx.byteSize();
            continue;
        }
        remap[i] = history.size();
        history.push_back(std::move(m_history[i]));
    }
    m_history = std::move(history);
    std::vector<usize> redo;
    for (usize i : m_redoStack) {
        if (remap[i] != ~usize{0}) redo.push_back(remap[i]);
    }
    m_redoStack = std::move(redo);
    m_lastCanMerge = false;
    m_selection.forgetDocument(id);
    DocAccess::remove(*m_workspace, id);
    JournalRecord rec;
    rec.kind = JournalRecordKind::Close;
    rec.doc = id;
    if (auto r = journalRecord(rec); !r) HELIOS_LOG_ERROR(LogTools, "journal: {}", r.error());
    emit({FrameworkEvent::Kind::Closed, id, {}});
    return {};
}

// ---- op application ---------------------------------------------------------------------------
Result<void> Framework::applyOp(const Op& op) {
    Workspace& ws = *m_workspace;
    switch (op.kind) {
    case OpKind::Create: {
        if (!op.after) return Error{ErrorCode::InvalidArgument, "create without a snapshot"};
        const refl::TypeInfo* type = ws.types().find(op.typeName);
        if (!type) return Error{ErrorCode::NotFound, std::format("unknown record type {}", op.typeName)};
        Document* existing = ws.find(op.doc);
        if (existing && !existing->destroyed()) return Error{ErrorCode::InvalidState, std::format("document {} already exists", op.doc)};
        const fs::Path abs = ws.absolute(op.file);
        if (Document* other = ws.findByPath(abs); other && other != existing) {
            return Error{ErrorCode::AlreadyExists, std::format("{} is already open", op.file)};
        }
        if (existing) {
            // Undo of a Destroy: restore the same document object.
            if (existing->type().qualifiedName != op.typeName) return Error{ErrorCode::InvalidState, "create: type mismatch"};
            refl::Value v(*type);
            refl::RecordHeader h;
            refl::ReadCtx ctx;
            HELIOS_TRY(refl::readRecord(*type, v.data(), *op.after, h, ctx));
            type->ops->copy(DocAccess::object(*existing), v.data());
            DocAccess::header(*existing) = h;
            DocAccess::setDestroyed(*existing, false);
            if (existing->text() != *op.after) return Error{ErrorCode::InvalidArgument, "create: snapshot is not canonical"};
            emit({FrameworkEvent::Kind::Created, op.doc, {}});
            return {};
        }
        if (fs::exists(abs)) return Error{ErrorCode::AlreadyExists, std::format("{} already exists on disk", op.file)};
        auto doc = std::make_unique<Document>(op.doc, *type, refl::RecordHeader{}, abs);
        refl::ReadCtx ctx;
        HELIOS_TRY(refl::readRecord(*type, DocAccess::object(*doc), *op.after, DocAccess::header(*doc), ctx));
        DocAccess::setPath(*doc, abs, ws.relativeTo(abs));
        if (doc->text() != *op.after) return Error{ErrorCode::InvalidArgument, "create: snapshot is not canonical"};
        DocAccess::touch(*doc);
        DocAccess::add(ws, std::move(doc));
        emit({FrameworkEvent::Kind::Created, op.doc, {}});
        return {};
    }
    case OpKind::Destroy: {
        Document* d = ws.find(op.doc);
        if (!d || d->destroyed()) return noDocument(op.doc);
        if (!op.before || d->text() != *op.before) return Error{ErrorCode::InvalidState, std::format("conflict: {} changed", d->name())};
        DocAccess::setDestroyed(*d, true);
        m_selection.forgetDocument(op.doc);
        emit({FrameworkEvent::Kind::Destroyed, op.doc, {}});
        return {};
    }
    default: break;
    }
    Document* d = liveDocument(ws, op.doc);
    if (!d) return noDocument(op.doc);
    if (op.kind == OpKind::Set && isHeaderPath(op.path)) {
        HELIOS_TRY(applyHeaderSet(*d, op));
    } else {
        HELIOS_TRY(detail::applyValueOp(d->type(), DocAccess::object(*d), op));
    }
    DocAccess::touch(*d);
    return {};
}

void Framework::rollback(std::vector<Op>& applied) {
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        if (auto r = applyOp(it->inverse()); !r) {
            // Every op was just applied successfully, so its inverse must apply; anything else is a
            // bug that would leave the document half-edited.
            HELIOS_LOG_ERROR(LogTools, "rollback of {} at '{}' failed: {}", opKindName(it->kind), it->path, r.error());
            HELIOS_ASSERT(false, "transaction rollback failed");
        }
    }
    applied.clear();
}

// ---- commit -----------------------------------------------------------------------------------
std::unique_ptr<TxBuilder> Framework::begin(Origin origin, std::string label) {
    return std::unique_ptr<TxBuilder>(new TxBuilder(*this, origin, std::move(label)));
}

Result<TxId> Framework::commit(TxBuilder& b) {
    if (b.m_done) return Error{ErrorCode::InvalidState, "transaction already committed or aborted"};
    if (b.m_ops.empty()) {
        b.m_done = true;
        return TxId{};
    }
    if (m_group && &b != m_group.get()) {
        // Fold into the open group (Editor.transaction, multi-command UI actions).
        for (const auto& [doc, rev] : b.m_baseRev) {
            const bool known = std::any_of(m_group->m_baseRev.begin(), m_group->m_baseRev.end(),
                                           [&](const auto& p) { return p.first == doc; });
            if (!known) m_group->m_baseRev.emplace_back(doc, rev);
        }
        for (Op& op : b.m_ops) m_group->m_ops.push_back(std::move(op));
        b.m_ops.clear();
        b.m_done = true;
        return TxId{};
    }
    Transaction tx;
    tx.kind = b.m_kind;
    tx.target = b.m_target;
    tx.label = sanitizeLabel(b.m_label, "Edit");
    tx.origin = b.m_origin;
    tx.author = m_config.user;
    tx.mergeKey = b.m_mergeKey;
    tx.ops = b.m_ops;
    for (const auto& [doc, rev] : b.m_baseRev) tx.baseRev[doc] = rev;
    if (!b.m_replay) {
        for (const PreCommitHook& hook : m_preHooks) {
            if (auto r = hook(*this, tx); !r) {
                b.abort();
                return std::move(r).error();
            }
        }
    }
    return detail::FwAccess::finishCommit(*this, b, std::move(tx));
}

namespace detail {

Result<TxId> FwAccess::finishCommit(Framework& fw, TxBuilder& b, Transaction tx) {
    tx.time = fw.now();
    tx.id = TxId{fw.m_config.user, fw.m_lamport + 1};
    for (const DocId& doc : tx.documents()) {
        const Document* d = fw.m_workspace->find(doc);
        tx.afterHash[doc] = d && !d->destroyed() ? d->contentHash() : 0;
    }
    // Journal before the commit becomes visible: an edit that is not journaled does not exist.
    if (fw.m_journal) {
        JournalRecord rec;
        rec.kind = JournalRecordKind::Tx;
        rec.tx = tx;
        if (auto r = fw.m_journal->append(rec); !r) {
            b.abort();
            return Error{r.error().code, "journal write failed, edit rolled back: " + r.error().message};
        }
    }
    ++fw.m_lamport;
    b.m_ops.clear();
    b.m_baseRev.clear();
    b.m_done = true;

    // History (merging continuous gestures).
    const bool canMerge = fw.m_lastCanMerge && !tx.mergeKey.empty() && !fw.m_history.empty() &&
                          !fw.m_history.back().undone && fw.m_history.back().tx.mergeKey == tx.mergeKey &&
                          fw.m_history.back().tx.origin == tx.origin && fw.m_history.back().tx.author == tx.author;
    // A new edit invalidates the redo entries of the documents it touches.
    const std::vector<DocId> docs = tx.documents();
    std::vector<usize> dropped;
    for (usize idx : fw.m_redoStack) {
        const auto rdocs = fw.m_history[idx].tx.documents();
        const bool shares = std::any_of(rdocs.begin(), rdocs.end(), [&](const DocId& d) {
            return std::find(docs.begin(), docs.end(), d) != docs.end();
        });
        if (shares) dropped.push_back(idx);
    }
    if (!dropped.empty()) eraseHistoryEntries(fw, dropped);

    // The log records the transaction exactly as journaled (its own ops, not a merged entry).
    Transaction logged = tx;
    if (canMerge) {
        Transaction& prev = fw.m_history.back().tx;
        fw.m_historyBytes -= prev.byteSize();
        for (Op& op : tx.ops) {
            Op* last = prev.ops.empty() ? nullptr : &prev.ops.back();
            if (last && last->kind == OpKind::Set && op.kind == OpKind::Set && last->doc == op.doc && last->path == op.path &&
                last->after == op.before) {
                last->after = std::move(op.after);
                if (last->before == last->after) prev.ops.pop_back();
            } else {
                prev.ops.push_back(std::move(op));
            }
        }
        for (const auto& [doc, rev] : tx.baseRev) prev.baseRev.emplace(doc, rev);
        for (const auto& [doc, hash] : tx.afterHash) prev.afterHash[doc] = hash;
        fw.m_historyBytes += prev.byteSize();
        if (prev.ops.empty()) {
            // The gesture returned to where it started: nothing left to undo.
            fw.m_historyBytes -= prev.byteSize();
            fw.m_history.pop_back();
            fw.m_lastCanMerge = false;
        }
    } else {
        HistoryEntry entry;
        entry.tx = tx;
        fw.m_historyBytes += entry.tx.byteSize();
        fw.m_history.push_back(std::move(entry));
        fw.m_lastCanMerge = !tx.mergeKey.empty();
    }
    const TxId id = logged.id;
    fw.pushLog(std::move(logged));
    fw.trimHistory();
    const Transaction& committed = fw.m_log.back();
    for (const PostCommitHook& hook : fw.m_postHooks) hook(fw, committed);
    fw.emit({FrameworkEvent::Kind::Committed, {}, id});
    return id;
}

void FwAccess::eraseHistoryEntries(Framework& fw, std::vector<usize> indices) {
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    std::vector<usize> remap(fw.m_history.size(), ~usize{0});
    std::vector<HistoryEntry> kept;
    kept.reserve(fw.m_history.size());
    usize k = 0;
    for (usize i = 0; i < fw.m_history.size(); ++i) {
        if (k < indices.size() && indices[k] == i) {
            fw.m_historyBytes -= fw.m_history[i].tx.byteSize();
            ++k;
            continue;
        }
        remap[i] = kept.size();
        kept.push_back(std::move(fw.m_history[i]));
    }
    fw.m_history = std::move(kept);
    std::vector<usize> redo;
    for (usize i : fw.m_redoStack) {
        if (remap[i] != ~usize{0}) redo.push_back(remap[i]);
    }
    fw.m_redoStack = std::move(redo);
}

} // namespace detail

void Framework::pushLog(Transaction tx) {
    m_log.push_back(std::move(tx));
}

void Framework::trimHistory() {
    // Oldest entries go first; the newest entry always stays undoable.
    usize drop = 0;
    u64 bytes = m_historyBytes;
    while (bytes > m_config.historyByteLimit && drop + 1 < m_history.size()) {
        bytes -= m_history[drop].tx.byteSize();
        ++drop;
    }
    if (drop > 0) {
        std::vector<usize> idx(drop);
        for (usize i = 0; i < drop; ++i) idx[i] = i;
        detail::FwAccess::eraseHistoryEntries(*this, std::move(idx));
    }
    // The log is capped by the same budget (it duplicates the history's data).
    u64 logBytes = 0;
    for (const Transaction& t : m_log) logBytes += t.byteSize();
    usize logDrop = 0;
    while (logBytes > m_config.historyByteLimit && logDrop + 1 < m_log.size()) {
        logBytes -= m_log[logDrop].byteSize();
        ++logDrop;
    }
    if (logDrop > 0) m_log.erase(m_log.begin(), m_log.begin() + static_cast<isize>(logDrop));
}

// ---- undo / redo ------------------------------------------------------------------------------
namespace {

bool touches(const Transaction& tx, const DocId& doc) {
    return std::any_of(tx.ops.begin(), tx.ops.end(), [&](const Op& o) { return o.doc == doc; });
}

} // namespace

bool Framework::canUndo(std::optional<DocId> doc) const {
    return std::any_of(m_history.rbegin(), m_history.rend(),
                       [&](const HistoryEntry& e) { return !e.undone && (!doc || touches(e.tx, *doc)); });
}

bool Framework::canRedo(std::optional<DocId> doc) const {
    return std::any_of(m_redoStack.rbegin(), m_redoStack.rend(), [&](usize i) { return !doc || touches(m_history[i].tx, *doc); });
}

std::string Framework::undoLabel(std::optional<DocId> doc) const {
    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (!it->undone && (!doc || touches(it->tx, *doc))) return it->tx.label;
    }
    return {};
}

std::string Framework::redoLabel(std::optional<DocId> doc) const {
    for (auto it = m_redoStack.rbegin(); it != m_redoStack.rend(); ++it) {
        if (!doc || touches(m_history[*it].tx, *doc)) return m_history[*it].tx.label;
    }
    return {};
}

Result<TxId> Framework::undo(Origin origin, std::optional<DocId> doc) {
    return revert(origin, doc, false);
}

Result<TxId> Framework::redo(Origin origin, std::optional<DocId> doc) {
    return revert(origin, doc, true);
}

Result<TxId> Framework::revert(Origin origin, std::optional<DocId> doc, bool redo) {
    if (m_group) return Error{ErrorCode::InvalidState, "cannot undo or redo inside a transaction group"};
    usize index = ~usize{0};
    usize redoPos = ~usize{0};
    if (redo) {
        for (usize k = m_redoStack.size(); k-- > 0;) {
            if (!doc || touches(m_history[m_redoStack[k]].tx, *doc)) {
                index = m_redoStack[k];
                redoPos = k;
                break;
            }
        }
    } else {
        for (usize i = m_history.size(); i-- > 0;) {
            if (!m_history[i].undone && (!doc || touches(m_history[i].tx, *doc))) {
                index = i;
                break;
            }
        }
    }
    if (index == ~usize{0}) return Error{ErrorCode::NotFound, redo ? "nothing to redo" : "nothing to undo"};
    const Transaction& target = m_history[index].tx;
    TxBuilder b(*this, origin, std::string(redo ? "Redo " : "Undo ") + target.label);
    b.m_replay = true;
    if (redo) {
        for (const Op& op : target.ops) {
            if (auto r = b.push(op); !r) {
                b.abort();
                return Error{r.error().code, std::format("cannot redo '{}': {}", target.label, r.error().message)};
            }
        }
    } else {
        for (auto it = target.ops.rbegin(); it != target.ops.rend(); ++it) {
            if (auto r = b.push(it->inverse()); !r) {
                b.abort();
                return Error{r.error().code, std::format("cannot undo '{}': {}", target.label, r.error().message)};
            }
        }
    }
    Transaction tx;
    tx.kind = redo ? TxKind::Redo : TxKind::Undo;
    tx.target = target.id;
    tx.label = b.m_label;
    tx.origin = origin;
    tx.author = m_config.user;
    tx.ops = b.m_ops;
    for (const auto& [d, rev] : b.m_baseRev) tx.baseRev[d] = rev;
    tx.time = now();
    tx.id = TxId{m_config.user, m_lamport + 1};
    for (const DocId& d : tx.documents()) {
        const Document* dd = m_workspace->find(d);
        tx.afterHash[d] = dd && !dd->destroyed() ? dd->contentHash() : 0;
    }
    if (m_journal) {
        JournalRecord rec;
        rec.kind = JournalRecordKind::Tx;
        rec.tx = tx;
        if (auto r = m_journal->append(rec); !r) {
            b.abort();
            return Error{r.error().code, "journal write failed, undo rolled back: " + r.error().message};
        }
    }
    ++m_lamport;
    b.m_ops.clear();
    b.m_baseRev.clear();
    b.m_done = true;
    m_history[index].undone = !redo;
    if (redo) {
        m_redoStack.erase(m_redoStack.begin() + static_cast<isize>(redoPos));
    } else {
        m_redoStack.push_back(index);
    }
    m_lastCanMerge = false;
    const TxId id = tx.id;
    pushLog(std::move(tx));
    trimHistory();
    for (const PostCommitHook& hook : m_postHooks) hook(*this, m_log.back());
    emit({redo ? FrameworkEvent::Kind::Redone : FrameworkEvent::Kind::Undone, {}, id});
    return id;
}

// ---- groups -----------------------------------------------------------------------------------
void Framework::beginGroup(Origin origin, std::string label) {
    if (m_groupDepth++ == 0) m_group = begin(origin, std::move(label));
}

Result<TxId> Framework::endGroup() {
    if (m_groupDepth == 0) return Error{ErrorCode::InvalidState, "no open transaction group"};
    if (--m_groupDepth > 0) return TxId{};
    std::unique_ptr<TxBuilder> group = std::move(m_group);
    return group->commit();
}

void Framework::cancelGroup() {
    if (m_groupDepth == 0) return;
    m_groupDepth = 0;
    std::unique_ptr<TxBuilder> group = std::move(m_group);
    group->abort();
}

// ---- external edits ---------------------------------------------------------------------------
Result<TxId> Framework::reloadFromDisk(const DocId& id) {
    return detail::FwAccess::syncWithDisk(*this, id, /*discardLocal=*/false);
}

namespace detail {

Result<TxId> FwAccess::syncWithDisk(Framework& fw, const DocId& id, bool discardLocal) {
    Document* d = liveDocument(*fw.m_workspace, id);
    if (!d) return noDocument(id);
    if (d->path().empty()) return Error{ErrorCode::InvalidState, std::format("{} has no file", d->name())};
    HELIOS_TRY_ASSIGN(const std::string text, fs::readTextFile(d->path()));
    const refl::TypeInfo& type = d->type();
    refl::Value theirs(type);
    refl::RecordHeader theirsHeader;
    refl::ReadCtx ctx;
    HELIOS_TRY(refl::readRecord(type, theirs.data(), text, theirsHeader, ctx));
    const std::string theirsText = recordText(type, theirs.data(), theirsHeader);
    if (theirsHeader.rid != d->header().rid) {
        return Error{ErrorCode::InvalidState, std::format("{}: $rid changed on disk", d->name())};
    }

    auto b = fw.begin(Origin::Import, (discardLocal ? "Revert " : "External edit: ") + d->name());
    b->m_replay = true;
    const auto setHeader = [&](std::string_view path, const std::string& value) -> Result<void> {
        return b->set(id, path, json::quote(value));
    };
    if (discardLocal || !d->dirty()) {
        HELIOS_TRY(setHeader("$name", theirsHeader.name));
        HELIOS_TRY(setHeader("$parent", theirsHeader.parent));
        HELIOS_TRY(setHeader("$comment", theirsHeader.comment));
        HELIOS_TRY(applyDiff(*b, *d, theirs.data()));
    } else {
        // Three-way merge: apply the file's changes since the base onto the local edits.
        refl::Value base(type);
        refl::RecordHeader baseHeader;
        refl::ReadCtx baseCtx;
        HELIOS_TRY(refl::readRecord(type, base.data(), d->baseText(), baseHeader, baseCtx));
        std::vector<std::string> conflicts;
        const auto mergeHeader = [&](std::string_view path, const std::string& baseV, const std::string& theirV) -> Result<void> {
            if (baseV == theirV) return {};
            HELIOS_TRY_ASSIGN(const std::string cur, headerValue(*d, path));
            if (cur == json::quote(theirV)) return {};
            if (cur != json::quote(baseV)) {
                conflicts.emplace_back(path);
                return {};
            }
            return setHeader(path, theirV);
        };
        HELIOS_TRY(mergeHeader("$name", baseHeader.name, theirsHeader.name));
        HELIOS_TRY(mergeHeader("$parent", baseHeader.parent, theirsHeader.parent));
        HELIOS_TRY(mergeHeader("$comment", baseHeader.comment, theirsHeader.comment));
        HELIOS_TRY_ASSIGN(const std::vector<Op> theirOps, diffOps(type, base.data(), theirs.data(), id));
        for (const Op& op : theirOps) {
            if (op.kind == OpKind::Set) {
                HELIOS_TRY_ASSIGN(const auto cur, valueAt(type, d->object(), op.path));
                if (cur == op.after) continue;  // both sides made the same change
                if (cur != op.before) {
                    conflicts.push_back(op.path.empty() ? std::string("<whole record>") : op.path);
                    continue;
                }
            }
            if (auto r = b->apply(op); !r) conflicts.push_back(op.path + " (" + r.error().message + ")");
        }
        if (!conflicts.empty()) {
            b->abort();
            std::string list;
            for (const std::string& c : conflicts) list += (list.empty() ? "" : ", ") + c;
            return Error{ErrorCode::InvalidState, std::format("{}: external edit conflicts with unsaved changes at {}", d->name(), list)};
        }
    }
    HELIOS_TRY_ASSIGN(const TxId tx, b->commit());
    DocAccess::setBase(*d, theirsText);
    fw.emit({FrameworkEvent::Kind::Reloaded, id, tx});
    return tx;
}

} // namespace detail

} // namespace helios::tf
