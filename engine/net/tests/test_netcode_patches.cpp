// Helios patches to the vendored netcode (third_party/netcode/patches/, third_party/MANIFEST.md "Patches").
// lint_vendor_patches checks that each patch is applied; these cases check what it is for.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "helios/core/time.h"
#include "helios/core/types.h"

// netcode.c defines its serialisation helpers with external linkage but does not declare them in netcode.h.
extern "C" void netcode_write_bytes(uint8_t** p, uint8_t* byte_array, int num_bytes);

using namespace helios;

namespace {

TEST_SUITE("net.netcode_patches") {
    TEST_CASE("netcode patch write-bytes-memcpy: the bytes written and the pointer advance are unchanged") {
        std::vector<u8> src(1200);
        for (usize i = 0; i < src.size(); ++i) src[i] = static_cast<u8>(i * 31 + 7);
        for (const int count : {0, 1, 7, 64, 700, 1200}) {
            CAPTURE(count);
            std::vector<u8> dst(1300, 0xEE);
            u8* p = dst.data() + 3;
            netcode_write_bytes(&p, src.data(), count);
            CHECK(p == dst.data() + 3 + count);
            CHECK(std::equal(src.begin(), src.begin() + count, dst.begin() + 3));
            CHECK(dst[2] == 0xEE);
            CHECK(dst[static_cast<usize>(3 + count)] == 0xEE);
        }
        // A negative count writes nothing, as the upstream loop did.
        std::vector<u8> dst(16, 0xEE);
        u8* p = dst.data();
        netcode_write_bytes(&p, src.data(), -5);
        CHECK(p == dst.data());
        CHECK(dst[0] == 0xEE);
    }

    TEST_CASE("perf: netcode patch write-bytes-memcpy: a 1,200 B payload is written in <= 0.5 us") {
        // Budget: <= 0.5 us per full payload (>= 2.4 GB/s), the best of 9 trials of 20,000 writes. On the dev
        // VM's 2.1 GHz Xeon (GCC 13 RelWithDebInfo, -fPIC) the per-byte loop it replaces took 1.28 us and the
        // patched copy 0.018 us; no CPU this runs on makes 1,200 calls in 0.5 us, and memcpy stays well
        // under it even under AddressSanitizer.
        std::vector<u8> src(1200, 0x5A);
        std::vector<u8> dst(1200);
        constexpr int kWrites = 20'000;
        f64 best = 1e9;
        bool advanced = true;
        for (int trial = 0; trial < 9; ++trial) {
            const f64 t0 = monotonicSeconds();
            for (int i = 0; i < kWrites; ++i) {
                u8* p = dst.data();
                src[0] = static_cast<u8>(i); // a different input each time, so the copy cannot be hoisted
                netcode_write_bytes(&p, src.data(), static_cast<int>(src.size()));
                advanced = advanced && p == dst.data() + dst.size();
            }
            best = std::min(best, (monotonicSeconds() - t0) / kWrites);
        }
        MESSAGE("netcode_write_bytes: " << best * 1e9 << " ns per 1,200 B payload (best of 9 trials)");
        CHECK(advanced);
        CHECK(dst[0] == static_cast<u8>(kWrites - 1));
        CHECK(best <= 0.5e-6);
    }
}

} // namespace
