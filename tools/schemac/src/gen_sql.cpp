// `--emit sql` (02 §3.5; 05 §3): PostgreSQL DDL for the structs marked `@sql(schema="svc_<service>")`.
// Per service schema, into --sql-out/<schema>/:
//   * schema.sql: the tables as the current schemas define them (a snapshot; CI applies it);
//   * migration.sql: a goose stub from the baseline lock (the lock as it was before this run, or
//     --sql-baseline) to now. It only expands (05 §3.3): new tables and columns, renames and type
//     widenings; removed fields become comments for the contract release, never a DROP.
// Columns follow the lock's field ids, so a table built by the migrations has the snapshot's layout.

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <optional>

#include "code_writer.h"
#include "generators.h"
#include "lock.h"
#include "text.h"

namespace helios::schemac {

namespace {

/// Key words PostgreSQL does not accept as column names (pg_get_keywords() catcode R and T in PostgreSQL 16);
/// such columns are quoted.
constexpr std::string_view kReserved[] = {
    "all", "analyse", "analyze", "and", "any", "array", "as", "asc", "asymmetric", "authorization", "binary", "both",
    "case", "cast", "check", "collate", "collation", "column", "concurrently", "constraint", "create", "cross",
    "current_catalog", "current_date", "current_role", "current_schema", "current_time", "current_timestamp",
    "current_user", "default", "deferrable", "desc", "distinct", "do", "else", "end", "except", "false", "fetch",
    "for", "foreign", "freeze", "from", "full", "grant", "group", "having", "ilike", "in", "initially", "inner",
    "intersect", "into", "is", "isnull", "join", "lateral", "leading", "left", "like", "limit", "localtime",
    "localtimestamp", "natural", "not", "notnull", "null", "offset", "on", "only", "or", "order", "outer", "overlaps",
    "placing", "primary", "references", "returning", "right", "select", "session_user", "similar", "some",
    "symmetric", "system_user", "table", "tablesample", "then", "to", "trailing", "true", "union", "unique", "user",
    "using", "variadic", "verbose", "when", "where", "window", "with",
};

std::string ident(const std::string& name) {
    const bool reserved = std::find(std::begin(kReserved), std::end(kReserved), name) != std::end(kReserved);
    return reserved ? "\"" + name + "\"" : name;
}

std::string sqlString(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') out += '\'';
        out += c;
    }
    return out + "'";
}

/// A column as PostgreSQL stores it. `check` uses `$` for the column name.
struct Column {
    std::string type;
    bool notNull = true;
    std::string defaultSql; ///< "" = none
    std::string check;      ///< "" = none
    bool json = false;
    bool keyable = true;
};

Column integerColumn(Prim p) {
    switch (p) {
    case Prim::I8: return {"SMALLINT", true, "0", "$ BETWEEN -128 AND 127"};
    case Prim::I16: return {"SMALLINT", true, "0", ""};
    case Prim::I32: return {"INTEGER", true, "0", ""};
    case Prim::I64: return {"BIGINT", true, "0", ""};
    case Prim::U8: return {"SMALLINT", true, "0", "$ BETWEEN 0 AND 255"};
    case Prim::U16: return {"INTEGER", true, "0", "$ BETWEEN 0 AND 65535"};
    case Prim::U32: return {"BIGINT", true, "0", "$ BETWEEN 0 AND 4294967295"};
    default: return {"NUMERIC(20)", true, "0", "$ BETWEEN 0 AND 18446744073709551615"};
    }
}

std::string floatSql(f64 v, bool f32) {
    if (std::isnan(v)) return "'NaN'";
    if (std::isinf(v)) return v > 0 ? "'Infinity'" : "'-Infinity'";
    return f32 ? formatF32(static_cast<float>(v)) : formatF64(v);
}

/// The column of a field type, or nullopt (with `why`) when it cannot be stored.
std::optional<Column> columnOf(const Type* t, std::string& why) {
    switch (t->kind) {
    case TypeKind::Prim:
        switch (t->prim) {
        case Prim::Bool: return Column{"BOOLEAN", true, "FALSE", ""};
        case Prim::F32: return Column{"REAL", true, "0", ""};
        case Prim::F64: return Column{"DOUBLE PRECISION", true, "0", ""};
        case Prim::String:
        case Prim::Name: return Column{"TEXT", true, "''", ""};
        default: return integerColumn(t->prim);
        }
    case TypeKind::Builtin:
        switch (t->builtin) {
        case Builtin::Guid: return Column{"UUID", true, "'00000000-0000-0000-0000-000000000000'", ""};
        case Builtin::EntityId: return Column{"BIGINT", true, "0", "$ >= 0"};
        case Builtin::Tick: return Column{"BIGINT", true, "0", "$ >= 0"};
        case Builtin::Duration: return Column{"BIGINT", true, "0", ""}; // nanoseconds
        case Builtin::LocString:
        case Builtin::TagQuery:
        case Builtin::HxlExpr: return Column{"TEXT", true, "''", ""};
        case Builtin::NetHandle:
            why = "a NetHandle is scoped to one zone instance (04 §4.6) and cannot be stored; store the EntityId";
            return std::nullopt;
        default: return Column{"JSONB", false, "", "jsonb_typeof($) = 'array'", true, false}; // math tuples, WorldPos, TagSet
        }
    case TypeKind::AssetRef: return Column{"UUID", true, "'00000000-0000-0000-0000-000000000000'", ""};
    case TypeKind::RecordRef: return Column{"BIGINT", true, "0", "$ >= 0"};
    case TypeKind::Enum:
    case TypeKind::Flags: return integerColumn(t->decl->underlying);
    case TypeKind::Optional: {
        std::optional<Column> c = columnOf(t->element, why);
        if (!c) return c;
        c->notNull = false;
        c->defaultSql.clear();
        c->keyable = false;
        return c;
    }
    case TypeKind::Map:
    case TypeKind::Struct: return Column{"JSONB", false, "", "jsonb_typeof($) = 'object'", true, false};
    case TypeKind::Variant: return Column{"JSONB", false, "", "", true, false};
    default: return Column{"JSONB", false, "", "jsonb_typeof($) = 'array'", true, false}; // list, set, keyed list, T[N]
    }
}

/// Column of a lock signature, for the prims (and their optionals) that widenings change.
std::optional<Column> columnOfSignature(std::string_view sig, const Type* current) {
    const bool optional = sig.ends_with('?');
    if (optional) sig.remove_suffix(1);
    std::optional<Column> c;
    static constexpr Prim kPrims[] = {Prim::I8, Prim::I16, Prim::I32, Prim::I64, Prim::U8, Prim::U16, Prim::U32, Prim::U64, Prim::F32, Prim::F64};
    for (const Prim p : kPrims) {
        if (primName(p) == sig) {
            std::string ignored;
            Type t;
            t.kind = TypeKind::Prim;
            t.prim = p;
            c = columnOf(&t, ignored);
        }
    }
    if (!c && current && current->kind == TypeKind::Optional && current->element->signature == sig) {
        std::string ignored;
        c = columnOf(current->element, ignored);
    }
    if (c && optional) {
        c->notNull = false;
        c->defaultSql.clear();
    }
    return c;
}

std::string defaultOf(const Field& f, const Column& c) {
    if (!c.notNull || c.json) return {};
    if (!f.defaultValue) {
        if (f.type->kind == TypeKind::Enum && !f.type->decl->values.empty()) return std::to_string(f.type->decl->values.front().value);
        return c.defaultSql;
    }
    const Value& v = *f.defaultValue;
    switch (v.kind) {
    case Value::Kind::Bool: return v.b ? "TRUE" : "FALSE";
    case Value::Kind::Int:
    case Value::Kind::Duration: return std::to_string(v.i);
    case Value::Kind::UInt:
    case Value::Kind::Flags: return std::to_string(v.u);
    case Value::Kind::Float: return floatSql(v.f, f.type->kind == TypeKind::Prim && f.type->prim == Prim::F32);
    case Value::Kind::String: return sqlString(v.s);
    case Value::Kind::Guid: return sqlString(formatGuid(v.u, v.u2));
    case Value::Kind::Enum:
        for (const EnumVal& e : f.type->decl->values) {
            if (e.name == v.s) return std::to_string(e.value);
        }
        return c.defaultSql;
    default: return c.defaultSql;
    }
}

std::string constraintName(const std::string& table, const std::string& column) {
    std::string name = table + "_" + column + "_check";
    if (name.size() > 63) name = name.substr(0, 54) + std::format("_{:08x}", fnv1a32(name));
    return name;
}

struct TableCol {
    const Field* field;
    std::string name; ///< snake_case column name
    Column col;
};

struct Table {
    const Decl* decl;
    std::string schema, name;
    std::vector<TableCol> cols; ///< by field id
    std::vector<std::string> key;
    const LockType* baseline = nullptr; ///< the table's entry in the baseline lock (nullptr: a new table)
};

class SqlGenerator {
public:
    SqlGenerator(const Schema& s, const Lock& baseline, const CompileOptions& o, DiagnosticEngine& d) : S(s), B(baseline), O(o), D(d) {}

