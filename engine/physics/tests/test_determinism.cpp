// RT-03, Phase 0 scope (02 §8.2, 09 WP-0.9): the scripted scenes' state hashes are golden constants,
// and rebuilding a scene so that every BodyID differs gives the same hash (the permuted variant).
//
// The SAME golden must hold on every determinism toolchain (both MSVC toolsets, clang-cl, GCC, Clang,
// MinGW; ADR-001a rule 7) at any worker count: Jolt runs with JPH_CROSS_PLATFORM_DETERMINISTIC and no
// FMA, every Helios-side order is by (layer, key), and the vendored stable-order patch
// (third_party/jolt/patches/0001-stable-order.patch) makes Jolt's own solver order use (layer, key)
// instead of BodyIDs. This container builds GCC, Clang and MinGW but runs only the Linux binaries;
// CI's Windows jobs (windows-msvc, windows-msvc-floor, windows-clang-cl) are the cross-compiler check
// for MSVC and clang-cl and run these same assertions. Never update a golden to make one platform pass:
// find the platform's codegen difference instead (09 §5.3).
#include <doctest/doctest.h>

#include <cstdio>

#include "helios/core/jobs.h"
#include "scene.h"

using namespace helios;
using namespace helios::physics;
using namespace helios::physics::test;

namespace {

// Golden of the scripted full scene (scene.cpp) after 600 fixed steps of 1/60 s. Re-pinned once when the
// stable-order patch landed: it sorts contact constraints by a hash of (layer, key) instead of BodyIDs
// and drops the ship hull's cross-step contact cache, so the solver order and the hull's warm start
// changed (stock Jolt 5.6.0 gave 0xff972a409e8145e1).
constexpr u64 kGoldenFullScene600 = 0x1000fc8781dc8b58ull;
// Golden of the independent-islands scene after 600 steps (one contact constraint per island, so the
// patch did not change it).
constexpr u64 kGoldenIndependent600 = 0x1cb97ff0f53a5b1eull;
// Golden of the tile scene after 600 steps.
constexpr u64 kGoldenTiles600 = 0x51afce82ae11d217ull;

std::string hex(u64 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llxull", static_cast<unsigned long long>(v));
    return buf;
}

/// Runs a plain and a permuted build of a scene side by side and checks their hashes after every second
/// (the first differing second bisects a divergence) and the character's supporting body after every
/// step; returns the plain build's final hash.
u64 checkPermutedMatches(SceneOptions plainOptions, SceneOptions permutedOptions) {
    permutedOptions.permuteBodyIds = true;
    Scene plain(plainOptions), permuted(permutedOptions);
    u32 groundMismatches = 0, firstMismatch = 0;
    for (u32 second = 1; second <= 10; ++second) {
        for (u32 i = 0; i < 60; ++i) {
            plain.step();
            permuted.step();
            if (!plain.character()) continue;
            // Of several equally supporting contacts (a character on a tile seam or corner), CharacterVirtual
            // takes the first in its contact order, so the ground key follows that order directly.
            const auto ga = plain.grid().characterState(plain.character());
            const auto gb = permuted.grid().characterState(permuted.character());
            REQUIRE(ga.ok());
            REQUIRE(gb.ok());
            if (ga->groundKey != gb->groundKey && groundMismatches++ == 0) firstMismatch = plain.stepIndex();
        }
        const u64 a = plain.grid().stateHash(), b = permuted.grid().stateHash();
        CHECK_MESSAGE(a == b, "t = " << second << " s: plain " << hex(a) << ", permuted " << hex(b));
    }
    CHECK_MESSAGE(groundMismatches == 0,
                  groundMismatches << " steps with a different ground key, first at step " << firstMismatch);
    return plain.grid().stateHash();
}

TEST_CASE("determinism: the scripted scene matches its golden hash (RT-03 Phase 0)") {
    PhysicsRuntime runtime;
    Scene s({});
    // Intermediate hashes help bisect a divergence to the first differing second.
    for (u32 second = 1; second <= 10; ++second) {
        const u64 h = s.run(60);
        MESSAGE("t = " << second << " s: " << hex(h));
    }
    const u64 h = s.grid().stateHash();
    CHECK_MESSAGE(h == kGoldenFullScene600, "state hash " << hex(h));
}

TEST_CASE("determinism: the golden holds at 1, 2, 4 and 16 workers") {
    PhysicsRuntime runtime;
    for (u32 workers : {1u, 2u, 4u, 16u}) {
        jobs::JobSystem js({.workerCount = workers, .name = "Rt03"});
        Scene s({.jobs = &js});
        const u64 h = s.run(600);
        CHECK_MESSAGE(h == kGoldenFullScene600, "workers " << workers << ": " << hex(h));
        Scene t({.jobs = &js, .tiles = true});
        const u64 ht = t.run(600);
        CHECK_MESSAGE(ht == kGoldenTiles600, "tile scene, workers " << workers << ": " << hex(ht));
    }
}

TEST_CASE("determinism: repeated runs in one process are identical") {
    PhysicsRuntime runtime;
    Scene a({}), b({});
    for (int i = 0; i < 300; ++i) {
        a.step();
        b.step();
        REQUIRE(a.grid().stateHash() == b.grid().stateHash());
    }
}

TEST_CASE("determinism: permuted BodyIDs give the same result when no island has two contacts") {
    // Every body rests on the ground alone, so the order of contacts inside an island cannot matter. The
    // roles in each contact constraint still could: stock Jolt makes the lower BodyID "body 1", and the
    // permuted build gives the ground a higher BodyID than every body (the patch orders by (layer, key)).
    PhysicsRuntime runtime;
    Scene plain({.independentOnly = true});
    Scene permuted({.permuteBodyIds = true, .independentOnly = true});
    const u64 a = plain.run(600), b = permuted.run(600);
    CHECK_MESSAGE(a == b, hex(a) << " vs " << hex(b));
    CHECK_MESSAGE(a == kGoldenIndependent600, "state hash " << hex(a));
}

TEST_CASE("determinism: permuted BodyIDs give the identical hash, multi-contact islands included (RT-03 permuted variant)") {
    // RT-03's permuted variant (02 §7.1, §8.2): the full scene rebuilt after 1,000 dummy add/remove
    // cycles, one body at a time, so that every BodyID differs from the plain build's and every pair of
    // bodies has the opposite BodyID order (scene.cpp), and so does the broadphase layout. Stock Jolt 5.6
    // ordered contact constraints by a hash of BodyIDs, made the lower BodyID "body 1" and ordered
    // CharacterVirtual contacts by BodyID, so the pyramid, the stacked spheres and the tumbling capsules
    // diverged in the low bits. With the stable-order patch every one of those orders is (layer, key). A
    // build without the patch fails this case.
    PhysicsRuntime runtime;
    const u64 a = checkPermutedMatches({}, {});
    CHECK_MESSAGE(a == kGoldenFullScene600, "state hash " << hex(a));
    // Neither the admission order nor the worker count of the permuted build matters either.
    jobs::JobSystem js({.workerCount = 4, .name = "Rt03Permuted"});
    for (u64 seed : {0x5eedull, 0xc0ffeeull}) {
        Scene permuted({.jobs = &js, .permuteBodyIds = true, .shuffleSeed = seed});
        const u64 b = permuted.run(600);
        CHECK_MESSAGE(b == kGoldenFullScene600, "shuffled with seed " << seed << " at 4 workers: " << hex(b));
    }
}

TEST_CASE("determinism: permuted BodyIDs give the identical hash with a hull, a chassis and a character on four tiles each") {
    // RT-03's permuted variant with its tile clause (02 §8.2): a ShipHull, a Vehicle-layer chassis, a box
    // stack and the CharacterVirtual each start over the shared corner of four Terrain tile bodies, and the
    // permuted build admits the tiles (and everything else) in reverse, or in a shuffled order. The hull
    // and the chassis keep no contact cache across steps (NoCrossUpdateCache), the stack is a multi-contact
    // island on several tiles, and the character, which walks across tile seams and corners, must stand
    // on the same tile in both builds: of equally supporting tiles, CharacterVirtual picks the first in its
    // contact order, which the patch keys by (layer, key) instead of BodyID.
    PhysicsRuntime runtime;
    const u64 a = checkPermutedMatches({.tiles = true}, {.tiles = true});
    CHECK_MESSAGE(a == kGoldenTiles600, "state hash " << hex(a));
    const u64 b = checkPermutedMatches({.tiles = true}, {.tiles = true, .shuffleSeed = 0x5eed});
    CHECK_MESSAGE(b == kGoldenTiles600, "shuffled: state hash " << hex(b));
}

} // namespace
