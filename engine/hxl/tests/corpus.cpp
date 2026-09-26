// C++ runner of the shared HXL corpus (format: tests/corpus/hxl/README.md). The Go runner
// (services/pkg/hxl/corpus_test.go) implements the same checks.
#include "corpus.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include <yyjson.h>

#include "helios/hxl/hxl.h"

namespace helios::hxl::test {
namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Exact hex-float parser (text after the "0x" prefix, no sign). Fails if not exactly representable.
bool parseHexFloat(std::string_view s, f64& out) {
    usize i = 0;
    u64 mant = 0;
    i64 exp = 0;
    bool digits = false;
    auto addDigit = [&](int d, bool fraction) -> bool {
        digits = true;
        if (mant == 0 && d == 0) {
            if (fraction) exp -= 4;
            return true;
        }
        if ((mant >> 60) != 0) return false;
        mant = mant * 16 + static_cast<u64>(d);
        if (fraction) exp -= 4;
        return true;
    };
    while (i < s.size() && hexDigit(s[i]) >= 0) {
        if (!addDigit(hexDigit(s[i]), false)) return false;
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && hexDigit(s[i]) >= 0) {
            if (!addDigit(hexDigit(s[i]), true)) return false;
            ++i;
        }
    }
    if (!digits || i >= s.size() || (s[i] != 'p' && s[i] != 'P')) return false;
    ++i;
    bool negExp = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negExp = s[i] == '-';
        ++i;
    }
    if (i >= s.size()) return false;
    i64 e = 0;
    while (i < s.size()) {
        if (s[i] < '0' || s[i] > '9') return false;
        e = e * 10 + (s[i] - '0');
        if (e > 100000) return false;
        ++i;
    }
    exp += negExp ? -e : e;
    if (mant == 0) {
        out = 0.0;
        return true;
    }
    const int h = 63 - std::countl_zero(mant);
    const i64 top = h + exp; // value in [2^top, 2^(top+1))
    if (top > 1023) return false;
    u64 bits = 0;
    if (top >= -1022) {
        const int shift = h - 52;
        u64 m53 = 0;
        if (shift > 0) {
            if ((mant & ((u64{1} << shift) - 1)) != 0) return false;
            m53 = mant >> shift;
        } else {
            m53 = mant << -shift;
        }
        bits = (static_cast<u64>(top + 1023) << 52) | (m53 & ((u64{1} << 52) - 1));
    } else {
        const i64 s2 = exp + 1074; // value = mant * 2^exp = m * 2^-1074
        u64 m = 0;
        if (s2 >= 0) {
            if (s2 > 52) return false;
            m = mant << s2;
            if ((m >> s2) != mant) return false;
        } else {
            if (-s2 >= 64) return false;
            if ((mant & ((u64{1} << -s2) - 1)) != 0) return false;
            m = mant >> -s2;
        }
        if (m >= (u64{1} << 52)) return false;
        bits = m;
    }
    out = std::bit_cast<f64>(bits);
    return true;
}

// Decimal text follows the HXL literal rule: zero or a normal finite double (both languages'
// correctly rounded parsers then agree; subnormals must be written as exact hex floats).
bool parseDecimal(std::string_view s, f64& out) {
    if (s.size() > limits::kMaxNumberBytes) return false; // like literals: Go is exact only this far
    f64 v = 0.0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, std::chars_format::general);
    if (ec != std::errc() || ptr != s.data() + s.size() || !std::isfinite(v)) return false;
    if (v == 0.0) {
        for (char c : s) {
            if (c == 'e' || c == 'E') break;
            if (c >= '1' && c <= '9') return false;
        }
    } else if (std::fabs(v) < std::numeric_limits<f64>::min()) {
        return false;
    }
    out = v;
    return true;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string_view str(yyjson_val* v) { return std::string_view(yyjson_get_str(v), yyjson_get_len(v)); }

class Runner {
public:
    explicit Runner(CorpusStats& stats) : m_stats(stats) {}