    std::vector<OutputFile> run() {
        const usize errorsBefore = D.errorCount();
        std::map<std::string, std::vector<Table>> schemas;
        std::map<std::string, const Decl*> seen;
        for (const Decl* d : S.decls) {
            if (!d->emitted || d->sqlTable.empty()) continue;
            if (auto [it, fresh] = seen.emplace(d->sqlTable, d); !fresh) {
                D.error(d->loc, std::format("'{}' and '{}' both map to the table {}", it->second->qualifiedName, d->qualifiedName, d->sqlTable));
                continue;
            }
            if (std::optional<Table> t = table(d)) schemas[t->schema].push_back(std::move(*t));
        }
        std::vector<OutputFile> out;
        if (D.errorCount() != errorsBefore) return out;
        for (auto& [schema, tables] : schemas) {
            std::sort(tables.begin(), tables.end(), [](const Table& a, const Table& b) { return a.name < b.name; });
            out.push_back(OutputFile{join(schema + "/schema.sql"), snapshot(schema, tables)});
            out.push_back(OutputFile{join(schema + "/migration.sql"), migration(schema, tables)});
        }
        return out;
    }

private:
    std::string join(const std::string& rel) const { return O.sqlOut.empty() || O.sqlOut == "." ? rel : O.sqlOut + "/" + rel; }

