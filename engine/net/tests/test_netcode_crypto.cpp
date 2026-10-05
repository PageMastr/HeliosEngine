// The libsodium subset bundled with netcode (third_party/netcode/sodium). By the repository owner's decision of
// 2026-10-05 (docs/evidence/netcode-crypto-owner-decision-2026-10-05.md) tp_netcode is compiled with HAVE_AVX_ASM
// (GCC/Clang on x86-64) and HAVE_EXPLICIT_BZERO (glibc), so that the library's own CPU probe sees AVX2 and picks
// the AVX2 ChaCha20 it compiles, and sodium_memzero wipes with explicit_bzero (SecureZeroMemory on Windows). These
// cases show what the library selected and that every ChaCha20 kernel it compiles, the fallbacks for CPUs
// without AVX2 included, still produces RFC 8439's keystream.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "helios/core/time.h"
#include "helios/core/types.h"

// Declared here because sodium.h is private to tp_netcode. The layout is sodium.h's
// crypto_stream_chacha20_implementation.
extern "C" {
struct crypto_stream_chacha20_implementation {
    int (*stream)(unsigned char* c, unsigned long long clen, const unsigned char* n, const unsigned char* k);
    int (*stream_ietf_ext)(unsigned char* c, unsigned long long clen, const unsigned char* n, const unsigned char* k);
    int (*stream_xor_ic)(unsigned char* c, const unsigned char* m, unsigned long long mlen, const unsigned char* n,
                         uint64_t ic, const unsigned char* k);
    int (*stream_ietf_ext_xor_ic)(unsigned char* c, const unsigned char* m, unsigned long long mlen,
                                  const unsigned char* n, uint32_t ic, const unsigned char* k);
};
extern crypto_stream_chacha20_implementation crypto_stream_chacha20_ref_implementation;
int sodium_init(void);
int sodium_runtime_has_ssse3(void);
int sodium_runtime_has_avx2(void);
int crypto_stream_chacha20_ietf_xor_ic(unsigned char* c, const unsigned char* m, unsigned long long mlen,
                                       const unsigned char* n, uint32_t ic, const unsigned char* k);
void sodium_memzero(void* pnt, size_t len);
}

// Which SIMD kernels the subset compiles: SSSE3 on every x86-64 compiler; AVX2 under GCC and Clang (per-function
// target pragmas) and under MSVC when the image is built with /arch:AVX2 (__AVX2__).
#if defined(__x86_64__) || defined(_M_X64)
#define HELIOS_TEST_SODIUM_SSSE3 1
extern "C" crypto_stream_chacha20_implementation crypto_stream_chacha20_dolbeau_ssse3_implementation;
#if defined(__GNUC__) || defined(__clang__) || defined(__AVX2__)
#define HELIOS_TEST_SODIUM_AVX2 1
extern "C" crypto_stream_chacha20_implementation crypto_stream_chacha20_dolbeau_avx2_implementation;
#endif
#endif

using namespace helios;

