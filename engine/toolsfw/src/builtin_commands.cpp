// Built-in ToolsFramework commands (07 §1.2): documents, edits, history, selection, validation.
// They run headless (helios-tool, RPC) and back the editor's menus and property grid.

#include <algorithm>
#include <format>

#include "helios/reflect/path.h"

#include "helios/toolsfw/framework.h"
#include "helios/toolsfw/json_util.h"
#include "helios/toolsfw/validate.h"

#include "framework_internal.h"

namespace helios::tf::detail {

namespace {

Result<Document*> docArg(const CommandContext& ctx, std::string_view name = "doc") {
    HELIOS_TRY_ASSIGN(const std::string key, ctx.stringArg(name));
    Document* d = ctx.framework().documents().find(key);
    if (!d) return Error{ErrorCode::NotFound, std::format("{}: no open document '{}'", ctx.desc().id, key)};
    return d;
}

std::optional<DocId> optionalDoc(const CommandContext& ctx) {
    if (const auto key = ctx.optionalString("doc")) {
        if (const Document* d = ctx.framework().documents().find(*key)) return d->id();
        return DocId{};  // unknown: matches nothing
    }
    return std::nullopt;
}

ArgDesc arg(std::string name, ArgType type, bool required, std::string doc) {
    return ArgDesc{std::move(name), type, required, std::move(doc)};
}

const ArgDesc kDocArg{"doc", ArgType::String, true, "Document: $name, GUID or project-relative path"};

std::string txResult(const TxId& id) {
    refl::JsonWriter w(refl::JsonStyle::Compact);
    w.beginObject();
    w.key("tx");
    w.string(id.isNull() ? std::string() : id.toString());
    w.endObject();
    return w.take();
}

} // namespace

Result<void> registerBuiltinCommands(Framework& fw) {
    CommandBus& bus = fw.commands();
    std::vector<CommandDesc> cmds;

    // ---- document edits ---------------------------------------------------------------------
    {
        CommandDesc c;
        c.id = "doc.setProperty";
        c.label = "Set Property";
        c.category = "Edit";
        c.doc = "Sets the value at a property path (JSON). Map keys and optionals are created as needed. "
                "Continuous gestures pass the same mergeKey so they undo as one step.";
        c.args = {kDocArg, arg("path", ArgType::String, true, "Property path (\"handling/yawRate\", \"thrusters[#9a1e]/maxForce\")"),
                  arg("value", ArgType::Any, true, "New value as JSON"),
                  arg("mergeKey", ArgType::String, false, "Coalesce with the previous transaction of the same gesture")};
        c.paletteOnly = true;
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            HELIOS_TRY_ASSIGN(const std::string path, ctx.stringArg("path"));
            HELIOS_TRY_ASSIGN(const std::string value, ctx.jsonArg("value"));
            ctx.setLabel(std::format("Set {}", path.empty() ? d->name() : path));
            if (auto key = ctx.optionalString("mergeKey")) ctx.setMergeKey(std::move(*key));
            return ctx.tx().set(d->id(), path, value);
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.remove";
        c.label = "Remove";
        c.category = "Edit";
        c.doc = "Removes a list element (`list[#key]`, `list[3]`) or map key, or resets an optional.";
        c.args = {kDocArg, arg("path", ArgType::String, true, "Property path of the element, key or optional")};
        c.paletteOnly = true;
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            HELIOS_TRY_ASSIGN(const std::string path, ctx.stringArg("path"));
            ctx.setLabel(std::format("Remove {}", path));
            return ctx.tx().remove(d->id(), path);
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.insertElement";
        c.label = "Insert Element";
        c.category = "Edit";
        c.doc = "Inserts an element into a list (default: append). Keyed lists get a new GUID key unless `key` is given.";
        c.args = {kDocArg, arg("path", ArgType::String, true, "Property path of the list"),
                  arg("index", ArgType::Integer, false, "Insert before this index (default: the end)"),
                  arg("value", ArgType::Any, false, "Element as JSON (default: the element type's defaults)"),
                  arg("key", ArgType::String, false, "Keyed-list GUID key")};
        c.paletteOnly = true;
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            HELIOS_TRY_ASSIGN(const std::string path, ctx.stringArg("path"));
            std::string value;
            if (ctx.has("value")) {
                HELIOS_TRY_ASSIGN(value, ctx.jsonArg("value"));
            }
            std::optional<Guid> key;
            if (const auto k = ctx.optionalString("key")) {
                HELIOS_TRY_ASSIGN(key, Guid::parse(*k));
            }
            u64 index = ~u64{0};
            if (ctx.has("index")) {
                HELIOS_TRY_ASSIGN(const i64 i, ctx.integerArg("index"));
                if (i < 0) return Error{ErrorCode::InvalidArgument, "index must not be negative"};
                index = static_cast<u64>(i);
            }
            if (index == ~u64{0}) {
                // Append: the list's current size.
                const auto count = [&]() -> Result<u64> {
                    auto p = refl::PropertyPath::parse(path);
                    if (!p) return std::move(p).error();
                    HELIOS_TRY_ASSIGN(const refl::ConstRef r, refl::resolve(d->type(), d->object(), *p));
                    const refl::TypeInfo* t = r.type;
                    const void* ptr = r.ptr;
                    while (t->kind == refl::Kind::Optional) {
                        if (!t->ops->has(ptr)) return u64{0};
                        ptr = t->ops->get(const_cast<void*>(ptr));
                        t = &t->element();
                    }
                    if (!refl::isSequenceKind(t->kind)) return Error{ErrorCode::InvalidArgument, std::format("'{}' is not a list", path)};
                    return static_cast<u64>(t->ops->size(ptr));
                }();
                HELIOS_TRY_ASSIGN(index, count);
            }
            ctx.setLabel(std::format("Add to {}", path));
            return ctx.tx().insert(d->id(), path, index, value, key);
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.moveElement";
        c.label = "Move Element";
        c.category = "Edit";
        c.doc = "Moves a list element to another index.";
        c.args = {kDocArg, arg("path", ArgType::String, true, "Property path of the element (`list[#key]`, `list[2]`)"),
                  arg("to", ArgType::Integer, true, "Destination index")};
        c.paletteOnly = true;
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            HELIOS_TRY_ASSIGN(const std::string path, ctx.stringArg("path"));
            HELIOS_TRY_ASSIGN(const i64 to, ctx.integerArg("to"));
            if (to < 0) return Error{ErrorCode::InvalidArgument, "to must not be negative"};
            ctx.setLabel(std::format("Move {}", path));
            return ctx.tx().move(d->id(), path, static_cast<u64>(to));
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.rename";
        c.label = "Rename";
        c.category = "Edit";
        c.doc = "Changes a record's $name (references use $rid and are unaffected).";
        c.args = {kDocArg, arg("name", ArgType::String, true, "New $name")};
        c.shortcut = "F2";
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            HELIOS_TRY_ASSIGN(const std::string name, ctx.stringArg("name"));
            ctx.setLabel(std::format("Rename {} to {}", d->name(), name));
            return ctx.tx().set(d->id(), "$name", json::quote(name));
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.create";
        c.label = "New Record";
        c.category = "File";
        c.doc = "Creates a record document of a schema record type at a project-relative .hrec path.";
        c.args = {arg("type", ArgType::String, true, "Qualified record type (\"sample.ship.ShipHullDef\")"),
                  arg("file", ArgType::String, true, "Project-relative file (\"records/hull/wren.hrec\")"),
                  arg("name", ArgType::String, true, "$name"),
                  arg("values", ArgType::Object, false, "Initial field values"),
                  arg("rid", ArgType::Integer, false, "$rid (default: newly minted)")};
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(const std::string typeName, ctx.stringArg("type"));
            const refl::TypeInfo* type = ctx.framework().types().find(typeName);
            if (!type) return Error{ErrorCode::NotFound, std::format("unknown type {}", typeName)};
            HELIOS_TRY_ASSIGN(const std::string file, ctx.stringArg("file"));
            refl::RecordHeader h;
            HELIOS_TRY_ASSIGN(h.name, ctx.stringArg("name"));
            if (ctx.has("rid")) {
                HELIOS_TRY_ASSIGN(const i64 rid, ctx.integerArg("rid"));
                h.rid = static_cast<refl::RecordId>(rid);
            }
            std::string values;
            if (ctx.has("values")) {
                HELIOS_TRY_ASSIGN(values, ctx.jsonArg("values"));
            }
            ctx.setLabel(std::format("New {}", h.name));
            HELIOS_TRY_ASSIGN(const DocId id, ctx.tx().createRecord(*type, file, h, values));
            ctx.setResult(json::quote(id.toString()));
            return {};
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.destroy";
        c.label = "Delete Record";
        c.category = "Edit";
        c.doc = "Deletes a record document (its file is removed when saved). Undoable.";
        c.args = {kDocArg};
        c.shortcut = "Delete";
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            ctx.setLabel(std::format("Delete {}", d->name()));
            return ctx.tx().destroy(d->id());
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.revert";
        c.label = "Revert";
        c.category = "File";
        c.doc = "Discards unsaved changes of a document (an undoable transaction back to the file's content).";
        c.args = {kDocArg};
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            if (ctx.framework().inGroup()) return Error{ErrorCode::InvalidState, "revert cannot join a transaction group"};
            HELIOS_TRY_ASSIGN(const TxId tx, FwAccess::syncWithDisk(ctx.framework(), d->id(), true));
            ctx.setResult(txResult(tx));
            return {};
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.reload";
        c.label = "Reload From Disk";
        c.category = "File";
        c.doc = "Applies external changes of the file (three-way merge when there are unsaved edits).";
        c.args = {kDocArg};
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            if (ctx.framework().inGroup()) return Error{ErrorCode::InvalidState, "reload cannot join a transaction group"};
            HELIOS_TRY_ASSIGN(const TxId tx, ctx.framework().reloadFromDisk(d->id()));
            ctx.setResult(txResult(tx));
            return {};
        };
        cmds.push_back(std::move(c));
    }