    void runFile(const std::filesystem::path& path) {
        m_file = path.filename().string();
        std::string text = readFile(path);
        yyjson_read_err err{};
        yyjson_doc* doc = yyjson_read_opts(text.data(), text.size(),
                                           YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS |
                                               YYJSON_READ_NUMBER_AS_RAW,
                                           nullptr, &err);
        if (!doc) {
            failure(std::format("JSON error at byte {}: {}", err.pos, err.msg));
            return;
        }
        ++m_stats.files;
        yyjson_val* root = yyjson_doc_get_root(doc);
        runRoot(root);
        yyjson_doc_free(doc);
    }

private:
    void failure(std::string msg) { m_stats.failures.push_back(std::format("{}: {}{}", m_file, m_case.empty() ? "" : m_case + ": ", msg)); }

    bool checkKeys(yyjson_val* obj, std::initializer_list<std::string_view> allowed, std::string_view what) {
        usize idx = 0, max = 0;
        yyjson_val* key = nullptr;
        yyjson_val* val = nullptr;
        bool ok = true;
        yyjson_obj_foreach(obj, idx, max, key, val) {
            (void)val;
            if (std::find(allowed.begin(), allowed.end(), str(key)) == allowed.end()) {
                failure(std::format("unknown key '{}' in {}", str(key), what));
                ok = false;
            }
        }
        return ok;
    }

    bool number(yyjson_val* v, f64& out) {
        if (yyjson_is_raw(v)) {
            const std::string_view raw(yyjson_get_raw(v), yyjson_get_len(v));
            if (!parseCorpusNumber(raw, out)) {
                failure(std::format("bad number '{}'", raw));
                return false;
            }
            return true;
        }
        if (yyjson_is_str(v)) {
            if (!parseCorpusNumber(str(v), out)) {
                failure(std::format("bad number '{}'", str(v)));
                return false;
            }
            return true;
        }
        failure("expected a number");
        return false;
    }

    bool curves(yyjson_val* obj, std::unordered_map<std::string, Curve>& out) {
        if (!yyjson_is_obj(obj)) {
            failure("curves must be an object");
            return false;
        }
        usize idx = 0, max = 0;
        yyjson_val* key = nullptr;
        yyjson_val* val = nullptr;
        yyjson_obj_foreach(obj, idx, max, key, val) {
            if (!checkKeys(val, {"keys", "values"}, "a curve")) return false;
            Curve c;
            for (const char* part : {"keys", "values"}) {
                yyjson_val* arr = yyjson_obj_get(val, part);
                if (!yyjson_is_arr(arr)) {
                    failure("curve keys/values must be arrays");
                    return false;
                }
                usize i = 0, n = 0;
                yyjson_val* e = nullptr;
                yyjson_arr_foreach(arr, i, n, e) {
                    f64 x = 0;
                    if (!number(e, x)) return false;
                    (std::string_view(part) == "keys" ? c.keys : c.values).push_back(x);
                }
            }
            if (auto r = c.validate(); !r) {
                failure(std::format("invalid curve '{}': {}", str(key), r.error().message));
                return false;
            }
            out[std::string(str(key))] = std::move(c);
        }
        return true;
    }

    bool numberMap(yyjson_val* obj, std::unordered_map<std::string, std::unordered_map<std::string, f64>>& out) {
        usize idx = 0, max = 0;
        yyjson_val* key = nullptr;
        yyjson_val* val = nullptr;
        yyjson_obj_foreach(obj, idx, max, key, val) {
            if (!yyjson_is_obj(val)) {
                failure("attrs/fields entries must be objects");
                return false;
            }
            usize j = 0, m = 0;
            yyjson_val* k2 = nullptr;
            yyjson_val* v2 = nullptr;
            yyjson_obj_foreach(val, j, m, k2, v2) {
                f64 x = 0;
                if (!number(v2, x)) return false;
                out[std::string(str(key))][std::string(str(k2))] = x;
            }
        }
        return true;
    }

