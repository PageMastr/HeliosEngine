#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "helios/core/cvar.h"
#include "helios/core/log.h"

using namespace helios;

// Static registration, as engine modules do it.
HELIOS_CVAR(bool, cvarTestVsync, "test.r.vsync", true, "Synchronize to the display", CVarFlags::Saved);
HELIOS_CVAR(i32, cvarTestMaxFps, "test.r.maxFps", 144, "Frame rate cap", CVarFlags::Saved);
HELIOS_CVAR(f32, cvarTestGamma, "test.r.gamma", 2.2f, "Display gamma");
HELIOS_CVAR(std::string, cvarTestName, "test.player.name", "Pilot", "Player name", CVarFlags::Saved);
HELIOS_CVAR(bool, cvarTestGodMode, "test.cheat.god", false, "Invulnerability", CVarFlags::Cheat);
HELIOS_CVAR(i32, cvarTestBuild, "test.sys.build", 7, "Build number", CVarFlags::ReadOnly);
HELIOS_CVAR(f32, cvarTestGravity, "test.sv.gravity", 9.81f, "World gravity", CVarFlags::Replicated);

namespace {
CVarRegistry& reg() { return CVarRegistry::instance(); }

struct QuietLogs {
    log::Level saved = log::level();
    QuietLogs() { log::setLevel(log::Level::Off); }
    ~QuietLogs() { log::setLevel(saved); }
};
} // namespace

TEST_CASE("cvar: static registration and typed access") {
    CHECK(cvarTestVsync.isRegistered());
    CHECK(reg().find("test.r.vsync") == &cvarTestVsync);
    CHECK(reg().find("TEST.R.VSYNC") == &cvarTestVsync); // case-insensitive
    CHECK(cvarTestVsync.get());
    CHECK(*cvarTestMaxFps == 144);
    CHECK(cvarTestGamma.get() == doctest::Approx(2.2f));
    CHECK(cvarTestName.get() == "Pilot");
    CHECK(cvarTestMaxFps.type() == CVarType::Int);
    CHECK(cvarTestName.description() == "Player name");
    CHECK(cvarTestMaxFps.defaultValue() == 144);
}

TEST_CASE("cvar: execute parses set/query/shorthand/toggle/reset") {
    REQUIRE(reg().execute("set test.r.vsync 0"));
    CHECK(!cvarTestVsync.get());
    REQUIRE(reg().execute("test.r.vsync on"));
    CHECK(cvarTestVsync.get());
    REQUIRE(reg().execute("toggle test.r.vsync"));
    CHECK(!cvarTestVsync.get());
    REQUIRE(reg().execute("reset test.r.vsync"));
    CHECK(cvarTestVsync.get());

    REQUIRE(reg().execute("test.r.maxFps 0x3C"));
    CHECK(cvarTestMaxFps.get() == 60);
    REQUIRE(reg().execute("set test.r.maxFps -5"));
    CHECK(cvarTestMaxFps.get() == -5);
    REQUIRE(reg().execute("test.r.gamma 1.8"));
    CHECK(cvarTestGamma.get() == doctest::Approx(1.8f));
    REQUIRE(reg().execute("test.player.name \"Captain \\\"Nova\\\" Reyes\""));
    CHECK(cvarTestName.get() == "Captain \"Nova\" Reyes");

    auto query = reg().execute("test.r.maxFps");
    REQUIRE(query);
    CHECK(query->find("test.r.maxFps = -5") != std::string::npos);
    CHECK(query->find("default 144") != std::string::npos);
    CHECK(reg().get("test.r.gamma").value() == "1.8");
    CHECK(reg().execute("get test.r.maxFps").value() == "-5");

    // Multiple statements, comments and blank statements.
    auto multi = reg().execute("test.r.maxFps 30; test.r.vsync 0 // trailing comment\n# full comment line\n;;");
    REQUIRE(multi);
    CHECK(cvarTestMaxFps.get() == 30);
    CHECK(!cvarTestVsync.get());
    cvarTestVsync.resetToDefault();
    cvarTestMaxFps.resetToDefault();
    cvarTestGamma.resetToDefault();
    cvarTestName.resetToDefault();
}

