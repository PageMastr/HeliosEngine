#pragma once
// Settings types for test_key_inputs.cpp: TextureSettings look-alikes and nested, recursive and deep types
// whose fingerprints (hashSettingsType) must differ exactly where a build step could see a difference.
// The TypeOf<> definitions are in key_probe_types.cpp.

#include <string>
#include <vector>

#include "helios/core/types.h"
#include "helios/reflect/builder.h"

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

// Nested through a list, so that a default-constructed object is not a deeply nested aggregate (MSVC
// limits initializer nesting): two walk levels per step, Deep<N>'s leaf at level 2N + 1.
template <int N>
struct Deep {
    std::vector<Deep<N - 1>> inner;
};
template <>
struct Deep<0> {
    u32 leaf = 0;
};

/// TextureSettings reflected with "maxSide" for "maxSize", and with `format` at field id 9.
const helios::refl::TypeInfo* renamedTextureSettings();
const helios::refl::TypeInfo* renumberedTextureSettings();

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
