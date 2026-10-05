// What the DDC key covers (02 §6.2, 07 §4.1.1 "incremental equals clean"): the settings type's
// fingerprint (hashSettingsType: fields, types and defaults, not only TypeInfo::layoutHash), and a build
// step that sees only keyed inputs, so a hit is always the product a clean build would make.

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

#include "assetpipe_test_util.h"
#include "helios/reflect/serialize.h"
#include "key_probe_types.h"

using namespace helios;
using namespace helios::refl;

namespace {

using namespace assetpipe_test;

Hash128 fingerprint(const TypeInfo& type) {
    return hashSettingsType(&type).value();
}

/// A build step that reads its settings the way a real importer does, through its own settings type, and
/// returns the resolved `mips` (one byte).
BuildFn mipsBuild(const TypeInfo* type) {
    return [type](const BuildContext& c) -> Result<std::vector<u8>> {
        Value settings(*type);
        ReadCtx ctx;
        HELIOS_TRY(fromJson(*type, settings.data(), c.settings, ctx));
        const FieldInfo* mips = type->field("mips");
        if (!mips) return Error{ErrorCode::InvalidArgument, "no 'mips' setting"};
        const bool value = *static_cast<const bool*>(mips->ptr(settings.data()));
        return std::vector<u8>{static_cast<u8>(value ? 1 : 0)};
    };
}

ImporterRegistry textureRegistry(const TypeInfo* settings) {
    ImporterInfo i = textureImporter();
    i.settings = settings;
    i.build = mipsBuild(settings);
    ImporterRegistry r;
    REQUIRE(r.add(i));
    return r;
}

std::unique_ptr<LocalDdc> freshDdc(const fs::Path& root) {
    LocalDdcOptions o;
    o.root = root;
    return LocalDdc::open(o).value();
}

TEST_CASE("settings type hash: names, ids, types and defaults change it; an identical type does not") {
    const Hash128 base = fingerprint(typeOf<TextureSettings>());
    CHECK(base != Hash128{});
    CHECK(fingerprint(typeOf<TextureSettings>()) == base);       // pure
    CHECK(fingerprint(typeOf<key_probe::SameAsTexture>()) == base); // content, not identity
    CHECK(hashSettingsType(nullptr).value() == Hash128{});        // an importer without settings

    // The two the review found: TypeInfo::layoutHash is blind to both, the fingerprint is not.
    CHECK(typeOf<key_probe::NewDefault>().layoutHash == typeOf<TextureSettings>().layoutHash);
    CHECK(fingerprint(typeOf<key_probe::NewDefault>()) != base); // a default changed
    CHECK(fingerprint(typeOf<key_probe::Retyped>()) != base);    // a field's type changed

    const TypeInfo* renamed = key_probe::renamedTextureSettings();
    const TypeInfo* renumbered = key_probe::renumberedTextureSettings();
    std::vector<Hash128> all = {base,
                                fingerprint(typeOf<key_probe::NewDefault>()),
                                fingerprint(typeOf<key_probe::Retyped>()),
                                fingerprint(*renamed),
                                fingerprint(*renumbered),
                                fingerprint(typeOf<TextureSettingsV2>())}; // a field added
    std::sort(all.begin(), all.end());
    CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());

    // Through nesting: a nested field's default in the outer type, a default reached only through a list's
    // element type, and an enum value.
    const Hash128 outer = fingerprint(typeOf<key_probe::Outer>());
    CHECK(typeOf<key_probe::OuterNestedDefault>().layoutHash == typeOf<key_probe::Outer>().layoutHash);
    CHECK(fingerprint(typeOf<key_probe::OuterNestedDefault>()) != outer);
    CHECK(fingerprint(typeOf<key_probe::OuterV2>()) != outer);
    CHECK(fingerprint(typeOf<key_probe::OuterFilterV2>()) != outer);
    CHECK(fingerprint(typeOf<key_probe::OuterV2>()) != fingerprint(typeOf<key_probe::OuterNestedDefault>()));
}

TEST_CASE("settings type hash: recursive types terminate, and nesting is bounded") {
    const Hash128 node = fingerprint(typeOf<key_probe::Node>());
    CHECK(node != Hash128{});
    CHECK(fingerprint(typeOf<key_probe::Node>()) == node);
    // kMaxSettingsTypeDepth levels below the settings type: Deep<62>'s leaf u32 is level 63.
    static_assert(kMaxSettingsTypeDepth == 64);
    CHECK(hashSettingsType(&typeOf<key_probe::Deep<62>>()));
    const auto deep = hashSettingsType(&typeOf<key_probe::Deep<70>>());
    REQUIRE(!deep);
    CHECK(deep.error().code == ErrorCode::LimitExceeded);
    // The registry refuses such a settings type, and caches the fingerprint of the ones it takes.
    ImporterRegistry r;
    ImporterInfo i = textureImporter();
    i.settings = &typeOf<key_probe::Deep<70>>();
    CHECK(r.add(i).error().code == ErrorCode::InvalidArgument);
    i.settings = &typeOf<TextureSettings>();
    REQUIRE(r.add(i));
    REQUIRE(r.add(fontImporter()));
    CHECK(r.settingsTypeHash("png") == fingerprint(typeOf<TextureSettings>()));
    CHECK(r.settingsTypeHash("font") == Hash128{}); // no settings
    CHECK(r.settingsTypeHash("nope") == Hash128{});
}