    std::optional<Table> table(const Decl* d) {
        Table t;
        t.decl = d;
        const usize dot = d->sqlTable.find('.');
        t.schema = d->sqlTable.substr(0, dot);
        t.name = d->sqlTable.substr(dot + 1);
        std::vector<const Field*> fields;
        for (const Field& f : d->fields) fields.push_back(&f);
        std::sort(fields.begin(), fields.end(), [](const Field* a, const Field* b) { return a->id < b->id; });
        std::map<std::string, const Field*> names;
        bool ok = true;
        for (const Field* f : fields) {
            std::string why;
            std::optional<Column> c = columnOf(f->type, why);
            if (!c) {
                D.error(f->loc, std::format("field '{}' of '{}' cannot be a column of {}: {}", f->name, d->name, d->sqlTable, why));
                ok = false;
                continue;
            }
            const std::string name = snakeCase(f->name);
            if (name.size() > 63) { // PostgreSQL would truncate it silently
                D.error(f->loc, std::format("the column name of field '{}' is longer than PostgreSQL's 63 bytes", f->name));
                ok = false;
            }
            if (auto [it, fresh] = names.emplace(name, f); !fresh) {
                D.error(f->loc, std::format("fields '{}' and '{}' both become the column {}", it->second->name, f->name, name));
                ok = false;
            }
            c->defaultSql = defaultOf(*f, *c);
            t.cols.push_back(TableCol{f, name, *c});
        }
        // Key: fields marked @key, or @key(a, b) on the struct.
        if (const Attr* k = d->attr("key"); k && !k->args.empty()) {
            for (const AttrArg& a : k->args) {
                const auto it = std::find_if(t.cols.begin(), t.cols.end(), [&](const TableCol& c) { return c.field->name == a.value; });
                if (it == t.cols.end()) {
                    D.error(a.loc, std::format("@key names '{}', which is not a field of '{}'", a.value, d->name));
                    ok = false;
                } else {
                    t.key.push_back(it->name);
                }
            }
        }
        for (const TableCol& c : t.cols) {
            if (!c.field->attr("key")) continue;
            if (std::find(t.key.begin(), t.key.end(), c.name) == t.key.end()) t.key.push_back(c.name);
        }
        for (const TableCol& c : t.cols) {
            const bool isKey = std::find(t.key.begin(), t.key.end(), c.name) != t.key.end();
            if (isKey && !c.col.keyable) {
                D.error(c.field->loc, std::format("key field '{}' of '{}' must be a non-optional scalar (it is '{}')", c.field->name, d->name,
                                                  c.field->type->signature));
                ok = false;
            }
        }
        if (t.key.empty()) {
            D.error(d->loc, std::format("'{}' is @sql but has no key: mark its key fields @key (every table has a primary key)", d->name));
            ok = false;
        }
        for (const auto& [name, lt] : B.types) {
            if (lt.sql == d->sqlTable) t.baseline = &lt;
        }
        if (t.baseline) {
            for (const TableCol& c : t.cols) {
                const bool known = std::any_of(t.baseline->fields.begin(), t.baseline->fields.end(),
                                               [&](const LockField& f) { return f.id == c.field->id && !f.tombstone; });
                if (!known && isKey(t, c)) {
                    D.error(c.field->loc, std::format("new field '{}' cannot join the key of the existing table {}; changing a primary key "
                                                      "needs a hand-written migration",
                                                      c.field->name, d->sqlTable));
                    ok = false;
                }
            }
        }
        if (!ok) return std::nullopt;
        return t;
    }

