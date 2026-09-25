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

## Conventions

* Errors cross module boundaries as `helios::Result<T>`; no exceptions on hot paths.
* Paths: `std::filesystem::path`; convert UTF-8 with `fs::pathFromUtf8` / `fs::pathToUtf8`.
* Everything that must be bit-identical between Windows and Linux (RNG, hashing, `DilatableClock`)
  is integer- or bit-exact and pinned by golden tests.
* Asserts are enabled in Debug and RelWithDebInfo (`HELIOS_ENABLE_ASSERTS`), compiled out of Release.

## Tests

`core_tests` (doctest, `tests/*.cpp`) covers every header, including job-system stress tests
(1M jobs, nested waits, random DAGs), a real crash in a child process (`tests/support/crash_child.cpp`)
and loading a plugin DLL/.so (`tests/support/test_plugin.cpp`).

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
* The job system uses lock-protected deques (short spin locks) rather than lock-free Chase-Lev
  deques; fibers are a Phase 2+ option behind the same API (ADR-011).