TEST_CASE("cvar: parse errors leave values unchanged") {
    CHECK(reg().execute("test.r.maxFps banana").error().code == ErrorCode::ParseError);
    CHECK(reg().execute("test.r.maxFps 99999999999").error().code == ErrorCode::OutOfRange);
    CHECK(reg().execute("test.r.vsync maybe").error().code == ErrorCode::ParseError);
    CHECK(reg().execute("test.r.gamma 1.5x").error().code == ErrorCode::ParseError);
    CHECK(reg().execute("set test.r.vsync").error().code == ErrorCode::InvalidArgument);
    CHECK(reg().execute("test.r.vsync 1 2").error().code == ErrorCode::InvalidArgument);
    CHECK(reg().execute("no.such.thing 1").error().code == ErrorCode::NotFound);
    CHECK(reg().execute("test.player.name \"unterminated").error().code == ErrorCode::ParseError);
    // Regression: the sign was stripped before std::from_chars, which accepts its own '-'.
    for (const char* bad : {"--5", "+-5", "-+5", "0x-5", "0x+5", "-", "+", "0x"}) {
        CAPTURE(bad);
        CHECK(helios::detail::parseCVarInt(bad).error().code == ErrorCode::ParseError);
    }
    CHECK(helios::detail::parseCVarInt("-5").value() == -5);
    CHECK(helios::detail::parseCVarInt("+5").value() == 5);
    CHECK(helios::detail::parseCVarInt("-0x10").value() == -16);
    CHECK(helios::detail::parseCVarInt("-2147483648").value() == INT32_MIN);
    CHECK(helios::detail::parseCVarInt("2147483648").error().code == ErrorCode::OutOfRange);
    for (const char* bad : {"+-1.5", "nan", "inf", "-inf", "1e999"}) {
        CAPTURE(bad);
        CHECK(!helios::detail::parseCVarFloat(bad));
    }
    CHECK(helios::detail::parseCVarFloat("+1.5").value() == 1.5f);
    CHECK(helios::detail::parseCVarFloat("-0.25").value() == -0.25f);
    CHECK(reg().execute("test.r.gamma nan").error().code == ErrorCode::ParseError);
    CHECK(cvarTestMaxFps.get() == 144);
    CHECK(cvarTestVsync.get());
}

TEST_CASE("cvar: flags are enforced per source") {
    // ReadOnly: code/config only.
    CHECK(reg().execute("test.sys.build 8").error().code == ErrorCode::PermissionDenied);
    CHECK(cvarTestBuild.set(8));
    CHECK(reg().set("test.sys.build", "9", CVarSource::Config));
    CHECK(cvarTestBuild.get() == 9);

    // Cheat: console needs cheats enabled.
    CHECK(reg().execute("test.cheat.god 1").error().code == ErrorCode::PermissionDenied);
    reg().setCheatsEnabled(true);
    CHECK(reg().execute("test.cheat.god 1"));
    CHECK(cvarTestGodMode.get());
    reg().setCheatsEnabled(false);
    CHECK(cvarTestGodMode.set(false));

    // Replicated: clients accept only Code/Server.
    reg().setReplicationClient(true);
    CHECK(reg().execute("test.sv.gravity 1.6").error().code == ErrorCode::PermissionDenied);
    CHECK(reg().set("test.sv.gravity", "1.6", CVarSource::Server));
    CHECK(cvarTestGravity.get() == doctest::Approx(1.6f));
    reg().setReplicationClient(false);
    CHECK(reg().execute("test.sv.gravity 9.81"));
    const auto replicated = reg().snapshot(CVarFlags::Replicated);
    CHECK(std::find(replicated.begin(), replicated.end(), std::pair<std::string, std::string>("test.sv.gravity", "9.81")) !=
          replicated.end());
}

TEST_CASE("cvar: change callbacks fire once per actual change") {
    int specific = 0;
    int global = 0;
    std::string lastName;
    const u64 a = cvarTestMaxFps.onChange([&](CVarBase& c) {
        ++specific;
        lastName = std::string(c.name());
    });
    const u64 b = reg().addChangeCallback("", [&](CVarBase&) { ++global; });
    REQUIRE(reg().execute("test.r.maxFps 120"));
    REQUIRE(reg().execute("test.r.maxFps 120")); // unchanged: no callback
    CHECK(cvarTestMaxFps.set(90));
    REQUIRE(reg().execute("test.r.vsync 0"));
    CHECK(specific == 2);
    CHECK(global == 3);
    CHECK(lastName == "test.r.maxFps");
    reg().removeChangeCallback(a);
    reg().removeChangeCallback(b);
    CHECK(cvarTestMaxFps.set(144));
    cvarTestVsync.resetToDefault();
    CHECK(specific == 2);
}

