// Reflection of the key_probe settings types (key_probe_types.h).

#include "key_probe_types.h"

#include "assetpipe_test_util.h"

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

const TypeInfo* key_probe::renamedTextureSettings() {
    static const TypeInfo* info = textureLike<key_probe::Renamed>("maxSide");
    return info;
}

const TypeInfo* key_probe::renumberedTextureSettings() {
    static const TypeInfo* info = textureLike<key_probe::Renumbered>("maxSize", 9);
    return info;
}