    // ---- files ------------------------------------------------------------------------------
    {
        CommandDesc c;
        c.id = "doc.open";
        c.label = "Open Record";
        c.category = "File";
        c.doc = "Opens a record file (project-relative or absolute).";
        c.args = {arg("file", ArgType::String, true, "Record file")};
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(const std::string file, ctx.stringArg("file"));
            HELIOS_TRY_ASSIGN(Document* d, ctx.framework().open(fs::pathFromUtf8(file)));
            ctx.setResult(json::quote(d->id().toString()));
            return {};
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.save";
        c.label = "Save";
        c.category = "File";
        c.doc = "Saves a document (default: the selected one).";
        c.args = {arg("doc", ArgType::String, false, "Document (default: the primary selection)")};
        c.shortcut = "Ctrl+S";
        c.canExecute = [](const CommandContext& ctx) {
            if (ctx.has("doc")) return true;
            const Document* d = ctx.framework().documents().find(ctx.framework().selection().primaryDocument());
            return d != nullptr && d->dirty();
        };
        c.execute = [](CommandContext& ctx) -> Result<void> {
            DocId id = ctx.framework().selection().primaryDocument();
            if (ctx.has("doc")) {
                HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
                id = d->id();
            }
            return ctx.framework().save(id);
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.saveAll";
        c.label = "Save All";
        c.category = "File";
        c.doc = "Saves every document with unsaved changes.";
        c.shortcut = "Ctrl+Shift+S";
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(const usize n, ctx.framework().saveAll());
            ctx.setResult(std::to_string(n));
            return {};
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "doc.close";
        c.label = "Close";
        c.category = "File";
        c.doc = "Closes a document (refused with unsaved changes unless discard is true).";
        c.args = {kDocArg, arg("discard", ArgType::Bool, false, "Discard unsaved changes")};
        c.paletteOnly = true;
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            const bool discard = ctx.has("discard") && ctx.boolArg("discard").valueOr(false);
            return ctx.framework().close(d->id(), discard);
        };
        cmds.push_back(std::move(c));
    }

