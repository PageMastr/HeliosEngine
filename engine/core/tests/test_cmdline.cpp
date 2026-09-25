#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "helios/core/cmdline.h"

using namespace helios;

TEST_CASE("cmdline: flags, options and positionals from argv") {
    const char* argv[] = {"game.exe", "-windowed", "--width=1920", "-Height=1080", "save1",
                          "-map=/content/maps/a.json", "-5",  "--", "--not-an-option", "-x"};
    const auto cl = CommandLine::parse(static_cast<int>(std::size(argv)), argv);
    CHECK(cl.program() == "game.exe");
    CHECK(cl.arguments().size() == std::size(argv) - 1);
    CHECK(cl.has("windowed"));
    CHECK(cl.has("WINDOWED"));
    CHECK(!cl.value("windowed").has_value());
    CHECK(cl.getInt("width", 0) == 1920);
    CHECK(cl.getInt("height", 0) == 1080);
    CHECK(cl.getString("map") == "/content/maps/a.json");
    CHECK(cl.positional() == std::vector<std::string>{"save1", "-5", "--not-an-option", "-x"});
    CHECK(!cl.has("x"));
    CHECK(cl.getBool("windowed", false));
    CHECK(!cl.getBool("fullscreen", false));
    CHECK(cl.getInt("missing", 7) == 7);
}

TEST_CASE("cmdline: typed getters and repeated keys") {
    const std::vector<std::string> args = {"-scale=1.5", "-vsync=off", "-debug=yes", "-mod=a", "-mod=b",
                                           "-bad=12x", "-empty="};
    const auto cl = CommandLine::parse(args, false);
    CHECK(cl.program().empty());
    CHECK(cl.getFloat("scale", 0.0) == doctest::Approx(1.5));
    CHECK(!cl.getBool("vsync", true));
    CHECK(cl.getBool("debug", false));
    CHECK(cl.value("mod") == "b");
    CHECK(cl.values("MOD") == std::vector<std::string_view>{"a", "b"});
    CHECK(cl.getInt("bad", -1) == -1);
    CHECK(cl.has("empty"));
    CHECK(cl.value("empty") == "");
    CHECK(cl.options().size() == 7);

    // One optional sign only (std::from_chars accepts its own '-' after a stripped '+').
    const std::vector<std::string> signs = {"-a=+5", "-b=+-5", "-c=-5", "-d=+-1.5", "-e=+2.5", "-f=++5"};
    const auto sl = CommandLine::parse(signs, false);
    CHECK(sl.getInt("a", 0) == 5);
    CHECK(sl.getInt("b", 99) == 99);
    CHECK(sl.getInt("c", 0) == -5);
    CHECK(sl.getFloat("d", 9.0) == 9.0);
    CHECK(sl.getFloat("e", 0.0) == 2.5);
    CHECK(sl.getInt("f", 99) == 99);
}

TEST_CASE("cmdline: Windows command-line splitting follows the MSVC CRT rules") {
    using V = std::vector<std::string>;
    CHECK(CommandLine::splitWindows(L"game.exe a b") == V{"game.exe", "a", "b"});
    CHECK(CommandLine::splitWindows(L"\"C:\\Program Files\\Helios\\game.exe\" -map=\"a b\"") ==
          V{"C:\\Program Files\\Helios\\game.exe", "-map=a b"});
    CHECK(CommandLine::splitWindows(L"p \"a b c\" d e") == V{"p", "a b c", "d", "e"});
    CHECK(CommandLine::splitWindows(L"p \"ab\\\"c\" \"\\\\\" d") == V{"p", "ab\"c", "\\", "d"});
    CHECK(CommandLine::splitWindows(L"p a\\\\\\b d\"e f\"g h") == V{"p", "a\\\\\\b", "de fg", "h"});
    CHECK(CommandLine::splitWindows(L"p a\\\\\\\"b c d") == V{"p", "a\\\"b", "c", "d"});
    CHECK(CommandLine::splitWindows(L"p a\\\\\\\\\"b c\" d e") == V{"p", "a\\\\b c", "d", "e"});
    CHECK(CommandLine::splitWindows(L"p \"\" x") == V{"p", "", "x"});
    CHECK(CommandLine::splitWindows(L"p \"a\"\"b\"") == V{"p", "a\"b"});
    CHECK(CommandLine::splitWindows(L"p\t  tabs\t") == V{"p", "tabs"});
    CHECK(CommandLine::splitWindows(L"C:\\dir\\prog.exe x") == V{"C:\\dir\\prog.exe", "x"}); // no escapes in argv[0]
    CHECK(CommandLine::splitWindows(L"p \u00e9t\u00e9") == V{"p", "\xC3\xA9t\xC3\xA9"});
    CHECK(CommandLine::splitWindows(L"") == V{""});

    const auto cl = CommandLine::parseWindows(L"\"C:\\Games\\Helios.exe\" -server -port=7777 \"my save\"");
    CHECK(cl.program() == "C:\\Games\\Helios.exe");
    CHECK(cl.has("server"));
    CHECK(cl.getInt("port", 0) == 7777);
    CHECK(cl.positional() == V{"my save"});
}

TEST_CASE("cmdline: the current process command line is available") {
    const auto cl = CommandLine::fromProcess();
    CHECK(!cl.program().empty());
}
