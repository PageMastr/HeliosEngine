#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "helios/core/time.h"
#include "helios/core/vfs.h"

using namespace helios;
using namespace helios::fs;

namespace {

struct TempDir {
    Path path;
    TempDir() {
        auto dir = createUniqueTempDirectory("helios-vfs-test");
        REQUIRE(dir);
        path = *dir;
    }
    ~TempDir() { (void)removeAll(path); }
};

std::vector<std::string> paths(const std::vector<VfsEntry>& entries) {
    std::vector<std::string> out;
    for (const auto& e : entries) out.push_back(e.path + (e.isDirectory ? "/" : ""));
    return out;
}

} // namespace

TEST_CASE("vfs: path normalization and sandboxing") {
    CHECK(Vfs::normalizePath("/content/ships/a.json").value() == "/content/ships/a.json");
    CHECK(Vfs::normalizePath("/content//ships/./a.json").value() == "/content/ships/a.json");
    CHECK(Vfs::normalizePath("\\content\\ships\\a.json").value() == "/content/ships/a.json");
    CHECK(Vfs::normalizePath("/content/ships/../stations/b.json").value() == "/content/stations/b.json");
    CHECK(Vfs::normalizePath("/content/").value() == "/content");
    CHECK(Vfs::normalizePath("/").value() == "/");
    CHECK(Vfs::normalizePath("/a/..").value() == "/");
    CHECK(Vfs::normalizePath("/../etc/passwd").error().code == ErrorCode::PermissionDenied);
    CHECK(Vfs::normalizePath("/content/../../x").error().code == ErrorCode::PermissionDenied);
    CHECK(Vfs::normalizePath("relative/path").error().code == ErrorCode::InvalidArgument);
    CHECK(Vfs::normalizePath("").error().code == ErrorCode::InvalidArgument);
    CHECK(Vfs::normalizePath("/c:/windows").error().code == ErrorCode::InvalidArgument);
    CHECK(Vfs::normalizePath(std::string_view("/a\0b", 4)).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("vfs: layered mounts resolve by priority") {
    TempDir base;
    TempDir mod;
    REQUIRE(createDirectories(base.path / "ships"));
    REQUIRE(createDirectories(mod.path / "ships"));
    REQUIRE(writeTextFile(base.path / "ships" / "hauler.json", "base-hauler"));
    REQUIRE(writeTextFile(base.path / "ships" / "fighter.json", "base-fighter"));
    REQUIRE(writeTextFile(mod.path / "ships" / "hauler.json", "mod-hauler"));

    Vfs vfs;
    const auto baseId = vfs.mountDirectory("/content", base.path, 0, true);
    REQUIRE(baseId);
    const auto modId = vfs.mountDirectory("/content", mod.path, 10, true);
    REQUIRE(modId);

    CHECK(vfs.readTextFile("/content/ships/hauler.json").value() == "mod-hauler");
    CHECK(vfs.readTextFile("/content/ships/fighter.json").value() == "base-fighter");
    CHECK(vfs.readTextFile("/content/ships/../ships/./fighter.json").value() == "base-fighter");
    CHECK(vfs.readFile("/content/ships/none.json").error().code == ErrorCode::NotFound);
    CHECK(vfs.readFile("/content/../../outside").error().code == ErrorCode::PermissionDenied);
    CHECK(vfs.readFile("/content/../outside").error().code == ErrorCode::NotFound); // stays inside the root
    CHECK(vfs.exists("/content/ships/fighter.json"));
    CHECK(vfs.isDirectory("/content/ships"));
    CHECK(!vfs.exists("/content/nothing"));

    auto listing = vfs.list("/content/ships");
    REQUIRE(listing);
    CHECK(paths(*listing) ==
          std::vector<std::string>{"/content/ships/fighter.json", "/content/ships/hauler.json"});
    auto native = vfs.resolveNativePath("/content/ships/hauler.json");
    REQUIRE(native);
    CHECK(*native == mod.path / "ships" / "hauler.json");

    CHECK(vfs.mounts().size() == 2);
    CHECK(vfs.unmount(*modId));
    CHECK(!vfs.unmount(*modId));
    CHECK(vfs.readTextFile("/content/ships/hauler.json").value() == "base-hauler");
}

TEST_CASE("vfs: equal priority prefers the most recent mount; memory mounts") {
    Vfs vfs;
    auto first = std::make_unique<MemoryMount>();
    first->addFile("config.txt", std::string_view("first"));
    first->addFile("only-first.txt", std::string_view("1"));
    auto second = std::make_unique<MemoryMount>();
    second->addFile("config.txt", std::string_view("second"));
    REQUIRE(vfs.mount("/cfg", std::move(first)));
    REQUIRE(vfs.mount("/cfg", std::move(second)));
    CHECK(vfs.readTextFile("/cfg/config.txt").value() == "second");
    CHECK(vfs.readTextFile("/cfg/only-first.txt").value() == "1");
    CHECK(!vfs.mount("/x", nullptr));
    CHECK(!vfs.mount("relative", std::make_unique<MemoryMount>()));
}

TEST_CASE("vfs: listing merges layers and shows mount points") {
    Vfs vfs;
    auto a = std::make_unique<MemoryMount>();
    a->addFile("maps/station.json", std::string_view("a"));
    a->addFile("maps/deep/sector.json", std::string_view("a"));
    a->addFile("readme.txt", std::string_view("a"));
    auto b = std::make_unique<MemoryMount>();
    b->addFile("maps/station.json", std::string_view("b"));
    b->addFile("maps/planet.json", std::string_view("b"));
    REQUIRE(vfs.mount("/content", std::move(a)));
    REQUIRE(vfs.mount("/content", std::move(b), 5));
    REQUIRE(vfs.mount("/saved/profiles", std::make_unique<MemoryMount>()));

    auto maps = vfs.list("/content/maps");
    REQUIRE(maps);
    CHECK(paths(*maps) == std::vector<std::string>{"/content/maps/deep/", "/content/maps/planet.json",
                                                   "/content/maps/station.json"});
    auto recursive = vfs.list("/content", true);
    REQUIRE(recursive);
    CHECK(paths(*recursive) == std::vector<std::string>{"/content/maps/", "/content/maps/deep/",
                                                        "/content/maps/deep/sector.json", "/content/maps/planet.json",
                                                        "/content/maps/station.json", "/content/readme.txt"});
    auto root = vfs.list("/");
    REQUIRE(root);
    CHECK(paths(*root) == std::vector<std::string>{"/content/", "/saved/"});
    CHECK(vfs.isDirectory("/saved"));
    CHECK(vfs.exists("/saved"));
    CHECK(vfs.list("/nowhere").error().code == ErrorCode::NotFound);
}

TEST_CASE("vfs: recursive listings include nested mounts, resolved by priority") {
    // Regression: recursive list() only consulted mounts *containing* the directory, so list("/")
    // returned bare mount-point directories and list("/content") missed a DLC mounted below it.
    Vfs vfs;
    auto base = std::make_unique<MemoryMount>();
    base->addFile("ships/hauler.json", std::string_view("base"));
    base->addFile("dlc/pack.json", std::string_view("base-copy")); // shadowed by the DLC mount
    auto dlc = std::make_unique<MemoryMount>();
    dlc->addFile("pack.json", std::string_view("dlc"));
    dlc->addFile("ships/cruiser.json", std::string_view("dlc"));
    auto saved = std::make_unique<MemoryMount>();
    saved->addFile("pilot.cfg", std::string_view("x"));
    REQUIRE(vfs.mount("/content", std::move(base)));
    REQUIRE(vfs.mount("/content/dlc", std::move(dlc), 10));
    REQUIRE(vfs.mount("/saved/profiles", std::move(saved)));

    auto content = vfs.list("/content", true);
    REQUIRE(content);
    CHECK(paths(*content) == std::vector<std::string>{"/content/dlc/", "/content/dlc/pack.json",
                                                      "/content/dlc/ships/", "/content/dlc/ships/cruiser.json",
                                                      "/content/ships/", "/content/ships/hauler.json"});
    // The listed winner matches what readFile() resolves to.
    CHECK(vfs.readTextFile("/content/dlc/pack.json").value() == "dlc");

    auto all = vfs.list("/", true);
    REQUIRE(all);
    CHECK(paths(*all) == std::vector<std::string>{"/content/", "/content/dlc/", "/content/dlc/pack.json",
                                                  "/content/dlc/ships/", "/content/dlc/ships/cruiser.json",
                                                  "/content/ships/", "/content/ships/hauler.json", "/saved/",
                                                  "/saved/profiles/", "/saved/profiles/pilot.cfg"});
    // Non-recursive listings still show nested mounts only as directories.
    auto top = vfs.list("/content");
    REQUIRE(top);
    CHECK(paths(*top) == std::vector<std::string>{"/content/dlc/", "/content/ships/"});
}

TEST_CASE("vfs: directory mounts reject names Windows would treat as devices or aliases") {
    TempDir dir;
    REQUIRE(writeTextFile(dir.path / "console.txt", "ok"));
    DirectoryMount mount(dir.path);
    for (const char* bad : {"con", "CON", "nul.json", "sub/aux", "com1", "com0", "LPT9.txt", "prn", "conin$",
                            "com\xC2\xB9", "LPT\xC2\xB3.log", "file.", "file ", "dir./x", "Con .txt"}) {
        CAPTURE(bad);
        CHECK(mount.readFile(bad).error().code == ErrorCode::InvalidArgument);
        CHECK(!mount.exists(bad));
    }
    // Look-alikes are ordinary names.
    CHECK(mount.readFile("console.txt"));
    for (const char* fine : {"com10", "comx", "lpt", "nullable.json", "a.b.c", ".hidden", "com\xC2\xB4"}) {
        CAPTURE(fine);
        CHECK(mount.readFile(fine).error().code == ErrorCode::NotFound);
    }
    CHECK(!mount.writeFile("nul", {}));

    Vfs vfs;
    REQUIRE(vfs.mountDirectory("/content", dir.path));
    CHECK(vfs.readFile("/content/con").error().code == ErrorCode::NotFound);
    CHECK(vfs.readTextFile("/content/console.txt").value() == "ok");
}

TEST_CASE("vfs: writes go to the highest-priority writable mount") {
    TempDir readOnly;
    TempDir writable;
    Vfs vfs;
    REQUIRE(vfs.mountDirectory(kSavedMount, writable.path, 0, false));
    REQUIRE(vfs.mountDirectory(kSavedMount, readOnly.path, 10, true));
    REQUIRE(vfs.writeTextFile("/saved/profiles/pilot.cfg", "callsign=Vega"));
    CHECK(readTextFile(writable.path / "profiles" / "pilot.cfg").value() == "callsign=Vega");
    CHECK(!fs::exists(readOnly.path / "profiles"));
    CHECK(vfs.readTextFile("/saved/profiles/pilot.cfg").value() == "callsign=Vega");
    CHECK(vfs.writeTextFile("/content/x.txt", "x").error().code == ErrorCode::PermissionDenied);

    Vfs ro;
    REQUIRE(ro.mountDirectory("/content", readOnly.path, 0, true));
    CHECK(!ro.writeTextFile("/content/x.txt", "x"));
    CHECK(!vfs.mountDirectory("/bad", readOnly.path / "missing"));
}

TEST_CASE("vfs: providers reject unsafe relative paths when used directly") {
    TempDir dir;
    REQUIRE(writeTextFile(dir.path / "inside.txt", "ok"));
    DirectoryMount mount(dir.path);
    CHECK(mount.readFile("inside.txt"));
    CHECK(mount.readFile("../outside.txt").error().code == ErrorCode::InvalidArgument);
    CHECK(mount.readFile("/etc/passwd").error().code == ErrorCode::InvalidArgument);
    CHECK(mount.readFile("a/../../x").error().code == ErrorCode::InvalidArgument);
    CHECK(mount.readFile("c:x").error().code == ErrorCode::InvalidArgument);
    CHECK(!mount.exists(".."));
    MemoryMount memory;
    CHECK(!memory.writeFile("../escape", {}));
}

TEST_CASE("vfs: watching a virtual directory reports virtual paths") {
    TempDir dir;
    REQUIRE(createDirectories(dir.path / "scripts"));
    Vfs vfs;
    REQUIRE(vfs.mountDirectory("/content", dir.path));
    std::vector<VfsEvent> events;
    auto id = vfs.watch("/content/scripts", [&](const VfsEvent& e) { events.push_back(e); });
    REQUIRE(id);
    REQUIRE(writeTextFile(dir.path / "scripts" / "ai.luau", "return 1", WriteMode::Direct));
    Stopwatch sw;
    auto seen = [&] {
        return std::any_of(events.begin(), events.end(),
                           [](const VfsEvent& e) { return e.path == "/content/scripts/ai.luau"; });
    };
    while (!seen() && sw.elapsedMillis() < 3000) {
        vfs.pollWatches();
        sleepMillis(10);
    }
    CHECK(seen());
    vfs.unwatch(*id);
    events.clear();
    REQUIRE(writeTextFile(dir.path / "scripts" / "more.luau", "x", WriteMode::Direct));
    sleepMillis(50);
    CHECK(vfs.pollWatches() == 0);
    CHECK(events.empty());

    Vfs memoryOnly;
    REQUIRE(memoryOnly.mount("/content", std::make_unique<MemoryMount>()));
    CHECK(memoryOnly.watch("/content", [](const VfsEvent&) {}).error().code == ErrorCode::Unsupported);
}