    // ---- history ----------------------------------------------------------------------------
    for (const bool redo : {false, true}) {
        CommandDesc c;
        c.id = redo ? "edit.redo" : "edit.undo";
        c.label = redo ? "Redo" : "Undo";
        c.category = "Edit";
        c.doc = redo ? "Re-applies the most recently undone transaction (of one document when `doc` is given)."
                     : "Reverts the most recent transaction (of one document when `doc` is given) with a new inverse transaction.";
        c.args = {arg("doc", ArgType::String, false, "Limit to this document's history")};
        c.shortcut = redo ? "Ctrl+Y" : "Ctrl+Z";
        c.canExecute = [redo](const CommandContext& ctx) {
            const auto doc = optionalDoc(ctx);
            return redo ? ctx.framework().canRedo(doc) : ctx.framework().canUndo(doc);
        };
        c.execute = [redo](CommandContext& ctx) -> Result<void> {
            if (ctx.framework().inGroup()) return Error{ErrorCode::InvalidState, "undo cannot run inside a transaction group"};
            const auto doc = optionalDoc(ctx);
            HELIOS_TRY_ASSIGN(const TxId id, redo ? ctx.framework().redo(ctx.origin(), doc) : ctx.framework().undo(ctx.origin(), doc));
            ctx.setResult(txResult(id));
            return {};
        };
        cmds.push_back(std::move(c));
    }

