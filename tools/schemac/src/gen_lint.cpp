// `--emit lint` (02 §3.5: "SEC-1/SEC-4, ledger/persist, keyed lists, naming, size budgets" for CI).
// The rule lints run on every compilation and fail it (sema); this emitter adds the size budgets and
// writes a report CI keeps: what each rule checked, the AAA-SEC-1 classification of every
// client→server message, and every finding, each with a rule id and a location.
//
// Size budgets (warnings; --Werror makes them errors):
//   * size.unbounded: a string, text builtin (LocString, TagQuery, HxlExpr), list, set, map or TagSet
//     reachable from a network-facing type (rpc arguments and `-> T` results, events, messages,
//     replicated fields of components) needs @max(n) (for a TagSet, n bytes of encoded tags, as for
//     a string); such data is hostile input, and an unbounded container lets one peer make the
//     receiver allocate at will. Elements that would need a bound but cannot carry one (a string or
//     container inside a list, set, map or T[N], map keys included), and an rpc result that is itself
//     a string or container (a result has no @max), are findings too;
//   * size.unreliable: the worst-case tagged payload of an unreliable rpc, and of its result, and of an
//     @unreliable event (EVENT_U, 04 §4.6) fits one message on an unreliable channel:
//     kLintUnreliableBudget, engine/net's smallest single-message payload (wire::maxPayloadFor(LATEST,
//     kMaxPacketPayload) = 1,186 B; EVENT_U allows 1,188 B; 04 §2.1). Gameplay messages are never
//     fragmented (04 §2.2: reliable fragmentation serves only CONTROL), so the same limit holds for
//     reliable rpcs and events on EVENT_R; checking those, and taking WP-1.10's rpc and event headers
//     out of the budget, is WP-1.10's.

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>

#include "generators.h"
#include "json_out.h"
#include "text.h"

namespace helios::schemac {

namespace {

constexpr u32 kMaxDepth = 64; ///< the tagged reader's nesting limit (worst cases past it are unbounded)
constexpr u64 kU64Max = std::numeric_limits<u64>::max();

/// Saturating arithmetic: a worst case at or past 2^64 B is over every budget, never a small wrap.
u64 satAdd(u64 a, u64 b) { return a > kU64Max - b ? kU64Max : a + b; }
u64 satMul(u64 a, u64 b) { return b != 0 && a > kU64Max / b ? kU64Max : a * b; }

struct Finding {
    std::string rule;
    SourceLoc loc;
    std::string message;
    std::string subject; ///< what the finding is about (dedup key with rule and loc)
    u32 depth = 0;       ///< nesting below the network type it was reached from (the shallowest is kept)
};

u64 varintMax(u64 v) {
    u64 n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

/// A worst case: tagged bytes (nullopt: unbounded) and the nesting levels below (structs and containers).
struct Size {
    std::optional<u64> bytes;
    u32 depth = 0;
};

/// `n` bytes of a length-prefixed value (its length varint and the bytes).
std::optional<u64> withLength(std::optional<u64> n) { return n ? std::optional<u64>(satAdd(varintMax(*n), *n)) : std::nullopt; }

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
            // A service rpc's arguments are the fields of its synthesized request struct; name them
            // after the rpc, as its result and the SEC-1 table do.
            const std::string owner = d->kind == DeclKind::Rpc ? rpcName(d) : d->name;
            for (const Field& f : d->fields) {
                if (d->isReplicatedComponent() && !f.replicated) continue;
                unbounded(std::format("'{}.{}'", owner, f.name), f.type, &f, f.loc, &f, seen, 0);
            }
            // An rpc's result is a message too: of a server->client rpc, the client's reply, which the
            // server decodes as hostile input.
            if (d->kind == DeclKind::Rpc && d->result) unbounded(std::format("'{}' result", rpcName(d)), d->result, nullptr, d->loc, d, seen, 0);
            if (d->kind == DeclKind::Rpc && d->attr("unreliable")) {
                ++m_counts["size.unreliable-rpcs"];
                budget(d, "rpc", "", messageSize(d));
                if (d->result) budget(d, "rpc", " result", resultSize(d->result));
            }
            // An @unreliable event goes on EVENT_U (fire-and-forget FX, 04 §4.6), which engine/net never
            // fragments either: Connection::send refuses it above wire::maxPayloadFor(EventUnreliable).
            if (d->kind == DeclKind::Event && d->attr("unreliable")) {
                ++m_counts["size.unreliable-events"];
                budget(d, "event", "", messageSize(d));
            }
        }
        m_counts["size.unbounded-checked"] = m_checked.size();
        for (const Finding& f : m_findings) D.warning(f.loc, std::format("[{}] {}", f.rule, f.message));
        return {OutputFile{O.lintOut, report()}};
    }

private:
    /// Records a finding once per rule, location and subject: a struct reached from several network types,
    /// or a message reached both as a network type and as a field type, is reported once, worded for the
    /// shallowest depth it was reached at. One field may have two findings, its own bound and its elements'.
    void add(std::string rule, SourceLoc loc, std::string message, std::string subject, u32 depth) {
        for (Finding& f : m_findings) {
            if (f.rule == rule && f.loc.file == loc.file && f.loc.line == loc.line && f.loc.col == loc.col && f.subject == subject) {
                if (depth < f.depth) {
                    f.message = std::move(message);
                    f.depth = depth;
                }
                return;
            }
        }
        m_findings.push_back(Finding{std::move(rule), loc, std::move(message), std::move(subject), depth});
    }

