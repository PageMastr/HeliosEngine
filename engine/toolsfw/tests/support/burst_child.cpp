// ED-1 burst child: commits workload transactions as fast as it can, journaled with the real
// group-commit settings, and prints "c <committed>" after each commit returns. The parent
// (test_ed1.cpp) kills it with kill -9 / TerminateProcess mid-burst and recovers the journal.
//
//   toolsfw_burst_child --root=<project> --journal=<journal root> --seed=<n> --session=<name>
//                       [--max-steps=<n>] [--max-seconds=<n>]

#include <string>

#include "helios/core/cmdline.h"
#include "helios/core/process.h"
#include "helios/core/time.h"
#include "test_project.h"
#include "workload.h"

using namespace helios;

int main(int argc, char** argv) {
    const CommandLine cmd = CommandLine::parse(argc, argv);
    const fs::Path root = fs::pathFromUtf8(cmd.getString("root"));
    const fs::Path journal = fs::pathFromUtf8(cmd.getString("journal"));
    const u64 seed = static_cast<u64>(cmd.getInt("seed", 1));
    const u64 maxSteps = static_cast<u64>(cmd.getInt("max-steps", 1'000'000));
    const f64 maxSeconds = cmd.getFloat("max-seconds", 60.0);
    const std::string session(cmd.getString("session", "burst"));
    if (root.empty() || journal.empty()) return 2;

    tf::FrameworkConfig cfg = tf::test::deterministicConfig(root, journal, seed, session);
    auto fw = tf::Framework::create(cfg);
    if (!fw) return 3;
    auto doc = tf::test::openSampleHull(**fw);
    if (!doc) return 4;
    PipeEnd out = PipeEnd::adopt(standardOutputHandle());
    // One transaction per committed step (no multi-commit gestures), so the printed count is the
    // number of journaled transactions.
    tf::test::Workload workload(seed, {.mergeKeys = false});
    u64 committed = 0;
    const f64 start = monotonicSeconds();
    for (u64 i = 0; i < maxSteps && monotonicSeconds() - start < maxSeconds; ++i) {
        auto r = workload.step(**fw, *doc);
        if (!r) {
            (void)out.release();
            return 5;
        }
        if (*r) {
            ++committed;
            const std::string line = "c " + std::to_string(committed) + "\n";
            if (!out.write(line)) break;
        }
    }
    (void)out.release();
    return 0;
}
