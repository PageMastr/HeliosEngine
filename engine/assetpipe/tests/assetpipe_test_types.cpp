// Reflection of the test settings types (assetpipe_test_util.h).

#include "assetpipe_test_util.h"

using assetpipe_test::TextureSettings;
using assetpipe_test::TextureSettingsV2;

const helios::refl::TypeInfo& helios::refl::TypeOf<TextureSettings>::get() noexcept {
    static const TypeInfo* info = StructBuilder<TextureSettings>("assetpipe_test.TextureSettings")
                                      .field("mips", &TextureSettings::mips)
                                      .field("maxSize", &TextureSettings::maxSize)
                                      .field("format", &TextureSettings::format)
                                      .field("lods", &TextureSettings::lods)
                                      .build();
    return *info;
}

const helios::refl::TypeInfo& helios::refl::TypeOf<TextureSettingsV2>::get() noexcept {
    static const TypeInfo* info = StructBuilder<TextureSettingsV2>("assetpipe_test.TextureSettingsV2")
                                      .field("mips", &TextureSettingsV2::mips)
                                      .field("maxSize", &TextureSettingsV2::maxSize)
                                      .field("format", &TextureSettingsV2::format)
                                      .field("lods", &TextureSettingsV2::lods)
                                      .field("srgb", &TextureSettingsV2::srgb)
                                      .build();
    return *info;
}