    bool inputs(yyjson_val* obj, MapEnv& env) {
        if (!checkKeys(obj, {"attrs", "fields", "tags", "stacks", "level", "curves"}, "inputs")) return false;
        if (yyjson_val* a = yyjson_obj_get(obj, "attrs"); a && !numberMap(a, env.attrs)) return false;
        if (yyjson_val* f = yyjson_obj_get(obj, "fields"); f && !numberMap(f, env.fields)) return false;
        if (yyjson_val* t = yyjson_obj_get(obj, "tags")) {
            usize idx = 0, max = 0;
            yyjson_val* key = nullptr;
            yyjson_val* val = nullptr;
            yyjson_obj_foreach(t, idx, max, key, val) {
                auto& list = env.tags[std::string(str(key))];
                usize i = 0, n = 0;
                yyjson_val* e = nullptr;
                yyjson_arr_foreach(val, i, n, e) { list.emplace_back(str(e)); }
            }
        }
        if (yyjson_val* s = yyjson_obj_get(obj, "stacks")) {
            env.hasStacks = true;
            if (!number(s, env.stacksValue)) return false;
        }
        if (yyjson_val* l = yyjson_obj_get(obj, "level")) {
            env.hasLevel = true;
            if (!number(l, env.levelValue)) return false;
        }
        if (yyjson_val* c = yyjson_obj_get(obj, "curves"); c && !curves(c, env.curves)) return false;
        return true;
    }

    bool expectValue(yyjson_val* expect, const Value& got, std::string_view where) {
        if (yyjson_is_bool(expect)) {
            if (got.type != Type::Bool || got.asBool() != yyjson_get_bool(expect)) {
                failure(std::format("{}expected {}, got {} {}", where, yyjson_get_bool(expect) ? "true" : "false",
                                    typeName(got.type), describeBits(got.number)));
                return false;
            }
            return true;
        }
        f64 want = 0;
        if (!number(expect, want)) return false;
        if (got.type != Type::Number || std::bit_cast<u64>(got.number) != std::bit_cast<u64>(want)) {
            failure(std::format("{}expected {}, got {} {}", where, describeBits(want), typeName(got.type),
                                describeBits(got.number)));
            return false;
        }
        return true;
    }

    // Checks that the expected value is the IEEE (unfused, one rounding per op) value of the row's
    // expression, so a vector cannot pin a wrong result both VMs share, and that fusing the row's
    // multiply-add would change it (06 §1.2 rule 7). This file compiles with FP contraction off.
    void checkFmaSensitive(std::string_view kind, const std::vector<f64>& in, f64 expected, usize row) {
        f64 unfused = 0.0;
        if (kind == "mul_add") {
            unfused = in[0] * in[1] + in[2];
        } else if (kind == "mul_sub") {
            unfused = in[0] * in[1] - in[2];
        } else if (kind == "sub_mul") {
            unfused = in[2] - in[0] * in[1];
        } else if (kind == "dot2") {
            unfused = in[0] * in[1] + in[2] * in[3];
        } else if (kind == "lerp") {
            unfused = in[0] + (in[1] - in[0]) * in[2];
        }
        if (std::bit_cast<u64>(unfused) != std::bit_cast<u64>(expected)) {
            failure(std::format("row {}: the expected value {} is not the unfused reference {}", row, describeBits(expected),
                                describeBits(unfused)));
        }
        std::vector<f64> fused;
        if (kind == "mul_add") {
            fused.push_back(std::fma(in[0], in[1], in[2]));
        } else if (kind == "mul_sub") {
            fused.push_back(std::fma(in[0], in[1], -in[2]));
        } else if (kind == "sub_mul") {
            fused.push_back(std::fma(-in[0], in[1], in[2]));
        } else if (kind == "dot2") {
            const f64 ab = in[0] * in[1];
            const f64 cd = in[2] * in[3];
            fused.push_back(std::fma(in[0], in[1], cd));
            fused.push_back(std::fma(in[2], in[3], ab));
        } else if (kind == "lerp") {
            fused.push_back(std::fma(in[1] - in[0], in[2], in[0]));
        } else {
            failure(std::format("unknown fma kind '{}'", kind));
            return;
        }
        for (f64 f : fused) {
            if (std::bit_cast<u64>(f) == std::bit_cast<u64>(expected)) {
                failure(std::format("row {}: not FMA-sensitive (fused result equals the expected value)", row));
                return;
            }
        }
    }