// BuildContext is exactly the keyed inputs. A member added to it breaks this binding: whoever adds one must
// add the same input to the DDC key (cook.cpp) and to this list.
static_assert(std::is_aggregate_v<BuildContext>);
template <class C>
constexpr bool kHasMeta = requires(const C& c) { c.meta; };
static_assert(!kHasMeta<BuildContext>, "the build step must not see the sidecar: it is not in the key");

TEST_CASE("cook: the build step sees only keyed inputs, so assets with the same bytes share one product") {
    TempDir dir;
    const fs::Path content = dir.path / "content";
    // A build step that returns everything its context offers.
    ImporterInfo info = textureImporter();
    info.build = [](const BuildContext& c) -> Result<std::vector<u8>> {
        const auto& [source, settings, platform] = c;
        std::string out = std::string(settings) + "|" + std::string(asset::hpakPlatformName(platform)) + "|";
        out.append(source.begin(), source.end());
        return std::vector<u8>(out.begin(), out.end());
    };
    ImporterRegistry r;
    REQUIRE(r.add(info));
    auto ddc = freshDdc(dir.path / "ddc");

    // Two assets, same bytes and settings, different GUIDs, paths, labels and provenance.
    writeText(content, "a.png", "same bytes");
    writeText(content, "other/b.png", "same bytes");
    const Guid a = ensureMeta(content, "a.png", newMeta(R"({"mips": false})"), r).value().meta.guid;
    NewMeta bInit = newMeta(R"({"mips": false})");
    bInit.labels = {"b-only"};
    bInit.provenance.author = "Someone else";
    const Guid b = ensureMeta(content, "other/b.png", bInit, r).value().meta.guid;
    REQUIRE(a != b);

    const CookResult ca = cookAsset(CookRequest{&r, ddc.get(), content, "a.png"}).value();
    const CookResult cb = cookAsset(CookRequest{&r, ddc.get(), content, "other/b.png"}).value();
    CHECK(!ca.hit);
    CHECK(cb.hit); // one product for both: nothing the build step saw differs
    CHECK(cb.key == ca.key);
    CHECK(cb.product == ca.product);
    CHECK(cb.meta.guid == b); // the result still names the asset that was cooked
    const std::string product(ca.product.begin(), ca.product.end());
    CHECK(product == R"({"mips":false}|pc-client|same bytes)");
    CHECK(product.find(a.toString()) == std::string::npos);
}

TEST_CASE("cook: a changed settings default or field type misses (incremental equals clean)") {
    TempDir dir;
    const fs::Path content = dir.path / "content";
    writeText(content, "a.png", "bytes");
    const ImporterRegistry before = textureRegistry(&typeOf<TextureSettings>());     // mips defaults to true
    const ImporterRegistry after = textureRegistry(&typeOf<key_probe::NewDefault>()); // ...now to false
    REQUIRE(ensureMeta(content, "a.png", newMeta(), before)); // settings "{}": every value at its default
    auto warm = freshDdc(dir.path / "warm");

    const CookResult old = cookAsset(CookRequest{&before, warm.get(), content, "a.png"}).value();
    CHECK(old.product == std::vector<u8>{1});
    auto empty = freshDdc(dir.path / "clean");
    const CookResult clean = cookAsset(CookRequest{&after, empty.get(), content, "a.png"}).value();
    CHECK(clean.product == std::vector<u8>{0});
    // The same sidecar and bytes on the warm DDC: a miss that rebuilds, not the old product.
    const CookResult incremental = cookAsset(CookRequest{&after, warm.get(), content, "a.png"}).value();
    CHECK(!incremental.hit);
    CHECK(incremental.key != old.key);
    CHECK(incremental.key == clean.key);
    CHECK(incremental.product == clean.product);

    // A field retyped (u32 -> f32) under the same name and layoutHash: another key too.
    const ImporterRegistry retyped = textureRegistry(&typeOf<key_probe::Retyped>());
    const CookResult r = cookAsset(CookRequest{&retyped, warm.get(), content, "a.png"}).value();
    CHECK(!r.hit);
    CHECK(r.key != old.key);
    CHECK(r.key != clean.key);
    // An identical type (another C++ struct, the same fields and defaults) keeps the key: no false misses.
    const ImporterRegistry same = textureRegistry(&typeOf<key_probe::SameAsTexture>());
    const CookResult s = cookAsset(CookRequest{&same, warm.get(), content, "a.png"}).value();
    CHECK(s.hit);
    CHECK(s.key == old.key);
}

} // namespace