namespace {

using IetfXorIc = int (*)(unsigned char*, const unsigned char*, unsigned long long, const unsigned char*, uint32_t,
                          const unsigned char*);

struct Kernel {
    const char* name;
    crypto_stream_chacha20_implementation* impl;
    bool usable; // the running CPU can execute it
};

std::vector<Kernel> compiledKernels() {
    std::vector<Kernel> k{{"ref", &crypto_stream_chacha20_ref_implementation, true}};
#if defined(HELIOS_TEST_SODIUM_SSSE3)
    k.push_back({"ssse3", &crypto_stream_chacha20_dolbeau_ssse3_implementation, sodium_runtime_has_ssse3() != 0});
#endif
#if defined(HELIOS_TEST_SODIUM_AVX2)
    k.push_back({"avx2", &crypto_stream_chacha20_dolbeau_avx2_implementation, sodium_runtime_has_avx2() != 0});
#endif
    return k;
}

// Spies swapped into the implementation tables to see which one crypto_stream_chacha20_ietf_xor_ic dispatches to.
std::array<IetfXorIc, 3> g_saved{};
int g_called = -1;
template <int I>
int spy(unsigned char* c, const unsigned char* m, unsigned long long mlen, const unsigned char* n, uint32_t ic,
        const unsigned char* k) {
    g_called = I;
    return g_saved[I](c, m, mlen, n, ic, k);
}
constexpr std::array<IetfXorIc, 3> kSpies = {&spy<0>, &spy<1>, &spy<2>};

/// The ChaCha20 kernel the library dispatches to ("ref", "ssse3", "avx2"), found by swapping a spy into every
/// compiled kernel's table for one call and restoring them before returning. Single-threaded use only.
const char* selectedKernel() {
    std::vector<Kernel> kernels = compiledKernels();
    for (usize i = 0; i < kernels.size(); ++i) {
        g_saved[i] = kernels[i].impl->stream_ietf_ext_xor_ic;
        kernels[i].impl->stream_ietf_ext_xor_ic = kSpies[i];
    }
    g_called = -1;
    std::array<unsigned char, 64> in{}, out{};
    const std::array<unsigned char, 32> key{};
    const std::array<unsigned char, 12> nonce{};
    (void)crypto_stream_chacha20_ietf_xor_ic(out.data(), in.data(), in.size(), nonce.data(), 0, key.data());
    for (usize i = 0; i < kernels.size(); ++i) kernels[i].impl->stream_ietf_ext_xor_ic = g_saved[i];
    return g_called >= 0 && static_cast<usize>(g_called) < kernels.size() ? kernels[static_cast<usize>(g_called)].name
                                                                          : "none";
}

/// The wipe sodium_memzero uses on this platform, by the code's own precedence (sodium.c) and the definitions
/// third_party/CMakeLists.txt passes.
const char* expectedMemzero() {
#if defined(_WIN32)
    return "SecureZeroMemory";
#elif defined(__GLIBC__)
    return "explicit_bzero";
#else
    return "volatile byte loop";
#endif
}

TEST_SUITE("net.netcode_crypto") {
    TEST_CASE("netcode crypto: libsodium's CPU probe sees AVX2, and ChaCha20 dispatches to the AVX2 kernel") {
        REQUIRE(sodium_init() >= 0);
        const char* kernel = selectedKernel();
        MESSAGE("bundled libsodium: ChaCha20 " << kernel << ", sodium_memzero " << expectedMemzero()
                                               << ", has_avx2 " << sodium_runtime_has_avx2());
#if defined(__AVX2__) && defined(HELIOS_TEST_SODIUM_AVX2)
        // This image is built for AVX2 and runs only where the CPU and OS support it (the CPU gate checks
        // CPUID and XCR0 before main), so the probe must see AVX2 and the dispatch must pick its kernel.
        CHECK(sodium_runtime_has_avx2() == 1);
        CHECK(std::strcmp(kernel, "avx2") == 0);
#else
        CHECK(std::strcmp(kernel, "none") != 0);
#endif
    }

    TEST_CASE("netcode crypto: every compiled ChaCha20 kernel produces RFC 8439's keystream") {
        REQUIRE(sodium_init() >= 0);
        // RFC 8439 §2.4.2: key 00..1f, nonce 00 00 00 00 00 00 00 4a 00 00 00 00, initial counter 1.
        std::array<unsigned char, 32> key{};
        for (usize i = 0; i < key.size(); ++i) key[i] = static_cast<unsigned char>(i);
        const std::array<unsigned char, 12> nonce{0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0};
        const char plain[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the "
                             "future, sunscreen would be it.";
        constexpr usize kPlainBytes = sizeof(plain) - 1;
        static_assert(kPlainBytes == 114);
        const unsigned char expected[kPlainBytes] = {
            0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81,
            0xe9, 0x7e, 0x7a, 0xec, 0x1d, 0x43, 0x60, 0xc2, 0x0a, 0x27, 0xaf, 0xcc, 0xfd, 0x9f, 0xae, 0x0b,
            0xf9, 0x1b, 0x65, 0xc5, 0x52, 0x47, 0x33, 0xab, 0x8f, 0x59, 0x3d, 0xab, 0xcd, 0x62, 0xb3, 0x57,
            0x16, 0x39, 0xd6, 0x24, 0xe6, 0x51, 0x52, 0xab, 0x8f, 0x53, 0x0c, 0x35, 0x9f, 0x08, 0x61, 0xd8,
            0x07, 0xca, 0x0d, 0xbf, 0x50, 0x0d, 0x6a, 0x61, 0x56, 0xa3, 0x8e, 0x08, 0x8a, 0x22, 0xb6, 0x5e,
            0x52, 0xbc, 0x51, 0x4d, 0x16, 0xcc, 0xf8, 0x06, 0x81, 0x8c, 0xe9, 0x1a, 0xb7, 0x79, 0x37, 0x36,
            0x5a, 0xf9, 0x0b, 0xbf, 0x74, 0xa3, 0x5b, 0xe6, 0xb4, 0x0b, 0x8e, 0xed, 0xf2, 0x78, 0x5e, 0x42,
            0x87, 0x4d};
        // Lengths around the kernels' block sizes (64 B; 256 B for 4-way SSSE3; 512 B for 8-way AVX2) and the
        // datagram sizes HTP sends, so the wide loops, their tails and the single-block path all run.
        std::vector<unsigned char> message(4096);
        for (usize i = 0; i < message.size(); ++i) message[i] = static_cast<unsigned char>(i * 131 + 17);
        const usize lengths[] = {0, 1, 63, 64, 65, 114, 255, 256, 257, 511, 512, 513, 700, 730, 1200, 1300, 4096};

        usize ran = 0;
        for (const Kernel& k : compiledKernels()) {
            CAPTURE(k.name);
            if (!k.usable) {
                MESSAGE("ChaCha20 " << k.name << ": not supported by this CPU, skipped");
                continue;
            }
            ++ran;
            std::array<unsigned char, kPlainBytes> out{};
            REQUIRE(k.impl->stream_ietf_ext_xor_ic(out.data(), reinterpret_cast<const unsigned char*>(plain),
                                                    kPlainBytes, nonce.data(), 1, key.data()) == 0);
            CHECK(std::memcmp(out.data(), expected, kPlainBytes) == 0);
            for (const usize n : lengths) {
                CAPTURE(n);
                std::vector<unsigned char> ref(n + 1, 0), got(n + 1, 0);
                crypto_stream_chacha20_ref_implementation.stream_ietf_ext_xor_ic(ref.data(), message.data(), n,
                                                                                 nonce.data(), 7, key.data());
                k.impl->stream_ietf_ext_xor_ic(got.data(), message.data(), n, nonce.data(), 7, key.data());
                CHECK(ref == got); // includes the byte after the message, which must stay untouched
            }
        }
        CHECK(ran >= 1);
        // The public entry point (whatever it dispatches to) agrees with the reference kernel.
        std::array<unsigned char, kPlainBytes> out{};
        REQUIRE(crypto_stream_chacha20_ietf_xor_ic(out.data(), reinterpret_cast<const unsigned char*>(plain),
                                                   kPlainBytes, nonce.data(), 1, key.data()) == 0);
        CHECK(std::memcmp(out.data(), expected, kPlainBytes) == 0);
    }

    TEST_CASE("netcode crypto: sodium_memzero clears exactly the bytes it is given") {
        std::vector<u8> buf(4096 + 2, 0xAA);
        sodium_memzero(buf.data() + 1, 4096);
        CHECK(buf.front() == 0xAA);
        CHECK(buf.back() == 0xAA);
        CHECK(std::all_of(buf.begin() + 1, buf.end() - 1, [](u8 b) { return b == 0; }));
        sodium_memzero(buf.data(), 0); // a zero-length wipe is valid and touches nothing
        CHECK(buf.front() == 0xAA);
    }

    TEST_CASE("perf: netcode crypto: sodium_memzero wipes 64 KB at least 4x faster than a volatile byte loop") {
        // A volatile byte loop is what sodium_memzero falls back to without HAVE_EXPLICIT_BZERO (sodium.c); on
        // glibc it must now run at explicit_bzero's speed, and on Windows at SecureZeroMemory's (rep stosb).
        // Relative, best of 9 trials of 32 wipes each, so it holds on fast and slow CPUs alike.
        std::vector<u8> buf(64 * 1024, 0xAA);
        const auto timeBest = [&](auto&& wipe) {
            f64 best = 1e9;
            for (int trial = 0; trial < 9; ++trial) {
                const f64 t0 = monotonicSeconds();
                for (int i = 0; i < 32; ++i) wipe();
                best = std::min(best, (monotonicSeconds() - t0) / 32);
            }
            return best;
        };
        const f64 library = timeBest([&] { sodium_memzero(buf.data(), buf.size()); });
        const f64 byteLoop = timeBest([&] {
            volatile unsigned char* volatile p = buf.data();
            for (usize i = 0; i < buf.size(); ++i) p[i] = 0;
        });
        MESSAGE("sodium_memzero (" << expectedMemzero() << "): " << library * 1e6 << " us per 64 KB; volatile byte loop "
                                   << byteLoop * 1e6 << " us");
#if defined(_WIN32) || defined(__GLIBC__)
        CHECK(library * 4.0 <= byteLoop);
#endif
    }
}

} // namespace
