#include <doctest/doctest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "helios/core/fs.h"
#include "helios/core/time.h"

using namespace helios;
using namespace helios::fs;

namespace {

struct TempDir {
    Path path;
    TempDir() {
        auto dir = createUniqueTempDirectory("helios-fs-test");
        REQUIRE(dir);
        path = *dir;
    }
    ~TempDir() { (void)removeAll(path); }
};

/// Polls the watcher until `pred` holds for the accumulated events or the timeout expires.
bool waitForEvents(FileWatcher& watcher, std::vector<FileEvent>& events,
                   const std::function<bool(const std::vector<FileEvent>&)>& pred, u32 timeoutMs = 3000) {
    Stopwatch sw;
    while (sw.elapsedMillis() < timeoutMs) {
        watcher.poll(events);
        if (pred(events)) return true;
        sleepMillis(10);
    }
    return pred(events);
}

bool hasEvent(const std::vector<FileEvent>& events, std::initializer_list<FileAction> actions, std::string_view path) {
    return std::any_of(events.begin(), events.end(), [&](const FileEvent& e) {
        return e.path == path && std::find(actions.begin(), actions.end(), e.action) != actions.end();
    });
}

} // namespace

TEST_CASE("fs: UTF-8 path conversion round-trips") {
    const std::string utf8 = "content/\xC3\xBCn\xC3\xAF\x63\xC3\xB8\x64\xC3\xA9/\xE2\x82\xAC.txt"; // ünïcødé/€.txt
    const Path p = pathFromUtf8(utf8);
    CHECK(pathToGenericUtf8(p) == utf8);
    CHECK(pathToUtf8(pathFromUtf8("plain")) == "plain");
}

