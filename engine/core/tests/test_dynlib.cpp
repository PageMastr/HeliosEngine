#include <doctest/doctest.h>

#include <string>

#include "helios/core/dynlib.h"
#include "helios/core/fs.h"

using namespace helios;

#ifndef HELIOS_TEST_PLUGIN_PATH
#error "HELIOS_TEST_PLUGIN_PATH must be defined by the build"
#endif

TEST_CASE("dynlib: load a plugin and resolve exported symbols") {
    auto lib = DynamicLibrary::loadUtf8(HELIOS_TEST_PLUGIN_PATH);
    REQUIRE_MESSAGE(lib, (lib ? "" : lib.error().toString()));
    CHECK(lib->isLoaded());
    CHECK(fs::pathToUtf8(lib->path()) == HELIOS_TEST_PLUGIN_PATH);

    auto add = lib->function<int (*)(int, int)>("helios_test_plugin_add");
    REQUIRE(add != nullptr);
    CHECK(add(40, 2) == 42);
    auto name = lib->function<const char* (*)()>("helios_test_plugin_name");
    REQUIRE(name != nullptr);
    CHECK(std::string(name()) == "helios-test-plugin");
    auto* counter = static_cast<int*>(lib->symbol("helios_test_plugin_counter"));
    REQUIRE(counter != nullptr);
    CHECK(*counter == 42);
    CHECK(lib->symbol("no_such_symbol") == nullptr);
    CHECK(lib->symbol(nullptr) == nullptr);

    DynamicLibrary moved = std::move(*lib);
    CHECK(!lib->isLoaded());
    CHECK(moved.isLoaded());
    moved.unload();
    CHECK(!moved.isLoaded());
    CHECK(moved.symbol("helios_test_plugin_add") == nullptr);
}

TEST_CASE("dynlib: load by std::filesystem::path") {
    auto lib = DynamicLibrary::load(fs::pathFromUtf8(HELIOS_TEST_PLUGIN_PATH));
    REQUIRE(lib);
    CHECK(lib->function<int (*)(int, int)>("helios_test_plugin_add")(1, 2) == 3);
}

TEST_CASE("dynlib: missing libraries report errors") {
    auto missing = DynamicLibrary::loadUtf8("definitely-not-a-real-library-name-xyz");
    CHECK(!missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(!missing.error().message.empty());
}

TEST_CASE("dynlib: platform naming") {
    if constexpr (platform::kIsWindows) {
        CHECK(DynamicLibrary::extension() == ".dll");
        CHECK(DynamicLibrary::decoratedName("game") == "game.dll");
    } else {
        CHECK(DynamicLibrary::decoratedName("game") == std::string("libgame") + std::string(DynamicLibrary::extension()));
    }
}
