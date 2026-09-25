#include "corpus.h"

#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>

#include <yyjson.h>

#include "helios/core/hash.h"

namespace helios::pcg::test {

namespace {

constexpr f64 kRadius1500km = 1'500'000.0;
constexpr f64 kRadius6400km = 6'400'000.0;

u64 splitmix(u64& state) {
    u64 z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::string hex(u64 v) { return std::format("\"{:#018x}\"", v); }
std::string hex32(u32 v) { return std::format("\"{:#010x}\"", v); }

bool parseHex(yyjson_val* v, u64& out) {
    if (!v || !yyjson_is_str(v)) return false;
    std::string_view s(yyjson_get_str(v), yyjson_get_len(v));
    if (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
    s.remove_prefix(2);
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out, 16);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

bool parseInt(yyjson_val* v, i64& out) {
    if (!v || !yyjson_is_int(v)) return false;
    out = yyjson_get_sint(v);
    return true;
}

std::string domainJson(const TileDomain& d) {
    if (d.kind == DomainKind::CubeSphere) {
        return std::format("{{\"kind\": \"cube\", \"face\": {}, \"level\": {}, \"x\": {}, \"y\": {}, \"radiusQ8\": {}}}",
                           static_cast<u32>(d.tile.face), d.tile.level, d.tile.x, d.tile.y, d.radiusQ8);
    }
    return std::format("{{\"kind\": \"planar\", \"origin\": [{}, {}, {}], \"spacing\": {}, \"x\": {}, \"y\": {}}}",
                       hex(static_cast<u64>(d.origin.x)), hex(static_cast<u64>(d.origin.y)),
                       hex(static_cast<u64>(d.origin.z)), hex(static_cast<u64>(d.spacingRaw)), d.tile.x, d.tile.y);
}

bool parseDomain(yyjson_val* obj, TileDomain& d) {
    if (!obj || !yyjson_is_obj(obj)) return false;
    yyjson_val* kind = yyjson_obj_get(obj, "kind");
    if (!kind || !yyjson_is_str(kind)) return false;
    const std::string_view k(yyjson_get_str(kind));
    i64 x = 0, y = 0;
    if (!parseInt(yyjson_obj_get(obj, "x"), x) || !parseInt(yyjson_obj_get(obj, "y"), y)) return false;
    if (k == "cube") {
        i64 face = 0, level = 0, radius = 0;
        if (!parseInt(yyjson_obj_get(obj, "face"), face) || !parseInt(yyjson_obj_get(obj, "level"), level) ||
            !parseInt(yyjson_obj_get(obj, "radiusQ8"), radius) || face < 0 || face > 5 || level < 0 ||
            level > static_cast<i64>(hnoise::kMaxCubeLevel)) {
            return false;
        }
        d = TileDomain{};
        d.kind = DomainKind::CubeSphere;
        d.tile = CubeTile{static_cast<CubeFace>(face), static_cast<u8>(level), static_cast<u32>(x), static_cast<u32>(y)};
        d.radiusQ8 = static_cast<u32>(radius);
        return true;
    }
    if (k == "planar") {
        yyjson_val* origin = yyjson_obj_get(obj, "origin");
        u64 o[3] = {}, spacing = 0;
        if (!origin || !yyjson_is_arr(origin) || yyjson_arr_size(origin) != 3) return false;
        for (usize i = 0; i < 3; ++i) {
            if (!parseHex(yyjson_arr_get(origin, i), o[i])) return false;
        }
        if (!parseHex(yyjson_obj_get(obj, "spacing"), spacing)) return false;
        d = TileDomain::planar({static_cast<i64>(o[0]), static_cast<i64>(o[1]), static_cast<i64>(o[2])},
                               static_cast<i64>(spacing), static_cast<u32>(x), static_cast<u32>(y));
        return true;
    }
    return false;
}

CompileOptions optionsFor(const TileDomain& d, bool collision) {
    CompileOptions o;
    o.collisionLevel = collision;
    o.sampleSpacingMetres = d.sampleSpacingMetres();
    return o;
}

Result<std::string> readText(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return makeError(ErrorCode::NotFound, "cannot open {}", p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Doc {
    yyjson_doc* doc = nullptr;
    ~Doc() { yyjson_doc_free(doc); }
};

Result<void> parseDoc(const std::string& text, const std::filesystem::path& p, Doc& out) {
    yyjson_read_err err{};
    out.doc = yyjson_read_opts(const_cast<char*>(text.data()), text.size(),
                               YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, nullptr, &err);
    if (!out.doc) return makeError(ErrorCode::ParseError, "{}: {} at byte {}", p.string(), err.msg, err.pos);
    return {};
}

} // namespace

Result<TerrainGraph> graphByName(std::string_view name) {
    if (name == "reference40") return makeReferenceGraph40();
    if (name.starts_with("node:")) {
        const std::string_view kind = name.substr(5);
        for (u32 k = 0; k < static_cast<u32>(NodeKind::Count); ++k) {
            if (nodeKindName(static_cast<NodeKind>(k)) == kind) return makeSingleNodeGraph(static_cast<NodeKind>(k));
        }
    }
    return makeError(ErrorCode::NotFound, "unknown corpus graph '{}'", name);
}

u64 hashPositions(std::span<const FixedPos> positions) {
    std::vector<i64> planes;
    planes.reserve(positions.size() * 3);
    for (const FixedPos& p : positions) planes.push_back(p.x);
    for (const FixedPos& p : positions) planes.push_back(p.y);
    for (const FixedPos& p : positions) planes.push_back(p.z);
    return hash64(planes.data(), planes.size() * sizeof(i64), 0x706F73ull);
}

std::vector<FixedPos> referencePositions(const TileDomain& domain) {
    std::vector<FixedPos> out(kTileSamples);
    for (u32 i = 0; i < kTileSamples; ++i) out[i] = samplePosition(domain, i);
    return out;
}

Corpus generateCorpus() {
    Corpus c;
    // Lattice hashes and noise values: edge cells (wrap of x + 1 at 2^32), edge fractions, random.
    u64 rng = 0x484E4F495345ull; // "HNOISE"
    const i32 edgeCells[] = {0, -1, 1, 2147483647, -2147483647 - 1, 65535, -65536};
    const i32 edgeFracs[] = {0, 1, 32768, 65535};
    for (i32 cell : edgeCells) {
        for (i32 frac : edgeFracs) {
            LatticeCase lc;
            lc.seed = static_cast<u32>(splitmix(rng));
            lc.cell = {cell, static_cast<i32>(~static_cast<u32>(cell)), static_cast<i32>(static_cast<u32>(cell) * 3u)};
            lc.frac = {frac, 65535 - frac, frac ^ 0x5A5A};
            c.lattice.push_back(lc);
        }
    }
    for (u32 seed : {0u, 1u, 0xFFFFFFFFu}) {
        LatticeCase lc;
        lc.seed = seed;
        c.lattice.push_back(lc);
    }
    for (int i = 0; i < 200; ++i) {
        LatticeCase lc;
        lc.seed = static_cast<u32>(splitmix(rng));
        for (usize a = 0; a < 3; ++a) {
            lc.cell[a] = static_cast<i32>(static_cast<u32>(splitmix(rng)));
            lc.frac[a] = static_cast<i32>(splitmix(rng) & 0xFFFF);
        }
        c.lattice.push_back(lc);
    }
    for (LatticeCase& lc : c.lattice) {
        lc.hash = hnoise::latticeHash(lc.seed, lc.cell[0], lc.cell[1], lc.cell[2]);
        LatticeCoord coord;
        coord.cell = lc.cell;
        coord.frac = lc.frac;
        lc.noise = hnoise::noise3(lc.seed, coord);
    }

    // Tiles.
    const FixedPos planarOrigin{Q32::fromDouble(-1'000'000.25).raw(), Q32::fromDouble(250'000.5).raw(),
                                Q32::fromDouble(1234.5).raw()};
    const auto add = [&](std::string name, std::string graph, TileDomain domain, bool collision) {
        TileCase t;
        t.name = std::move(name);
        t.graph = std::move(graph);
        t.domain = domain;
        t.options = optionsFor(domain, collision);
        c.tiles.push_back(std::move(t));
    };
    for (u32 k = 0; k < static_cast<u32>(NodeKind::Count); ++k) {
        const std::string kind(nodeKindName(static_cast<NodeKind>(k)));
        const CubeTile tile{static_cast<CubeFace>(k % 6), 15, 1000u + 2345u * k, 30000u - 1777u * k};
        add("node." + kind + ".sphere", "node:" + kind, TileDomain::cubeSphere(tile, kRadius1500km), true);
        add("node." + kind + ".planar", "node:" + kind, TileDomain::planar(planarOrigin, i64(1) << 32, 3, 7), true);
    }
    for (u32 f = 0; f < 6; ++f) {
        const CubeTile tile{static_cast<CubeFace>(f), 15, 16384u + 97u * f, 16000u - 311u * f};
        add(std::format("ref40.r1500km.l15.face{}.collision", f), "reference40", TileDomain::cubeSphere(tile, kRadius1500km),
            true);
    }
    add("ref40.r1500km.l15.edge.collision", "reference40",
        TileDomain::cubeSphere({CubeFace::NegY, 15, 32767u, 0u}, kRadius1500km), true);
    add("ref40.r1500km.l0.face3.adaptive", "reference40", TileDomain::cubeSphere({CubeFace::NegY, 0, 0, 0}, kRadius1500km),
        false);
    for (u8 level : {u8(6), u8(8), u8(10), u8(12)}) {
        const u32 n = 1u << level;
        add(std::format("ref40.r1500km.l{}.adaptive", level), "reference40",
            TileDomain::cubeSphere({CubeFace::PosX, level, n / 3u, (2u * n) / 5u}, kRadius1500km), false);
        add(std::format("ref40.r1500km.l{}.full", level), "reference40",
            TileDomain::cubeSphere({CubeFace::PosX, level, n / 3u, (2u * n) / 5u}, kRadius1500km), true);
    }
    add("ref40.r6400km.l17.collision", "reference40",
        TileDomain::cubeSphere({CubeFace::PosZ, 17, 70000u, 12345u}, kRadius6400km), true);
    add("ref40.r1500km.l25.collision", "reference40",
        TileDomain::cubeSphere({CubeFace::NegZ, 25, 20000000u, 13000000u}, kRadius1500km), true);
    add("ref40.planar.collision", "reference40", TileDomain::planar(planarOrigin, i64(1) << 31, 5, 2), true);
    add("ref40.planar.adaptive", "reference40", TileDomain::planar(planarOrigin, i64(40) << 32, 1, 1), false);

    TileEvaluator eval(KernelKind::Scalar);
    std::vector<i64> heights(kTileSamples);
    for (TileCase& t : c.tiles) {
        const TerrainGraph g = graphByName(t.graph).value();
        const TerrainProgram program = compileTerrainGraph(g, t.options).value();
        t.programHash = program.hash();
        t.positionsHash = hashPositions(referencePositions(t.domain));
        HELIOS_VERIFY(eval.evaluate(program, t.domain, heights).ok());
        t.heightsHash = hashHeights(heights);
        t.probes = {heights[0], heights[kTileSamples / 2], heights[kTileSamples - 1]};
    }
    return c;
}

Result<Corpus> loadCorpus(const std::filesystem::path& dir) {
    Corpus c;
    {
        const std::filesystem::path p = dir / "lattice.jsonc";
        HELIOS_TRY_ASSIGN(const std::string text, readText(p));
        Doc doc;
        HELIOS_TRY(parseDoc(text, p, doc));
        yyjson_val* cases = yyjson_obj_get(yyjson_doc_get_root(doc.doc), "cases");
        if (!cases || !yyjson_is_arr(cases)) return makeError(ErrorCode::ParseError, "{}: no cases array", p.string());
        usize i, n;
        yyjson_val* e;
        yyjson_arr_foreach(cases, i, n, e) {
            LatticeCase lc;
            u64 seed = 0, hash = 0, noise = 0;
            yyjson_val* cell = yyjson_obj_get(e, "cell");
            yyjson_val* frac = yyjson_obj_get(e, "frac");
            if (!parseHex(yyjson_obj_get(e, "seed"), seed) || !parseHex(yyjson_obj_get(e, "hash"), hash) ||
                !parseHex(yyjson_obj_get(e, "noise"), noise) || !cell || yyjson_arr_size(cell) != 3 || !frac ||
                yyjson_arr_size(frac) != 3 || yyjson_obj_size(e) != 5) {
                return makeError(ErrorCode::ParseError, "{}: malformed lattice case {}", p.string(), i);
            }
            for (usize a = 0; a < 3; ++a) {
                u64 cv = 0;
                i64 fv = 0;
                if (!parseHex(yyjson_arr_get(cell, a), cv) || !parseInt(yyjson_arr_get(frac, a), fv) || fv < 0 || fv > 65535) {
                    return makeError(ErrorCode::ParseError, "{}: malformed lattice case {}", p.string(), i);
                }
                lc.cell[a] = static_cast<i32>(static_cast<u32>(cv));
                lc.frac[a] = static_cast<i32>(fv);
            }
            lc.seed = static_cast<u32>(seed);
            lc.hash = static_cast<u32>(hash);
            lc.noise = static_cast<i32>(static_cast<u32>(noise));
            c.lattice.push_back(lc);
        }
    }
    {
        const std::filesystem::path p = dir / "tiles.jsonc";
        HELIOS_TRY_ASSIGN(const std::string text, readText(p));
        Doc doc;
        HELIOS_TRY(parseDoc(text, p, doc));
        yyjson_val* cases = yyjson_obj_get(yyjson_doc_get_root(doc.doc), "cases");
        if (!cases || !yyjson_is_arr(cases)) return makeError(ErrorCode::ParseError, "{}: no cases array", p.string());
        usize i, n;
        yyjson_val* e;
        yyjson_arr_foreach(cases, i, n, e) {
            TileCase t;
            yyjson_val* name = yyjson_obj_get(e, "name");
            yyjson_val* graph = yyjson_obj_get(e, "graph");
            yyjson_val* collision = yyjson_obj_get(e, "collision");
            yyjson_val* probes = yyjson_obj_get(e, "probes");
            if (!name || !yyjson_is_str(name) || !graph || !yyjson_is_str(graph) || !collision ||
                !yyjson_is_bool(collision) || !parseDomain(yyjson_obj_get(e, "domain"), t.domain) ||
                !parseHex(yyjson_obj_get(e, "programHash"), t.programHash) ||
                !parseHex(yyjson_obj_get(e, "positionsHash"), t.positionsHash) ||
                !parseHex(yyjson_obj_get(e, "heightsHash"), t.heightsHash) || !probes || yyjson_arr_size(probes) != 3 ||
                yyjson_obj_size(e) != 8) {
                return makeError(ErrorCode::ParseError, "{}: malformed tile case {}", p.string(), i);
            }
            for (usize a = 0; a < 3; ++a) {
                u64 v = 0;
                if (!parseHex(yyjson_arr_get(probes, a), v)) {
                    return makeError(ErrorCode::ParseError, "{}: malformed probes in case {}", p.string(), i);
                }
                t.probes[a] = static_cast<i64>(v);
            }
            t.name = yyjson_get_str(name);
            t.graph = yyjson_get_str(graph);
            t.options = optionsFor(t.domain, yyjson_get_bool(collision));
            c.tiles.push_back(std::move(t));
        }
    }
    return c;
}

Result<void> writeCorpus(const std::filesystem::path& dir, const Corpus& c) {
    {
        std::ofstream out(dir / "lattice.jsonc", std::ios::binary | std::ios::trunc);
        if (!out) return makeError(ErrorCode::IoError, "cannot write {}", (dir / "lattice.jsonc").string());
        out << "// hnoise lattice conformance cases (generated by pcg_tests from the scalar reference with\n"
               "// HELIOS_UPDATE_HNOISE_CORPUS=1; see README.md). hash = xxHash32 of the cell, noise = Q16 noise3.\n"
               "{\n  \"cases\": [\n";
        for (usize i = 0; i < c.lattice.size(); ++i) {
            const LatticeCase& l = c.lattice[i];
            out << std::format("    {{\"seed\": {}, \"cell\": [{}, {}, {}], \"frac\": [{}, {}, {}], \"hash\": {}, \"noise\": {}}}{}\n",
                               hex32(l.seed), hex32(static_cast<u32>(l.cell[0])), hex32(static_cast<u32>(l.cell[1])),
                               hex32(static_cast<u32>(l.cell[2])), l.frac[0], l.frac[1], l.frac[2], hex32(l.hash),
                               hex32(static_cast<u32>(l.noise)), i + 1 < c.lattice.size() ? "," : "");
        }
        out << "  ]\n}\n";
    }
    {
        std::ofstream out(dir / "tiles.jsonc", std::ios::binary | std::ios::trunc);
        if (!out) return makeError(ErrorCode::IoError, "cannot write {}", (dir / "tiles.jsonc").string());
        out << "// hnoise tile conformance cases (generated by pcg_tests from the scalar reference with\n"
               "// HELIOS_UPDATE_HNOISE_CORPUS=1; see README.md). Hashes are XXH3 of the little-endian Q32.32 values.\n"
               "{\n  \"cases\": [\n";
        for (usize i = 0; i < c.tiles.size(); ++i) {
            const TileCase& t = c.tiles[i];
            out << std::format("    {{\"name\": \"{}\", \"graph\": \"{}\", \"collision\": {},\n     \"domain\": {},\n"
                               "     \"programHash\": {}, \"positionsHash\": {}, \"heightsHash\": {},\n"
                               "     \"probes\": [{}, {}, {}]}}{}\n",
                               t.name, t.graph, t.options.collisionLevel ? "true" : "false", domainJson(t.domain),
                               hex(t.programHash), hex(t.positionsHash), hex(t.heightsHash),
                               hex(static_cast<u64>(t.probes[0])), hex(static_cast<u64>(t.probes[1])),
                               hex(static_cast<u64>(t.probes[2])), i + 1 < c.tiles.size() ? "," : "");
        }
        out << "  ]\n}\n";
    }
    return {};
}

std::filesystem::path corpusDir() { return HELIOS_HNOISE_CORPUS_DIR; }

} // namespace helios::pcg::test
