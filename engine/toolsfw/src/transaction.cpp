#include "helios/toolsfw/transaction.h"

#include <algorithm>
#include <string>

#include "helios/toolsfw/json_util.h"

namespace helios::tf {

std::string_view opKindName(OpKind kind) noexcept {
    switch (kind) {
    case OpKind::Set: return "set";
    case OpKind::Insert: return "insert";
    case OpKind::Remove: return "remove";
    case OpKind::Move: return "move";
    case OpKind::Create: return "create";
    case OpKind::Destroy: return "destroy";
    }
    return "unknown";
}

std::string_view txKindName(TxKind kind) noexcept {
    switch (kind) {
    case TxKind::Do: return "do";
    case TxKind::Undo: return "undo";
    case TxKind::Redo: return "redo";
    }
    return "unknown";
}

Op Op::inverse() const {
    Op r = *this;
    std::swap(r.before, r.after);
    switch (kind) {
    case OpKind::Set: break;
    case OpKind::Insert: r.kind = OpKind::Remove; break;
    case OpKind::Remove: r.kind = OpKind::Insert; break;
    case OpKind::Move: std::swap(r.index, r.toIndex); break;
    case OpKind::Create: r.kind = OpKind::Destroy; break;
    case OpKind::Destroy: r.kind = OpKind::Create; break;
    }
    return r;
}

std::vector<DocId> Transaction::documents() const {
    std::vector<DocId> out;
    for (const Op& op : ops) {
        if (!containsDoc(out, op.doc)) out.push_back(op.doc);
    }
    return out;
}

usize Transaction::byteSize() const noexcept {
    usize n = sizeof(Transaction) + label.size() + mergeKey.size() + author.size() + id.user.size();
    for (const Op& op : ops) {
        n += sizeof(Op) + op.path.size() + op.key.size() + op.typeName.size() + op.file.size();
        if (op.before) n += op.before->size();
        if (op.after) n += op.after->size();
    }
    n += (baseRev.size() + afterHash.size()) * 48;
    return n;
}

namespace {

void writeTxId(refl::JsonWriter& out, const TxId& id) {
    out.beginObject();
    out.key("user");
    out.string(id.user);
    out.key("lamport");
    out.unsignedInteger(id.lamport);
    out.endObject();
}

Result<TxId> readTxId(refl::JsonValue v) {
    if (!v.isObject()) return Error{ErrorCode::ParseError, "transaction id: expected an object"};
    TxId id;
    const auto user = json::getString(v, "user");
    u64 lamport = 0;
    if (!user || !v.get("lamport").getU64(lamport)) return Error{ErrorCode::ParseError, "transaction id: needs user and lamport"};
    id.user = std::string(*user);
    id.lamport = lamport;
    return id;
}

bool isSnapshotOp(OpKind kind) noexcept {
    return kind == OpKind::Create || kind == OpKind::Destroy;
}

void writeValue(refl::JsonWriter& out, std::string_view key, const std::optional<std::string>& value, bool asString) {
    if (!value) return;
    out.key(key);
    if (asString) {
        out.string(*value);
    } else {
        out.raw(*value);
    }
}

} // namespace

void writeOpJson(refl::JsonWriter& out, const Op& op) {
    out.beginObject();
    out.key("op");
    out.string(opKindName(op.kind));
    out.key("doc");
    out.string(op.doc.toString());
    if (!isSnapshotOp(op.kind)) {
        out.key("path");
        out.string(op.path);
    }
    switch (op.kind) {
    case OpKind::Set: break;
    case OpKind::Insert:
    case OpKind::Remove:
        out.key("index");
        out.unsignedInteger(op.index);
        out.key("key");
        out.string(op.key);
        break;
    case OpKind::Move:
        out.key("index");
        out.unsignedInteger(op.index);
        out.key("to");
        out.unsignedInteger(op.toIndex);
        out.key("key");
        out.string(op.key);
        break;
    case OpKind::Create:
    case OpKind::Destroy:
        out.key("type");
        out.string(op.typeName);
        out.key("file");
        out.string(op.file);
        break;
    }
    const bool asString = isSnapshotOp(op.kind);
    writeValue(out, "before", op.before, asString);
    writeValue(out, "after", op.after, asString);
    out.endObject();
}

Result<Op> readOpJson(refl::JsonValue v) {
    if (!v.isObject()) return Error{ErrorCode::ParseError, "op: expected an object"};
    Op op;
    const auto kind = json::getString(v, "op");
    if (!kind) return Error{ErrorCode::ParseError, "op: missing \"op\""};
    bool known = false;
    for (OpKind k : {OpKind::Set, OpKind::Insert, OpKind::Remove, OpKind::Move, OpKind::Create, OpKind::Destroy}) {
        if (opKindName(k) == *kind) {
            op.kind = k;
            known = true;
        }
    }
    if (!known) return Error{ErrorCode::ParseError, "op: unknown kind '" + std::string(*kind) + "'"};
    const auto doc = json::getString(v, "doc");
    if (!doc) return Error{ErrorCode::ParseError, "op: missing \"doc\""};
    HELIOS_TRY_ASSIGN(op.doc, Guid::parse(*doc));
    const bool snapshot = isSnapshotOp(op.kind);
    if (!snapshot) {
        const auto path = json::getString(v, "path");
        if (!path) return Error{ErrorCode::ParseError, "op: missing \"path\""};
        op.path = std::string(*path);
    }
    if (op.kind == OpKind::Insert || op.kind == OpKind::Remove || op.kind == OpKind::Move) {
        if (!v.get("index").getU64(op.index)) return Error{ErrorCode::ParseError, "op: missing \"index\""};
        if (const auto key = json::getString(v, "key")) op.key = std::string(*key);
    }
    if (op.kind == OpKind::Move && !v.get("to").getU64(op.toIndex)) return Error{ErrorCode::ParseError, "op: missing \"to\""};
    if (snapshot) {
        const auto type = json::getString(v, "type");
        const auto file = json::getString(v, "file");
        if (!type || !file) return Error{ErrorCode::ParseError, "op: create/destroy need type and file"};
        op.typeName = std::string(*type);
        op.file = std::string(*file);
    }
    for (const auto& [name, slot] : {std::pair{"before", &op.before}, std::pair{"after", &op.after}}) {
        const refl::JsonValue value = v.get(name);
        if (!value.isValid()) continue;
        if (snapshot) {
            if (!value.isString()) return Error{ErrorCode::ParseError, "op: snapshot must be a string"};
            *slot = std::string(value.asString());
        } else {
            *slot = json::compact(value);
        }
    }
    if (op.kind == OpKind::Insert && !op.after) return Error{ErrorCode::ParseError, "op: insert needs \"after\""};
    if (op.kind == OpKind::Remove && !op.before) return Error{ErrorCode::ParseError, "op: remove needs \"before\""};
    if (op.kind == OpKind::Create && !op.after) return Error{ErrorCode::ParseError, "op: create needs \"after\""};
    if (op.kind == OpKind::Destroy && !op.before) return Error{ErrorCode::ParseError, "op: destroy needs \"before\""};
    return op;
}

void Transaction::writeJson(refl::JsonWriter& out) const {
    out.beginObject();
    out.key("id");
    writeTxId(out, id);
    out.key("kind");
    out.string(txKindName(kind));
    if (kind != TxKind::Do) {
        out.key("target");
        writeTxId(out, target);
    }
    out.key("label");
    out.string(label);
    out.key("origin");
    out.string(originName(origin));
    out.key("author");
    out.string(author);
    out.key("time");
    out.integer(time);
    if (!mergeKey.empty()) {
        out.key("mergeKey");
        out.string(mergeKey);
    }
    out.key("baseRev");
    out.beginObject();
    for (const auto& [doc, rev] : baseRev) {
        out.key(doc.toString());
        out.unsignedInteger(rev);
    }
    out.endObject();
    out.key("afterHash");
    out.beginObject();
    for (const auto& [doc, hash] : afterHash) {
        out.key(doc.toString());
        out.string(hashHex(hash));
    }
    out.endObject();
    out.key("ops");
    out.beginArray();
    for (const Op& op : ops) writeOpJson(out, op);
    out.endArray();
    out.endObject();
}

std::string Transaction::toJson() const {
    refl::JsonWriter w(refl::JsonStyle::Compact);
    writeJson(w);
    return w.take();
}

Result<Transaction> Transaction::fromJson(std::string_view text) {
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, refl::JsonDocument::parse(text, "<transaction>"));
    return fromJson(doc.root());
}

