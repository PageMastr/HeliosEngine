#include "lock.h"

#include <algorithm>
#include <format>
#include <set>

#include <yyjson.h>

#include "json_out.h"
#include "text.h"

namespace helios::schemac {

namespace {

std::string lockKindOf(const Decl* d) {
    switch (d->kind) {
    case DeclKind::Enum: return "enum";
    case DeclKind::Flags: return "flags";
    case DeclKind::Variant: return "variant";
    default: return "struct";
    }
}

/// Loader with path-qualified error messages.
class LockReader {
public:
    LockReader(u32 file, DiagnosticEngine& diags) : m_file(file), m_diags(diags) {}

    bool fail(const std::string& path, const std::string& message) {
        m_diags.error(SourceLoc{m_file, 1, 1}, std::format("invalid schema lock at {}: {}", path, message));
        return false;
    }

    static std::string str(yyjson_val* v) { return std::string(yyjson_get_str(v), yyjson_get_len(v)); }

    bool readU32(yyjson_val* obj, const char* key, const std::string& path, u32& out, bool required = true) {
        yyjson_val* v = yyjson_obj_get(obj, key);
        if (!v) {
            if (required) return fail(path, std::format("missing '{}'", key));
            return true;
        }
        if (!yyjson_is_uint(v) || yyjson_get_uint(v) > 0xFFFFFFFFull) return fail(path + "." + key, "expected an unsigned 32-bit integer");
        out = static_cast<u32>(yyjson_get_uint(v));
        return true;
    }

    bool readString(yyjson_val* obj, const char* key, const std::string& path, std::string& out, bool required = true) {
        yyjson_val* v = yyjson_obj_get(obj, key);
        if (!v) {
            if (required) return fail(path, std::format("missing '{}'", key));
            return true;
        }
        if (!yyjson_is_str(v)) return fail(path + "." + key, "expected a string");
        out = str(v);
        return true;
    }

    bool readBool(yyjson_val* obj, const char* key, const std::string& path, bool& out) {
        yyjson_val* v = yyjson_obj_get(obj, key);
        if (!v) return true;
        if (!yyjson_is_bool(v)) return fail(path + "." + key, "expected true or false");
        out = yyjson_get_bool(v);
        return true;
    }

    bool readStrings(yyjson_val* obj, const char* key, const std::string& path, std::vector<std::string>& out) {
        yyjson_val* v = yyjson_obj_get(obj, key);
        if (!v) return true;
        if (!yyjson_is_arr(v)) return fail(path + "." + key, "expected an array of strings");
        usize i, n;
        yyjson_val* e;
        yyjson_arr_foreach(v, i, n, e) {
            if (!yyjson_is_str(e)) return fail(path + "." + key, "expected an array of strings");
            out.push_back(str(e));
        }
        return true;
    }

    bool readField(yyjson_val* v, const std::string& path, LockField& f) {
        if (!yyjson_is_obj(v)) return fail(path, "expected an object");
        return readU32(v, "id", path, f.id) && readString(v, "name", path, f.name) && readString(v, "type", path, f.type, false) &&
               readString(v, "default", path, f.defaultJson, false) && readBool(v, "tombstone", path, f.tombstone) &&
               readStrings(v, "was", path, f.was);
    }

