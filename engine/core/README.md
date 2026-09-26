# engine/core — Helios L1 core runtime

`helios::core` (target `helios_core`) is the bottom layer of the engine DAG. It is HEADLESS (linked by
cell servers, tools, client and editor) and depends only on the C++ standard library, the OS, and
vendored xxHash (hashing) and mimalloc (heap backing). Public headers live in
`include/helios/core/`; all OS-specific code is in `src/platform/win32/` (MSVC, clang-cl, MinGW)
and `src/platform/posix/` behind the internal interface `src/platform/os.h`.

| Header | What it provides |
|---|---|
| `platform.h` | `HELIOS_PLATFORM_*`, `HELIOS_COMPILER_*`, `HELIOS_ARCH_*` (defined to 1 only when true), `HELIOS_FORCEINLINE/NOINLINE/LIKELY/UNLIKELY/ASSUME/DEBUG_BREAK`, export macros, `helios::platform::k*` constants |
| `types.h` | `u8..u64`, `i8..i64`, `f32/f64`, `usize/isize`, alignment, byte-order and flag-enum helpers |
| `log.h` | `HELIOS_LOG_{TRACE,DEBUG,INFO,WARN,ERROR,FATAL}([channel,] fmt, ...)`, channels, sinks (console, file, ring buffer, debugger, callback), compile-time stripping |
| `assert.h` | `HELIOS_ASSERT`, `HELIOS_VERIFY`, `HELIOS_UNREACHABLE`, overridable handler |
| `result.h` | `Result<T>` / `Result<void>`, `Error{ErrorCode, message}`, `HELIOS_TRY`, `HELIOS_TRY_ASSIGN` |
| `hash.h` | XXH3-64/128 (+ streaming), constexpr FNV-1a, `hashCombine`, `mix64` |
| `name.h` | `Name` (interned string, 32-bit id), `StringId` (constexpr 64-bit hash + debug registry) |
| `handle.h` | `Handle<Tag>` (index + generation), `HandlePool<T>` slot map with stale-handle detection |
| `memory.h` | Memory tags with budgets, tracked `alignedAlloc`, `Allocator`, `LinearAllocator`, `FrameAllocator`, `PoolAllocator`, `VirtualMemory` |
| `time.h` | Monotonic clock, `Stopwatch`, sleeps, `DilatableClock` (fixed-step game clock with time dilation) |
| `thread.h` | `Thread` (named, pinnable), `SpinLock`, `Semaphore`, `ManualResetEvent`, lock aliases |
| `jobs.h` | `JobSystem` (work stealing, priorities, helping waits), `Counter`, `parallelFor`, `TaskGraph`, `BackgroundPool` |
| `fs.h` | Whole-file I/O (atomic replace), `File`, `MappedFile`, directory listing, `FileWatcher` |
| `vfs.h` | `Vfs` mount table (`/content`, `/cache`, `/saved`), layered priorities, sandboxed paths, `IMountProvider` |
| `dynlib.h` | `DynamicLibrary` (LoadLibraryExW / dlopen) |
| `cvar.h` | Console variables (`HELIOS_CVAR`), commands, `execute("set r.vsync 0")`, flags, callbacks |
| `cmdline.h` | `-key=value`, `--flag`, positionals; MSVC-CRT-exact Windows command-line splitting |
| `guid.h` | Random v4 `Guid`, canonical text, ordering, hashing |
| `random.h` | `SplitMix64`, `Pcg32`, `Xoshiro256`, exact portable distributions, `Random` |
| `utf.h` | UTF-8 ⇄ UTF-16/UTF-32/wide, validation, U+FFFD replacement |
| `containers.h` | `SmallVector`, `RingBuffer`, bounded `MpmcQueue`, `SpscQueue` |
| `crash.h` | Minidumps (Windows) / signal backtraces (POSIX), on-demand reports |
| `version.h` | Version constants and build info (compiler, config, git hash via `-DHELIOS_GIT_HASH`) |
| `cpu.h` | CPU gate: CPUID/XGETBV feature detection (AVX2, FMA, BMI1/2, F16C, LZCNT, POPCNT, AVX-512, OS YMM/ZMM state), `cpuGate()` report + message, synthetic `CpuidSnapshot` evaluation |
| `process.h` | `Process::spawn` (CreateProcessW / posix_spawn): UTF-8 args, env, working dir, stdio pipes/null/inherit, inherit-handle whitelist, `wait(timeout)`, `kill()`, `communicate()`; `Pipe`/`PipeEnd`; Windows argument quoting |
| `async_io.h` | `fs::readFileAsync` / `fs::readAsync` on a `BackgroundPool`: `AsyncRead` handle (poll, counter, callback, cancel); IORing / io_uring plan |