Result<Transaction> Transaction::fromJson(refl::JsonValue v) {
    if (!v.isObject()) return Error{ErrorCode::ParseError, "transaction: expected an object"};
    Transaction tx;
    HELIOS_TRY_ASSIGN(tx.id, readTxId(v.get("id")));
    const auto kind = json::getString(v, "kind");
    if (!kind) return Error{ErrorCode::ParseError, "transaction: missing kind"};
    if (*kind == "do") {
        tx.kind = TxKind::Do;
    } else if (*kind == "undo") {
        tx.kind = TxKind::Undo;
    } else if (*kind == "redo") {
        tx.kind = TxKind::Redo;
    } else {
        return Error{ErrorCode::ParseError, "transaction: unknown kind '" + std::string(*kind) + "'"};
    }
    if (tx.kind != TxKind::Do) {
        HELIOS_TRY_ASSIGN(tx.target, readTxId(v.get("target")));
    }
    tx.label = std::string(json::getString(v, "label").value_or(""));
    const auto origin = parseOrigin(json::getString(v, "origin").value_or(""));
    if (!origin) return Error{ErrorCode::ParseError, "transaction: unknown origin"};
    tx.origin = *origin;
    tx.author = std::string(json::getString(v, "author").value_or(""));
    tx.time = json::getInteger(v, "time").value_or(0);
    tx.mergeKey = std::string(json::getString(v, "mergeKey").value_or(""));
    for (const auto& m : v.get("baseRev").members()) {
        HELIOS_TRY_ASSIGN(const Guid doc, Guid::parse(m.key));
        u64 rev = 0;
        if (!m.value.getU64(rev)) return Error{ErrorCode::ParseError, "transaction: bad baseRev"};
        tx.baseRev[doc] = rev;
    }
    for (const auto& m : v.get("afterHash").members()) {
        HELIOS_TRY_ASSIGN(const Guid doc, Guid::parse(m.key));
        const auto hash = parseHashHex(m.value.asString());
        if (!m.value.isString() || !hash) return Error{ErrorCode::ParseError, "transaction: bad afterHash"};
        tx.afterHash[doc] = *hash;
    }
    const refl::JsonValue ops = v.get("ops");
    if (!ops.isArray()) return Error{ErrorCode::ParseError, "transaction: missing ops"};
    for (refl::JsonValue o : ops.elements()) {
        HELIOS_TRY_ASSIGN(Op op, readOpJson(o));
        tx.ops.push_back(std::move(op));
    }
    return tx;
}

} // namespace helios::tf