    /// The rpc's name as the report prints it: `Service.Method` for a service rpc.
    static std::string rpcName(const Decl* d) { return d->service ? d->service->name + "." + d->rpcName : d->name; }

    /// size.unreliable for the rpc or event `d` (`kind`), or its result (`part`).
    void budget(const Decl* d, std::string_view kind, std::string_view part, const Size& size) {
        if (size.bytes && *size.bytes <= kLintUnreliableBudget) return;
        add("size.unreliable", d->loc,
            std::format("unreliable {} '{}'{} has a worst-case payload of {} (budget {} B, the largest unreliable message engine/net "
                        "sends unfragmented, 04 §2.1): bound its fields with @max or make it reliable",
                        kind, rpcName(d), part, size.bytes ? std::to_string(*size.bytes) + " B" : std::string("unbounded"), kLintUnreliableBudget),
            std::string(part), 0);
    }

    /// Strings, text-like builtins and containers other than T[N] need @max on network input.
    static bool needsMax(const Type* t) {
        if (t->kind == TypeKind::Optional) t = t->element;
        if (t->kind == TypeKind::Prim) return t->prim == Prim::String || t->prim == Prim::Name;
        if (t->kind == TypeKind::Builtin)
            return t->builtin == Builtin::TagSet || t->builtin == Builtin::LocString || t->builtin == Builtin::TagQuery || t->builtin == Builtin::HxlExpr;
        return t->isContainer() && t->kind != TypeKind::Array;
    }

    /// The element, map key or map value inside container `t` (through optionals and T[N]) that needs
    /// a bound but cannot carry @max, or nullptr. @max bounds a field's own length or count only.
    static const Type* unboundableElement(const Type* t) {
        if (t->kind == TypeKind::Optional) t = t->element;
        if (!t->isContainer()) return nullptr;
        for (const Type* e : {t->key, t->element}) {
            if (!e) continue;
            const Type* inner = e->kind == TypeKind::Optional ? e->element : e;
            if (needsMax(inner)) return e;
            if (const Type* deeper = unboundableElement(inner)) return deeper;
        }
        return nullptr;
    }