## Threading rules

* **Free functions are thread-safe** unless their doc comment says otherwise. Objects are owned by
  one thread unless the class comment says it is thread-safe (e.g. `MpmcQueue`, `FrameAllocator::allocate`,
  `Vfs`, `CVarRegistry`, the logger).
* **Never block a job-system worker on an OS primitive** waiting for other jobs; use a `jobs::Counter`
  and `JobSystem::wait()`, which executes other jobs while waiting. Blocking work (file I/O, shader
  compiles, pathfinding) goes to a `BackgroundPool`.
* A job that waits must not depend on work produced only *after* the wait of a job lower on the same
  thread's stack (help-while-waiting without fibers). Jobs must not throw.
* Helping nests on the waiting thread's stack. Past ~256 KiB (or 256 levels) of nested waits a
  thread only runs jobs spawned by the job it waits in, so thousands of jobs waiting on one counter
  cannot overflow a 1 MiB Windows thread stack (`JobSystemStats::limitedWaits` counts this).
* `BackgroundPool::wait` may be used with counters shared with `JobSystem` jobs.
* `Counter`s must outlive the jobs that reference them. Destroying a `JobSystem` drains queued work;
  nobody may submit concurrently with destruction. `waitIdle()` is for non-job threads only.
* `HandlePool`, `SmallVector`, `RingBuffer`, `LinearAllocator`, `FileWatcher` and `DilatableClock`
  are single-threaded. `FrameAllocator::beginFrame()` runs between frames with no allocation in flight.
* Log sinks and assert handlers are called concurrently from any thread; they must not log/assert
  recursively (re-entrant log calls are dropped).
* CVar change callbacks run on the thread that changed the value, outside registry locks.

## CPU gate (02 §1.1, 08 §2.2)

`cpuGate()` checks the running CPU against what every AVX2 image needs (x86-64-v2 + AVX, AVX2,
BMI1, BMI2, F16C, LZCNT and OS-enabled YMM state; FMA is reported, never required). The probe is the
C unit `src/cpugate/cpu_gate.c`, compiled at the x86-64-v1 baseline with no libc calls, so it runs
on any x86-64 CPU and before the C runtime. `helios_cpu_gate(<exe>)` (applied by
`helios_executable()` to client, cell, gateway, voice, editor, bot and tool images) links the
per-OS pre-initializer `src/platform/{posix,win32}/cpu_gate_hook.c`: an ELF `.preinit_array` entry
or a `.CRT$XIB` C initializer, which runs before any C++ initializer, prints

> Helios requires an AVX2 CPU (Intel Haswell / AMD Excavator or newer). Detected: <brand>. Missing: AVX2, BMI2.

to stderr (a message box when there is no console) and exits with **78** (`kCpuGateExitCode`). On a
supported CPU it installs a SIGILL / `STATUS_ILLEGAL_INSTRUCTION` backstop with the same kind of
message; core's crash handler replaces it once installed. Deliberate traps (`ud2`/`ud1`/`ud0`:
`__builtin_trap`, clang-cl's trap-on-unreachable, sanitizer traps) are passed through, so they still
crash normally and reach the crash handler instead of being reported as an unsupported CPU. The gate
TUs are built with `helios_cpu_gate_sources()`: x86-64-v1, no stack protector (`/GS-`,
`-fno-stack-protector`) and no sanitizer instrumentation, because they run before those runtimes. The ISA audit (`tools/lint`) checks the
gate objects' flags, disassembly and symbols on every build; `core_cpugate_child_snb` (the hook
evaluating a recorded Sandy Bridge) tests the refusal path on any machine (CL-17, early).