TEST_CASE("fs: whole-file write/read, atomic replace and errors") {
    TempDir tmp;
    const Path file = tmp.path / "data.bin";
    std::vector<u8> payload(100'000);
    for (usize i = 0; i < payload.size(); ++i) payload[i] = static_cast<u8>(i * 31);
    REQUIRE(writeFile(file, payload));
    auto read = readFile(file);
    REQUIRE(read);
    CHECK(*read == payload);
    CHECK(fileSize(file).value() == payload.size());

    // Atomic overwrite leaves exactly one file (no temp files behind).
    REQUIRE(writeTextFile(file, "replaced"));
    CHECK(readTextFile(file).value() == "replaced");
    REQUIRE(writeTextFile(tmp.path / "direct.txt", "direct", WriteMode::Direct));
    CHECK(readTextFile(tmp.path / "direct.txt").value() == "direct");
    auto listing = listDirectory(tmp.path);
    REQUIRE(listing);
    CHECK(listing->size() == 2);

    const auto missing = readFile(tmp.path / "missing.bin");
    CHECK(!missing);
    CHECK(missing.error().code == ErrorCode::NotFound);
    CHECK(!writeTextFile(tmp.path / "no-such-dir" / "x.txt", "x"));
    CHECK(fs::exists(file));
    CHECK(isFile(file));
    CHECK(!isDirectory(file));
    CHECK(isDirectory(tmp.path));
    CHECK(lastWriteTime(file));
}

TEST_CASE("fs: readFile is exact for any size and does not trust the reported size") {
    TempDir tmp;
    for (usize size : {usize(0), usize(1), usize(4095), usize(4096), usize(4097), usize(65'539)}) {
        CAPTURE(size);
        std::vector<u8> payload(size);
        for (usize i = 0; i < size; ++i) payload[i] = static_cast<u8>(i * 7 + 3);
        const Path file = tmp.path / ("exact-" + std::to_string(size) + ".bin");
        REQUIRE(writeFile(file, payload));
        auto read = readFile(file);
        REQUIRE(read);
        CHECK(*read == payload);
    }
    if constexpr (platform::kIsLinux) {
        // procfs reports st_size == 0; the content must still be read to EOF.
        auto status = readTextFile("/proc/self/status");
        REQUIRE(status);
        CHECK(status->find("Pid:") != std::string::npos);
    }
}

TEST_CASE("fs: non-ASCII file names") {
    TempDir tmp;
    const Path file = tmp.path / pathFromUtf8("\xE8\x88\xB9-ship-\xC3\xA9.json"); // 船-ship-é.json
    REQUIRE(writeTextFile(file, "{}"));
    auto listing = listDirectory(tmp.path);
    REQUIRE(listing);
    REQUIRE(listing->size() == 1);
    CHECK((*listing)[0].relativePath == "\xE8\x88\xB9-ship-\xC3\xA9.json");
    CHECK(readTextFile(file).value() == "{}");
}

TEST_CASE("fs: File handle operations") {
    TempDir tmp;
    const Path path = tmp.path / "handle.bin";
    {
        auto f = File::open(path, OpenMode::Write);
        REQUIRE(f);
        CHECK(f->isOpen());
        REQUIRE(f->write("0123456789", 10));
        CHECK(f->tell().value() == 10);
        REQUIRE(f->sync());
    }
    {
        auto f = File::open(path, OpenMode::Append);
        REQUIRE(f);
        REQUIRE(f->write("AB", 2));
    }
    auto f = File::open(path, OpenMode::Read);
    REQUIRE(f);
    CHECK(f->size().value() == 12);
    char buf[16] = {};
    REQUIRE(f->readExact(buf, 4));
    CHECK(std::string_view(buf, 4) == "0123");
    CHECK(f->seek(-2, SeekOrigin::End).value() == 10);
    CHECK(f->read(buf, 16).value() == 2);
    CHECK(std::string_view(buf, 2) == "AB");
    CHECK(f->read(buf, 16).value() == 0); // EOF
    CHECK(f->readAt(5, buf, 3).value() == 3);
    CHECK(std::string_view(buf, 3) == "567");
    CHECK(f->readAt(100, buf, 3).value() == 0);
    REQUIRE(f->seek(8));
    CHECK(f->readExact(buf, 10).error().code == ErrorCode::EndOfFile);
    File moved = std::move(*f);
    CHECK(moved.isOpen());
    CHECK(!f->isOpen());
    moved.close();
    CHECK(!moved.isOpen());
    CHECK(moved.read(buf, 1).error().code == ErrorCode::InvalidState);

    CHECK(File::open(tmp.path / "nope", OpenMode::Read).error().code == ErrorCode::NotFound);
    CHECK(!File::open(tmp.path, OpenMode::Read)); // directories are not files

    // Concurrent positional reads on one handle.
    auto shared = File::open(path, OpenMode::Read);
    REQUIRE(shared);
    std::vector<std::thread> threads;
    std::atomic<int> bad{0};
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 200; ++i) {
                char c = 0;
                const u64 off = static_cast<u64>((t + i) % 10);
                if (shared->readAt(off, &c, 1).value() != 1 || c != static_cast<char>('0' + off)) bad.fetch_add(1);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(bad.load() == 0);
}

TEST_CASE("fs: memory-mapped files") {
    TempDir tmp;
    const Path path = tmp.path / "mapped.txt";
    REQUIRE(writeTextFile(path, "mapped contents"));
    auto m = MappedFile::open(path);
    REQUIRE(m);
    CHECK(m->isOpen());
    CHECK(m->size() == 15);
    CHECK(m->text() == "mapped contents");
    CHECK(m->bytes()[0] == 'm');
    MappedFile moved = std::move(*m);
    CHECK(moved.text() == "mapped contents");
    moved.close();
    CHECK(!moved.isOpen());

    REQUIRE(writeTextFile(tmp.path / "empty.txt", ""));
    auto empty = MappedFile::open(tmp.path / "empty.txt");
    REQUIRE(empty);
    CHECK(empty->size() == 0);
    CHECK(empty->bytes().empty());
    CHECK(MappedFile::open(tmp.path / "missing").error().code == ErrorCode::NotFound);
}

TEST_CASE("fs: directory listing, rename and removal") {
    TempDir tmp;
    REQUIRE(createDirectories(tmp.path / "a" / "b"));
    REQUIRE(writeTextFile(tmp.path / "a" / "one.json", "1"));
    REQUIRE(writeTextFile(tmp.path / "a" / "b" / "two.JSON", "22"));
    REQUIRE(writeTextFile(tmp.path / "a" / "b" / "three.txt", "333"));
    REQUIRE(writeTextFile(tmp.path / "root.txt", "r"));

    auto flat = listDirectory(tmp.path);
    REQUIRE(flat);
    REQUIRE(flat->size() == 2);
    CHECK((*flat)[0].relativePath == "a");
    CHECK((*flat)[0].isDirectory);
    CHECK((*flat)[1].relativePath == "root.txt");
    CHECK((*flat)[1].size == 1);

    ListOptions options;
    options.recursive = true;
    auto all = listDirectory(tmp.path, options);
    REQUIRE(all);
    CHECK(all->size() == 6);
    options.includeDirectories = false;
    options.extension = ".json";
    auto json = listDirectory(tmp.path, options);
    REQUIRE(json);
    REQUIRE(json->size() == 2);
    CHECK((*json)[0].relativePath == "a/b/two.JSON");
    CHECK((*json)[1].relativePath == "a/one.json");
    CHECK(!listDirectory(tmp.path / "missing"));

    REQUIRE(fs::rename(tmp.path / "root.txt", tmp.path / "a" / "one.json")); // replaces the target
    CHECK(readTextFile(tmp.path / "a" / "one.json").value() == "r");
    CHECK(!fs::exists(tmp.path / "root.txt"));
    CHECK(fs::remove(tmp.path / "a" / "one.json"));
    CHECK(fs::remove(tmp.path / "a" / "one.json").error().code == ErrorCode::NotFound);
    CHECK(removeAll(tmp.path / "a").value() == 4);
    CHECK(!fs::exists(tmp.path / "a"));
    CHECK(removeAll(tmp.path / "a").value() == 0);

    auto exe = executablePath();
    REQUIRE(exe);
    CHECK(isFile(*exe));
}

TEST_CASE("fs: native file watcher reports changes") {
    TempDir tmp;
    REQUIRE(createDirectories(tmp.path / "sub"));
    FileWatcher watcher;
    REQUIRE(watcher.start(tmp.path, true, WatchBackend::Auto));
    CHECK(watcher.isWatching());
    CHECK(watcher.directory() == tmp.path);
    if (!watcher.isNative()) MESSAGE("native watcher unavailable; using polling");

    std::vector<FileEvent> events;
    REQUIRE(writeTextFile(tmp.path / "sub" / "new.txt", "v1", WriteMode::Direct));
    CHECK(waitForEvents(watcher, events, [](const auto& ev) {
        return hasEvent(ev, {FileAction::Added, FileAction::Modified}, "sub/new.txt");
    }));

    events.clear();
    REQUIRE(writeTextFile(tmp.path / "sub" / "new.txt", "version two", WriteMode::Direct));
    CHECK(waitForEvents(watcher, events,
                        [](const auto& ev) { return hasEvent(ev, {FileAction::Modified}, "sub/new.txt"); }));

    events.clear();
    REQUIRE(fs::remove(tmp.path / "sub" / "new.txt"));
    CHECK(waitForEvents(watcher, events,
                        [](const auto& ev) { return hasEvent(ev, {FileAction::Removed}, "sub/new.txt"); }));

    // Atomic saves arrive as a rename to the final name (or Added/Modified on polling backends).
    events.clear();
    REQUIRE(writeTextFile(tmp.path / "saved.cfg", "x"));
    CHECK(waitForEvents(watcher, events, [](const auto& ev) {
        return hasEvent(ev, {FileAction::RenamedNew, FileAction::Added, FileAction::Modified}, "saved.cfg");
    }));

    // Directories created after start() are watched too.
    events.clear();
    REQUIRE(createDirectories(tmp.path / "late"));
    sleepMillis(20);
    REQUIRE(writeTextFile(tmp.path / "late" / "deep.txt", "d", WriteMode::Direct));
    CHECK(waitForEvents(watcher, events, [](const auto& ev) {
        return hasEvent(ev, {FileAction::Added, FileAction::Modified}, "late/deep.txt");
    }));
    watcher.stop();
    CHECK(!watcher.isWatching());
    CHECK(watcher.poll().empty());
}

TEST_CASE("fs: polling file watcher reports changes") {
    TempDir tmp;
    FileWatcher watcher;
    REQUIRE(watcher.start(tmp.path, true, WatchBackend::Polling, std::chrono::milliseconds(0)));
    CHECK(!watcher.isNative());
    REQUIRE(writeTextFile(tmp.path / "p.txt", "1"));
    auto events = watcher.poll();
    CHECK(hasEvent(events, {FileAction::Added}, "p.txt"));
    REQUIRE(writeTextFile(tmp.path / "p.txt", "22222"));
    events = watcher.poll();
    CHECK(hasEvent(events, {FileAction::Modified}, "p.txt"));
    REQUIRE(fs::remove(tmp.path / "p.txt"));
    events = watcher.poll();
    CHECK(hasEvent(events, {FileAction::Removed}, "p.txt"));
    CHECK(watcher.poll().empty());
    CHECK(!FileWatcher().start(tmp.path / "missing"));
    CHECK(fileActionName(FileAction::RenamedNew) == "RenamedNew");
}
