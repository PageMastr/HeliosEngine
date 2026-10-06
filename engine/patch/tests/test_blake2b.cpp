// BLAKE2b-256 over Monocypher: known answers, streaming equivalence, hex.
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <string_view>

#include "patch_test_util.h"

using namespace helios;
using namespace helios::patch;

namespace {

std::span<const u8> text(std::string_view s) { return {reinterpret_cast<const u8*>(s.data()), s.size()}; }

TEST_CASE("blake2b: BLAKE2b-256 known answers") {
    // BLAKE2b with a 32-byte digest (RFC 7693 parameters; the values b2sum -l 256 prints).
    CHECK(blake2b256({}).toHex() == "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");
    CHECK(blake2b256(text("abc")).toHex() ==
          "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");
    CHECK(blake2b256(text("The quick brown fox jumps over the lazy dog")).toHex() ==
          "01718cec35cd3d796dd00020e0bfecb473ad23457d063b75eff29c0ffa2e58a9");
}

TEST_CASE("blake2b: streaming in any pieces equals one shot, and finish() resets") {
    const std::vector<u8> data = test::randomBytes(5, 300'000);
    const Hash256 whole = blake2b256(data);
    Blake2b256 h;
    usize at = 0;
    for (usize step = 1; at < data.size(); step = step * 3 + 1) {
        const usize n = std::min(step, data.size() - at);
        h.update(std::span<const u8>(data).subspan(at, n));
        at += n;
    }
    h.update({}); // empty updates are fine
    CHECK(h.finish() == whole);
    CHECK(h.finish() == blake2b256({})); // reset after finish
    h.update(data);
    CHECK(h.finish() == whole);
}

TEST_CASE("blake2b: hex round trip, ordering and zero") {
    const Hash256 a = blake2b256(text("a"));
    CHECK(Hash256::fromHex(a.toHex()) == a);
    std::string upper = a.toHex();
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(Hash256::fromHex(upper) == a);
    CHECK_FALSE(Hash256::fromHex(a.toHex().substr(1)).has_value());
    CHECK_FALSE(Hash256::fromHex(std::string(64, 'g')).has_value());
    CHECK(Hash256{}.isZero());
    CHECK_FALSE(a.isZero());
    Hash256 lo, hi;
    hi.bytes[0] = 1;
    lo.bytes[31] = 0xFF;
    CHECK(lo < hi); // byte-wise (memcmp) order, as Go's bytes.Compare
}

} // namespace
