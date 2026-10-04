// ED-1 (07 §5.2): headless ToolsFramework round-trips 10k random transactions (apply, undo, redo)
// to byte-identical JSONC on every compiler; kill -9 mid-burst loses at most one transaction.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "helios/core/hash.h"
#include "helios/core/process.h"
#include "test_util.h"
#include "workload.h"

using namespace helios;
using namespace helios::tf;
using namespace helios::tf::test;

namespace {

constexpr u64 kTransactions = 10'000;
/// XXH3-64 of the Frigate's canonical JSONC after kTransactions committed steps of the golden
/// workload (seed 0x0ED1, every step kind). Identical on MSVC, clang-cl, GCC, Clang and MinGW:
/// the workload uses only integer randomness and exact binary fractions (workload.h).
constexpr u64 kGoldenHash = 0x8c27863e2a92a004ull;

struct Run {
    fs::Path root;
    std::unique_ptr<Framework> fw;
    DocId doc;
};

Run start(std::string_view name, const fs::Path& journalRoot = {}, u64 seed = 1) {
    const fs::Path root = freshDir(name);
    REQUIRE(writeProject(root));
    Run r;
    r.root = root;
    auto fw = Framework::create(deterministicConfig(root, journalRoot, seed));
    REQUIRE(fw);
    r.fw = std::move(*fw);
    auto doc = openSampleHull(*r.fw);
    REQUIRE(doc);
    r.doc = *doc;
    return r;
}

const std::string& text(const Run& r) {
    return r.fw->documents().find(r.doc)->text();
}

TEST_CASE("ED-1: 10k random transactions round-trip through undo and redo byte-identically") {
    Run r = start("ed1_roundtrip");
    FrameworkConfig cfg = r.fw->config();
    Workload w(0xED1A, {.undoRedo = false, .invalid = true, .mergeKeys = false});
    const std::string original = text(r);
    std::vector<u64> hashes;
    hashes.reserve(kTransactions + 1);
    hashes.push_back(hash64(original));
    while (hashes.size() <= kTransactions) {
        const u64 before = hash64(text(r));
        auto committed = w.step(*r.fw, r.doc);
        if (!committed) FAIL(committed.error().toString());
        if (!*committed) {
            REQUIRE(hash64(text(r)) == before);  // a rejected edit changed nothing
            continue;
        }
        hashes.push_back(hash64(text(r)));
    }
    CHECK(w.rejected() > 0);
    const std::string final = text(r);
    REQUIRE(r.fw->history().size() == kTransactions);
    for (u64 i = kTransactions; i > 0; --i) {
        REQUIRE(r.fw->undo(Origin::Cli));
        REQUIRE(hash64(text(r)) == hashes[i - 1]);
    }
    CHECK(text(r) == original);
    CHECK_FALSE(r.fw->canUndo());
    for (u64 i = 1; i <= kTransactions; ++i) {
        REQUIRE(r.fw->redo(Origin::Cli));
        REQUIRE(hash64(text(r)) == hashes[i]);
    }
    CHECK(text(r) == final);
    CHECK_FALSE(r.fw->canRedo());
}

TEST_CASE("ED-1: interleaved undo/redo, gestures and rejected edits stay byte-exact, and the journal replays them") {
    const fs::Path journalRoot = freshDir("ed1_mixed_journal");
    Run r = start("ed1_mixed", journalRoot, 2);
    const std::string original = text(r);
    Workload w(0xED1B, {});
    u64 committed = 0;
    while (committed < kTransactions) {
        auto c = w.step(*r.fw, r.doc);
        if (!c) FAIL(c.error().toString());
        if (*c) ++committed;
    }
    const std::string final = text(r);
    // Undo everything that is still in the history, then redo all of it.
    usize undone = 0;
    while (r.fw->canUndo()) {
        REQUIRE(r.fw->undo(Origin::Cli));
        ++undone;
    }
    CHECK(undone == r.fw->history().size());
    CHECK(text(r) == original);
    while (r.fw->canRedo()) REQUIRE(r.fw->redo(Origin::Cli));
    CHECK(text(r) == final);

    // Replaying the journal (every do, undo and redo) onto the original file reproduces it.
    const fs::Path journalFile = r.fw->journal()->path();
    const u64 journaled = r.fw->log().size();  // every do, undo and redo, merged gestures included
    r.fw.reset();
    const fs::Path root = freshDir("ed1_mixed_replay");
    REQUIRE(writeProject(root));
    auto fw = Framework::create(deterministicConfig(root, {}, 3));
    REQUIRE(fw);
    auto report = (*fw)->recover(journalFile);
    REQUIRE(report);
    CHECK(report->replayed == journaled);
    const Document* d = (*fw)->documents().find("records/hull/frigate.hrec");
    REQUIRE(d);
    CHECK(d->text() == final);
}

TEST_CASE("ED-1: the golden workload hashes identically on every compiler") {
    Run r = start("ed1_golden");
    Workload w(0x0ED1, {});
    u64 committed = 0;
    while (committed < kTransactions) {
        auto c = w.step(*r.fw, r.doc);
        if (!c) FAIL(c.error().toString());
        if (*c) ++committed;
    }
    const u64 hash = hash64(text(r));
    MESSAGE("golden workload hash: 0x" << hashHex(hash));
    // Kept for diffing when two compilers disagree (the work directory is per build).
    (void)fs::writeTextFile(r.root / "golden_final.jsonc", text(r));
    CHECK(hash == kGoldenHash);
}

TEST_CASE("ED-1: kill -9 mid-burst loses at most the transaction in flight") {
    const fs::Path root = freshDir("ed1_kill");
    const fs::Path project = root / "project";
    const fs::Path journal = root / "journal";
    REQUIRE(writeProject(project));
    constexpr u64 kSeed = 0xED1C;
    for (int round = 0; round < 3; ++round) {
        const std::string session = "burst" + std::to_string(round);
        REQUIRE(writeProject(project));
        ProcessDesc desc;
        desc.executable = fs::pathFromUtf8(HELIOS_TOOLSFW_BURST_CHILD);
        desc.args = {"--root=" + fs::pathToUtf8(project), "--journal=" + fs::pathToUtf8(journal), "--seed=" + std::to_string(kSeed),
                     "--session=" + session, "--max-seconds=120"};
        desc.stdoutMode = StdioMode::Pipe;
        desc.stderrMode = StdioMode::Null;
        auto child = Process::spawn(desc);
        REQUIRE(child);
        // Let it commit a few hundred transactions (more each round), then kill it mid-burst.
        const u64 target = 200 + 400 * static_cast<u64>(round);
        u64 acknowledged = 0;
        std::string buffer;
        char chunk[4096];
        bool killed = false;
        for (;;) {
            auto got = child->stdoutPipe().read(chunk, sizeof(chunk));
            REQUIRE(got);
            if (*got == 0) break;
            buffer.append(chunk, *got);
            for (usize nl = buffer.find('\n'); nl != std::string::npos; nl = buffer.find('\n')) {
                std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.starts_with("c ")) acknowledged = std::stoull(line.substr(2));
            }
            if (!killed && acknowledged >= target) {
                REQUIRE(child->kill());
                killed = true;
            }
        }
        REQUIRE(killed);
        auto exit = child->wait();
        REQUIRE(exit);
        CHECK(exit->value_or(0) == kKilledExitCode);

        // Recover into a fresh framework over the same (never saved) project.
        auto fw = Framework::create(deterministicConfig(project, {}, 9, "recovery"));
        REQUIRE(fw);
        auto report = (*fw)->recover(journalDirectory(journal, "test-project") / fs::pathFromUtf8(session + ".hjl"));
        REQUIRE(report);
        const u64 recovered = report->replayed;
        INFO("acknowledged " << acknowledged << ", recovered " << recovered << ", torn bytes " << report->tornBytes);
        CHECK(recovered >= acknowledged);      // nothing the child reported as committed is lost
        CHECK(recovered <= acknowledged + 1);  // at most the one it committed but had not reported
        const Document* d = (*fw)->documents().find("records/hull/frigate.hrec");
        REQUIRE(d);

        // The same workload run in-process for `recovered` commits gives the same bytes.
        const fs::Path again = root / ("again" + std::to_string(round));
        REQUIRE(writeProject(again));
        auto ref = Framework::create(deterministicConfig(again, {}, kSeed));
        REQUIRE(ref);
        auto refDoc = openSampleHull(**ref);
        REQUIRE(refDoc);
        Workload w(kSeed, {.mergeKeys = false});  // as in the child: one transaction per step
        u64 committed = 0;
        while (committed < recovered) {
            auto c = w.step(**ref, *refDoc);
            REQUIRE(c);
            if (*c) ++committed;
        }
        const std::string& expected = (*ref)->documents().find(*refDoc)->text();
        CHECK(d->text() == expected);
        if (d->text() != expected) {
            (void)fs::writeTextFile(root / ("recovered" + std::to_string(round) + ".txt"), d->text());
            (void)fs::writeTextFile(root / ("expected" + std::to_string(round) + ".txt"), expected);
        }
    }
}

} // namespace