    bool read(yyjson_val* root, Lock& out) {
        if (!yyjson_is_obj(root)) return fail("<root>", "expected an object");
        u32 format = 0;
        if (!readU32(root, "format", "<root>", format)) return false;
        if (format != 1) return fail("format", std::format("unsupported lock format {} (this helios-schemac writes 1)", format));
        yyjson_val* types = yyjson_obj_get(root, "types");
        if (!types || !yyjson_is_obj(types)) return fail("<root>", "missing 'types' object");
        usize i, n;
        yyjson_val *k, *v;
        yyjson_obj_foreach(types, i, n, k, v) {
            const std::string name = str(k);
            const std::string path = "types." + name;
            if (!yyjson_is_obj(v)) return fail(path, "expected an object");
            LockType t;
            if (!readU32(v, "id", path, t.id) || !readString(v, "kind", path, t.kind) || !readU32(v, "version", path, t.version, false) ||
                !readString(v, "base", path, t.base, false) || !readStrings(v, "was", path, t.was) ||
                !readU32(v, "nextField", path, t.nextField, false))
                return false;
            if (t.kind != "struct" && t.kind != "enum" && t.kind != "flags" && t.kind != "variant")
                return fail(path + ".kind", std::format("unknown kind '{}'", t.kind));
            const char* listKey = t.kind == "variant" ? "alternatives" : "fields";
            if (yyjson_val* fields = yyjson_obj_get(v, listKey)) {
                if (!yyjson_is_arr(fields)) return fail(path + "." + listKey, "expected an array");
                usize j, m;
                yyjson_val* e;
                yyjson_arr_foreach(fields, j, m, e) {
                    LockField f;
                    if (!readField(e, std::format("{}.{}[{}]", path, listKey, j), f)) return false;
                    t.fields.push_back(std::move(f));
                }
            }
            if (yyjson_val* values = yyjson_obj_get(v, "values")) {
                if (!yyjson_is_arr(values)) return fail(path + ".values", "expected an array");
                usize j, m;
                yyjson_val* e;
                yyjson_arr_foreach(values, j, m, e) {
                    const std::string vp = std::format("{}.values[{}]", path, j);
                    if (!yyjson_is_obj(e)) return fail(vp, "expected an object");
                    LockEnumValue ev;
                    if (!readString(e, "name", vp, ev.name) || !readBool(e, "tombstone", vp, ev.tombstone)) return false;
                    yyjson_val* val = yyjson_obj_get(e, "value");
                    if (!val || !yyjson_is_int(val)) return fail(vp, "missing integer 'value'");
                    ev.value = yyjson_is_sint(val) ? yyjson_get_sint(val) : static_cast<i64>(yyjson_get_uint(val));
                    t.values.push_back(std::move(ev));
                }
            }
            out.types.emplace(name, std::move(t));
        }
        return validate(out);
    }

    /// Detects hand edits: duplicate ids, deleted entries, counters behind the ids.
    bool validate(const Lock& lock) {
        std::map<u32, std::string> typeIds;
        bool ok = true;
        for (const auto& [name, t] : lock.types) {
            const std::string path = "types." + name;
            if (t.id == 0) ok = fail(path, "type id 0 is invalid");
            if (auto [it, fresh] = typeIds.emplace(t.id, name); !fresh)
                ok = fail(path, std::format("type id {} is also used by '{}'", t.id, it->second));
            std::set<u32> ids;
            std::set<std::string> liveNames;
            for (const LockField& f : t.fields) {
                if (f.id == 0) ok = fail(path, std::format("'{}' has id 0", f.name));
                if (!ids.insert(f.id).second) ok = fail(path, std::format("id {} is used twice", f.id));
                if (f.id >= t.nextField) ok = fail(path, std::format("id {} of '{}' is not below nextField {}", f.id, f.name, t.nextField));
                if (!f.tombstone && !liveNames.insert(f.name).second) ok = fail(path, std::format("'{}' appears twice", f.name));
            }
            for (u32 id = 1; id < t.nextField && ok; ++id) {
                if (!ids.contains(id))
                    ok = fail(path, std::format("entry with id {} is missing; lock entries must never be deleted (restore it from version control)", id));
            }
            std::set<i64> values;
            std::set<std::string> valueNames;
            for (const LockEnumValue& ev : t.values) {
                if (!values.insert(ev.value).second) ok = fail(path, std::format("enum value {} is used twice", ev.value));
                if (!valueNames.insert(ev.name).second) ok = fail(path, std::format("enum value '{}' appears twice", ev.name));
            }
        }
        return ok;
    }

private:
    u32 m_file;
    DiagnosticEngine& m_diags;
};

u32 mintTypeId(const std::string& name, const std::set<u32>& used) {
    for (u32 salt = 0;; ++salt) {
        const u32 id = fnv1a32(salt == 0 ? name : name + "#" + std::to_string(salt));
        if (id != 0 && !used.contains(id)) return id;
    }
}

/// Replaces whole-token occurrences of a type name inside a type signature.
std::string renameInSignature(const std::string& sig, const std::string& from, const std::string& to) {
    std::string out;
    usize i = 0;
    auto isNameChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; };
    while (i < sig.size()) {
        const usize at = sig.find(from, i);
        if (at == std::string::npos) {
            out += sig.substr(i);
            break;
        }
        const bool startOk = at == 0 || !isNameChar(sig[at - 1]);
        const usize end = at + from.size();
        const bool endOk = end == sig.size() || !isNameChar(sig[end]);
        out += sig.substr(i, at - i);
        out += (startOk && endOk) ? to : from;
        i = end;
    }
    return out;
}

class Merger {
public:
    Merger(Schema& s, Lock& l, DiagnosticEngine& d, const LockOptions& o, std::vector<std::string>& c)
        : S(s), L(l), D(d), O(o), C(c) {}

