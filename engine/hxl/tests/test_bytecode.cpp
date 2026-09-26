// HXL bytecode: canonical encoding, verifier rejections and a deterministic mutation fuzz (decode
// never crashes; whatever decodes evaluates safely within its static cost).
#include <doctest/doctest.h>

#include <algorithm>
#include <bit>

#include "helios/hxl/hxl.h"

using namespace helios;
using namespace helios::hxl;

namespace {

Program mustCompile(std::string_view src, std::vector<std::string> params = {"self"}) {
    CompileOptions opts;
    opts.params = std::move(params);
    auto p = compile(src, opts);
    REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
    return std::move(*p);
}

Status decodeStatus(const std::vector<u8>& bytes) {
    Diagnostic d;
    auto p = Program::decode(bytes, &d);
    return p ? Status::Ok : d.status;
}

// Answers every input, so any decodable program evaluates.
class TotalEnv final : public Env {
public:
    Curve c{{0.0, 1.0}, {0.0, 2.0}};
    bool attr(u32, u32, f64& out) noexcept override {
        out = 1.5;
        return true;
    }
    bool field(u32, u32, f64& out) noexcept override {
        out = -2.0;
        return true;
    }
    bool tag(u32, u32 s, bool& out) noexcept override {
        out = (s & 1) != 0;
        return true;
    }
    const Curve* curve(u32) noexcept override { return &c; }
    bool stacks(f64& out) noexcept override {
        out = 3.0;
        return true;
    }
    bool level(f64& out) noexcept override {
        out = 4.0;
        return true;
    }
};

// Offset of the code bytes in an encoding (the u32 length precedes them).
usize codeOffset(const Program& p) { return p.encode().size() - p.code().size(); }

} // namespace

TEST_CASE("hxl bytecode: encode/decode round trip is canonical") {
    const char* sources[] = {
        "1",
        "attr(self, A.B) * self.x + stacks() - level()",
        "select(tag(self, T.U), curve(C, 2), min(1, 2, 3))",
        "true && (false || !(1 < 2))",
        "clamp(lerp(1, 2, 0.5), 0, sqrt(abs(floor(-3.5)) + ceil(2.1)))",
        "formula F(a, b) = exp(ln(attr(a, X))) ^ asinh(b.y)",
    };
    for (const char* src : sources) {
        CAPTURE(src);
        const Program p = mustCompile(src);
        const std::vector<u8> bytes = p.encode();
        auto q = Program::decode(bytes);
        REQUIRE(q.ok());
        CHECK(*q == p);
        CHECK(q->encode() == bytes);
        CHECK(q->hash() == p.hash());
        CHECK(q->maxStack() == p.maxStack());
        CHECK(q->cost() == p.cost());
        CHECK_FALSE(p.disassemble().empty());
    }
    // The hash is FNV-1a 64 over the encoding: a fixed program pins the format.
    const Program one = mustCompile("1");
    const std::vector<u8> expected = {'H', 'X', 'L', '1', 1, 0, 1, 0, 1, 0, 0, 0, // magic ver type stack cost
                                      0, 0,                                        // name
                                      1, 4, 0, 's', 'e', 'l', 'f',                  // params
                                      1, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f,           // constants
                                      0, 0, 0, 0, 0, 0, 0, 0,                       // symbol tables
                                      3, 0, 0, 0, 0x01, 0, 0};                      // code
    CHECK(one.encode() == expected);
    CHECK(one.usesStacks() == false);
    CHECK(mustCompile("stacks()").usesStacks());
    CHECK(mustCompile("level()").usesLevel());
}