TEST_CASE("cvar: console commands with arguments") {
    std::vector<std::string> received;
    ConsoleCommand echo("test.echo", "Echo arguments", [&](std::span<const std::string> args) -> Result<std::string> {
        received.assign(args.begin(), args.end());
        std::string out;
        for (const auto& a : args) out += "[" + a + "]";
        return out;
    });
    auto out = reg().execute("test.echo one \"two words\" 3");
    REQUIRE(out);
    CHECK(*out == "[one][two words][3]");
    CHECK(received.size() == 3);
    CHECK(reg().hasCommand("TEST.ECHO"));
    CHECK(reg().registerCommand("test.echo", "dup", [](auto) -> Result<std::string> { return std::string(); })
              .error()
              .code == ErrorCode::AlreadyExists);

    ConsoleCommand failing("test.fail", "Always fails", [](std::span<const std::string>) -> Result<std::string> {
        return Error{ErrorCode::InvalidState, "nope"};
    });
    CHECK(reg().execute("test.fail").error().code == ErrorCode::InvalidState);
    // Execution stops at the first failing statement.
    received.clear();
    CHECK(!reg().execute("test.fail; test.echo after"));
    CHECK(received.empty());

    ConsoleCommand cheat("test.spawn", "Spawn", [](std::span<const std::string>) -> Result<std::string> { return std::string("ok"); },
                         CVarFlags::Cheat);
    CHECK(reg().execute("test.spawn").error().code == ErrorCode::PermissionDenied);

    auto listing = reg().execute("list test.r.");
    REQUIRE(listing);
    CHECK(listing->find("test.r.vsync") != std::string::npos);
    CHECK(listing->find("test.player.name") == std::string::npos);
    const auto completions = reg().complete("test.e");
    CHECK(completions == std::vector<std::string>{"test.echo"});
}

TEST_CASE("cvar: saved variables serialize and reload through config") {
    QuietLogs quiet;
    CHECK(cvarTestMaxFps.set(75));
    CHECK(cvarTestName.set("Ace Pilot"));
    const std::string saved = reg().serializeSaved();
    CHECK(saved.find("test.r.maxFps 75\n") != std::string::npos);
    CHECK(saved.find("test.player.name \"Ace Pilot\"\n") != std::string::npos);
    CHECK(saved.find("test.r.vsync") == std::string::npos); // default value: not written
    CHECK(saved.find("test.r.gamma") == std::string::npos); // not Saved

    cvarTestMaxFps.resetToDefault();
    cvarTestName.resetToDefault();
    CHECK(reg().executeConfig(saved + "bogus.line with too many\n") == 1);
    CHECK(cvarTestMaxFps.get() == 75);
    CHECK(cvarTestName.get() == "Ace Pilot");
    cvarTestMaxFps.resetToDefault();
    cvarTestName.resetToDefault();

    // Config values for variables that register later (e.g. a plugin loaded afterwards).
    CHECK(reg().executeConfig("test.late.value 1234\n") == 0);
    {
        CVar<i32> late("test.late.value", 1, "Registered after the config was read");
        CHECK(late.get() == 1234);
    }
    CHECK(reg().find("test.late.value") == nullptr); // unregistered on destruction
}

TEST_CASE("cvar: concurrent reads and writes") {
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    std::atomic<int> weird{0};
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                const i32 v = cvarTestMaxFps.get();
                if (v != 144 && (v < 1000 || v >= 2000)) weird.fetch_add(1);
                const std::string name = cvarTestName.get();
                if (name != "Pilot" && name.rfind("Pilot-", 0) != 0) weird.fetch_add(1);
            }
        });
    }
    for (int i = 0; i < 2000; ++i) {
        CHECK(cvarTestMaxFps.set(1000 + i % 1000));
        (void)reg().execute("test.player.name Pilot-" + std::to_string(i));
    }
    stop = true;
    for (auto& t : readers) t.join();
    CHECK(weird.load() == 0);
    cvarTestMaxFps.resetToDefault();
    cvarTestName.resetToDefault();
}

TEST_CASE("cvar: duplicate names are rejected") {
    QuietLogs quiet;
    CVar<bool> dup("test.r.vsync", false, "duplicate");
    CHECK(!dup.isRegistered());
    CHECK(reg().find("test.r.vsync") == &cvarTestVsync);
}