    static std::string check(const std::string& expr, const std::string& column) {
        std::string out;
        for (const char c : expr) out += c == '$' ? column : std::string(1, c);
        return out;
    }

    /// `name TYPE [NOT NULL] [DEFAULT x] [CONSTRAINT c CHECK (…)]`.
    std::string columnDef(const Table& t, const TableCol& c, bool isKey) const {
        std::string s = std::format("{} {}", ident(c.name), c.col.type);
        if (c.col.notNull) s += " NOT NULL";
        if (!c.col.defaultSql.empty() && !isKey) s += " DEFAULT " + c.col.defaultSql;
        if (!c.col.check.empty()) s += std::format(" CONSTRAINT {} CHECK ({})", constraintName(t.name, c.name), check(c.col.check, ident(c.name)));
        return s;
    }

    static bool isKey(const Table& t, const TableCol& c) { return std::find(t.key.begin(), t.key.end(), c.name) != t.key.end(); }

    std::string header(const std::string& schema, std::string_view what) const {
        std::vector<std::string> files;
        for (const auto& fp : S.files) {
            if (fp->generate) files.push_back(fp->logicalPath);
        }
        return std::format("-- {} of {}: generated by helios-schemac --emit sql from {}. DO NOT EDIT.\n"
                           "-- Tables of the structs marked @sql (tools/schemac/README.md \"Generated SQL\"; 05 §3).\n",
                           what, schema, helios::schemac::join(files, ", "));
    }

