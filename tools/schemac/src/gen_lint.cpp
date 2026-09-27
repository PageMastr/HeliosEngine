// `--emit lint` (02 §3.5: "SEC-1/SEC-4, ledger/persist, keyed lists, naming, size budgets" for CI).
// The rule lints run on every compilation and fail it (sema); this emitter adds the size budgets and
// writes a report CI keeps: what each rule checked, the AAA-SEC-1 classification of every
// client→server message, and every finding, each with a rule id and a location.
//
// Size budgets (warnings; --Werror makes them errors):
//   * size.unbounded: a string, list, set, map or TagSet reachable from a network-facing type (rpc
//     arguments, events, messages, replicated fields of components) needs @max(n); such data is
//     hostile input, and an unbounded container lets one peer make the receiver allocate at will;
//   * size.unreliable: an unreliable rpc's worst-case tagged payload fits one netcode payload,
//     1,200 B (04 §1), since an unreliable message is never fragmented.

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include "generators.h"
#include "json_out.h"
#include "text.h"

namespace helios::schemac {

namespace {

constexpr u64 kUnreliablePayload = 1200; ///< netcode payload bytes (04 §1)
constexpr u32 kMaxDepth = 64;             ///< the tagged reader's nesting limit (worst cases past it are unbounded)

struct Finding {
    std::string rule;
    SourceLoc loc;
    std::string message;
};

u64 varintMax(u64 v) {
    u64 n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

class LintGenerator {
public:
    LintGenerator(const Schema& s, const CompileOptions& o, DiagnosticEngine& d) : S(s), O(o), D(d) {}

    std::vector<OutputFile> run() {
        std::vector<const Decl*> network;
        for (const Decl* d : S.decls) {
            if (!d->emitted) continue;
            const bool net = d->kind == DeclKind::Rpc || d->kind == DeclKind::Event || d->kind == DeclKind::Message || d->isReplicatedComponent();
            if (net) network.push_back(d);
            if (d->kind == DeclKind::Rpc) ++m_counts["sec1.rpcs"];
            if (d->kind == DeclKind::Rpc && d->direction == "client->server") ++m_counts["sec1.client-to-server"];
            if (d->attr("store") && !d->attr("store")->args.empty() && d->attr("store")->args[0].value == "ledger") ++m_counts["ledger.types"];
            if (d->kind == DeclKind::ScriptFn) ++m_counts["fuel.fns"];
            for (const Field& f : d->fields) {
                if (f.attr("keyed")) ++m_counts["keyed.lists"];
                if (!f.serverOnly && f.type) ++m_counts["sec4.fields"];
            }
        }
        for (const Decl* d : network) {
            std::set<const Decl*> seen;
            for (const Field& f : d->fields) {
                if (d->isReplicatedComponent() && !f.replicated) continue;
                unbounded(d, f, seen, 0);
            }
            if (d->kind == DeclKind::Rpc && d->attr("unreliable")) {
                const std::optional<u64> size = messageSize(d, 0);
                ++m_counts["size.unreliable-rpcs"];
                if (!size || *size > kUnreliablePayload) {
                    add("size.unreliable", d->loc,
                        std::format("unreliable rpc '{}' has a worst-case payload of {} (budget {} B, one netcode payload, 04 §1): bound its "
                                    "fields with @max or make it reliable",
                                    d->name, size ? std::to_string(*size) + " B" : std::string("unbounded"), kUnreliablePayload));
                }
            }
        }
        for (const Finding& f : m_findings) D.warning(f.loc, std::format("[{}] {}", f.rule, f.message));
        return {OutputFile{O.lintOut, report()}};
    }

private:
    /// Records a finding once (a struct reached from several network types is reported once).
    void add(std::string rule, SourceLoc loc, std::string message) {
        for (const Finding& f : m_findings) {
            if (f.rule == rule && f.loc.file == loc.file && f.loc.line == loc.line && f.loc.col == loc.col) return;
        }
        m_findings.push_back(Finding{std::move(rule), loc, std::move(message)});
    }

    /// Strings, text-like builtins and containers other than T[N] need @max on network input.
    static bool needsMax(const Type* t) {
        if (t->kind == TypeKind::Optional) t = t->element;
        if (t->kind == TypeKind::Prim) return t->prim == Prim::String || t->prim == Prim::Name;
        if (t->kind == TypeKind::Builtin)
            return t->builtin == Builtin::TagSet || t->builtin == Builtin::LocString || t->builtin == Builtin::TagQuery || t->builtin == Builtin::HxlExpr;
        return t->isContainer() && t->kind != TypeKind::Array;
    }

    /// size.unbounded for field `f` of `owner`, then for the fields of every struct or variant it
    /// reaches (through containers and optionals), once each. @max bounds the field's own length or
    /// count; the language has no bound for the elements of a list of strings (the worst-case size
    /// of such a field is unbounded, which size.unreliable reports).
    void unbounded(const Decl* owner, const Field& f, std::set<const Decl*>& seen, u32 depth) {
        if (depth > kMaxDepth || !f.type) return;
        if (needsMax(f.type)) {
            ++m_counts["size.unbounded-checked"];
            if (!f.attr("max"))
                add("size.unbounded", f.loc,
                    std::format("'{}.{}' ({}) is {} without @max: bound it so a peer cannot make the receiver allocate at will", owner->name, f.name,
                                f.type->signature, depth == 0 ? "network input" : "reachable from network input"));
        }
        for (const Type* t = f.type; t; t = t->element) {
            if ((t->kind != TypeKind::Struct && t->kind != TypeKind::Variant) || !seen.insert(t->decl).second) continue;
            for (const Field& inner : t->decl->fields) unbounded(t->decl, inner, seen, depth + 1);
            for (const Alternative& a : t->decl->alternatives) {
                for (const Field& inner : a.type->fields) unbounded(a.type, inner, seen, depth + 1);
            }
        }
    }

    static std::optional<u64> maxOf(const Field* f) {
        if (!f) return std::nullopt;
        const Attr* m = f->attr("max");
        u64 n = 0;
        if (!m || m->args.empty() || !parseSchemaUnsigned(m->args[0].value, n)) return std::nullopt;
        return n;
    }

    static std::optional<u64> add(std::optional<u64> a, std::optional<u64> b) {
        if (!a || !b) return std::nullopt;
        return *a + *b;
    }

    /// Worst-case tagged bytes of a struct's fields (each with its tag), or nullopt if unbounded.
    std::optional<u64> messageSize(const Decl* d, u32 depth) {
        if (depth > kMaxDepth) return std::nullopt;
        u64 total = 0;
        for (const Field& f : d->fields) {
            const std::optional<u64> v = valueSize(f.type, &f, depth);
            if (!v) return std::nullopt;
            total += varintMax(static_cast<u64>(f.id) << 3) + *v;
        }
        return total;
    }

    /// Worst-case bytes of one value after its tag (LEN values include their length prefix).
    std::optional<u64> valueSize(const Type* t, const Field* f, u32 depth) {
        if (!t) return std::nullopt;
        switch (t->kind) {
        case TypeKind::Prim:
            switch (t->prim) {
            case Prim::F32: return 4;
            case Prim::F64: return 8;
            case Prim::String:
            case Prim::Name: {
                const std::optional<u64> n = maxOf(f);
                if (!n) return std::nullopt;
                return varintMax(*n) + *n; // @max counts bytes on the wire
            }
            default: return 10;
            }
        case TypeKind::Builtin:
            switch (t->builtin) {
            case Builtin::Guid: return 17;
            case Builtin::EntityId: return 8;
            case Builtin::NetHandle:
            case Builtin::Tick:
            case Builtin::Duration: return 10;
            default: {
                const u32 n = tupleSize(t->builtin);
                if (n == 0) return maxOf(f) ? std::optional<u64>(varintMax(*maxOf(f)) + *maxOf(f)) : std::nullopt; // text-like
                const u64 bytes = static_cast<u64>(n) * (tupleIsF64(t->builtin) ? 8 : 4);
                return varintMax(bytes) + bytes;
            }
            }
        case TypeKind::Enum:
        case TypeKind::Flags: return 10;
        case TypeKind::RecordRef: return 8;
        case TypeKind::AssetRef: return 17;
        case TypeKind::Optional: return valueSize(t->element, f, depth);
        case TypeKind::Struct: {
            const std::optional<u64> body = messageSize(t->decl, depth + 1);
            if (!body) return std::nullopt;
            return varintMax(*body) + *body;
        }
        case TypeKind::Variant: {
            u64 best = 0;
            for (const Alternative& a : t->decl->alternatives) {
                const std::optional<u64> body = messageSize(a.type, depth + 1);
                if (!body) return std::nullopt;
                best = std::max(best, varintMax(static_cast<u64>(a.id) << 3) + varintMax(*body) + *body);
            }
            return varintMax(best) + best;
        }
        case TypeKind::Array: {
            const std::optional<u64> e = valueSize(t->element, nullptr, depth + 1);
            if (!e) return std::nullopt;
            return varintMax(t->arraySize * (*e + 5)) + t->arraySize * (*e + 5);
        }
        default: { // list, set, keyed list, map: one entry (with its tag) per element
            const std::optional<u64> n = maxOf(f);
            if (!n) return std::nullopt;
            std::optional<u64> e = valueSize(t->element, nullptr, depth + 1);
            if (t->key) e = add(e, valueSize(t->key, nullptr, depth + 1));
            if (!e) return std::nullopt;
            return *n * (*e + 5 + 17); // entry tag and length, and a keyed list's key
        }
        }
    }

    std::string report() {
        JsonOut o;
        o.beginObject();
        o.key("format");
        o.num(1);
        o.key("comment");
        o.str("Generated by helios-schemac --emit lint (02 §3.5; tools/schemac/README.md \"Lint report\"). DO NOT EDIT.");
        o.key("files");
        o.beginArray(true);
        for (const auto& fp : S.files) {
            if (fp->generate) o.str(fp->logicalPath);
        }
        o.endArray();
        o.key("checked");
        o.beginObject();
        for (const char* k : {"sec1.rpcs", "sec1.client-to-server", "sec4.fields", "ledger.types", "keyed.lists", "fuel.fns",
                              "size.unbounded-checked", "size.unreliable-rpcs"}) {
            o.key(k);
            o.unum(m_counts[k]);
        }
        o.endObject();
        // AAA-SEC-1: every client->server message is classified (sema rejects one without @ratelimit and @intent).
        o.key("clientToServer");
        o.beginArray();
        std::vector<const Decl*> rpcs;
        for (const Decl* d : S.decls) {
            if (d->emitted && d->kind == DeclKind::Rpc && d->direction == "client->server") rpcs.push_back(d);
        }
        std::sort(rpcs.begin(), rpcs.end(), [](const Decl* a, const Decl* b) { return a->qualifiedName < b->qualifiedName; });
        for (const Decl* d : rpcs) {
            o.beginObject(true);
            o.key("rpc");
            o.str(d->service ? d->service->qualifiedName + "." + d->rpcName : d->qualifiedName);
            const Attr* rate = d->attr("ratelimit") ? d->attr("ratelimit") : d->attr("rate");
            o.key("ratelimit");
            o.str(rate && !rate->args.empty() ? rate->args[0].value : "");
            const Attr* intent = d->attr("intent");
            o.key("intent");
            o.str(intent && !intent->args.empty() ? intent->args[0].value : "");
            o.key("reliable");
            o.boolean(d->attr("unreliable") == nullptr);
            o.endObject();
        }
        o.endArray();
        o.key("findings");
        o.beginArray();
        std::vector<Finding> all = m_findings;
        for (const Diagnostic& d : D.diagnostics()) {
            if (d.severity == Severity::Warning && !d.message.starts_with("[size.")) all.push_back(Finding{"schemac", d.loc, d.message});
        }
        std::stable_sort(all.begin(), all.end(), [&](const Finding& a, const Finding& b) {
            const std::string pa = a.loc.file ? D.filePath(a.loc.file) : std::string();
            const std::string pb = b.loc.file ? D.filePath(b.loc.file) : std::string();
            return std::tie(pa, a.loc.line, a.loc.col, a.rule) < std::tie(pb, b.loc.line, b.loc.col, b.rule);
        });
        for (const Finding& f : all) {
            o.beginObject(true);
            o.key("rule");
            o.str(f.rule);
            o.key("file");
            o.str(f.loc.file ? logical(D.filePath(f.loc.file)) : std::string());
            o.key("line");
            o.unum(f.loc.line);
            o.key("col");
            o.unum(f.loc.col);
            o.key("message");
            o.str(f.message);
            o.endObject();
        }
        o.endArray();
        o.endObject();
        return o.take();
    }

    /// The include-relative path of a diagnostics file (stable across checkouts).
    std::string logical(const std::string& path) const {
        for (const auto& fp : S.files) {
            if (fp->path == path) return fp->logicalPath;
        }
        return path;
    }

    const Schema& S;
    const CompileOptions& O;
    DiagnosticEngine& D;
    std::vector<Finding> m_findings;
    std::map<std::string, u64> m_counts;
};

} // namespace

std::vector<OutputFile> generateLint(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags) {
    LintGenerator g(schema, options, diags);
    return g.run();
}

} // namespace helios::schemac