    bool run() {
        const usize before = D.errorCount();
        for (const auto& [name, t] : L.types) m_used.insert(t.id);
        std::map<std::string, const Decl*> claimedBy;
        for (Decl* d : S.decls) {
            if (!d->emitted || !d->isLockable()) continue;
            LockType* lt = findOrCreate(d);
            if (!lt) continue;
            if (auto [it, fresh] = claimedBy.emplace(d->qualifiedName, d); !fresh) {
                D.error(d->loc, std::format("'{}' claims the same lock entry as '{}'", d->qualifiedName, it->second->qualifiedName));
                continue;
            }
            d->typeId = lt->id;
            const std::string kind = lockKindOf(d);
            if (lt->kind != kind) {
                D.error(d->loc, std::format("'{}' was a {} (lock id {}); a type cannot change kind — declare a new type instead",
                                            d->qualifiedName, lt->kind, lt->id));
                continue;
            }
            if (d->version < lt->version) {
                D.error(d->loc, std::format("@version of '{}' decreased from {} to {}", d->qualifiedName, lt->version, d->version));
            } else if (d->version != lt->version) {
                C.push_back(std::format("{}: version {} -> {}", d->qualifiedName, lt->version, d->version));
                lt->version = d->version;
            }
            if (d->kind == DeclKind::Enum || d->kind == DeclKind::Flags) {
                mergeEnum(d, *lt);
            } else if (d->kind == DeclKind::Variant) {
                mergeAlternatives(d, *lt);
            } else {
                mergeFields(d, *lt);
            }
        }
        return D.errorCount() == before;
    }

private:
    LockType* findOrCreate(Decl* d) {
        auto it = L.types.find(d->qualifiedName);
        if (it != L.types.end()) return &it->second;
        for (const std::string& w : d->was) {
            for (const std::string& candidate : {w, d->package.empty() ? w : d->package + "." + w}) {
                auto old = L.types.find(candidate);
                if (old == L.types.end() || S.declsByName.contains(candidate)) continue;
                LockType moved = std::move(old->second);
                L.types.erase(old);
                if (std::find(moved.was.begin(), moved.was.end(), candidate) == moved.was.end()) moved.was.push_back(candidate);
                for (auto& [name, t] : L.types) {
                    for (LockField& f : t.fields) f.type = renameInSignature(f.type, candidate, d->qualifiedName);
                }
                for (LockField& f : moved.fields) f.type = renameInSignature(f.type, candidate, d->qualifiedName);
                C.push_back(std::format("renamed type {} -> {} (id {})", candidate, d->qualifiedName, moved.id));
                return &L.types.emplace(d->qualifiedName, std::move(moved)).first->second;
            }
        }
        LockType t;
        t.id = mintTypeId(d->qualifiedName, m_used);
        m_used.insert(t.id);
        t.kind = lockKindOf(d);
        t.version = d->version;
        if (d->kind == DeclKind::Enum || d->kind == DeclKind::Flags) t.base = std::string(primName(d->underlying));
        C.push_back(std::format("new type {} (id {})", d->qualifiedName, t.id));
        return &L.types.emplace(d->qualifiedName, std::move(t)).first->second;
    }