## Processes

`Process::spawn` runs a program with UTF-8 arguments (quoted by `quoteWindowsArgument` so the MSVC CRT
splits them back exactly), an optional working directory, environment overrides or a clean
environment, and per-stream stdio (inherit, null, pipe). Only the standard streams and the handles in
`ProcessDesc::inheritHandles` reach the child: `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` on Windows; on
POSIX close-on-exec for Helios descriptors plus, in the child, a close of every other descriptor above
2 (gap closes and `posix_spawn_file_actions_addclosefrom_np`, glibc >= 2.34; `POSIX_SPAWN_CLOEXEC_DEFAULT`
on macOS), so sockets or `fopen` files that third-party code opened without close-on-exec do not leak
either. That whitelist is the launcher's launch-code channel (WP-1.21): create a `Pipe`, list one end,
pass its value on the command line, `PipeEnd::adopt()` it in the child. `wait(timeout)`, `kill()`
(exit code 137 everywhere) and `communicate()` (threaded, no pipe deadlocks) complete it. Inputs are
validated for untrusted callers: arguments and environment values with NUL bytes and variable names
that are empty or contain `=` are refused (`InvalidArgument`), and so are Windows batch files
(`.bat`/`.cmd`, even with trailing dots or spaces), which `CreateProcessW` runs through cmd.exe where
no quoting is safe. A relative program path is relative to the parent's working directory on every
platform, also when `workingDirectory` is set. POSIX needs glibc >= 2.29 / musl >= 1.1.24 for
`posix_spawn_file_actions_addchdir_np` and close-on-exec clearing by `adddup2(fd, fd)`.

## Asynchronous reads

`fs::readFileAsync(pool, path, {offset, size})` and `fs::readAsync(pool, file, offset, dst)` queue
positional reads on an IO `BackgroundPool` and return an `AsyncRead`: poll `isReady()`, wait on its
`counter()` with `JobSystem::wait` (which helps), or pass an `onComplete` callback (runs on the IO
thread; `take()`/`bytesRead()` work inside it). A request completes only once its callback has returned:
`isReady()`, the counter, `wait()`, `take()` and `bytesRead()` all report that same moment, so a thread
that saw a read complete also sees the callback's effects and may free what the callback used (such as
the destination memory). Queued requests can be cancelled. Fire-and-forget is
safe: the request's job owns its state until the counter is released, so dropping every `AsyncRead`
right away is fine. Phase 0 issues one blocking `pread`/`ReadFile` per
request; the Phase 3 backends (Windows 11 IORing, Linux io_uring with fixed files and buffers, batched
per streaming tick, pool fallback when unavailable) keep the same request shape and completion
counter, so callers do not change.

## Memory-tag accounting at scale

Tag counters are sharded per thread (16 shards, assigned round-robin on a thread's first
allocation), so threads allocating under one tag no longer bounce one cache line. Live bytes and
allocation counts stay exact (readers sum the shards); the peak and budget crossings are exact for a
tag used by one thread, and within 16 × 64 KiB when several threads race. The peak is written only
when it rises. `trackAllocations(tag, bytes, count)` / `trackDeallocations` account a batch in one
call (pools, arenas, a heap flushing a thread-local tally). Budget: <= 25 ns per tracked
allocate+free pair per thread, flat from 1 to 8 threads. `core_memory_bench` measures it against a
replica of the old single-slot counters (4-core container, ns per pair, best of 3):

| Threads | Old counters | Sharded | `alignedAlloc` + free (64 B) |
|---|---|---|---|
| 1 | 29 ns | 29 ns | 41 ns |
| 2 | 368 ns | 30 ns | 41 ns |
| 4 | 866 ns | 31 ns | 42 ns |

