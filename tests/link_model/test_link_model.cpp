// Link-model tests (ADR-016, 02 §1.4, WP-0.6c part 1). They run only in a modular dev build
// (HELIOS_MODULAR=ON), where the engine modules form the link-group shared libraries: a plugin image
// built against them must share the engine's registries, its heaps and the CRT heap with the executable.
// A plugin that carried its own engine (the rejected alternative (a) of ADR-016) fails each case.
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "helios/core/cvar.h"
#include "helios/core/dynlib.h"
#include "helios/core/memory.h"
#include "helios/runtime_api.h"

#ifndef HELIOS_LINK_MODEL_PROBE_PATH
#error "HELIOS_LINK_MODEL_PROBE_PATH must be defined by the build"
#endif

namespace {

using helios::DynamicLibrary;

bool hasWord(std::string_view list, std::string_view word) {
    std::string_view rest = list;
    while (!rest.empty()) {
        const auto space = rest.find(' ');
        if (rest.substr(0, space) == word) return true;
        if (space == std::string_view::npos) break;
        rest.remove_prefix(space + 1);
    }
    return false;
}

static_assert(HELIOS_LINK_MODULAR == 1, "the link-model tests are built only in modular dev builds");

TEST_CASE("link model: helios_runtime exports its module list") {
    const std::string_view modules = helios_runtime_link_group_modules();
    CHECK(hasWord(modules, "core"));
    CHECK(hasWord(modules, "reflect"));
    CHECK(hasWord(modules, "ecs"));
    // HEADLESS modules only: client and editor modules live in their own groups.
    CHECK_FALSE(hasWord(modules, "rhi"));
    CHECK_FALSE(hasWord(modules, "assetpipe"));
}

#if defined(HELIOS_LINK_MODEL_CLIENT_PATH) || defined(HELIOS_LINK_MODEL_EDITOR_PATH)
TEST_CASE("link model: the client and editor groups export their module lists") {
    struct Group {
        const char* path;
        const char* symbol;
        const char* member;
    };
    std::vector<Group> groups;
#if defined(HELIOS_LINK_MODEL_CLIENT_PATH)
    groups.push_back({HELIOS_LINK_MODEL_CLIENT_PATH, "helios_client_link_group_modules", "rhi"});
#endif
#if defined(HELIOS_LINK_MODEL_EDITOR_PATH)
    groups.push_back({HELIOS_LINK_MODEL_EDITOR_PATH, "helios_editor_link_group_modules", "assetpipe"});
#endif
    for (const Group& g : groups) {
        CAPTURE(g.path);
        auto lib = DynamicLibrary::loadUtf8(g.path);
        REQUIRE_MESSAGE(lib, (lib ? "" : lib.error().toString()));
        auto fn = lib->function<const char* (*)()>(g.symbol);
        REQUIRE(fn != nullptr);
        CHECK(hasWord(fn(), g.member));
        CHECK_FALSE(hasWord(fn(), "core"));
    }
}
#endif

TEST_CASE("link model: a plugin image registers into the engine's own registries") {
    auto lib = DynamicLibrary::loadUtf8(HELIOS_LINK_MODEL_PROBE_PATH);
    REQUIRE_MESSAGE(lib, (lib ? "" : lib.error().toString()));
    auto reg = lib->function<int (*)()>("helios_link_model_probe_register");
    auto unreg = lib->function<int (*)()>("helios_link_model_probe_unregister");
    auto runtimeModules = lib->function<const char* (*)()>("helios_link_model_probe_runtime_modules");
    REQUIRE(reg != nullptr);
    REQUIRE(unreg != nullptr);
    REQUIRE(runtimeModules != nullptr);

    // One helios_runtime image in the process: the plugin sees the same data, at the same address.
    CHECK(runtimeModules() == helios_runtime_link_group_modules());

    auto& registry = helios::CVarRegistry::instance();
    REQUIRE_FALSE(registry.hasCommand("link_model.probe"));
    REQUIRE(reg() == 1);
    CHECK(registry.hasCommand("link_model.probe"));
    auto out = registry.execute("link_model.probe a b");
    REQUIRE_MESSAGE(out, (out ? "" : out.error().toString()));
    CHECK(out->find("probe:2") != std::string::npos);
    CHECK(unreg() == 1);
    CHECK_FALSE(registry.hasCommand("link_model.probe"));
}

TEST_CASE("link model: memory crosses the image boundary") {
    auto lib = DynamicLibrary::loadUtf8(HELIOS_LINK_MODEL_PROBE_PATH);
    REQUIRE_MESSAGE(lib, (lib ? "" : lib.error().toString()));
    auto fill = lib->function<void (*)(std::string*, std::vector<unsigned>*)>("helios_link_model_probe_fill");
    auto alloc = lib->function<void* (*)(unsigned)>("helios_link_model_probe_alloc");
    REQUIRE(fill != nullptr);
    REQUIRE(alloc != nullptr);

    SUBCASE("STL containers filled in the plugin are freed here: one CRT heap (/MD on Windows)") {
        for (int round = 0; round < 64; ++round) {
            std::string text;
            std::vector<unsigned> values;
            fill(&text, &values);
            CHECK(text.size() == 4096);
            CHECK(values.size() == 1000);
            text.append("-grown-in-the-executable");
            values.push_back(round);
        }
    }
    SUBCASE("engine allocations are tagged and freed through the one engine heap") {
        const helios::MemoryTag tag = helios::registerMemoryTag("link_model.probe");
        const auto before = helios::memoryTagStats(tag);
        void* p = alloc(1u << 20);
        REQUIRE(p != nullptr);
        CHECK(helios::alignedAllocTag(p) == tag);
        CHECK(helios::memoryTagStats(tag).liveBytes >= before.liveBytes + (1u << 20));
        helios::alignedFree(p);
        CHECK(helios::memoryTagStats(tag).liveBytes == before.liveBytes);
    }
}

} // namespace
