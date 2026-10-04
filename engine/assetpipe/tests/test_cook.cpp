// Cooking through the local DDC (02 §6.2): the second cook of an unchanged asset is a hit with
// byte-identical output; the importer version, a setting, the source bytes and the platform each miss;
// moving or touching the source does not; a damaged entry is rebuilt.

#include "assetpipe_test_util.h"

namespace {

using namespace assetpipe_test;

struct Project {
    TempDir dir;
    ImporterRegistry importers = makeRegistry();
    std::unique_ptr<LocalDdc> ddc;
    Project() {
        LocalDdcOptions o;
        o.root = dir.path / "ddc";
        ddc = LocalDdc::open(o).value();
    }
    Result<CookResult> cook(std::string_view path,
                            asset::HpakPlatform platform = asset::HpakPlatform::PcClient) {
        return cookAsset(CookRequest{&importers, ddc.get(), dir.path / "content", path, platform});
    }
};

TEST_CASE("cook: the second cook is a DDC hit with byte-identical output") {
    Project p;
    writeText(p.dir.path / "content", "ships/hull.png", "hull source bytes");
    REQUIRE(ensureMeta(p.dir.path / "content", "ships/hull.png", newMeta(R"({"mips": false})"), p.importers));
    const int runs = buildRuns().load();

    const CookResult first = p.cook("ships/hull.png").value();
    CHECK(!first.hit);
    CHECK(first.stored);
    CHECK(buildRuns().load() == runs + 1);
    const std::string expected = std::string("png|pc-client|{\"mips\":false}|") + "setyb ecruos lluh";
    CHECK(std::string(first.product.begin(), first.product.end()) == expected);

    const CookResult second = p.cook("ships/hull.png").value();
    CHECK(second.hit);
    CHECK(!second.stored);
    CHECK(buildRuns().load() == runs + 1); // the importer did not run
    CHECK(second.key == first.key);
    CHECK(second.product == first.product);
    CHECK(second.meta == first.meta);

    // Moving and touching the source (same bytes, same sidecar) keeps the key: still a hit.
    REQUIRE(moveAsset(p.dir.path / "content", "ships/hull.png", "kestrel/Hull.png", p.importers));
    REQUIRE(fs::setLastWriteTime(p.dir.path / "content" / "kestrel" / "Hull.png",
                                 std::filesystem::file_time_type::clock::now() + std::chrono::hours(1)));
    const CookResult moved = p.cook("kestrel/Hull.png").value();
    CHECK(moved.hit);
    CHECK(moved.key == first.key);
    CHECK(moved.product == first.product);
    CHECK(buildRuns().load() == runs + 1);
}

TEST_CASE("cook: the importer version, a setting, the source and the platform each miss") {
    Project p;
    const fs::Path content = p.dir.path / "content";
    writeText(content, "a.png", "bytes");
    AssetMeta meta = ensureMeta(content, "a.png", newMeta(), p.importers).value().meta;
    const CookResult base = p.cook("a.png").value();
    CHECK(!base.hit);
    CHECK(p.cook("a.png").value().hit);

    SUBCASE("importer version") {
        const ImporterRegistry bumped = makeRegistry(3); // the same importer at version 3, on the same store
        const CookResult r = cookAsset(CookRequest{&bumped, p.ddc.get(), content, "a.png"}).value();
        CHECK(!r.hit);
        CHECK(r.key != base.key);
        CHECK(r.product == base.product); // this test importer's output does not depend on its version
        CHECK(cookAsset(CookRequest{&bumped, p.ddc.get(), content, "a.png"}).value().hit);
        CHECK(p.cook("a.png").value().hit); // version 2's entry is still there
    }
    SUBCASE("a setting") {
        meta.settings = R"({"maxSize":512})";
        REQUIRE(saveMeta(content, "a.png", meta, p.importers));
        const CookResult r = p.cook("a.png").value();
        CHECK(!r.hit);
        CHECK(r.key != base.key);
        CHECK(r.product != base.product);
        CHECK(p.cook("a.png").value().hit);
        // Back to the defaults: the original entry again.
        meta.settings = "{}";
        REQUIRE(saveMeta(content, "a.png", meta, p.importers));
        CHECK(p.cook("a.png").value().key == base.key);
        CHECK(p.cook("a.png").value().hit);
    }
    SUBCASE("labels and provenance are not build inputs") {
        meta.labels = {"retagged"};
        meta.provenance.notes = "edited";
        REQUIRE(saveMeta(content, "a.png", meta, p.importers));
        CHECK(p.cook("a.png").value().hit);
    }
    SUBCASE("the source bytes") {
        writeText(content, "a.png", "bytes!");
        const CookResult r = p.cook("a.png").value();
        CHECK(!r.hit);
        CHECK(r.key != base.key);
    }
    SUBCASE("the platform (02 §6.5: one key per consumer)") {
        const CookResult server = p.cook("a.png", asset::HpakPlatform::Server).value();
        CHECK(!server.hit);
        CHECK(server.key != base.key);
        CHECK(std::string(server.product.begin(), server.product.end()).starts_with("png|server|"));
        CHECK(p.cook("a.png", asset::HpakPlatform::Server).value().hit);
        CHECK(p.cook("a.png").value().hit);
    }
}

TEST_CASE("cook: a damaged DDC entry is rebuilt, and cook errors are clean") {
    Project p;
    const fs::Path content = p.dir.path / "content";
    writeText(content, "a.png", "bytes");
    REQUIRE(ensureMeta(content, "a.png", newMeta(), p.importers));
    const CookResult base = p.cook("a.png").value();
    std::vector<u8> raw = fs::readFile(p.ddc->entryPath(base.key)).value();
    raw.back() ^= 1;
    REQUIRE(fs::writeFile(p.ddc->entryPath(base.key), raw));
    const CookResult rebuilt = p.cook("a.png").value();
    CHECK(!rebuilt.hit);
    CHECK(rebuilt.stored);
    CHECK(rebuilt.product == base.product);
    CHECK(p.cook("a.png").value().hit);

    writeText(content, "loose.png", "no sidecar");
    CHECK(p.cook("loose.png").error().code == ErrorCode::NotFound);
    CHECK(p.cook("../a.png").error().code == ErrorCode::InvalidArgument);
    writeText(content, "a.png.meta", "{}");
    CHECK(p.cook("a.png").error().code == ErrorCode::ParseError);
    CHECK(cookAsset(CookRequest{}).error().code == ErrorCode::InvalidArgument);

    // An importer without a build step cooks only what the DDC already has.
    ImporterRegistry noBuild;
    ImporterInfo info = textureImporter();
    info.build = nullptr;
    REQUIRE(noBuild.add(info));
    writeText(content, "b.png", "other bytes");
    REQUIRE(ensureMeta(content, "b.png", newMeta(), noBuild));
    CHECK(cookAsset(CookRequest{&noBuild, p.ddc.get(), content, "b.png"}).error().code ==
          ErrorCode::Unsupported);
}

} // namespace
