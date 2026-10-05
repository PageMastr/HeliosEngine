# netcode's bundled libsodium may use its own faster settings (repository owner's decision, 2026-10-05)

| | |
|---|---|
| Decision | **Enable it.** `tp_netcode`'s bundled libsodium subset is compiled with `HAVE_AVX_ASM` (GCC and Clang on x86-64) and `HAVE_EXPLICIT_BZERO` (where libc has `explicit_bzero`, i.e. glibc Linux), by compile definitions only |
| Decided by | The repository owner (the user), on 2026-10-05 (about 15:40 UTC) |
| Recorded by | The WP-0.13r implementer (Claude Code agent), on 2026-10-05, from the lead's task brief, which relays the question and the answer; the implementer did not see the exchange itself |
| Criterion served | **NS-0.2** (04 §11.4), "Loopback 100k pps/core without loss": headroom for the encrypted stack. The 100k threshold and the advisory scope of the 2026-09-30 approval ([`ns-0.2-owner-approval-2026-09-30.md`](ns-0.2-owner-approval-2026-09-30.md)) are unchanged |
| Applied in | `third_party/CMakeLists.txt` (`tp_netcode`), recorded in `third_party/MANIFEST.md` (netcode); WP-0.13r's PR |
| Not changed | Every vendored source file under `third_party/netcode/sodium/` (no patch); the ciphers, the tags and the wire format |

## The question and the answer

WP-0.13r's profile found two ways in which the bundled libsodium 1.0.22 subset runs slower than upstream
libsodium's own build on the same machine, and proposed fixing both by defining two of the configuration
macros that upstream's `configure` would set. Because the change alters how a cryptographic library runs, the
task brief required a decision instead of a silent change, and the lead asked the owner. As the lead's brief
records it, the question was:

> Should the networking code's built-in encryption library be allowed to use its faster built-in settings?

with the proposal "define HAVE_AVX_ASM and HAVE_EXPLICIT_BZERO for tp_netcode, no source edit, measured 164k vs
132k packets per core in the dev container", and the owner chose:

> Yes, enable it (Recommended)

The lead's brief adds the conditions this record and the PR follow: compile definitions only (`third_party`
stays unedited); `HAVE_AVX_ASM` only where the compiler supports the GNU inline `xgetbv` it guards, MSVC
already reading XCR0 through `_xgetbv`; `HAVE_EXPLICIT_BZERO` only where `explicit_bzero` exists, Windows
keeping `SecureZeroMemory`; a test that shows which ChaCha20 and `sodium_memzero` implementations are selected
and that the non-AVX2 kernels still work; and the ISA rules kept.

## What the two settings do

- **`HAVE_AVX_ASM`.** The subset compiles three ChaCha20 kernels (reference, SSSE3, AVX2) and two Poly1305
  kernels (donna, SSE2) and picks one of each at `sodium_init()` from its CPU probe. The probe reports AVX, and
  so AVX2, only after reading XCR0 (that the OS saves the YMM registers), which it does through `_xgetbv` under
  MSVC and through GNU inline assembly when `HAVE__XGETBV` or `HAVE_AVX_ASM` is defined. The amalgamation
  defines neither, so every GCC and Clang build (the Linux nightlies, MinGW, clang-cl) kept the SSSE3 ChaCha20
  on AVX2 hardware. With the definition the probe reads XCR0 and ChaCha20 dispatches to the AVX2 kernel; a CPU
  or OS without AVX2 still gets the SSSE3 or reference kernel, as before. MSVC builds are unaffected (`cl`
  has no GNU inline assembly and reads XCR0 already).
- **`HAVE_EXPLICIT_BZERO`.** `sodium_memzero`, which wipes keys and cipher state after every encryption and
  decryption, prefers `SecureZeroMemory` on Windows, then `memset_s`, `explicit_bzero`, `explicit_memset`, a
  weak-symbol `memset`, and last a loop that stores one byte at a time through a volatile pointer. Linux builds
  had none of the earlier options defined and ran the byte loop. With the definition they call glibc's
  `explicit_bzero`, which clears the same bytes and, like the loop, cannot be optimized away.

Neither changes what is computed: every ChaCha20 kernel produces the same keystream, and the wipe clears the
same bytes.

## The measurements behind it

Dev VM (shared 4-vCPU, GCC 13 RelWithDebInfo), `net_bench --stack 3`, the encrypted HTP stack's packets per
core; variants interleaved run by run so that the VM's varying load hits each the same way (5 runs each,
2026-10-05 14:59 UTC, load average about 1.3–1.7):

| Build | Runs | Median |
|---|---|---|
| main plus netcode patch `0001-write-bytes-memcpy` (this PR without the definitions) | 139,319 132,153 133,807 129,939 120,054 | 132,153 |
| the same plus `HAVE_AVX_ASM` and `HAVE_EXPLICIT_BZERO` | 172,168 146,490 175,680 150,935 163,980 | 163,980 |
| the same plus both, with Poly1305 forced to donna32 (what MSVC builds run, having no `__int128`) | 149,049 158,435 152,627 135,037 141,772 | 149,049 |

`perf record` of the defined build shows `chacha20_encrypt_bytes__avx2` in place of the SSSE3 kernel and no
`sodium_memzero` time; together the two definitions save about 1.5 µs of the 10 µs budget per packet here.
The 10-minute `net_bench --gate` runs before and after WP-0.13r are in its PR and in `engine/net/README.md`.

## What this does not decide

- Poly1305 on MSVC builds: without `__int128` they compile neither the donna64 nor the SSE2 Poly1305 and run
  the portable donna32 kernel (about 0.6 µs more per packet in the third row above). Changing that would need
  a source patch or a different Windows compiler, and is not part of this decision.
- Any new cryptographic code or dependency: none is added.
- NS-0.2's threshold, its advisory scope, or NS-0.7.