    void runCase(yyjson_val* c, const std::unordered_map<std::string, Curve>& fileCurves) {
        yyjson_val* nameVal = yyjson_obj_get(c, "name");
        m_case = yyjson_is_str(nameVal) ? std::string(str(nameVal)) : std::string("<unnamed>");
        if (!yyjson_is_str(nameVal)) {
            failure("case without a name");
            return;
        }
        if (!m_names.insert(m_case).second) failure("duplicate case name");
        if (!checkKeys(c, {"name", "src", "params", "expectType", "maxCost", "error", "at", "bytecode", "type",
                           "inputs", "expect", "evalError", "rows", "fma", "comment"},
                       "a case")) {
            return;
        }
        ++m_stats.cases;
        yyjson_val* srcVal = yyjson_obj_get(c, "src");
        if (!yyjson_is_str(srcVal)) {
            failure("missing src");
            return;
        }
        CompileOptions opts;
        if (yyjson_val* p = yyjson_obj_get(c, "params")) {
            opts.params.clear();
            usize i = 0, n = 0;
            yyjson_val* e = nullptr;
            yyjson_arr_foreach(p, i, n, e) { opts.params.emplace_back(str(e)); }
        }
        if (yyjson_val* t = yyjson_obj_get(c, "expectType")) opts.expectedType = str(t) == "bool" ? Type::Bool : Type::Number;
        if (yyjson_val* m = yyjson_obj_get(c, "maxCost")) {
            f64 x = 0;
            if (!number(m, x)) return;
            opts.maxCost = static_cast<u32>(x);
        }
        Diagnostic diag;
        auto compiled = compile(str(srcVal), opts, &diag);
        if (yyjson_val* e = yyjson_obj_get(c, "error")) {
            ++m_stats.errorCases;
            yyjson_val* at = yyjson_obj_get(c, "at");
            const std::string want = std::format("{} {}", str(e), at ? str(at) : "?");
            const std::string got = std::format("{} {}:{}", statusName(diag.status), diag.line, diag.column);
            if (compiled) {
                failure(std::format("expected compile error {}, but it compiled", want));
            } else if (want != got) {
                failure(std::format("expected compile error {}, got {} ({})", want, got, diag.message));
            }
            return;
        }
        if (!compiled) {
            failure(std::format("compile failed: {}", diag.toString()));
            return;
        }
        // A case that compiles must check something: a name plus a source would pass vacuously.
        if (!yyjson_obj_get(c, "bytecode") && !yyjson_obj_get(c, "expect") && !yyjson_obj_get(c, "evalError") &&
            !yyjson_obj_get(c, "rows")) {
            failure("the case compiles but checks nothing (add bytecode, expect, evalError or rows)");
        }
        const Program& prog = *compiled;
        // Canonical bytecode round trip.
        const std::vector<u8> bytes = prog.encode();
        Diagnostic dd;
        auto decoded = Program::decode(bytes, &dd);
        if (!decoded) {
            failure(std::format("decode(encode()) failed: {}", dd.toString()));
            return;
        }
        if (decoded->encode() != bytes) failure("decode(encode()) is not canonical");
        if (yyjson_val* h = yyjson_obj_get(c, "bytecode")) {
            ++m_stats.bytecodeHashes;
            const std::string got = std::format("{:016x}", prog.hash());
            if (str(h) != got) failure(std::format("bytecode hash {} != expected {}", got, str(h)));
        }
        if (yyjson_val* t = yyjson_obj_get(c, "type")) {
            if (str(t) != typeName(prog.resultType())) {
                failure(std::format("result type {} != expected {}", typeName(prog.resultType()), str(t)));
            }
        }
        MapEnv env;
        env.curves = fileCurves;
        if (yyjson_val* in = yyjson_obj_get(c, "inputs"); in && !inputs(in, env)) return;
        yyjson_val* expect = yyjson_obj_get(c, "expect");
        yyjson_val* evalErr = yyjson_obj_get(c, "evalError");
        if (expect || evalErr) {
            ++m_stats.evaluations;
            env.bind(*decoded);
            Value v;
            const Status s = eval(*decoded, env, v);
            if (evalErr) {
                if (statusName(s) != str(evalErr)) {
                    failure(std::format("expected eval error {}, got {}", str(evalErr), statusName(s)));
                }
            } else if (s != Status::Ok) {
                failure(std::format("eval failed: {}", statusName(s)));
            } else {
                expectValue(expect, v, "");
            }
        }
        if (yyjson_val* rows = yyjson_obj_get(c, "rows")) runRows(rows, *decoded, env, yyjson_obj_get(c, "fma"));
    }

