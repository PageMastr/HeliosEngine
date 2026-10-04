// What the DDC key covers (02 §6.2, 07 §4.1.1 "incremental equals clean"): the settings type's
// fingerprint (hashSettingsType: fields, types and defaults, not only TypeInfo::layoutHash), and a build
// step that sees only keyed inputs, so a hit is always the product a clean build would make.

#include <algorithm>
#include <string>
#include <type_traits>
#include <vector>

#include "assetpipe_test_util.h"
#include "helios/reflect/serialize.h"

namespace key_probe {

using helios::f32;
using helios::u32;

// TextureSettings look-alikes. Several are registered under TextureSettings' own name, so that only the
// difference under test separates them (the fingerprint also records the name).
struct SameAsTexture { // identical: the same fingerprint
    bool mips = true;
    u32 maxSize = 2048;
    std::string format = "bc7";
    std::vector<u32> lods;
};
struct NewDefault { // mips defaults to false: TypeInfo::layoutHash cannot see it
    bool mips = false;
    u32 maxSize = 2048;
    std::string format = "bc7";
    std::vector<u32> lods;
};
struct Retyped { // maxSize is an f32: StructBuilder's layoutHash cannot see it
    bool mips = true;
    f32 maxSize = 2048;
    std::string format = "bc7";
    std::vector<u32> lods;
};
using Renamed = SameAsTexture;    // reflected with "maxSide" for "maxSize"
using Renumbered = SameAsTexture; // reflected with format at field id 9

enum class Filter : helios::u8 { Linear, Nearest };
struct Inner {
    u32 level = 1;
    Filter filter = Filter::Linear;
};
struct Outer {
    Inner inner;
    std::vector<Inner> extra;
};
struct OuterNestedDefault { // the same fields; `inner` defaults to level 2
    Inner inner{2, Filter::Linear};
    std::vector<Inner> extra;
};
struct InnerV2 { // registered as Inner: a different default reached only through Outer's list element
    u32 level = 3;
    Filter filter = Filter::Linear;
};
struct OuterV2 {
    Inner inner;
    std::vector<InnerV2> extra;
};
enum class FilterV2 : helios::u8 { Linear, Nearest, Cubic }; // registered as Filter, one more value
struct InnerFilterV2 {
    u32 level = 1;
    FilterV2 filter = FilterV2::Linear;
};
struct OuterFilterV2 {
    InnerFilterV2 inner;
    std::vector<InnerFilterV2> extra;
};

struct Node { // recursive through a list
    std::string name;
    std::vector<Node> children;
};

template <int N>
struct Deep {
    Deep<N - 1> inner;
};
template <>
struct Deep<0> {
    u32 leaf = 0;
};

} // namespace key_probe

HELIOS_REFLECT_TYPE(key_probe::SameAsTexture);
HELIOS_REFLECT_TYPE(key_probe::NewDefault);
HELIOS_REFLECT_TYPE(key_probe::Retyped);
HELIOS_REFLECT_ENUM(key_probe::Filter);
HELIOS_REFLECT_ENUM(key_probe::FilterV2);
HELIOS_REFLECT_TYPE(key_probe::Inner);
HELIOS_REFLECT_TYPE(key_probe::Outer);
HELIOS_REFLECT_TYPE(key_probe::OuterNestedDefault);
HELIOS_REFLECT_TYPE(key_probe::InnerV2);
HELIOS_REFLECT_TYPE(key_probe::OuterV2);
HELIOS_REFLECT_TYPE(key_probe::InnerFilterV2);
HELIOS_REFLECT_TYPE(key_probe::OuterFilterV2);
HELIOS_REFLECT_TYPE(key_probe::Node);

