// AssetId fold (02 §6.1) and the packaging collision check.

#include <doctest/doctest.h>

#include <format>
#include <unordered_set>

#include "helios/asset/asset_id.h"

namespace {

using namespace helios;
using namespace helios::asset;

TEST_CASE("asset id: the fold is XXH3-64 of the GUID's canonical bytes, pinned") {
    const Guid g = Guid::parse("3f2504e0-4f89-41d3-9a0c-0305e82c3301").value();
    const auto bytes = g.toBytes();
    CHECK(AssetId::fromGuid(g).value() == hash64(bytes.data(), bytes.size()));
    // Pinned: paks store this value, so it must never change across platforms, compilers or releases.
    CHECK(AssetId::fromGuid(g).value() == 0x7877b63059538101ull);
    CHECK(AssetId::fromGuid(g).toString().size() == 16);
    CHECK(std::format("{}", AssetId{0xABCull}) == "0000000000000abc");
}

TEST_CASE("asset id: nil folds to the invalid id; structured GUIDs do not collide") {
    CHECK_FALSE(AssetId::fromGuid(Guid::nil()).isValid());
    CHECK_FALSE(AssetId{}.isValid());
    // Equal halves would all fold to 0 under high ^ low; the hash keeps them apart.
    std::unordered_set<u64> seen;
    for (u64 i = 1; i <= 1000; ++i) {
        const AssetId id = AssetId::fromGuid(Guid(i, i));
        CHECK(id.isValid());
        seen.insert(id.value());
    }
    CHECK(seen.size() == 1000);
}

TEST_CASE("asset id set: duplicates, collisions and the reserved id are rejected") {
    AssetIdSet ids;
    const Guid a = Guid::parse("00000000-0000-4000-8000-000000000001").value();
    const Guid b = Guid::parse("00000000-0000-4000-8000-000000000002").value();
    REQUIRE(ids.insert(a).ok());
    CHECK(ids.insert(a).errorCode() == ErrorCode::AlreadyExists);
    CHECK(ids.insert(Guid::nil()).errorCode() == ErrorCode::InvalidArgument);
    const Result<AssetId> idB = ids.insert(b);
    REQUIRE(idB.ok());
    CHECK(*idB == AssetId::fromGuid(b));
    REQUIRE(ids.find(*idB) != nullptr);
    CHECK(*ids.find(*idB) == b);
    CHECK(ids.size() == 2);

    // A weak fold forces the collision path: same high half, different GUIDs.
    AssetIdSet weak([](const Guid& g) { return AssetId{g.high}; });
    REQUIRE(weak.insert(Guid(5, 1)).ok());
    CHECK(weak.check(Guid(5, 2)).errorCode() == ErrorCode::InvalidArgument);
    const Result<AssetId> clash = weak.insert(Guid(5, 2));
    CHECK(clash.errorCode() == ErrorCode::InvalidArgument);
    CHECK(clash.error().message.find("collision") != std::string::npos);
    CHECK(weak.size() == 1); // a failed insert changes nothing
    CHECK(*weak.find(AssetId{5}) == Guid(5, 1));
    // A GUID whose fold is the reserved 0.
    CHECK(weak.insert(Guid(0, 9)).errorCode() == ErrorCode::InvalidArgument);
    // check() reports without inserting.
    CHECK(weak.check(Guid(6, 1)).ok());
    CHECK(weak.size() == 1);
}

} // namespace