    /// size.unbounded for one value: field `f` (which may carry @max) or an rpc result (`f` null; a result
    /// cannot carry @max), named `what`. Checks its own bound, the elements inside it that cannot be
    /// bounded, then the fields of every struct or variant it reaches (through containers and
    /// optionals), once each per network type. `key` identifies the value for the checked count.
    void unbounded(const std::string& what, const Type* type, const Field* f, SourceLoc loc, const void* key, std::set<const Decl*>& seen,
                   u32 depth) {
        if (depth > kMaxDepth || !type) return;
        const Type* base = type->kind == TypeKind::Optional ? type->element : type;
        const char* where = depth == 0 ? "network input" : "reachable from network input";
        if (needsMax(type) || base->isContainer()) m_checked.insert(key);
        if (needsMax(type) && !f) {
            add("size.unbounded", loc,
                std::format("{} ({}) is {} and cannot carry @max: return a struct with a bounded field instead", what, type->signature, where),
                what, depth);
        } else if (needsMax(type) && !f->attr("max")) {
            add("size.unbounded", loc,
                std::format("{} ({}) is {} without @max: bound it so a peer cannot make the receiver allocate at will", what, type->signature,
                            where),
                what, depth);
        }
        if (const Type* e = unboundableElement(type)) {
            add("size.unbounded", loc,
                std::format("{} ({}) is {} with elements of type {}, which cannot carry @max: hold them in a struct with a bounded "
                            "field instead",
                            what, type->signature, where, e->signature),
                what + " elements", depth);
        }
        for (const Type* t = type; t; t = t->element) {
            if ((t->kind != TypeKind::Struct && t->kind != TypeKind::Variant) || !seen.insert(t->decl).second) continue;
            for (const Field& inner : t->decl->fields)
                unbounded(std::format("'{}.{}'", t->decl->name, inner.name), inner.type, &inner, inner.loc, &inner, seen, depth + 1);
            for (const Alternative& a : t->decl->alternatives) {
                for (const Field& inner : a.type->fields)
                    unbounded(std::format("'{}.{}'", a.type->name, inner.name), inner.type, &inner, inner.loc, &inner, seen, depth + 1);
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

    /// Worst-case tagged bytes of a struct's fields (each with its tag), memoized per declaration so a
    /// shared struct DAG costs one walk per type. A recursive type has no finite worst case, nor has a
    /// struct nested past the tagged reader's depth limit.
    Size messageSize(const Decl* d) {
        if (const auto it = m_sizes.find(d); it != m_sizes.end()) return it->second;
        if (!m_inProgress.insert(d).second) return Size{std::nullopt, 0}; // recursive
        Size total{u64{0}, 0};
        for (const Field& f : d->fields) {
            const Size v = valueSize(f.type, &f);
            total.depth = std::max(total.depth, v.depth);
            total.bytes = total.bytes && v.bytes ? std::optional<u64>(satAdd(*total.bytes, satAdd(varintMax(static_cast<u64>(f.id) << 3), *v.bytes)))
                                                 : std::nullopt;
        }
        if (total.depth > kMaxDepth) total.bytes.reset();
        m_inProgress.erase(d);
        m_sizes.emplace(d, total);
        return total;
    }

    /// An rpc result as a message: a struct is its fields; any other value travels as one tagged field.
    Size resultSize(const Type* t) {
        if (t->kind == TypeKind::Struct) return messageSize(t->decl);
        const Size v = valueSize(t, nullptr);
        return Size{v.bytes ? std::optional<u64>(satAdd(1, *v.bytes)) : std::nullopt, v.depth};
    }

    /// Worst-case bytes of one value after its tag (LEN values include their length prefix).
    Size valueSize(const Type* t, const Field* f) {
        if (!t) return {};
        switch (t->kind) {
        case TypeKind::Prim:
            switch (t->prim) {
            case Prim::F32: return {4};
            case Prim::F64: return {8};
            case Prim::String:
            case Prim::Name: return {withLength(maxOf(f))}; // @max counts bytes on the wire
            default: return {10};
            }
        case TypeKind::Builtin:
            switch (t->builtin) {
            case Builtin::Guid: return {17};
            case Builtin::EntityId: return {8};
            case Builtin::NetHandle:
            case Builtin::Tick:
            case Builtin::Duration: return {10};
            default: {
                const u32 n = tupleSize(t->builtin);
                if (n == 0) return {withLength(maxOf(f))}; // text-like, and TagSet: @max counts its encoded bytes
                return {withLength(static_cast<u64>(n) * (tupleIsF64(t->builtin) ? 8 : 4))};
            }
            }
        case TypeKind::Enum:
        case TypeKind::Flags: return {10};
        case TypeKind::RecordRef: return {8};
        case TypeKind::AssetRef: return {17};
        case TypeKind::Optional: return valueSize(t->element, f);
        case TypeKind::Struct: {
            const Size body = messageSize(t->decl);
            return {withLength(body.bytes), body.depth + 1};
        }
        case TypeKind::Variant: {
            u64 best = 0;
            u32 depth = 0;
            bool bounded = true;
            for (const Alternative& a : t->decl->alternatives) {
                const Size body = messageSize(a.type);
                depth = std::max(depth, body.depth + 1);
                bounded = bounded && body.bytes.has_value();
                if (body.bytes) best = std::max(best, satAdd(varintMax(static_cast<u64>(a.id) << 3), satAdd(varintMax(*body.bytes), *body.bytes)));
            }
            return {bounded ? withLength(best) : std::nullopt, depth};
        }
        case TypeKind::Array: {
            const Size e = valueSize(t->element, nullptr);
            return {e.bytes ? withLength(satMul(t->arraySize, satAdd(*e.bytes, 5))) : std::nullopt, e.depth + 1};
        }
        default: { // list, set, keyed list, map: one entry (with its tag) per element
            const std::optional<u64> n = maxOf(f);
            const Size e = valueSize(t->element, nullptr);
            const Size k = t->key ? valueSize(t->key, nullptr) : Size{u64{0}, 0};
            const u32 depth = std::max(e.depth, k.depth) + 1;
            if (!n || !e.bytes || !k.bytes) return {std::nullopt, depth};
            return {satMul(*n, satAdd(satAdd(*e.bytes, *k.bytes), 5 + 17)), depth}; // entry tag and length, and a keyed list's key
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
        std::vector<std::string> files; // sorted: the report does not depend on the command line's order
        for (const auto& fp : S.files) {
            if (fp->generate) files.push_back(fp->logicalPath);
        }
        std::sort(files.begin(), files.end());
        for (const std::string& f : files) o.str(f);
        o.endArray();
        o.key("checked");
        o.beginObject();
        for (const char* k : {"sec1.rpcs", "sec1.client-to-server", "sec4.fields", "ledger.types", "keyed.lists", "fuel.fns",
                              "size.unbounded-checked", "size.unreliable-rpcs", "size.unreliable-events"}) {
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
        auto printed = [](const Decl* d) { return d->service ? d->service->qualifiedName + "." + d->rpcName : d->qualifiedName; };
        std::sort(rpcs.begin(), rpcs.end(), [&](const Decl* a, const Decl* b) { return printed(a) < printed(b); });
        for (const Decl* d : rpcs) {
            o.beginObject(true);
            o.key("rpc");
            o.str(printed(d));
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
        // The compiler's other warnings, including those --Werror made errors (the gate's own run).
        for (const Diagnostic& d : D.diagnostics()) {
            if ((d.severity == Severity::Warning || d.promoted) && !d.message.starts_with("[size."))
                all.push_back(Finding{"schemac", d.loc, d.message, {}, 0});
        }
        // Sorted by what is printed (the include-relative path), so the order does not depend on where
        // the checkout is. Ties keep the walk's order, which add() makes one finding per subject: a
        // field's own bound before its elements'.
        std::stable_sort(all.begin(), all.end(), [&](const Finding& a, const Finding& b) {
            const std::string pa = a.loc.file ? logical(D.filePath(a.loc.file)) : std::string();
            const std::string pb = b.loc.file ? logical(D.filePath(b.loc.file)) : std::string();
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
    std::set<const void*> m_checked;            ///< fields and rpc results size.unbounded checked (each once)
    std::map<const Decl*, Size> m_sizes;        ///< messageSize memo
    std::set<const Decl*> m_inProgress;         ///< messageSize's current path (a repeat is recursion)
};

} // namespace

std::vector<OutputFile> generateLint(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags) {
    LintGenerator g(schema, options, diags);
    return g.run();
}

} // namespace helios::schemac
