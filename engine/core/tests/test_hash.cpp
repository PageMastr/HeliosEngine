#include <doctest/doctest.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "helios/core/hash.h"

using namespace helios;
using namespace helios::literals;

// Reference values produced by the upstream xxHash 0.8.3 library (xxhsum / python-xxhash) and the
// FNV reference test vectors.
TEST_CASE("hash: XXH3-64 known vectors") {
    CHECK(hash64("", 0) == 0x2d06800538d394c2ull);
    CHECK(hash64(std::string_view("a")) == 0xe6c632b61e964e1full);
    CHECK(hash64(std::string_view("abc")) == 0x78af5f94892f3950ull);
    CHECK(hash64(std::string_view("Helios")) == 0x6aedf60582d4458cull);
    CHECK(hash64(std::string_view("The quick brown fox jumps over the lazy dog")) == 0xce7d19a5418fb365ull);
    CHECK(hash64(std::string_view(""), 12345) == 0xa706d6c022c3723bull);
    CHECK(hash64(std::string_view("abc"), 12345) == 0x717e64eafd9a85d0ull);

    std::vector<u8> bytes(512);
    for (usize i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(i & 0xFF);
    CHECK(hash64(bytes.data(), bytes.size()) == 0x1059105ad19bfa09ull);
    CHECK(hash64(bytes.data(), bytes.size(), 12345) == 0xfe9e34b355b57b67ull);
}

TEST_CASE("hash: XXH3-128 known vectors") {
    const Hash128 empty = hash128("", 0);
    CHECK(empty.low == 0x6001c324468d497full);
    CHECK(empty.high == 0x99aa06d3014798d8ull);
    CHECK(empty.toHex() == "99aa06d3014798d86001c324468d497f");
    const Hash128 helios = hash128(std::string_view("Helios"));
    CHECK(helios.low == 0xd1054e5c2d3d52d9ull);
    CHECK(helios.high == 0x6edfcfa5d771d191ull);
    const Hash128 fox = hash128(std::string_view("The quick brown fox jumps over the lazy dog"));
    CHECK(fox.low == 0x24a1cc2e3a8a7651ull);
    CHECK(fox.high == 0xddd650205ca3e7faull);
    CHECK(hash128(std::string_view(""), 12345).low == 0xc426fd87a4f77c66ull);
}

TEST_CASE("hash: streaming equals one-shot for every split point") {
    std::string data;
    for (int i = 0; i < 1500; ++i) data.push_back(static_cast<char>('a' + (i * 7) % 26));
    for (usize split : {usize(0), usize(1), usize(15), usize(64), usize(239), usize(240), usize(241), usize(1024), data.size()}) {
        Hasher64 h64(99);
        h64.update(std::string_view(data).substr(0, split));
        h64.update(std::string_view(data).substr(split));
        CHECK(h64.digest() == hash64(data, 99));

        Hasher128 h128(7);
        h128.update(std::string_view(data).substr(0, split));
        h128.update(std::string_view(data).substr(split));
        CHECK(h128.digest() == hash128(data, 7));
    }
    Hasher64 h;
    h.updateValue(u32{0x01020304});
    const u32 v = 0x01020304;
    CHECK(h.digest() == hash64(&v, sizeof(v)));
    h.reset();
    CHECK(h.digest() == hash64("", 0));
    // A Hasher can be copied mid-stream (the state has no self pointers).
    Hasher64 a;
    a.update("abc");
    Hasher64 b = a;
    a.update("def");
    b.update("def");
    CHECK(a.digest() == b.digest());
}

TEST_CASE("hash: constexpr FNV-1a") {
    static_assert(fnv1a64("") == 0xcbf29ce484222325ull);
    static_assert(fnv1a64("a") == 0xaf63dc4c8601ec8cull);
    static_assert(fnv1a64("foobar") == 0x85944171f73967e8ull);
    static_assert("Helios"_fnv == 0xbd7b9fe094e878b7ull);
    static_assert(fnv1a32("") == 0x811c9dc5u);
    static_assert(fnv1a32("a") == 0xe40c292cu);
    constexpr u64 id = fnv1a64("player.health");
    switch (fnv1a64("player.health")) {
    case id: CHECK(true); break;
    default: CHECK(false); break;
    }
}

TEST_CASE("hash: combine and mix") {
    CHECK(hashCombine(1, 2) != hashCombine(2, 1));
    CHECK(hashCombine(0, 0) != 0);
    static_assert(mix64(0) == 0);
    std::unordered_set<u64> seen;
    for (u64 i = 0; i < 10000; ++i) seen.insert(mix64(i));
    CHECK(seen.size() == 10000);
    const u64 key = 0x1234;
    CHECK(hashValue(key) == hash64(&key, sizeof(key)));
    std::unordered_set<Hash128> set;
    set.insert(hash128(std::string_view("x")));
    set.insert(hash128(std::string_view("y")));
    CHECK(set.size() == 2);
    CHECK((Hash128{1, 0} < Hash128{0, 1}));
}