    void createTable(CodeWriter& w, const Table& t) const {
        const std::string docLine = t.decl->doc.substr(0, t.decl->doc.find('\n'));
        w.line(std::format("-- {} (lock id {}){}", t.decl->qualifiedName, t.decl->typeId, docLine.empty() ? "" : ": " + docLine));
        w.open(std::format("CREATE TABLE {}.{} (", t.schema, ident(t.name)));
        for (const TableCol& c : t.cols) {
            w.line(columnDef(t, c, isKey(t, c)) + "," + (c.col.json ? " -- canonical JSONC; NULL = the field's default" : ""));
        }
        std::string key;
        for (const std::string& k : t.key) key += (key.empty() ? "" : ", ") + ident(k);
        w.line(std::format("PRIMARY KEY ({})", key));
        w.close(");");
    }

    std::string snapshot(const std::string& schema, const std::vector<Table>& tables) const {
        CodeWriter w;
        w.raw(header(schema, "Snapshot"));
        w.line(std::format("-- Applies to an empty database; the goose migrations reach the same tables. Column order is lock-id order."));
        w.line();
        w.line(std::format("CREATE SCHEMA IF NOT EXISTS {};", schema));
        for (const Table& t : tables) {
            w.line();
            createTable(w, t);
        }
        return w.take();
    }

    std::string migration(const std::string& schema, const std::vector<Table>& tables) const {
        CodeWriter up, down;
        std::vector<std::string> downs; // reversed at the end
        std::vector<std::string> contract;
        for (const Table& t : tables) {
            const std::string qt = t.schema + "." + ident(t.name);
            if (!t.baseline) {
                up.line();
                createTable(up, t);
                downs.push_back(std::format("DROP TABLE IF EXISTS {};", qt));
                continue;
            }
            std::map<u32, const LockField*> old;
            for (const LockField& f : t.baseline->fields) old.emplace(f.id, &f);
            std::vector<std::string> steps;
            for (const TableCol& c : t.cols) {
                const auto it = old.find(c.field->id);
                if (it == old.end() || it->second->tombstone) {
                    const bool revived = it != old.end();
                    std::string def = columnDef(t, c, false);
                    if (revived) def = "IF NOT EXISTS " + def;
                    steps.push_back(std::format("ALTER TABLE {} ADD COLUMN {};{}", qt, def, revived ? " -- revived field (lock id " + std::to_string(c.field->id) + ")" : ""));
                    downs.push_back(std::format("ALTER TABLE {} DROP COLUMN IF EXISTS {};", qt, ident(c.name)));
                    continue;
                }
                const LockField& lf = *it->second;
                const bool retyped = lf.type != c.field->type->signature;
                const std::optional<Column> was = retyped ? columnOfSignature(lf.type, c.field->type) : std::optional<Column>(c.col);
                const std::string oldName = snakeCase(lf.name);
                if (oldName != c.name) {
                    steps.push_back(std::format("ALTER TABLE {} RENAME COLUMN {} TO {};", qt, ident(oldName), ident(c.name)));
                    downs.push_back(std::format("ALTER TABLE {} RENAME COLUMN {} TO {};", qt, ident(c.name), ident(oldName)));
                    if (was && !was->check.empty()) { // the CHECK keeps the snapshot's name, which follows the column
                        const std::string from = constraintName(t.name, oldName);
                        const std::string to = constraintName(t.name, c.name);
                        steps.push_back(std::format("ALTER TABLE {} RENAME CONSTRAINT {} TO {};", qt, from, to));
                        downs.push_back(std::format("ALTER TABLE {} RENAME CONSTRAINT {} TO {};", qt, to, from));
                    }
                }
                if (retyped) {
                    if (!was) {
                        steps.push_back(std::format("-- TODO: {}.{} changed from {} to {}; write this step by hand", qt, c.name, lf.type,
                                                    c.field->type->signature));
                        continue;
                    }
                    alterColumn(steps, downs, t, c, *was);
                }
                if (lf.defaultJson != (c.field->defaultValue ? c.field->defaultValue->json : std::string()) && !c.col.defaultSql.empty() &&
                    !isKey(t, c)) {
                    steps.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} SET DEFAULT {};", qt, ident(c.name), c.col.defaultSql));
                }
            }
            for (const LockField& f : t.baseline->fields) {
                if (f.tombstone) continue;
                const bool live = std::any_of(t.cols.begin(), t.cols.end(), [&](const TableCol& c) { return c.field->id == f.id; });
                if (!live)
                    contract.push_back(std::format("--   ALTER TABLE {} DROP COLUMN {}; -- field {} (lock id {}) was removed", qt,
                                                   ident(snakeCase(f.name)), f.name, f.id));
            }
            if (!steps.empty()) {
                up.line();
                up.line(std::format("-- {} (lock id {})", t.decl->qualifiedName, t.decl->typeId));
                for (const std::string& s : steps) up.line(s);
            }
        }
        CodeWriter w;
        w.raw(header(schema, "Migration stub"));
        w.line("-- Expands the schema from the baseline lock to the current one (05 §3.3: release N only adds). Copy it to");
        w.line(std::format("-- services/migrations/<service>/<version>_<name>.sql and review it; rerunning schemac against the"));
        w.line("-- updated lock yields an empty stub.");
        w.line();
        w.line("-- +goose Up");
        if (up.str().empty()) {
            w.line("-- No changes since the baseline lock.");
            w.line("SELECT 1;");
        } else {
            w.raw(up.str().substr(1)); // (without the leading blank line)
        }
        if (!contract.empty()) {
            w.line();
            w.line("-- Contract release (N+2, 05 §3.3), once no reader uses these columns:");
            for (const std::string& c : contract) w.line(c);
        }
        w.line();
        w.line("-- +goose Down");
        if (downs.empty()) w.line("SELECT 1;");
        for (auto it = downs.rbegin(); it != downs.rend(); ++it) w.line(*it);
        return w.take();
    }