Before the change, `alignedAlloc` + free cost 42 / 94 / 230 ns at 1 / 2 / 4 threads (same machine).
`core_memory_bench --gate` fails unless sharded tracking is >= 2x faster than the old counters at
>= 4 threads (not part of the default CTest run, which only checks that the accounting balances).

## Conventions

* Errors cross module boundaries as `helios::Result<T>`; no exceptions on hot paths.
* Paths: `std::filesystem::path`; convert UTF-8 with `fs::pathFromUtf8` / `fs::pathToUtf8`.
* Everything that must be bit-identical between Windows and Linux (RNG, hashing, `DilatableClock`)
  is integer- or bit-exact and pinned by golden tests.
* Asserts are enabled in Debug and RelWithDebInfo (`HELIOS_ENABLE_ASSERTS`), compiled out of Release.

## Tests

`core_tests` (doctest, `tests/*.cpp`) covers every header, including job-system stress tests
(1M jobs, nested waits, random DAGs), a real crash in a child process (`tests/support/crash_child.cpp`),
loading a plugin DLL/.so (`tests/support/test_plugin.cpp`), processes against
`tests/support/process_child.cpp` (argument round trips, pipes, env, cwd, kill, inherited-handle
whitelist) and the CPU gate's pre-initializer against `tests/support/cpugate_child.cpp`.

```
cmake -S . -B build/core -G Ninja -DHELIOS_BUILD_GRAPHICS=OFF
ninja -C build/core helios_core core_tests && ctest --test-dir build/core -R core_tests --output-on-failure
```

## Known limitations

* Thread affinity covers the first 64 logical CPUs (no Windows processor groups yet).
* POSIX stack-overflow reports only work on the thread that installed the crash handler (alternate
  signal stacks are per thread). Backtraces need glibc (`<execinfo.h>`).
* The polling file watcher compares size + mtime, so same-size edits within the filesystem's
  timestamp granularity can be missed. `Vfs::watch` only covers native-directory mounts present at
  the time of the call.
* `DirectoryMount` rejects path components Win32 would not treat as plain names (DOS devices such
  as `con` / `nul.json`, names ending in `.` or space) on every platform, so content resolves the
  same on Windows and Linux.
* Windows crash handling covers SEH exceptions (unhandled-exception filter) and every `abort()`
  (failed asserts, `HELIOS_LOG_FATAL`, `std::terminate` on any thread) through a process-wide
  SIGABRT hook; MSVC pure-call / invalid-parameter failures are hooked too. `__fastfail` paths
  (e.g. /GS buffer overruns) cannot be intercepted in-process.
* `File::readAt` moves the file position on Windows (positional reads use OVERLAPPED offsets).
* The CPU gate's message box and localized messages (08 §2.1.1) are the bootstrap's job; the core
  hook prints English text to stderr (message box only when there is no console). The gate's
  display name is "Helios" until product stamping (08 §2.10) lands.
* `Process`: POSIX `wait(timeout)` polls `waitpid` with a 50 µs–5 ms backoff (no pidfd yet); a
  Process destroyed without `wait()` leaves a zombie until the parent exits. Handles listed in
  `inheritHandles` are made inheritable for the duration of `spawn()` only; third-party code that
  calls `CreateProcess` with `bInheritHandles` and no handle list at that moment could see them.
* Async reads block one IO thread per in-flight request until the IORing / io_uring backends land.
* The job system uses lock-protected deques (short spin locks) rather than lock-free Chase-Lev
  deques; fibers are a Phase 2+ option behind the same API (ADR-011).

## Plan conformance

Plan-Rev: 3

The CPU-gate code was written to plan revision 3, before the ADR-011 amendment (revision 4). Open deltas are in
09 §5.10.4 (b), for WP-0.5r: gate placement, the failure path, the exports, and the round-5 backstop
classifier and crash-handler handover. §5.10.4 (c) has one more, for WP-0.5: 02 §2.2's ≤ 3× `mi_malloc`
accounting target. The rest of the module has no open delta.
