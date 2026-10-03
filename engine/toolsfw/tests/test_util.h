#pragma once
// Shared helpers of the toolsfw tests.

#include <doctest/doctest.h>

#include <string>

#include "helios/core/fs.h"
#include "helios/reflect/path.h"
#include "helios/toolsfw/framework.h"
#include "test_project.h"

namespace helios::tf::test {

/// A fresh directory under the test work directory (removed first if it exists).
inline fs::Path freshDir(std::string_view name) {
    const fs::Path dir = fs::pathFromUtf8(HELIOS_TOOLSFW_TEST_WORK_DIR) / fs::pathFromUtf8(name);
    (void)fs::removeAll(dir);
    REQUIRE(fs::createDirectories(dir));
    return dir;
}

/// A framework over a fresh Frigate project (journal off unless `journal`).
struct Fixture {
    fs::Path root;
    fs::Path journalRoot;
    std::unique_ptr<Framework> fw;
    DocId frigate;

    explicit Fixture(std::string_view name, bool journal = false, u64 seed = 1) {
        root = freshDir(name);
        REQUIRE(writeProject(root / "project"));
        if (journal) journalRoot = root / "journal";
        auto created = Framework::create(deterministicConfig(root / "project", journalRoot, seed));
        REQUIRE(created);
        fw = std::move(*created);
        auto doc = openSampleHull(*fw);
        REQUIRE(doc);
        frigate = *doc;
    }
    const Document& doc() const { return *fw->documents().find(frigate); }
    std::string get(std::string_view path) const {
        auto v = refl::getJson(doc().type(), doc().object(), path);
        REQUIRE(v);
        return *v;
    }
};

/// Replays `journal` into a fresh framework over the fixture's project (a restart after a crash).
struct Recovered {
    std::unique_ptr<Framework> fw;
    RecoveryReport report;
    /// The frigate after recovery ("" when it was not recovered).
    std::string text() const {
        const Document* d = fw->documents().find("records/hull/frigate.hrec");
        return d ? d->text() : std::string();
    }
    DocRecovery status() const {
        REQUIRE(report.documents.size() == 1);
        return report.documents[0].status;
    }
};

inline Recovered recoverFixture(const Fixture& f, const fs::Path& journal, u64 seed = 5) {
    auto created = Framework::create(deterministicConfig(f.root / "project", {}, seed));
    REQUIRE(created);
    Recovered out;
    out.fw = std::move(*created);
    auto report = out.fw->recover(journal);
    REQUIRE_MESSAGE(report, (report ? std::string() : report.error().toString()));
    out.report = std::move(*report);
    return out;
}

} // namespace helios::tf::test