    void mergeFields(Decl* d, LockType& lt) {
        std::vector<bool> claimed(lt.fields.size(), false);
        auto find = [&](const std::string& name, bool tombstone) -> std::optional<usize> {
            for (usize i = 0; i < lt.fields.size(); ++i) {
                if (!claimed[i] && lt.fields[i].name == name && lt.fields[i].tombstone == tombstone) return i;
            }
            return std::nullopt;
        };
        for (Field& f : d->fields) {
            if (!f.type) continue;
            const std::string sig = f.type->signature;
            const std::string def = f.defaultValue ? f.defaultValue->json : std::string();
            std::optional<usize> idx = find(f.name, false);
            if (!idx) {
                for (const std::string& w : f.was) {
                    if ((idx = find(w, false)) || (idx = find(w, true))) {
                        LockField& lf = lt.fields[*idx];
                        C.push_back(std::format("{}: renamed field {} -> {} (id {})", d->qualifiedName, lf.name, f.name, lf.id));
                        if (std::find(lf.was.begin(), lf.was.end(), lf.name) == lf.was.end()) lf.was.push_back(lf.name);
                        lf.name = f.name;
                        lf.tombstone = false;
                        break;
                    }
                }
            }
            if (!idx && (idx = find(f.name, true))) {
                C.push_back(std::format("{}: revived field {} (id {})", d->qualifiedName, f.name, lt.fields[*idx].id));
                lt.fields[*idx].tombstone = false;
            }
            if (!idx) {
                LockField lf;
                lf.id = lt.nextField++;
                lf.name = f.name;
                lf.type = sig;
                lf.defaultJson = def;
                C.push_back(std::format("{}: new field {} (id {})", d->qualifiedName, f.name, lf.id));
                lt.fields.push_back(std::move(lf));
                claimed.push_back(true);
                f.id = lt.fields.back().id;
                continue;
            }
            LockField& lf = lt.fields[*idx];
            claimed[*idx] = true;
            f.id = lf.id;
            if (lf.type != sig) {
                if (!lf.type.empty() && !isWidening(lf.type, sig)) {
                    D.error(f.loc, std::format("the type of field '{}' changed from '{}' to '{}' (lock id {}); only widenings keep the id "
                                               "(i32→i64, u8→u32, f32→f64, T→T?) — add a new field instead",
                                               f.name, lf.type, sig, lf.id));
                    continue;
                }
                C.push_back(std::format("{}: field {} type {} -> {}", d->qualifiedName, f.name, lf.type, sig));
                lf.type = sig;
            }
            if (lf.defaultJson != def) {
                if (!O.allowDefaultChange) {
                    D.error(f.loc, std::format("the default of field '{}' changed from {} to {} (lock id {}); writers omit default values, so "
                                               "defaults are part of the wire contract (02 §3.4) — pass --allow-default-change once every "
                                               "reader and stored blob tolerates it",
                                               f.name, lf.defaultJson.empty() ? "<implicit>" : lf.defaultJson,
                                               def.empty() ? "<implicit>" : def, lf.id));
                    continue;
                }
                C.push_back(std::format("{}: field {} default {} -> {}", d->qualifiedName, f.name, lf.defaultJson, def));
                lf.defaultJson = def;
            }
        }
        for (usize i = 0; i < lt.fields.size(); ++i) {
            if (!claimed[i] && !lt.fields[i].tombstone) {
                lt.fields[i].tombstone = true;
                C.push_back(std::format("{}: removed field {} (id {} is retired)", d->qualifiedName, lt.fields[i].name, lt.fields[i].id));
            }
        }
        std::sort(lt.fields.begin(), lt.fields.end(), [](const LockField& a, const LockField& b) { return a.id < b.id; });
    }