    // ---- selection --------------------------------------------------------------------------
    {
        CommandDesc c;
        c.id = "selection.set";
        c.label = "Select";
        c.category = "Selection";
        c.doc = "Selects a document (optionally a property path inside it).";
        c.args = {kDocArg, arg("path", ArgType::String, false, "Sub-selection property path")};
        c.paletteOnly = true;
        c.execute = [](CommandContext& ctx) -> Result<void> {
            HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
            ObjRef ref;
            ref.doc = d->id();
            ref.path = ctx.optionalString("path").value_or("");
            ctx.framework().selection().set(ref);
            return {};
        };
        cmds.push_back(std::move(c));
    }
    {
        CommandDesc c;
        c.id = "selection.clear";
        c.label = "Clear Selection";
        c.category = "Selection";
        c.doc = "Clears the selection.";
        c.shortcut = "Escape";
        c.paletteOnly = true;
        c.execute = [](CommandContext& ctx) -> Result<void> {
            ctx.framework().selection().clear();
            return {};
        };
        cmds.push_back(std::move(c));
    }
    for (const bool fwd : {false, true}) {
        CommandDesc c;
        c.id = fwd ? "selection.forward" : "selection.back";
        c.label = fwd ? "Selection Forward" : "Selection Back";
        c.category = "Selection";
        c.doc = fwd ? "Goes forward in the selection history." : "Goes back in the selection history.";
        c.shortcut = fwd ? "Alt+Right" : "Alt+Left";
        c.canExecute = [fwd](const CommandContext& ctx) {
            return fwd ? ctx.framework().selection().canGoForward() : ctx.framework().selection().canGoBack();
        };
        c.execute = [fwd](CommandContext& ctx) -> Result<void> {
            const bool ok = fwd ? ctx.framework().selection().forward() : ctx.framework().selection().back();
            if (!ok) return Error{ErrorCode::NotFound, "no selection history in that direction"};
            return {};
        };
        cmds.push_back(std::move(c));
    }

    // ---- validation -------------------------------------------------------------------------
    {
        CommandDesc c;
        c.id = "validate.run";
        c.label = "Validate";
        c.category = "Tools";
        c.doc = "Validates documents against their schema (ranges, @max, keys, headers). Result: {\"errors\": n, \"issues\": [...]}.";
        c.args = {arg("doc", ArgType::String, false, "Validate only this document")};
        c.execute = [](CommandContext& ctx) -> Result<void> {
            std::vector<const Document*> docs;
            if (ctx.has("doc")) {
                HELIOS_TRY_ASSIGN(Document* d, docArg(ctx));
                docs.push_back(d);
            } else {
                for (const Document* d : ctx.framework().documents().documents()) {
                    if (!d->destroyed()) docs.push_back(d);
                }
            }
            std::vector<Issue> issues;
            for (const Document* d : docs) {
                const usize first = issues.size();
                validateHeader(d->header(), issues);
                validateObject(d->type(), d->object(), issues);
                for (usize i = first; i < issues.size(); ++i) issues[i].file = d->relativePath().empty() ? d->name() : d->relativePath();
            }
            refl::JsonWriter w(refl::JsonStyle::Compact);
            w.beginObject();
            w.key("errors");
            w.unsignedInteger(static_cast<u64>(std::count_if(issues.begin(), issues.end(), [](const Issue& i) {
                return i.severity == Issue::Severity::Error;
            })));
            w.key("issues");
            w.beginArray();
            for (const Issue& i : issues) {
                w.beginObject();
                w.key("severity");
                w.string(severityName(i.severity));
                w.key("file");
                w.string(i.file);
                w.key("path");
                w.string(i.path);
                w.key("rule");
                w.string(i.rule);
                w.key("message");
                w.string(i.message);
                w.endObject();
            }
            w.endArray();
            w.endObject();
            ctx.setResult(w.take());
            return {};
        };
        cmds.push_back(std::move(c));
    }

    for (CommandDesc& c : cmds) HELIOS_TRY(bus.add(std::move(c)));
    return {};
}

} // namespace helios::tf::detail