namespace helios::refl {
template <int N>
struct TypeOf<key_probe::Deep<N>> {
    static const TypeInfo& get() noexcept {
        static const TypeInfo* info = [] {
            StructBuilder<key_probe::Deep<N>> b("key_probe.Deep" + std::to_string(N));
            if constexpr (N == 0) {
                b.field("leaf", &key_probe::Deep<N>::leaf);
            } else {
                b.field("inner", &key_probe::Deep<N>::inner);
            }
            return b.build();
        }();
        return *info;
    }
};
template <int N>
struct Codec<key_probe::Deep<N>> : WalkCodec<key_probe::Deep<N>> {};
} // namespace helios::refl

using namespace helios;
using namespace helios::refl;

namespace {

constexpr std::string_view kTextureName = "assetpipe_test.TextureSettings";

template <class T>
const TypeInfo* textureLike(std::string_view maxSizeName = "maxSize", u32 formatId = 0) {
    return StructBuilder<T>(kTextureName)
        .field("mips", &T::mips)
        .field(maxSizeName, &T::maxSize)
        .field("format", &T::format, {.id = formatId})
        .field("lods", &T::lods)
        .build();
}

template <class O>
const TypeInfo* outerLike() {
    return StructBuilder<O>("key_probe.Outer").field("inner", &O::inner).field("extra", &O::extra).build();
}

template <class I>
const TypeInfo* innerLike() {
    return StructBuilder<I>("key_probe.Inner").field("level", &I::level).field("filter", &I::filter).build();
}

} // namespace

const TypeInfo& TypeOf<key_probe::SameAsTexture>::get() noexcept {
    static const TypeInfo* info = textureLike<key_probe::SameAsTexture>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::NewDefault>::get() noexcept {
    static const TypeInfo* info = textureLike<key_probe::NewDefault>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::Retyped>::get() noexcept {
    static const TypeInfo* info = textureLike<key_probe::Retyped>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::Filter>::get() noexcept {
    static const TypeInfo* info = EnumBuilder<key_probe::Filter>("key_probe.Filter")
                                      .value("Linear", key_probe::Filter::Linear)
                                      .value("Nearest", key_probe::Filter::Nearest)
                                      .build();
    return *info;
}
const TypeInfo& TypeOf<key_probe::FilterV2>::get() noexcept {
    static const TypeInfo* info = EnumBuilder<key_probe::FilterV2>("key_probe.Filter")
                                      .value("Linear", key_probe::FilterV2::Linear)
                                      .value("Nearest", key_probe::FilterV2::Nearest)
                                      .value("Cubic", key_probe::FilterV2::Cubic)
                                      .build();
    return *info;
}
const TypeInfo& TypeOf<key_probe::Inner>::get() noexcept {
    static const TypeInfo* info = innerLike<key_probe::Inner>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::InnerV2>::get() noexcept {
    static const TypeInfo* info = innerLike<key_probe::InnerV2>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::InnerFilterV2>::get() noexcept {
    static const TypeInfo* info = innerLike<key_probe::InnerFilterV2>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::Outer>::get() noexcept {
    static const TypeInfo* info = outerLike<key_probe::Outer>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::OuterNestedDefault>::get() noexcept {
    static const TypeInfo* info = outerLike<key_probe::OuterNestedDefault>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::OuterV2>::get() noexcept {
    static const TypeInfo* info = outerLike<key_probe::OuterV2>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::OuterFilterV2>::get() noexcept {
    static const TypeInfo* info = outerLike<key_probe::OuterFilterV2>();
    return *info;
}
const TypeInfo& TypeOf<key_probe::Node>::get() noexcept {
    static const TypeInfo* info = StructBuilder<key_probe::Node>("key_probe.Node")
                                      .field("name", &key_probe::Node::name)
                                      .field("children", &key_probe::Node::children)
                                      .build();
    return *info;
}

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
        return std::vector<u8>{static_cast<u8>(*static_cast<const bool*>(mips->ptr(settings.data())) ? 1 : 0)};
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

    static const TypeInfo* renamed = textureLike<key_probe::Renamed>("maxSide");
    static const TypeInfo* renumbered = textureLike<key_probe::Renumbered>("maxSize", 9);
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