    void runRows(yyjson_val* rows, const Program& prog, MapEnv& env, yyjson_val* fma) {
        if (!checkKeys(rows, {"param", "fields", "data"}, "rows")) return;
        yyjson_val* pv = yyjson_obj_get(rows, "param");
        const std::string param = pv ? std::string(str(pv)) : std::string("v");
        std::vector<std::string> fields;
        {
            usize i = 0, n = 0;
            yyjson_val* e = nullptr;
            yyjson_arr_foreach(yyjson_obj_get(rows, "fields"), i, n, e) { fields.emplace_back(str(e)); }
        }
        yyjson_val* data = yyjson_obj_get(rows, "data");
        usize r = 0, rn = 0;
        yyjson_val* row = nullptr;
        yyjson_arr_foreach(data, r, rn, row) {
            if (yyjson_arr_size(row) != fields.size() + 1) {
                failure(std::format("row {} has {} values, expected {}", r, yyjson_arr_size(row), fields.size() + 1));
                return;
            }
            std::vector<f64> in(fields.size());
            for (usize k = 0; k < fields.size(); ++k) {
                if (!number(yyjson_arr_get(row, k), in[k])) return;
                env.fields[param][fields[k]] = in[k];
            }
            env.bind(prog);
            Value v;
            const Status s = eval(prog, env, v);
            ++m_stats.evaluations;
            yyjson_val* expect = yyjson_arr_get(row, fields.size());
            if (s != Status::Ok) {
                failure(std::format("row {}: eval failed: {}", r, statusName(s)));
                continue;
            }
            if (!expectValue(expect, v, std::format("row {}: ", r))) continue;
            if (fma) {
                ++m_stats.fmaRows;
                checkFmaSensitive(str(fma), in, v.number, r);
            }
        }
    }

    void runRoot(yyjson_val* root) {
        m_case.clear();
        if (!yyjson_is_obj(root) || !checkKeys(root, {"description", "curves", "cases"}, "the file")) {
            failure("the file must be an object with description, curves and cases");
            return;
        }
        std::unordered_map<std::string, Curve> fileCurves;
        if (yyjson_val* cv = yyjson_obj_get(root, "curves"); cv && !curves(cv, fileCurves)) return;
        yyjson_val* cases = yyjson_obj_get(root, "cases");
        usize i = 0, n = 0;
        yyjson_val* c = nullptr;
        yyjson_arr_foreach(cases, i, n, c) {
            runCase(c, fileCurves);
            m_case.clear();
        }
    }

    CorpusStats& m_stats;
    std::string m_file;
    std::string m_case;
    std::set<std::string> m_names;
};

} // namespace

bool parseCorpusNumber(std::string_view text, f64& out) {
    if (text == "nan") {
        out = std::bit_cast<f64>(kCanonicalNaNBits);
        return true;
    }
    bool neg = false;
    std::string_view s = text;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        s.remove_prefix(1);
    }
    f64 v = 0.0;
    if (s == "inf") {
        v = std::numeric_limits<f64>::infinity();
    } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        if (!parseHexFloat(s.substr(2), v)) return false;
    } else {
        if (s.empty() || !(std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '.')) return false;
        if (s.find_first_of("xXnN_") != std::string_view::npos) return false;
        if (!parseDecimal(s, v)) return false;
    }
    out = neg ? -v : v;
    return true;
}

std::string describeBits(f64 value) {
    return std::format("0x{:016x} ({})", std::bit_cast<u64>(value), value);
}

CorpusStats runCorpus(const std::string& dir) {
    CorpusStats stats;
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".jsonc") files.push_back(entry.path());
    }
    if (ec) {
        stats.failures.push_back(std::format("cannot list {}: {}", dir, ec.message()));
        return stats;
    }
    std::sort(files.begin(), files.end());
    Runner runner(stats);
    for (const auto& f : files) runner.runFile(f);
    return stats;
}

} // namespace helios::hxl::test