    void alterColumn(std::vector<std::string>& steps, std::vector<std::string>& downs, const Table& t, const TableCol& c, const Column& was) const {
        const std::string qt = t.schema + "." + ident(t.name);
        const std::string col = ident(c.name);
        const std::string cname = constraintName(t.name, c.name);
        std::vector<std::string> rev;
        if (was.check != c.col.check && !was.check.empty()) {
            steps.push_back(std::format("ALTER TABLE {} DROP CONSTRAINT IF EXISTS {};", qt, cname));
        }
        if (was.type != c.col.type) {
            steps.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} TYPE {};", qt, col, c.col.type));
            rev.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} TYPE {};", qt, col, was.type));
        }
        if (was.check != c.col.check && !c.col.check.empty()) {
            steps.push_back(std::format("ALTER TABLE {} ADD CONSTRAINT {} CHECK ({});", qt, cname, check(c.col.check, col)));
        }
        if (was.check != c.col.check) {
            rev.insert(rev.begin(), std::format("ALTER TABLE {} DROP CONSTRAINT IF EXISTS {};", qt, cname));
            if (!was.check.empty()) rev.push_back(std::format("ALTER TABLE {} ADD CONSTRAINT {} CHECK ({});", qt, cname, check(was.check, col)));
        }
        if (was.notNull && !c.col.notNull) {
            steps.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} DROP DEFAULT;", qt, col));
            steps.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} DROP NOT NULL;", qt, col));
            rev.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} SET NOT NULL;", qt, col));
            if (!was.defaultSql.empty()) rev.push_back(std::format("ALTER TABLE {} ALTER COLUMN {} SET DEFAULT {};", qt, col, was.defaultSql));
        }
        // downs are replayed in reverse, so push this column's reverse steps back to front.
        for (auto it = rev.rbegin(); it != rev.rend(); ++it) downs.push_back(*it);
    }

    const Schema& S;
    const Lock& B;
    const CompileOptions& O;
    DiagnosticEngine& D;
};

} // namespace

std::vector<OutputFile> generateSql(const Schema& schema, const Lock& baseline, const CompileOptions& options, DiagnosticEngine& diags) {
    SqlGenerator g(schema, baseline, options, diags);
    return g.run();
}

} // namespace helios::schemac
