#pragma once
// Text goldens for render_tests (Null trace goldens, 03 §8.4). HELIOS_UPDATE_GOLDENS=1 rewrites
// them; on mismatch the actual text is written next to the test output for diffing.

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

#include "helios/core/fs.h"

namespace goldentest {

inline std::filesystem::path outputDir() {
    std::filesystem::path dir(HELIOS_RENDER_TEST_OUTPUT_DIR);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

/// Normalizes line endings so goldens checked out with CRLF on Windows still compare.
inline std::string normalized(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c != '\r') out += c;
    }
    return out;
}

inline void checkTextGolden(std::string_view name, const std::string& actual) {
    const std::filesystem::path golden = std::filesystem::path(HELIOS_RENDER_GOLDEN_DIR) / (std::string(name) + ".txt");
    const char* update = std::getenv("HELIOS_UPDATE_GOLDENS");
    if (update && std::string_view(update) == "1") {
        REQUIRE(helios::fs::writeTextFile(golden, actual).ok());
        MESSAGE("updated golden " << golden.string());
        return;
    }
    auto expected = helios::fs::readTextFile(golden);
    if (!expected.ok()) {
        (void)helios::fs::writeTextFile(outputDir() / (std::string(name) + ".actual.txt"), actual);
        FAIL("missing golden " << golden.string() << " (run with HELIOS_UPDATE_GOLDENS=1 to create it)");
        return;
    }
    const bool same = normalized(expected.value()) == actual;
    if (!same) (void)helios::fs::writeTextFile(outputDir() / (std::string(name) + ".actual.txt"), actual);
    INFO("golden " << golden.string() << " differs; actual written to " << (outputDir() / name).string()
                   << ".actual.txt");
    CHECK(same);
}

} // namespace goldentest
