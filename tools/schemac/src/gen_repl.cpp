// `--emit repl` (02 §3.5; 04 §4.1, §4.5, §4.6): per generated file, <file>.repl.gen.h/.cpp next to the
// C++ output with, for each replicated component, its ComponentRepDesc (audience, LOD group, and per
// field the lock id, offset, change-mask index, LOD, prediction, interpolation, quantizer and worst-case
// bits) and typed full-state codecs; the file's rpc and event tables; and a protocol hash over all of
// them (04 §11.3's Phase 0: "descriptors, full state"; change masks and deltas are WP-1.10).

#include <algorithm>
#include <bit>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>

#include "code_writer.h"
#include "generators.h"
#include "go_names.h"
#include "text.h"

namespace helios::schemac {

namespace {

std::string cppNs(const std::string& pkg) {
    std::string out;
    for (const char c : pkg) out += c == '.' ? std::string("::") : std::string(1, c);
    return out;
}

std::string declName(const Decl* d) { return "::" + cppNs(d->package) + "::" + join(d->cppPath, "::"); }

std::string basePath(const std::string& logical) {
    const usize dot = logical.rfind(".hschema");
    return dot == std::string::npos ? logical : logical.substr(0, dot);
}

std::string f64Literal(f64 v) {
    std::string s = formatF64(v);
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

/// The unit suffix of a @quant number ("4096m" -> "m", "1/256m" -> "m", "8" -> ""). A hex number
/// ("0xA", whose digits are letters) has none.
std::string quantUnit(const std::string& text) {
    std::string_view last(text);
    if (const usize slash = last.rfind('/'); slash != std::string_view::npos) last.remove_prefix(slash + 1);
    if (last.starts_with("±")) last.remove_prefix(std::string_view("±").size());
    if (last.starts_with('-') || last.starts_with('+')) last.remove_prefix(1);
    if (last.starts_with("0x") || last.starts_with("0X")) return {};
    usize at = last.size();
    while (at > 0 && std::isalpha(static_cast<unsigned char>(last[at - 1]))) --at;
    return std::string(last.substr(at));
}

/// "4096m" -> 4096, "1/256m" -> 0.00390625, "±8" -> 8 (the sign is the caller's business). Lengths
/// are metres: "m" or no unit; any other unit is rejected (callers check quantUnit first).
std::optional<f64> quantNumber(std::string text) {
    if (text.starts_with("±")) text = text.substr(std::string_view("±").size());
    const std::string unit = quantUnit(text);
    if (!unit.empty() && unit != "m") return std::nullopt;
    text.resize(text.size() - unit.size());
    const usize slash = text.find('/');
    f64 a = 0, b = 1;
    if (slash == std::string::npos) {
        if (!parseSchemaNumber(text, a)) return std::nullopt;
    } else if (!parseSchemaNumber(text.substr(0, slash), a) || !parseSchemaNumber(text.substr(slash + 1), b) || b == 0) {
        return std::nullopt;
    }
    const f64 v = a / b;
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

enum class Quant : u8 { Raw, Range, Smallest3, FrameCell };

struct QuantSpec {
    Quant kind = Quant::Raw;
    u32 bits = 0;
    f64 min = 0, max = 0, cell = 0, res = 0;
};

/// A replicated field as the full-state codec carries it.
struct RepField {
    const Field* field;
    QuantSpec quant;
    u32 maxBits = 0;
    std::string lod;    ///< "Core" / "Near"
    std::string interp; ///< "None" / "Linear" / "Slerp"
};

/// Float components of a math type (0 = not a float tuple), and whether they are f64.
u32 floatComponents(const Type* t, bool& f64Out) {
    f64Out = false;
    if (t->kind == TypeKind::Prim && (t->prim == Prim::F32 || t->prim == Prim::F64)) {
        f64Out = t->prim == Prim::F64;
        return 1;
    }
    if (t->kind != TypeKind::Builtin) return 0;
    switch (t->builtin) {
    case Builtin::Vec2f: return 2;
    case Builtin::Vec3f: return 3;
    case Builtin::Vec4f:
    case Builtin::Color: return 4;
    case Builtin::Vec3d: f64Out = true; return 3;
    default: return 0;
    }
}

/// Raw full-state bits of a fixed-size value, or 0 when the Phase 0 codec does not carry it.
u32 rawBits(const Type* t) {
    switch (t->kind) {
    case TypeKind::Prim:
        switch (t->prim) {
        case Prim::Bool: return 1;
        case Prim::I8:
        case Prim::U8: return 8;
        case Prim::I16:
        case Prim::U16: return 16;
        case Prim::I32:
        case Prim::U32:
        case Prim::F32: return 32;
        case Prim::I64:
        case Prim::U64:
        case Prim::F64: return 64;
        default: return 0; // strings and Names are not fixed-size (Name ids are process-local)
        }
    case TypeKind::Enum:
    case TypeKind::Flags: {
        Type u;
        u.kind = TypeKind::Prim;
        u.prim = t->decl->underlying;
        return rawBits(&u);
    }
    case TypeKind::RecordRef: return 64;
    case TypeKind::Builtin:
        switch (t->builtin) {
        case Builtin::Vec2f: return 64;
        case Builtin::Vec3f: return 96;
        case Builtin::Vec4f:
        case Builtin::Quatf:
        case Builtin::Color: return 128;
        case Builtin::Vec3d:
        case Builtin::WorldPos: return 192;
        case Builtin::Quatd: return 256;
        case Builtin::EntityId:
        case Builtin::Tick:
        case Builtin::Duration: return 64;
        case Builtin::NetHandle: return 32;
        default: return 0;
        }
    default: return 0;
    }
}

class ReplGenerator {
public:
    ReplGenerator(const Schema& s, const CompileOptions& o, DiagnosticEngine& d) : S(s), O(o), D(d) {}

    std::vector<OutputFile> run() {
        const usize errorsBefore = D.errorCount();
        std::vector<FileTables> files;
        for (const auto& fp : S.files) {
            if (!fp->generate) continue;
            FileTables t{fp.get(), {}, rpcs(fp.get()), events(fp.get())};
            for (const Decl* d : S.declsOfFile(fp.get())) {
                if (d->isReplicatedComponent()) t.comps.emplace_back(d, fields(d));
            }
            files.push_back(std::move(t));
        }
        std::vector<OutputFile> out;
        if (D.errorCount() != errorsBefore) return out;
        for (const FileTables& t : files) {
            const std::string base = basePath(t.file->logicalPath);
            out.push_back(OutputFile{joinCpp(base + ".repl.gen.h"), header(t.file, t.comps)});
            out.push_back(OutputFile{joinCpp(base + ".repl.gen.cpp"), source(t)});
        }
        return out;
    }

private:
    std::string joinCpp(const std::string& rel) const { return O.cppOut.empty() || O.cppOut == "." ? rel : O.cppOut + "/" + rel; }

    // --- @quant -------------------------------------------------------------------------------
    std::optional<QuantSpec> quantOf(const Decl* d, const Field& f) {
        const Attr* a = f.attr("quant");
        QuantSpec q;
        if (!a) return q;
        auto fail = [&](const std::string& why) -> std::optional<QuantSpec> {
            D.error(a->loc, std::format("@quant on '{}.{}': {}", d->name, f.name, why));
            return std::nullopt;
        };
        std::string form;
        std::optional<f64> range, cell, res;
        std::optional<u64> bits;
        std::vector<std::string> keys;
        for (const AttrArg& arg : a->args) {
            // A repeated argument or a second form would leave all but the last one ignored.
            if (arg.key.empty() && !form.empty()) return fail(std::format("a second form '{}' after '{}' (give one form)", arg.value, form));
            if (!arg.key.empty() && std::find(keys.begin(), keys.end(), arg.key) != keys.end())
                return fail(std::format("{}= is given twice", arg.key));
            if (!arg.key.empty()) keys.push_back(arg.key);
            if (arg.key == "range" || arg.key == "cell" || arg.key == "res") {
                // A unit other than metres would be dropped silently (cell=4km would be a 4 m cell).
                if (const std::string unit = quantUnit(arg.value); !unit.empty() && unit != "m")
                    return fail(std::format("{}={}: unit '{}' is not supported (lengths are in m, or unitless)", arg.key, arg.value, unit));
            }
            if (arg.key.empty()) {
                form = arg.value;
            } else if (arg.key == "range") {
                // range=x is the symmetric range ±x, as 02 §3.1's example writes it (04 §4.1 writes ±x).
                if (!(range = quantNumber(arg.value)) || *range <= 0) return fail("range= needs a positive bound, e.g. range=±64");
            } else if (arg.key == "bits") {
                u64 n = 0;
                if (!parseSchemaUnsigned(arg.value, n) || n < 1 || n > 32) return fail("bits= needs 1 to 32");
                bits = n;
            } else if (arg.key == "cell") {
                cell = quantNumber(arg.value);
            } else if (arg.key == "res") {
                res = quantNumber(arg.value);
            } else {
                return fail(std::format("unknown argument '{}' (use range=±x and bits=n, smallest3 and bits=n, or frame_cell with cell= and res=)", arg.key));
            }
        }
        bool isF64 = false;
        const u32 comps = floatComponents(f.type, isF64);
        // An argument the form does not use would be ignored silently (frame_cell's offset width is
        // derived from cell/res, so a bits= there changes nothing on the wire).
        auto unused = [&](std::initializer_list<std::string_view> used) -> std::optional<std::string> {
            for (const std::string& k : keys) {
                if (std::find(used.begin(), used.end(), k) == used.end()) return k;
            }
            return std::nullopt;
        };
        const std::string formName = form.empty() ? "range" : form;
        const std::optional<std::string> extra = form == "frame_cell"  ? unused({"cell", "res"})
                                                 : form == "smallest3" ? unused({"bits"})
                                                 : form.empty()        ? unused({"range", "bits"})
                                                                       : std::nullopt;
        if (extra) return fail(std::format("{}= is not an argument of the {} form (it would be ignored)", *extra, formName));
        if (form == "frame_cell") {
            if (f.type->kind != TypeKind::Builtin || f.type->builtin != Builtin::WorldPos) return fail("frame_cell needs a WorldPos");
            if (!cell || !res || *cell <= 0 || *res <= 0) return fail("frame_cell needs cell=<size> and res=<resolution>, e.g. cell=4096m, res=1/256m");
            const f64 steps = *cell / *res;
            if (std::fabs(steps - std::round(steps)) > 1e-9 * steps || steps < 2 || steps > 4294967296.0)
                return fail("cell must be a whole multiple (2 to 2^32) of res");
            q.kind = Quant::FrameCell;
            q.cell = *cell;
            q.res = *res;
            // ⌈log₂ steps⌉ in integers, so the wire width cannot depend on the build host's libm.
            q.bits = static_cast<u32>(std::bit_width(static_cast<u64>(std::round(steps)) - 1));
            return q;
        }
        if (form == "smallest3") {
            if (f.type->kind != TypeKind::Builtin || f.type->builtin != Builtin::Quatf) return fail("smallest3 needs a quatf");
            if (!bits) return fail("smallest3 needs bits=n");
            // At 1 or 2 bits rounding pushes most quaternions past unit length (a 1-bit step is ±1/√2).
            if (*bits < 3) return fail("smallest3 needs bits=3 to 32");
            q.kind = Quant::Smallest3;
            q.bits = static_cast<u32>(*bits);
            return q;
        }
        if (!form.empty()) return fail(std::format("unknown form '{}' (frame_cell, smallest3, or range=±x with bits=n)", form));
        if (!range || !bits) return fail("needs range=±x and bits=n");
        if (comps == 0) return fail(std::format("range quantization needs a float scalar or vector, not '{}'", f.type->signature));
        // The decoder computes min + (max - min) * q / steps and casts it to the field's type: the
        // bounds must fit f32 for f32 fields, and max - min must be finite.
        if (!isF64 && *range > static_cast<f64>(FLT_MAX)) return fail(std::format("range=±{} does not fit an f32 component", formatF64(*range)));
        if (!std::isfinite(2 * *range)) return fail(std::format("range=±{}: the range's width is not a finite f64", formatF64(*range)));
        q.kind = Quant::Range;
        q.min = -*range;
        q.max = *range;
        q.bits = static_cast<u32>(*bits);
        return q;
    }

    std::vector<RepField> fields(const Decl* d) {
        std::vector<RepField> out;
        for (const Field& f : d->fields) {
            if (!f.replicated || !f.type) continue;
            std::optional<QuantSpec> q = quantOf(d, f);
            if (!q) continue;
            RepField r{&f, *q, 0, "Core", "None"};
            bool isF64 = false;
            switch (q->kind) {
            case Quant::Raw:
                r.maxBits = rawBits(f.type);
                if (r.maxBits == 0) {
                    D.error(f.loc, std::format("replicated field '{}.{}' is '{}', which the Phase 0 full-state codec does not carry (fixed-size "
                                               "scalars, enums, flags, math tuples, WorldPos and ids only; WP-1.10 adds the rest)",
                                               d->name, f.name, f.type->signature));
                    continue;
                }
                break;
            case Quant::Range: r.maxBits = floatComponents(f.type, isF64) * q->bits; break;
            case Quant::Smallest3: r.maxBits = 2 + 3 * q->bits; break;
            case Quant::FrameCell: r.maxBits = 3 * (80 + q->bits); break; // (a 64-bit varint takes at most 10 groups)
            }
            const Attr* lod = f.attr("lod");
            const std::string lodName = lod && !lod->args.empty() ? lod->args[0].value : d->lod.empty() ? "core" : d->lod;
            if (lodName != "core" && lodName != "near") {
                D.error(lod ? lod->loc : d->loc, std::format("lod({}) on '{}': expected core or near (04 §4.1)", lodName, d->name));
                continue;
            }
            r.lod = lodName == "near" ? "Near" : "Core";
            if (const Attr* ip = f.attr("interp"); ip && !ip->args.empty()) {
                const std::string& v = ip->args[0].value;
                if (v != "linear" && v != "slerp") {
                    D.error(ip->loc, std::format("@interp({}) on '{}.{}': expected linear or slerp", v, d->name, f.name));
                    continue;
                }
                r.interp = v == "linear" ? "Linear" : "Slerp";
            }
            out.push_back(r);
        }
        std::sort(out.begin(), out.end(), [](const RepField& a, const RepField& b) { return a.field->repIndex < b.field->repIndex; });
        return out;
    }

    // --- hashes -------------------------------------------------------------------------------
    static std::string quantText(const QuantSpec& q) {
        switch (q.kind) {
        case Quant::Raw: return "raw";
        case Quant::Range: return std::format("range({},{},{})", formatF64(q.min), formatF64(q.max), q.bits);
        case Quant::Smallest3: return std::format("smallest3({})", q.bits);
        case Quant::FrameCell: return std::format("frame_cell({},{},{})", formatF64(q.cell), formatF64(q.res), q.bits);
        }
        return "?";
    }

    static std::string audienceName(const Decl* d) {
        switch (d->replicate) {
        case RepAudience::All: return "All";
        case RepAudience::Owner: return "Owner";
        case RepAudience::Server: return "Server";
        default: return "None";
        }
    }

    static u64 descHash(const Decl* d, const std::vector<RepField>& fs) {
        std::string text = std::format("component {} {} {} {};", d->qualifiedName, d->typeId, audienceName(d), d->lod.empty() ? "core" : d->lod);
        std::map<std::string, const Decl*> reached; // enums and flags: a reader rejects undeclared values
        for (const RepField& r : fs) {
            text += std::format("{}:{}:{}:{}:{}:{}:{}:{}:{};", r.field->id, r.field->name, r.field->type->signature, r.field->repIndex, r.lod,
                                r.field->predicted ? 1 : 0, r.interp, quantText(r.quant), r.maxBits);
            std::vector<const Decl*> types;
            reachedBy(r.field->type, types);
            for (const Decl* t : types) reached.emplace(t->qualifiedName, t);
        }
        for (const auto& [name, t] : reached) text += std::format(" {}{{{}}}", name, layoutText(t, false));
        return fnv1a64(text);
    }

    /// Struct, variant and enum declarations a type refers to (through containers and optionals). A
    /// record ref travels as an id, so the record's own fields are not part of the payload.
    static void reachedBy(const Type* t, std::vector<const Decl*>& out) {
        if (!t) return;
        reachedBy(t->element, out);
        reachedBy(t->key, out);
        const bool layout =
            t->kind == TypeKind::Struct || t->kind == TypeKind::Variant || t->kind == TypeKind::Enum || t->kind == TypeKind::Flags;
        if (layout && t->decl) out.push_back(t->decl);
    }

    /// One declaration's part of a payload: per field its name, type, explicit default and @max (and
    /// lock id when `withIds`), an enum's or flags' underlying type (its raw wire width) and values, and
    /// variant alternatives. (@quant is valid only on replicated component fields, which descHash
    /// covers, so a payload has none.)
    static std::string layoutText(const Decl* d, bool withIds) {
        std::string out;
        for (const Field& f : d->fields) {
            out += withIds ? std::format("{}:", f.id) : std::string();
            out += std::format("{}:{}:{}", f.name, f.type ? f.type->signature : "?", f.defaultValue ? f.defaultValue->json : "");
            // A decoder rejects more than @max elements or bytes, so peers must agree on it.
            if (const Attr* m = f.attr("max"); m && !m->args.empty()) {
                u64 n = 0;
                out += parseSchemaUnsigned(m->args[0].value, n) ? std::format(":max={}", n) : ":max=" + m->args[0].value;
            }
            out += ';';
        }
        if (d->kind == DeclKind::Enum || d->kind == DeclKind::Flags) out += std::format(":{};", primName(d->underlying));
        for (const EnumVal& v : d->values) out += std::format("{}={};", v.name, v.value);
        for (const Alternative& a : d->alternatives) out += withIds ? std::format("|{}:{};", a.id, a.name) : std::format("|{};", a.name);
        return out;
    }

    /// The payload of an rpc's arguments or an event's fields, for the protocol hash: the declaration's
    /// own fields with their lock ids, then every struct, variant, enum and flags type it reaches (each once,
    /// sorted by name), so any change a peer's codec depends on changes the hash. Reached types
    /// contribute no lock ids, as in the layout hash: an imported type has ids only when its own file
    /// is compiled, and the hash must not depend on the file set.
    static std::string payloadText(const Decl* root) {
        std::map<std::string, const Decl*> reached;
        std::set<const Decl*> seen{root};
        std::vector<const Decl*> todo{root};
        while (!todo.empty()) {
            const Decl* d = todo.back();
            todo.pop_back();
            std::vector<const Decl*> next;
            reachedBy(d->result, next); // an rpc's `-> T` (04 §4.6 defines no reply yet; hashed so peers agree on it)
            for (const Field& f : d->fields) reachedBy(f.type, next);
            for (const Alternative& a : d->alternatives) {
                if (a.type) next.push_back(a.type);
            }
            for (const Decl* x : next) {
                if (!seen.insert(x).second) continue;
                reached.emplace(x->qualifiedName, x);
                todo.push_back(x);
            }
        }
        std::string text = layoutText(root, true);
        if (root->result) text += "->" + root->result->signature + ";";
        for (const auto& [name, d] : reached) text += std::format(" {}{{{}}}", name, layoutText(d, false));
        return text;
    }

    // --- rpcs and events ----------------------------------------------------------------------
    struct Rpc {
        const Decl* decl;
        std::string direction;
        bool reliable;
        f64 rate;
        std::string intent;
    };
    struct Event {
        const Decl* decl;
        std::string audience;
        bool reliable; ///< EVENT_R, or EVENT_U with @unreliable (04 §2.2)
    };

    std::vector<Rpc> rpcs(const SourceFile* f) {
        std::vector<Rpc> out;
        for (const Decl* d : S.declsOfFile(f)) {
            if (d->kind != DeclKind::Rpc || d->service) continue; // service rpcs are backend calls (05), not netcode
            Rpc r{d, d->direction == "client->server" ? "ClientToServer" : d->direction == "server->client" ? "ServerToClient" : "ServerToServer",
                  d->attr("unreliable") == nullptr, 0, ""};
            if (const Attr* rate = d->attr("ratelimit") ? d->attr("ratelimit") : d->attr("rate"); rate && !rate->args.empty()) {
                const std::string& v = rate->args[0].value;
                const usize slash = v.find('/');
                f64 n = 0;
                if (slash != std::string::npos && parseSchemaNumber(v.substr(0, slash), n)) {
                    const std::string unit = v.substr(slash + 1);
                    r.rate = unit == "m" ? n / 60 : unit == "h" ? n / 3600 : n;
                }
            }
            if (const Attr* in = d->attr("intent"); in && !in->args.empty()) r.intent = in->args[0].value;
            out.push_back(r);
        }
        return out;
    }

    std::vector<Event> events(const SourceFile* f) {
        std::vector<Event> out;
        for (const Decl* d : S.declsOfFile(f)) {
            if (d->kind != DeclKind::Event) continue;
            std::string a = "relevant";
            if (const Attr* at = d->attr("audience"); at && !at->args.empty()) a = at->args[0].value;
            if (a != "owner" && a != "relevant" && a != "party") {
                D.error(d->loc, std::format("event '{}' has @audience({}); expected owner, relevant or party (04 §4.6)", d->name, a));
                continue;
            }
            out.push_back(Event{d, a == "owner" ? "Owner" : a == "party" ? "Party" : "Relevant", d->attr("unreliable") == nullptr});
        }
        return out;
    }

    u64 fileHash(const std::vector<std::pair<const Decl*, std::vector<RepField>>>& comps, const std::vector<Rpc>& rs, const std::vector<Event>& es) const {
        std::vector<u64> parts;
        for (const auto& [d, fs] : comps) parts.push_back(descHash(d, fs));
        for (const Rpc& r : rs)
            parts.push_back(fnv1a64(std::format("rpc {} {} {} {} {} {};{}", r.decl->qualifiedName, r.decl->typeId, r.direction, r.reliable,
                                                formatF64(r.rate), r.intent, payloadText(r.decl))));
        for (const Event& e : es)
            parts.push_back(fnv1a64(std::format("event {} {} {} {};{}", e.decl->qualifiedName, e.decl->typeId, e.audience, e.reliable,
                                                payloadText(e.decl))));
        return combine(parts);
    }

    /// Same combination as helios::refl::repl::protocolHash (sorted, FNV-1a over the bytes).
    static u64 combine(std::vector<u64> parts) {
        std::sort(parts.begin(), parts.end());
        u64 h = 0xcbf29ce484222325ull;
        for (const u64 p : parts) {
            for (u32 i = 0; i < 8; ++i) {
                h ^= (p >> (8 * i)) & 0xFF;
                h *= 0x100000001b3ull;
            }
        }
        return h;
    }

    using Comps = std::vector<std::pair<const Decl*, std::vector<RepField>>>;
    struct FileTables {
        const SourceFile* file;
        Comps comps;
        std::vector<Rpc> rpcs;
        std::vector<Event> events;
    };

    // --- output -------------------------------------------------------------------------------
    std::string header(const SourceFile* f, const std::vector<std::pair<const Decl*, std::vector<RepField>>>& comps) {
        CodeWriter w;
        const std::string base = basePath(f->logicalPath);
        w.line(std::format("// {}.repl.gen.h — generated by helios-schemac (--emit repl) from {}. DO NOT EDIT.", base.substr(base.rfind('/') + 1),
                           f->logicalPath));
        w.line("// Replication descriptors and full-state codecs (04 §4.1; tools/schemac/README.md \"Generated replication\").");
        w.line("#pragma once");
        w.line();
        w.line("#include \"helios/reflect/repl.h\"");
        w.line(std::format("#include \"{}\"", cppHeaderPath(f->logicalPath)));
        w.line();
        w.line(std::format("namespace {} {{", cppNs(f->ast.package)));
        w.line();
        w.line(std::format("/// Replicated components, rpcs, events and the protocol hash of {} (immutable; any thread).", f->logicalPath));
        w.line(std::format("const ::helios::refl::repl::FileRepTables& {}Replication() noexcept;", camelCase(f->stem)));
        w.line();
        w.line(std::format("}} // namespace {}", cppNs(f->ast.package)));
        if (!comps.empty()) {
            w.line();
            w.line("namespace helios::refl::repl {");
            for (const auto& [d, fs] : comps) {
                w.line();
                w.line(std::format("/// {}: audience {}, {} replicated field{}.", d->qualifiedName, audienceName(d), fs.size(), fs.size() == 1 ? "" : "s"));
                w.line("template <>");
                w.open(std::format("struct RepOf<{}> {{", declName(d)));
                w.line("static const ComponentRepDesc& desc() noexcept;");
                w.line("/// Every replicated field in change-mask order, quantized (04 §4.2's full-state chunk).");
                w.line(std::format("static void writeFullState(BitWriter& w, const {}& c);", declName(d)));
                w.line("/// Reads what writeFullState wrote; fails on truncated or invalid input and leaves `c` partly updated.");
                w.line(std::format("static Result<void> readFullState(BitReader& r, {}& c);", declName(d)));
                w.close("};");
            }
            w.line();
            w.line("} // namespace helios::refl::repl");
        }
        return w.take();
    }

    static std::string enumType(const Type* t) { return declName(t->decl); }

    /// Statements writing value expression `v` of type `t` raw.
    void writeRaw(CodeWriter& w, const Type* t, const std::string& v) {
        switch (t->kind) {
        case TypeKind::Prim:
            switch (t->prim) {
            case Prim::Bool: w.line(std::format("w.writeBool({});", v)); return;
            case Prim::F32: w.line(std::format("writeF32(w, {});", v)); return;
            case Prim::F64: w.line(std::format("writeF64(w, {});", v)); return;
            default:
                w.line(std::format("w.write(static_cast<::helios::u64>(static_cast<std::make_unsigned_t<decltype({0})>>({0})), {1});", v, rawBits(t)));
                return;
            }
        case TypeKind::Enum:
        case TypeKind::Flags:
            w.line(std::format("w.write(static_cast<::helios::u64>(static_cast<std::make_unsigned_t<std::underlying_type_t<{0}>>>({1})), {2});",
                               enumType(t), v, rawBits(t)));
            return;
        case TypeKind::RecordRef: w.line(std::format("w.write({}.id, 64);", v)); return;
        case TypeKind::Builtin:
            switch (t->builtin) {
            case Builtin::EntityId: w.line(std::format("w.write({}.value, 64);", v)); return;
            case Builtin::NetHandle: w.line(std::format("w.write({}.value, 32);", v)); return;
            case Builtin::Tick: w.line(std::format("w.write({}, 64);", v)); return;
            case Builtin::Duration: w.line(std::format("w.write(static_cast<::helios::u64>({}.nanos), 64);", v)); return;
            case Builtin::WorldPos:
                for (const char* c : {"x", "y", "z"}) w.line(std::format("writeF64(w, {}.local.{});", v, c));
                return;
            default: {
                bool isF64 = t->builtin == Builtin::Vec3d || t->builtin == Builtin::Quatd;
                const char* comps4[] = {"x", "y", "z", "w"};
                const char* rgba[] = {"r", "g", "b", "a"};
                const u32 n = t->builtin == Builtin::Vec2f ? 2 : (t->builtin == Builtin::Vec3f || t->builtin == Builtin::Vec3d) ? 3 : 4;
                for (u32 i = 0; i < n; ++i)
                    w.line(std::format("writeF{}(w, {}.{});", isF64 ? 64 : 32, v, t->builtin == Builtin::Color ? rgba[i] : comps4[i]));
                return;
            }
            }
        default: return;
        }
    }

    /// Statements reading into lvalue `v` of type `t` raw.
    void readRaw(CodeWriter& w, const Type* t, const std::string& v, const std::string& what) {
        auto get = [&](const std::string& call, const std::string& assign) {
            w.open("{");
            w.line(std::format("auto x = {};", call));
            w.line("if (!x) return x.error();");
            w.line(assign);
            w.close();
        };
        switch (t->kind) {
        case TypeKind::Prim:
            switch (t->prim) {
            case Prim::Bool: get("r.readBool()", v + " = *x;"); return;
            case Prim::F32: get("readF32(r)", v + " = *x;"); return;
            case Prim::F64: get("readF64(r)", v + " = *x;"); return;
            default:
                get(std::format("r.read({})", rawBits(t)),
                    std::format("{0} = static_cast<decltype({0})>(static_cast<std::make_unsigned_t<decltype({0})>>(*x));", v));
                return;
            }
        case TypeKind::Enum: {
            std::string valid;
            for (const EnumVal& e : t->decl->values) {
                // -9223372036854775808ll is unary minus on a literal too large for long long (GCC and
                // Clang warn), so INT64_MIN is spelled as gen_cpp spells it.
                const std::string lit = e.value == std::numeric_limits<i64>::min() ? "(-9223372036854775807ll - 1)" : std::to_string(e.value) + "ll";
                valid += (valid.empty() ? "" : " && ") + std::format("v != {}", lit);
            }
            w.open("{");
            w.line(std::format("auto x = r.read({});", rawBits(t)));
            w.line("if (!x) return x.error();");
            w.line(std::format("const auto v = static_cast<long long>(static_cast<std::underlying_type_t<{0}>>(static_cast<std::make_unsigned_t<std::underlying_type_t<{0}>>>(*x)));",
                               enumType(t)));
            w.line(std::format("if ({}) return ::helios::Error(::helios::ErrorCode::Corrupt, \"{}: not a {} value\");", valid.empty() ? "true" : valid,
                               what, t->decl->name));
            w.line(std::format("{} = static_cast<{}>(v);", v, enumType(t)));
            w.close();
            return;
        }
        case TypeKind::Flags: {
            u64 declared = 0;
            for (const EnumVal& e : t->decl->values) declared |= static_cast<u64>(e.value);
            w.open("{");
            w.line(std::format("auto x = r.read({});", rawBits(t)));
            w.line("if (!x) return x.error();");
            w.line(std::format("if ((*x & ~{:#x}ull) != 0) return ::helios::Error(::helios::ErrorCode::Corrupt, \"{}: undeclared {} bits\");", declared,
                               what, t->decl->name));
            w.line(std::format("{0} = static_cast<{1}>(static_cast<std::underlying_type_t<{1}>>(*x));", v, enumType(t)));
            w.close();
            return;
        }
        case TypeKind::RecordRef: get("r.read(64)", v + ".id = *x;"); return;
        case TypeKind::Builtin:
            switch (t->builtin) {
            case Builtin::EntityId: get("r.read(64)", v + ".value = *x;"); return;
            case Builtin::NetHandle: get("r.read(32)", v + ".value = static_cast<::helios::u32>(*x);"); return;
            case Builtin::Tick: get("r.read(64)", v + " = *x;"); return;
            case Builtin::Duration: get("r.read(64)", v + ".nanos = static_cast<::helios::i64>(*x);"); return;
            case Builtin::WorldPos:
                for (const char* c : {"x", "y", "z"}) get("readF64(r)", std::format("{}.local.{} = *x;", v, c));
                return;
            default: {
                const bool isF64 = t->builtin == Builtin::Vec3d || t->builtin == Builtin::Quatd;
                const char* comps4[] = {"x", "y", "z", "w"};
                const char* rgba[] = {"r", "g", "b", "a"};
                const u32 n = t->builtin == Builtin::Vec2f ? 2 : (t->builtin == Builtin::Vec3f || t->builtin == Builtin::Vec3d) ? 3 : 4;
                for (u32 i = 0; i < n; ++i)
                    get(std::format("readF{}(r)", isF64 ? 64 : 32), std::format("{}.{} = *x;", v, t->builtin == Builtin::Color ? rgba[i] : comps4[i]));
                return;
            }
            }
        default: return;
        }
    }

    std::vector<std::string> components(const Type* t, const std::string& v) {
        bool isF64 = false;
        const u32 n = floatComponents(t, isF64);
        if (t->kind == TypeKind::Prim) return {v};
        const char* comps4[] = {"x", "y", "z", "w"};
        const char* rgba[] = {"r", "g", "b", "a"};
        std::vector<std::string> out;
        for (u32 i = 0; i < n; ++i) out.push_back(v + "." + (t->builtin == Builtin::Color ? rgba[i] : comps4[i]));
        return out;
    }

    void emitCodec(CodeWriter& w, const Decl* d, const std::vector<RepField>& fs) {
        const std::string C = declName(d);
        w.open(std::format("void RepOf<{}>::writeFullState(BitWriter& w, const {}& c) {{", C, C));
        if (fs.empty()) {
            w.line("(void)w;");
            w.line("(void)c;");
        }
        for (const RepField& r : fs) {
            const std::string v = "c." + cppFieldName(r.field->name);
            switch (r.quant.kind) {
            case Quant::Raw: writeRaw(w, r.field->type, v); break;
            case Quant::Range:
                for (const std::string& c : components(r.field->type, v))
                    w.line(std::format("w.write(quantizeRange({}, {}, {}, {}), {});", c, f64Literal(r.quant.min), f64Literal(r.quant.max), r.quant.bits,
                                       r.quant.bits));
                break;
            case Quant::Smallest3: w.line(std::format("writeSmallest3(w, {}, {});", v, r.quant.bits)); break;
            case Quant::FrameCell:
                w.line(std::format("writeFrameCell(w, {}, {}, {}, {});", v, f64Literal(r.quant.cell), f64Literal(r.quant.res), r.quant.bits));
                break;
            }
        }
        w.close();
        w.line();
        w.open(std::format("Result<void> RepOf<{}>::readFullState(BitReader& r, {}& c) {{", C, C));
        if (fs.empty()) {
            w.line("(void)r;");
            w.line("(void)c;");
        }
        for (const RepField& rf : fs) {
            const std::string v = "c." + cppFieldName(rf.field->name);
            const std::string what = d->name + "." + rf.field->name;
            switch (rf.quant.kind) {
            case Quant::Raw: readRaw(w, rf.field->type, v, what); break;
            case Quant::Range: {
                const bool isF64 = rf.field->type->kind == TypeKind::Prim ? rf.field->type->prim == Prim::F64 : rf.field->type->builtin == Builtin::Vec3d;
                for (const std::string& c : components(rf.field->type, v)) {
                    w.open("{");
                    w.line(std::format("auto x = r.read({});", rf.quant.bits));
                    w.line("if (!x) return x.error();");
                    const std::string deq = std::format("dequantizeRange(*x, {}, {}, {})", f64Literal(rf.quant.min), f64Literal(rf.quant.max), rf.quant.bits);
                    w.line(std::format("{} = {};", c, isF64 ? deq : "static_cast<::helios::f32>(" + deq + ")"));
                    w.close();
                }
                break;
            }
            case Quant::Smallest3:
                w.open("{");
                w.line(std::format("auto x = readSmallest3(r, {});", rf.quant.bits));
                w.line("if (!x) return x.error();");
                w.line(v + " = *x;");
                w.close();
                break;
            case Quant::FrameCell:
                w.open("{");
                w.line(std::format("auto x = readFrameCell(r, {}, {}, {});", f64Literal(rf.quant.cell), f64Literal(rf.quant.res), rf.quant.bits));
                w.line("if (!x) return x.error();");
                w.line(v + " = *x;");
                w.close();
                break;
            }
        }
        w.line("return {};");
        w.close();
        w.line();
    }

    std::string source(const FileTables& t) {
        const SourceFile* f = t.file;
        const Comps& comps = t.comps;
        const std::vector<Rpc>& rs = t.rpcs;
        const std::vector<Event>& es = t.events;
        CodeWriter w;
        const std::string base = basePath(f->logicalPath);
        w.line(std::format("// {}.repl.gen.cpp — generated by helios-schemac (--emit repl) from {}. DO NOT EDIT.", base.substr(base.rfind('/') + 1),
                           f->logicalPath));
        w.line(std::format("#include \"{}.repl.gen.h\"", base));
        w.line();
        w.line("#include <cstddef>");
        w.line("#include <type_traits>");
        w.line();
        w.line("namespace helios::refl::repl {");
        w.line();
        for (const auto& [d, fs] : comps) {
            const std::string C = declName(d);
            w.open(std::format("const ComponentRepDesc& RepOf<{}>::desc() noexcept {{", C));
            u32 total = 0;
            if (!fs.empty()) {
                w.line("HELIOS_GEN_OFFSETOF_BEGIN");
                w.open("static const FieldRep fields[] = {");
                for (const RepField& r : fs) {
                    total += r.maxBits;
                    w.line(std::format("{{{}, {}u, static_cast<::helios::u32>(offsetof({}, {})), {}, Lod::{}, {}, Interp::{}, "
                                       "Quantizer{{Quant::{}, {}, {}, {}, {}, {}}}, {}u}},",
                                       cppQuote(r.field->name), r.field->id, C, cppFieldName(r.field->name), r.field->repIndex, r.lod,
                                       r.field->predicted ? "true" : "false", r.interp,
                                       r.quant.kind == Quant::Raw ? "Raw" : r.quant.kind == Quant::Range ? "Range" : r.quant.kind == Quant::Smallest3 ? "Smallest3" : "FrameCell",
                                       r.quant.bits, f64Literal(r.quant.min), f64Literal(r.quant.max), f64Literal(r.quant.cell), f64Literal(r.quant.res),
                                       r.maxBits));
                }
                w.close("};");
                w.line("HELIOS_GEN_OFFSETOF_END");
            }
            w.line(std::format("static const ComponentRepDesc desc{{{}, {:#010x}u, Audience::{}, Lod::{}, {}, {}u, {:#018x}ull}};", cppQuote(d->qualifiedName),
                               d->typeId, audienceName(d), d->lod == "near" ? "Near" : "Core", fs.empty() ? "{}" : "fields", total, descHash(d, fs)));
            w.line("return desc;");
            w.close();
            w.line();
            emitCodec(w, d, fs);
        }
        w.line("} // namespace helios::refl::repl");
        w.line();
        w.line(std::format("namespace {} {{", cppNs(f->ast.package)));
        w.line();
        w.open(std::format("const ::helios::refl::repl::FileRepTables& {}Replication() noexcept {{", camelCase(f->stem)));
        w.line("using namespace ::helios::refl::repl;");
        if (!comps.empty()) {
            w.open("static const ComponentRepDesc* const components[] = {");
            for (const auto& [d, fs] : comps) w.line(std::format("&RepOf<{}>::desc(),", declName(d)));
            w.close("};");
        }
        if (!rs.empty()) {
            w.open("static const RpcRep rpcs[] = {");
            for (const Rpc& r : rs)
                w.line(std::format("{{{}, {:#010x}u, RpcDirection::{}, {}, {}, {}}},", cppQuote(r.decl->qualifiedName), r.decl->typeId, r.direction,
                                   r.reliable ? "true" : "false", f64Literal(r.rate), cppQuote(r.intent)));
            w.close("};");
        }
        if (!es.empty()) {
            w.open("static const EventRep events[] = {");
            for (const Event& e : es)
                w.line(std::format("{{{}, {:#010x}u, EventAudience::{}, {}}},", cppQuote(e.decl->qualifiedName), e.decl->typeId, e.audience,
                                   e.reliable ? "true" : "false"));
            w.close("};");
        }
        w.line(std::format("static const FileRepTables tables{{{}, {}, {}, {:#018x}ull}};", comps.empty() ? "{}" : "components", rs.empty() ? "{}" : "rpcs",
                           es.empty() ? "{}" : "events", fileHash(comps, rs, es)));
        w.line("return tables;");
        w.close();
        w.line();
        w.line(std::format("}} // namespace {}", cppNs(f->ast.package)));
        return w.take();
    }

    const Schema& S;
    const CompileOptions& O;
    DiagnosticEngine& D;
};

} // namespace

std::vector<OutputFile> generateRepl(const Schema& schema, const CompileOptions& options, DiagnosticEngine& diags) {
    ReplGenerator g(schema, options, diags);
    return g.run();
}

} // namespace helios::schemac