TEST_CASE("hxl bytecode: the verifier rejects malformed programs") {
    const Program p = mustCompile("select(1 < 2, attr(self, A), 3)");
    const std::vector<u8> good = p.encode();
    const usize code = codeOffset(p);
    REQUIRE(decodeStatus(good) == Status::Ok);

    auto mutated = [&](auto&& fn) {
        std::vector<u8> b = good;
        fn(b);
        return decodeStatus(b);
    };
    CHECK(mutated([](auto& b) { b[0] = 'X'; }) == Status::Bytecode);             // magic
    CHECK(mutated([](auto& b) { b[4] = 2; }) == Status::Bytecode);               // version
    CHECK(mutated([](auto& b) { b[5] = 1; }) == Status::Bytecode);               // result type
    CHECK(mutated([](auto& b) { b[6] = 9; }) == Status::Bytecode);               // max stack
    CHECK(mutated([](auto& b) { b[8] ^= 1; }) == Status::Bytecode);              // cost
    CHECK(mutated([](auto& b) { b.push_back(0); }) == Status::Bytecode);         // trailing bytes
    CHECK(mutated([](auto& b) { b.pop_back(); }) == Status::Bytecode);           // truncated
    CHECK(mutated([&](auto& b) { b[code] = 0xEE; }) == Status::Bytecode);        // invalid opcode
    CHECK(mutated([&](auto& b) { b[code + 1] = 7; }) == Status::Bytecode);       // constant index
    // Code layout: 0 Const0, 3 Const1, 6 Lt, 7 JIF(+7), 10 Attr(0,0), 14 Jump(+3), 17 Const2.
    CHECK(mutated([&](auto& b) { b[code + 8] = 1; b[code + 9] = 0; }) == Status::Bytecode); // JIF mid-instruction
    CHECK(mutated([&](auto& b) { b[code + 8] = 0xFF; }) == Status::Bytecode);     // JIF out of range
    CHECK(mutated([&](auto& b) { b[code + 15] = 0; }) == Status::Bytecode);       // empty Jump
    CHECK(mutated([&](auto& b) { b[code + 11] = 1; }) == Status::Bytecode);       // Attr param out of range
    CHECK(mutated([&](auto& b) { b[code + 12] = 1; }) == Status::Bytecode);       // Attr symbol out of range
    CHECK(mutated([&](auto& b) { b[code + 6] = static_cast<u8>(Op::Add); }) == Status::Bytecode); // JIF pops a number
    CHECK(mutated([&](auto& b) { b[code + 6] = static_cast<u8>(Op::Not); }) == Status::Bytecode); // Not on a number
    CHECK(mutated([&](auto& b) { b[code + 17] = static_cast<u8>(Op::True); b.resize(b.size() - 2); }) ==
          Status::Bytecode); // branches disagree (and the code length no longer matches)

    // Hand-built programs through the public round trip: an empty program and an unused constant.
    CHECK(decodeStatus(std::vector<u8>{}) == Status::Bytecode);
    const Program twoConst = mustCompile("1 + 2");
    std::vector<u8> swapped = twoConst.encode();
    const usize c2 = codeOffset(twoConst);
    std::swap(swapped[c2 + 1], swapped[c2 + 4]); // Const 1, Const 0: not first-use order
    CHECK(decodeStatus(swapped) == Status::Bytecode);
    std::vector<u8> sameConst = twoConst.encode();
    sameConst[c2 + 4] = 0; // Const 0 twice: constant 1 unused
    CHECK(decodeStatus(sameConst) == Status::Bytecode);
}

TEST_CASE("hxl bytecode: mutation fuzz never crashes and decoded programs stay within bounds") {
    const Program seeds[] = {
        mustCompile("select(tag(self, T), attr(self, A) * 2 + curve(C, self.x), min(stacks(), level(), 3))"),
        mustCompile("true && (1 < 2 || !false) && exp(1) > ln(2)"),
        mustCompile("formula F(a, b) = clamp(lerp(attr(a, X), b.y, 0.5), -1, pow(2, 3)) ^ 0.5", {}),
    };
    u64 state = 0x9E3779B97F4A7C15ull;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    TotalEnv env;
    usize decoded = 0;
    for (int iter = 0; iter < 30000; ++iter) {
        std::vector<u8> b = seeds[iter % 3].encode();
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int e = 0; e < edits; ++e) {
            const usize at = next() % b.size();
            switch (next() % 4) {
            case 0: b[at] = static_cast<u8>(next()); break;
            case 1: b[at] ^= static_cast<u8>(1u << (next() % 8)); break;
            case 2: b.insert(b.begin() + static_cast<isize>(at), static_cast<u8>(next())); break;
            default:
                if (b.size() > 1) b.erase(b.begin() + static_cast<isize>(at));
                break;
            }
        }
        auto p = Program::decode(b);
        if (!p) continue;
        ++decoded;
        CHECK(p->maxStack() <= limits::kMaxStack);
        CHECK(p->cost() <= limits::kMaxCost);
        Value v;
        CHECK(eval(*p, env, v) == Status::Ok);
        CHECK(v.type == p->resultType());
        CHECK(p->encode() == b); // anything accepted is canonical
    }
    MESSAGE("mutants that decoded: " << decoded);
}

TEST_CASE("hxl bytecode: decode reports size and cost limits as E_BYTECODE at 0:0, like Go (review regression)") {
    // Hand-assembled `exp(exp(...exp(1)...))` with 600 Exp ops: cost 2 + 600 * 8 > limits::kMaxCost.
    std::vector<u8> b = {'H', 'X', 'L', '1', 1, 0, 1, 0, 0, 0, 0, 0, // magic ver type stack cost
                         0, 0,                                        // name
                         1, 4, 0, 's', 'e', 'l', 'f',                  // params
                         1, 0, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f,           // constants: 1.0
                         0, 0, 0, 0, 0, 0, 0, 0};                      // symbol tables
    const u32 codeSize = 3 + 600;
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(codeSize >> (8 * i)));
    b.insert(b.end(), {static_cast<u8>(Op::Const), 0, 0});
    b.insert(b.end(), 600, static_cast<u8>(Op::Exp));
    Diagnostic d;
    d.line = 7;
    d.column = 7;
    auto p = Program::decode(b, &d);
    REQUIRE_FALSE(p.ok());
    CHECK(d.status == Status::Bytecode);
    CHECK(d.line == 0);
    CHECK(d.column == 0);
    CHECK(d.message.find("cost") != std::string::npos);
    // Over budget from source: E_LIMIT at 1:1 (the cost is a property of the whole program).
    std::string src = "max(exp(1)";
    for (int i = 0; i < 599; ++i) src += ", exp(1)";
    src += ")";
    Diagnostic cd;
    CHECK_FALSE(compile(src, {}, &cd).ok());
    CHECK(cd.status == Status::Limit);
    CHECK(cd.line == 1);
    CHECK(cd.column == 1);
}