    void mergeAlternatives(Decl* d, LockType& lt) {
        std::vector<bool> claimed(lt.fields.size(), false);
        for (Alternative& a : d->alternatives) {
            std::optional<usize> idx;
            for (usize i = 0; i < lt.fields.size(); ++i) {
                if (!claimed[i] && lt.fields[i].name == a.name) idx = i;
            }
            if (!idx) {
                LockField lf;
                lf.id = lt.nextField++;
                lf.name = a.name;
                C.push_back(std::format("{}: new alternative {} (id {})", d->qualifiedName, a.name, lf.id));
                lt.fields.push_back(std::move(lf));
                claimed.push_back(true);
                a.id = lt.fields.back().id;
                continue;
            }
            claimed[*idx] = true;
            if (lt.fields[*idx].tombstone) {
                lt.fields[*idx].tombstone = false;
                C.push_back(std::format("{}: revived alternative {}", d->qualifiedName, a.name));
            }
            a.id = lt.fields[*idx].id;
        }
        for (usize i = 0; i < lt.fields.size(); ++i) {
            if (!claimed[i] && !lt.fields[i].tombstone) {
                lt.fields[i].tombstone = true;
                C.push_back(std::format("{}: removed alternative {}", d->qualifiedName, lt.fields[i].name));
            }
        }
        std::sort(lt.fields.begin(), lt.fields.end(), [](const LockField& a, const LockField& b) { return a.id < b.id; });
    }

    void mergeEnum(Decl* d, LockType& lt) {
        const std::string base(primName(d->underlying));
        if (lt.base.empty()) {
            lt.base = base;
        } else if (lt.base != base) {
            if (!isWidening(lt.base, base)) {
                D.error(d->loc, std::format("the underlying type of '{}' changed from {} to {}; only widenings are allowed", d->qualifiedName,
                                            lt.base, base));
                return;
            }
            C.push_back(std::format("{}: underlying type {} -> {}", d->qualifiedName, lt.base, base));
            lt.base = base;
        }
        std::vector<bool> claimed(lt.values.size(), false);
        for (const EnumVal& v : d->values) {
            std::optional<usize> byName;
            for (usize i = 0; i < lt.values.size(); ++i) {
                if (lt.values[i].name == v.name) byName = i;
            }
            if (byName) {
                LockEnumValue& lv = lt.values[*byName];
                claimed[*byName] = true;
                if (lv.value != v.value) {
                    D.error(v.loc, std::format("the value of '{}.{}' changed from {} to {}; enum values are stable (append new values instead)",
                                               d->qualifiedName, v.name, lv.value, v.value));
                    continue;
                }
                if (lv.tombstone) {
                    lv.tombstone = false;
                    C.push_back(std::format("{}: revived value {}", d->qualifiedName, v.name));
                }
                continue;
            }
            bool clash = false;
            for (const LockEnumValue& lv : lt.values) {
                if (lv.value == v.value) {
                    D.error(v.loc, std::format("value {} of '{}.{}' was used by '{}'{}; pick a new value", v.value, d->qualifiedName, v.name,
                                               lv.name, lv.tombstone ? " (removed)" : ""));
                    clash = true;
                }
            }
            if (clash) continue;
            lt.values.push_back(LockEnumValue{v.name, v.value, false});
            claimed.push_back(true);
            C.push_back(std::format("{}: new value {} = {}", d->qualifiedName, v.name, v.value));
        }
        for (usize i = 0; i < claimed.size(); ++i) {
            if (!claimed[i] && !lt.values[i].tombstone) {
                lt.values[i].tombstone = true;
                C.push_back(std::format("{}: removed value {} ({} is retired)", d->qualifiedName, lt.values[i].name, lt.values[i].value));
            }
        }
        std::stable_sort(lt.values.begin(), lt.values.end(), [](const LockEnumValue& a, const LockEnumValue& b) { return a.value < b.value; });
    }

    Schema& S;
    Lock& L;
    DiagnosticEngine& D;
    const LockOptions& O;
    std::vector<std::string>& C;
    std::set<u32> m_used;
};

} // namespace

