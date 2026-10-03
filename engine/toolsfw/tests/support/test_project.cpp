#include "test_project.h"

#include <memory>
#include <mutex>

#include "helios/core/random.h"
#include "helios/toolsfw/samples.h"

namespace helios::tf::test {

void ensureSampleTypes() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (auto r = samples::registerSampleTypes(); !r) HELIOS_LOG_FATAL("sample types: {}", r.error());
    });
}

Result<void> writeProject(const fs::Path& root, bool frigate) {
    ensureSampleTypes();
    HELIOS_TRY(fs::removeAll(root));
    HELIOS_TRY(fs::createDirectories(root / "records" / "hull"));
    if (frigate) HELIOS_TRY(fs::writeTextFile(root / "records" / "hull" / "frigate.hrec", samples::sampleHullRecordText()));
    return {};
}

FrameworkConfig deterministicConfig(const fs::Path& root, const fs::Path& journalRoot, u64 seed, std::string_view session) {
    ensureSampleTypes();
    FrameworkConfig cfg;
    cfg.project = "test-project";
    cfg.projectRoot = root;
    cfg.user = "tester";
    cfg.journal = !journalRoot.empty();
    cfg.journalRoot = journalRoot;
    cfg.session = std::string(session);
    auto time = std::make_shared<i64>(1'700'000'000'000'000'000ll);
    cfg.clock = [time] { return *time += 1'000'000; };
    auto rng = std::make_shared<Xoshiro256>(seed ^ 0x6b657973ull);
    cfg.newKey = [rng] {
        // Version-4 shaped GUIDs from the seeded stream (never nil).
        const u64 hi = (rng->nextU64() & ~0xF000ull) | 0x4000ull;
        const u64 lo = (rng->nextU64() & ~(3ull << 62)) | (2ull << 62);
        return Guid(hi, lo);
    };
    return cfg;
}

Result<DocId> openSampleHull(Framework& fw) {
    HELIOS_TRY_ASSIGN(Document* d, fw.open("records/hull/frigate.hrec"));
    return d->id();
}

} // namespace helios::tf::test
