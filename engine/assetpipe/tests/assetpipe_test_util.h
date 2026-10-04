#pragma once
// Shared fixtures for the `.meta`, DDC and cook tests: a temp project directory, reflected settings types
// and an importer registry with a texture-like and a font-like importer whose build steps are cheap,
// deterministic functions of the source bytes and settings.

#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "helios/assetpipe/cook.h"
#include "helios/assetpipe/ddc.h"
#include "helios/assetpipe/importer.h"
#include "helios/assetpipe/meta.h"
#include "helios/core/fs.h"
#include "helios/reflect/builder.h"

namespace assetpipe_test {

struct TextureSettings {
    bool mips = true;
    helios::u32 maxSize = 2048;
    std::string format = "bc7";
    std::vector<helios::u32> lods;
};

/// TextureSettings with one more field: another layout hash.
struct TextureSettingsV2 {
    bool mips = true;
    helios::u32 maxSize = 2048;
    std::string format = "bc7";
    std::vector<helios::u32> lods;
    bool srgb = true;
};

} // namespace assetpipe_test

HELIOS_REFLECT_TYPE(assetpipe_test::TextureSettings);
HELIOS_REFLECT_TYPE(assetpipe_test::TextureSettingsV2);

namespace assetpipe_test {

using namespace helios;
using namespace helios::assetpipe;

struct TempDir {
    fs::Path path;
    TempDir() {
        auto dir = fs::createUniqueTempDirectory("helios-assetpipe-test");
        REQUIRE(dir);
        path = *dir;
    }
    ~TempDir() { (void)fs::removeAll(path); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

inline void writeText(const fs::Path& root, std::string_view rel, std::string_view text) {
    const fs::Path p = root / fs::pathFromUtf8(rel);
    if (p.has_parent_path()) REQUIRE(fs::createDirectories(p.parent_path()));
    REQUIRE(fs::writeTextFile(p, text));
}

inline std::string readText(const fs::Path& root, std::string_view rel) {
    return fs::readTextFile(root / fs::pathFromUtf8(rel)).value();
}

inline bool fileExists(const fs::Path& root, std::string_view rel) {
    return fs::isFile(root / fs::pathFromUtf8(rel));
}

inline Provenance original(std::string licence = "MIT") {
    Provenance p;
    p.origin = Origin::Original;
    p.author = "Owner";
    p.licence = std::move(licence);
    return p;
}

/// Counts build-step runs so tests can tell a DDC hit (no run) from a miss.
inline std::atomic<int>& buildRuns() {
    static std::atomic<int> runs{0};
    return runs;
}

/// "<importer id>|<platform>|<settings>|" + the source bytes reversed: deterministic and input-sensitive.
inline BuildFn reversingBuild(std::string id) {
    return [id](const BuildContext& c) -> Result<std::vector<u8>> {
        buildRuns().fetch_add(1);
        const std::string head =
            id + "|" + std::string(asset::hpakPlatformName(c.platform)) + "|" + std::string(c.settings) + "|";
        std::vector<u8> out(head.begin(), head.end());
        out.insert(out.end(), c.source.rbegin(), c.source.rend());
        return out;
    };
}

inline ImporterInfo textureImporter(u32 version = 2) {
    ImporterInfo i;
    i.id = "png";
    i.version = version;
    i.extensions = {".png", ".tga"};
    i.settings = &refl::typeOf<TextureSettings>();
    i.build = reversingBuild("png");
    return i;
}

inline ImporterInfo fontImporter() {
    ImporterInfo i;
    i.id = "font";
    i.version = 1;
    i.extensions = {".ttf"};
    i.fonts = true;
    i.build = reversingBuild("font");
    return i;
}

inline ImporterRegistry makeRegistry(u32 textureVersion = 2) {
    ImporterRegistry r;
    REQUIRE(r.add(textureImporter(textureVersion)));
    REQUIRE(r.add(fontImporter()));
    return r;
}

inline NewMeta newMeta(std::string settings = "{}") {
    NewMeta m;
    m.settings = std::move(settings);
    m.provenance = original();
    return m;
}

} // namespace assetpipe_test