bool isWidening(std::string_view from, std::string_view to) {
    if (from == to) return true;
    if (from.ends_with('?') && to.ends_with('?')) return isWidening(from.substr(0, from.size() - 1), to.substr(0, to.size() - 1));
    if (to.ends_with('?')) return isWidening(from, to.substr(0, to.size() - 1));
    static const std::vector<std::vector<std::string_view>> kChains = {
        {"i8", "i16", "i32", "i64"}, {"u8", "u16", "u32", "u64"}, {"f32", "f64"}};
    for (const auto& chain : kChains) {
        const auto a = std::find(chain.begin(), chain.end(), from);
        const auto b = std::find(chain.begin(), chain.end(), to);
        if (a != chain.end() && b != chain.end() && a < b) return true;
    }
    return false;
}

bool loadLock(std::string_view text, u32 fileIndex, Lock& out, DiagnosticEngine& diags) {
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(text.data()), text.size(),
                                       YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, nullptr, &err);
    if (!doc) {
        u32 line = 1, col = 1;
        for (usize i = 0; i < err.pos && i < text.size(); ++i) {
            if (text[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        diags.error(SourceLoc{fileIndex, line, col}, std::format("schema lock is not valid JSONC: {}", err.msg ? err.msg : "parse error"));
        return false;
    }
    LockReader reader(fileIndex, diags);
    const bool ok = reader.read(yyjson_doc_get_root(doc), out);
    yyjson_doc_free(doc);
    return ok;
}

std::string writeLock(const Lock& lock) {
    JsonOut o;
    o.beginObject();
    o.key("format");
    o.num(1);
    o.key("types");
    o.beginObject();
    for (const auto& [name, t] : lock.types) {
        o.key(name);
        o.beginObject();
        o.key("id");
        o.unum(t.id);
        o.key("kind");
        o.str(t.kind);
        if (t.version != 0) {
            o.key("version");
            o.unum(t.version);
        }
        if (!t.base.empty()) {
            o.key("base");
            o.str(t.base);
        }
        if (!t.was.empty()) {
            o.key("was");
            o.beginArray(true);
            for (const std::string& w : t.was) o.str(w);
            o.endArray();
        }
        if (t.kind == "enum" || t.kind == "flags") {
            o.key("values");
            o.beginArray();
            for (const LockEnumValue& v : t.values) {
                o.beginObject(true);
                o.key("name");
                o.str(v.name);
                o.key("value");
                o.num(v.value);
                if (v.tombstone) {
                    o.key("tombstone");
                    o.boolean(true);
                }
                o.endObject();
            }
            o.endArray();
        } else {
            o.key("nextField");
            o.unum(t.nextField);
            o.key(t.kind == "variant" ? "alternatives" : "fields");
            o.beginArray();
            for (const LockField& f : t.fields) {
                o.beginObject(true);
                o.key("id");
                o.unum(f.id);
                o.key("name");
                o.str(f.name);
                if (!f.type.empty()) {
                    o.key("type");
                    o.str(f.type);
                }
                if (!f.defaultJson.empty()) {
                    o.key("default");
                    o.str(f.defaultJson);
                }
                if (f.tombstone) {
                    o.key("tombstone");
                    o.boolean(true);
                }
                if (!f.was.empty()) {
                    o.key("was");
                    o.beginArray(true);
                    for (const std::string& w : f.was) o.str(w);
                    o.endArray();
                }
                o.endObject();
            }
            o.endArray();
        }
        o.endObject();
    }
    o.endObject();
    o.endObject();
    return "// helios-schemac schema lock (docs/plan/02-engine-runtime.md §3.4). Generated; append-only.\n"
           "// Commit this file. Never edit or delete entries: ids must never be reused. CI runs --check-lock.\n" +
           o.take();
}

bool applyLock(Schema& schema, Lock& lock, DiagnosticEngine& diags, const LockOptions& options, std::vector<std::string>& changes) {
    Merger m(schema, lock, diags, options, changes);
    return m.run();
}

} // namespace helios::schemac
