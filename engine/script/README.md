# engine/script — Luau scripting host (WP-0.10, 02 §7.4)

`helios::script` (target `helios_script`, HEADLESS, L3) hosts sandboxed [Luau](../../third_party/luau)
0.739 VMs: one `lua_State` per zone instance on cells, one for the client and one for the editor.
It implements the normative parts of 02 §7.4, 04 §3.1 / §10.2 and 06 §11: fuel-metered budgets with
**no involuntary yields**, sticky kills, `task.checkpoint`, typed `StaleHandle` errors, heap caps,
a coroutine scheduler on the dilatable zone clock, modules with hot reload, and the Phase 0
hand-written binding layer (schema-generated `@script` bindings replace the registration code in
WP-1.6). Depends on `helios::core`, `helios::math` and `helios::tp::luau`, including two vendored Luau
patches that fuel metering needs (below; `third_party/MANIFEST.md`, "Patches").

| Header (`helios/script/…`) | Contents |
|---|---|
| `vm.h` | `ScriptVm`, `VmConfig`, `ModuleOptions`, `ModuleInfo`: creation, modules, tasks, ticks, async completion, diagnostics |
| `binding.h` | `Binder` (functions, async functions, object types, methods, properties), `FuelCost` charging, `checkObject`/`pushObject`, `WorldPos` push/check, `callLuau`, `beginAsync`/`yieldAsync`, `raiseError` |
| `compiler.h` | `compile()` (luau_compile), `CompileOptions`, `BytecodeCache`, `wrappedBuiltins()` / `disabledBuiltins()` |
| `types.h` | `TaskId`, `ScriptError`/`ScriptErrorCode`, `KillReason`, `YieldReason`, `TaskState`, `FuelBudget`, `FuelCost`, events, `TickStats`, `VmStats` |
| `script.h` | umbrella |
| [`defs/helios.d.luau`](defs/helios.d.luau) | Luau type definitions of the built-in API (luau-lsp / Luau.Analysis) |

```cpp
using namespace helios::script;
VmConfig cfg;                         // Cell profile: deterministic fuel budgets
cfg.clock = &zoneClock;               // DilatableClock: wait() runs on zone time (TiDi applies)
auto vm = ScriptVm::create(cfg, [&](Binder& b) {
    ObjectType ship = b.poolType("Ship", shipPool);                 // HandlePool<Ship>
    b.property(ship, "hull", &shipHull, FuelCost{2});
    b.function("Ship", "boost", &shipBoost, FuelCost{10, 50, 2});   // 10 + 0.05/item of arg 2
    b.asyncFunction("Market", "quote", &marketQuote, FuelCost{5}); // yields until completeAsync
}).value();
vm->loadModule("door", source);       // compiled through the (shareable) bytecode cache
vm->spawnScript("door", doorEntityId);
// every zone tick, after the clock stepped:
TickStats ts = vm->tick();            // wakes waits, resumes tasks while lane fuel remains
```

## Sandbox (02 §7.4, 06 §11)

* Setup order: `luaL_openlibs` → Helios API (built-ins, charging wrappers, then the registrar) →
  `luaL_sandbox` (globals and libraries frozen, safeenv). Every module instance gets its own
  environment through `luaL_sandboxthread`: writes go to a private table, reads fall through to the
  frozen globals.
* Absent: `io`, `os`, `debug`, `loadstring`/`load`/`dofile`, `getfenv`/`setfenv`, `math.randomseed`.
  On cells also `gcinfo` (Luau 0.739 has no `collectgarbage`), and `setmetatable` rejects `__mode`
  (weak tables would make GC timing observable). `math.random` draws from a seeded, VM-wide
  deterministic stream (`VmConfig::randomSeed`).
* `print` goes to the `Script` log channel and `VmConfig::onPrint`. A line keeps at most 4 KiB (the
  rest is summarized as `... (N bytes truncated)`), is charged 100 fuel plus 1 per 64 bytes of its
  string arguments (untruncated), so scripts cannot flood the host log or heap.
* Typed errors are immutable `ScriptError` userdata (`code`, `message`, `reason`); scripts can catch
  and inspect them but never construct or forge one. Userdata metatables are locked (`__metatable`).
* `coroutine.resume`/`close` refuse task coroutines (only the scheduler drives them).
  `coroutine.running()` returns nil on the main thread, so callbacks cannot leak it either.

## Budgets and kills (RT-13)

* **Fuel.** Each `gc < 0` safepoint (loop back-edges, calls, returns, pattern-matcher steps) costs
  1 fuel. The VM counts them itself, through the vendored `fuel-counter` patch: an inline counter
  (`lua_fuelcounter`) that every safepoint decrements, in the interpreter and in native code, and that
  calls the `interrupt` callback only when it reaches zero. The host arms it with the fuel left until its
  next decision point (the kill, the soft budget, the next wall-clock read), re-arms it after each one,
  and binding charges subtract from it, so the fuel counted is exactly what calling the host at every
  safepoint counted, at a decrement and a branch per safepoint. With no limit the host is never called;
  with wall limits, once per 64 fuel. Every resume and top-level run starts at
  `FuelBudget::resumeCost` (16 fuel, ≈ the measured ~200 ns of a trivial resume), so a lane of tasks
  that only yield cannot resume far more tasks per tick than its fuel pays for.
  Every Luau-callable C++ function registered through `Binder` runs behind a trampoline that
  charges its `FuelCost` (`base + ceil(items × perItemMilli / 1000)`) **before** the call. The
  sandbox wraps the builtins whose C work grows with their input: 02 §7.4's list
  (`string.rep/format/gsub/find/match/gmatch/split`, `table.concat/sort/move/create/clone/find`,
  `buffer.fill/copy`; `table.sort` is charged n·⌈log₂ n⌉ up front) plus every other such builtin in
  Luau 0.739 (`string.sub/upper/lower/reverse/pack/unpack`, positional `table.insert`/`table.remove`,
  `table.clear/maxn`, `buffer.create/fromstring/tostring/readstring/writestring`,
  `utf8.len/offset`). Pure builtins that produce large strings (`string.gsub/sub/pack/unpack`,
  `table.concat`, `buffer.tostring/readstring`) are also charged per result byte right after the
  call (02 §7.4's `of=result` rule: no side effect, deterministic). Charges never depend on
  `tostring` of a table or userdata (its heap address has a run- and platform-dependent length):
  `string.format` and `print` charge per byte of their string arguments instead. All wrappers are compiled as
  `disabledBuiltins` so no FASTCALL bypasses a charge, except `table.insert`, whose FASTCALL only
  covers the O(1) append and falls back to the charging wrapper for positional inserts
  (`fastcallWrappedBuiltins()`). `tostring`, `string.char` and `string.sub` are compiled as plain
  calls anyway: their FASTCALL paths fall back to a counted call when a GC step is due, which would
  make fuel depend on GC pacing. On cells pattern subjects are capped at 64 KiB. Costs are
  placeholders until `--calibrate-fuel`.
* **No involuntary yields.** The interrupt never yields: past `fuelPerResume` the resume is flagged
  (`ScriptOverBudget` event) and `task.checkpoint()` starts yielding; at `fuelKill` the resume is
  killed. Scripts yield only at `wait`, `task.wait/yield/checkpoint`, `coroutine.yield` and async
  bindings; each yield is recorded (`VmStats::yieldsByReason`) and `VmStats::involuntaryYields`
  counts any yield the scheduler could not attribute (always 0). `wait`/async calls raise
  `InvalidState` where yielding is impossible (metamethod, sort comparator, callback, module load,
  nested coroutine); `task.checkpoint()` returns `false` there.
* **Kills are sticky.** The kill raises a pre-built `ScriptError{code="Killed"}` (no allocation);
  the run is marked killed and every later safepoint, binding call or charge raises again, so
  `pcall`, `xpcall` handlers, nested coroutines and bindings (`callLuau`, nested `callExport`)
  cannot swallow it. Luau is built with C++ exceptions, so binding frames unwind through RAII (no
  lock stays held). When the resume returns the scheduler closes the coroutine
  (`lua_resetthread`) and emits `TaskKilled` (ScriptKilled telemetry). The resume is charged exactly
  `fuelKill`; a charge that trips the kill has no side effect, and a charge the sticky kill refuses
  leaves the run's fuel unchanged, even when it saturates (a `pcall` that swallowed the kill, then a
  binding reached through `__index` with no safepoint in between: WP-0.10 subtracted the whole
  saturated charge and reported 0 fuel for such a cell kill). Fuel and the lane and VM totals saturate
  instead of wrapping. Three kills of a module within
  `killWindowNanos` of zone time disable it (tasks cancelled, spawns refused) until
  `enableModule`/`reloadModule`.
* **Wall time.** Cells: a 20 ms backstop (fault path, `KillReason::WallBackstop`). Clients/editor
  (`FuelBudget::client()`): 2 ms soft, 5 ms kill, 1 ms lane per tick, all wall time; fuel is still
  counted for the profiler. The clock is read every 64 fuel, before every binding call, and at the
  first safepoint after every GC step: a single slow operation (a multi-MB concatenation costs one
  safepoint) is therefore caught at the next safepoint, not 64 safepoints later. The GC-step hook
  only schedules the read (it re-arms the counter at distance 0); it never raises and never changes
  fuel.
* **Lane.** `tick()` resumes ready tasks while lane fuel `< fuelPerTick`, so a tick spends at most
  `fuelPerTick + fuelKill`; the rest are deferred with their original wake tick.

## Scheduler

* Tasks are `lua_newthread` coroutines: `spawnScript` (a module chunk with a fresh environment; its
  top level may wait), `spawnExport` (an exported function) and `task.spawn` from scripts. New and
  woken tasks run on the next `tick()`, in **(wake tick, owner EntityId, sequence)** order.
* `wait(s)` sleeps `s` seconds of **zone time** (`DilatableClock::gameTimeNanos`, so time dilation
  stretches it in wall time) and returns the elapsed zone seconds; `wait(0)`/`task.yield()` resume
  on the next tick.
* Async host calls: an async binding calls `beginAsync(L)`, hands the `AsyncToken` to the
  operation and `return yieldAsync(L)`. `ScriptVm::completeAsync(token, pushResults)` or
  `failAsync(token, message)` makes the task ready; its continuation pushes the results (or raises
  `HostError`) inside the protected resume. The host may also complete the token synchronously,
  inside the binding before it yields (a cache hit): the yield then makes the task ready for the
  next tick instead of leaving it awaiting. A token completes at most once; tokens of cancelled
  tasks go stale.
* `cancelTask`/`task.cancel` close a non-running task. `TaskId`s are generational. Queue entries left
  behind by cancelled or rescheduled tasks are compacted once they outnumber the live tasks
  (`VmStats::queuedEntries`), so cancelled `wait(1e9)`s do not pile up.
* 10k idle tasks cost ≈ 1.37 KB each (≈ 13.8 MB peak for 10k in a 16 MB VM, tested).

## Handles and WorldPos

Object types are tagged userdata (`lua_newuserdatataggedwithmetatable`, tags 32–127) holding a
64-bit generational handle. Every property access and every method's `checkObject` resolves the
handle through the type's `ResolveFn`; a destroyed or recycled object raises `StaleHandle` instead
of touching memory. `obj:isValid()` revalidates without raising (06 §11 rule 6: revalidate after
every yield). `WorldPos` is frame-local f64 userdata (`FramePos`): `x/y/z/frame`, `+ - * /`,
`distance/length/offset/lerp`; never a float `vector` (ADR-005).

## Modules and hot reload

`require(name)` runs a module's chunk once (non-yielding, fuel charged to the caller) and caches
its exports; cycles, missing modules and disabled modules (even ones required before) raise typed
errors (`NotFound`, `ModuleDisabled`). `reloadModule(name, source)` has
**re-require semantics**: it compiles the new source (a failure keeps the old version), re-runs
the chunk in a fresh environment if the module was instantiated, calls `new.__reload(oldExports)`
for state migration (a failure there also keeps the old version), then points the registry at the
new exports — later `require`, `callExport`, `spawnExport` and `spawnScript` use the new code, while
tables captured earlier and coroutines already running finish on the old code. Reloads happen only
between resumes (tick boundary), bump `ModuleInfo::version` and clear the module's kill history.

Errors carry stack traces mapped to module names: chunks are named `@<module>`, so messages read
`door:12: …` and `ScriptError::stack` lists `{source, line, function}` frames (innermost first),
captured from the dead coroutine or, for callbacks, by an error handler at the error point.

## Memory

A custom `lua_Alloc` routes every block through `alignedAlloc` under the VM's memory tag (the
shared `Script` tag by default) and enforces `heapLimitBytes` as a hard cap (16 MB small / 256 MB
large zones). Each module gets its own Luau memory category (`lua_setmemcat`, 254 dedicated + one
overflow category); `moduleHeapLimitBytes` caps a category at allocation-page granularity. The
allocator cannot see which thread allocates, so the runtime keeps every running thread's category
equal to the one it checks (`VmState::activeMemcat`): task threads carry their module's category
(including `task.spawn` children spawned inside another module's `require`), module top levels run
in their own category, and `coroutine.resume` attributes a nested coroutine to its resumer. A failed
allocation raises a Luau memory error in the script; afterwards the host runs a full GC (Luau has no
emergency collection). A host-level call that fails for memory collects garbage and reports
`OutOfMemory` without retrying (setup and spawn bodies run host callbacks that must not run twice).
Shrinks never fail.

## Compilation and native code

`compile()` wraps `luau_compile` (optimization/debug/type-info/coverage levels, the disabled
builtin list); `BytecodeCache` (thread-safe, shareable between VMs of one content version) keys
bytecode by the XXH3-128 of the source plus the options fingerprint. Native codegen is opt-in on
clients and editors: `VmConfig::enableNativeCodegen` (off by default), gated by
`luau_codegen_supported()`, then per module with `ModuleOptions::native`. **Cells and world-script
hosts (`HostProfile::Cell`) refuse it**: `create()` fails with `InvalidArgument` on every target (02 §7.4).
Native code counts the same fuel as the interpreter and a kill stops it at the same program point,
because the vendored `codegen-fornloop-fuel` patch emits the numeric-`for` interrupt in `FORNLOOP` as the
interpreter does (stock Luau 0.739 put it at the top of the loop body, one body earlier, so a loop left by
`break` or `return` cost one extra fuel). That patch is the precondition for native code on cells, not
the permission: lifting the refusal is 02 §8.1's P3 "codegen opt-in on cells" item, behind 04 §10.2's
interpreter-versus-native run over the script corpus.

### Vendored Luau patches

`third_party/luau/patches/` (applied by `tools/vendor/fetch_third_party.sh`, checked by the
`lint_vendor_patches` CTest; `src/vm.cpp` also fails to compile without `LUA_FUELCOUNTER`):

| Patch | Effect here | Tests |
|---|---|---|
| `0001-codegen-fornloop-fuel` | Native code reaches the interpreter's safepoints at the same program points, so fuel and kill positions are identical in both (RT-13) | `determinism: numeric for loops left early count the same fuel in native code` (stock 0.739: +80 fuel), `luau patches: codegen-fornloop-fuel …` (safepoint counts; a kill at every safepoint k stops both modes at one point) |
| `0002-fuel-counter` | The inline counter above: the host runs only at decision points | `luau patches: fuel-counter …` (one decrement per safepoint in the VM, pattern matcher and native code), `fuel: the inline counter counts exactly what per-safepoint counting did`, `fuel: the host runs only at decision points`, `perf: fuel metering overhead and ns per fuel` (≤ 10 %) |

Both are inputs of `sim_abi.script` (04 §6.7) with the planned `det-math` patch; `sim_abi` is computed by
WP-3.1, from the patch list in `third_party/MANIFEST.md`. They are rebased on every Luau bump (K10).

## Threading rules

* A `ScriptVm` is **owned by one job at a time**. It is not thread-safe: all members, bindings and
  sinks run on the owner's thread; ownership may move between threads only between calls. A
  development-build assert catches concurrent use.
* Bindings run on the owner thread inside a resume. They may call `ScriptVm` APIs (nested
  `callExport` shares the running resume's budget; `spawn*` schedules for a later tick) but never
  `tick`, `reloadModule` or `instantiateModule`, and must hold no locks while calling back into Luau.
  Call back into Luau only through `callLuau` or nested `callExport`: both run the callee in a
  non-yieldable frame, so `wait()` there raises `InvalidState` even from an async binding (whose
  continuation would otherwise let the callee yield the task through the binding's C++ frame). A raw
  `lua_call`/`lua_pcall` from an async binding is not allowed.
* Async completions produced on other threads (services, I/O) must be marshalled to the owner (the
  zone's SimInbox) before `completeAsync`/`failAsync`.
* `ScriptEventSink`/`PrintSink` run synchronously on the owner thread (event sinks can run inside
  the interrupt callback); they must not call back into the VM or throw.
* `compile()` is pure and thread-safe; `BytecodeCache` is internally synchronized.

## Tests

`script_tests` (doctest, `tests/*.cpp`, 97 cases): sandbox escapes; kills at `fuelKill` inside a
metamethod, a `table.sort` comparator and C++→Luau callbacks (RAII/lock release, VM usable
afterwards); sticky kills through `pcall`/`xpcall`/coroutines/bindings; instrumented yields;
`task.checkpoint`; lane bound; binding and builtin charges; wall budgets and backstop; the
three-kills rule; scheduler ordering; zone-time `wait` under dilation; async calls; cancellation;
10k tasks in 16 MB; `StaleHandle` across a `wait` and slot recycling; `WorldPos` precision at
10¹³ m; heap and module caps; hot reload; stack traces; determinism (golden fuel count, fuel
independent of GC pacing, interpreter vs native fuel, including loops left early); `.d.luau` parse + API
coverage; the vendored Luau patches at the API level (one decrement per safepoint, host calls only at
zero, the helpers' counter reset, kill positions interpreter vs native); the inline counter's
bookkeeping (fuel identical with and without clock reads, around GC-forced reads, cheap bindings,
charges landing exactly on a decision point, top-level runs, sticky re-raises, and saturating charges
before and after a kill, in the safepoint and charge slow paths and the task, lane and VM totals; and
the golden count under the production cell budgets); the refused cell codegen config; fuel-metering
overhead (≤ 10 %, `perf:`). Review regressions: synchronous async completion, callbacks from async
bindings cannot yield, charges of every input-proportional builtin, prompt wall kills on
allocation-heavy loops, module categories of `task.spawn` children and resumed coroutines, `require`
of a disabled module, bounded `print` and error messages, charges independent of heap addresses,
queue compaction after cancelled waits, `resumeCost` bounding trivial resumes per tick, clean failure
of unbounded binding↔Luau recursion.

```
cmake -S . -B build/script -G Ninja -DHELIOS_BUILD_GRAPHICS=OFF
ninja -C build/script helios_script script_tests && ./build/script/bin/script_tests
```

## Known limitations (Phase 0)

* **Metering cost** (perf workload, interpreter, dev container, GCC 13 and Clang 18, RelWithDebInfo;
  budget ≤ 10 %, asserted by the `perf:` case in optimized builds). The Helios host, reading the clock
  every 64 fuel as on cells, runs ≈ 2–8 % slower than unmetered plain Luau, where calling the host at
  every safepoint cost ≈ 25–28 % before `fuel-counter`. In plain Luau on the patched VM, a callback
  every 64 safepoints costs ≈ 0–3 % and one at every safepoint (counter unarmed) ≈ 12–20 %. The
  decrement itself is free: a standalone A/B against stock 0.739 measured stock unmetered 2.82 ms,
  stock with a callback at every safepoint 3.19 ms (≈ +13 %), the patched VM unmetered 2.76–2.79 ms and
  with the host every 64 safepoints 2.81–2.84 ms. ≈ 11 ns per fuel. Native code is not in the perf case:
  it calls an out-of-line helper when the counter reaches zero, so a host that never arms the counter
  pays that call at every native safepoint (Helios always arms it). The A64 native-code half of
  `fuel-counter` is compiled on every target but runs only on arm64 hosts, which CI does not have.
* Weak tables are rejected at `setmetatable` time only; adding `__mode` to a metatable after it is
  attached is left to the planned `simdet` Luau analyzer rule, as is iteration over tables keyed by
  tables/userdata/functions.
* `math.random` is one VM-wide stream; named streams (`Rand`, 06 §11) come with the gameplay API.
* Module heap caps are page-granular and approximate for `coroutine.wrap` generators: a wrapped
  coroutine keeps the category active when it was created (the wrap closure resumes it without going
  through `coroutine.resume`), while the cap check uses the resumer's category. The VM cap is exact.
* Some costs are not charged because Luau exposes no hook for them: the `..` operator copies both
  operands (one safepoint per concatenation), and `table.clear`/`table.clone`/`table.maxn` are
  charged by the array length only (hash parts are invisible through the API). The heap cap bounds
  each such operation, and the GC-step clock read catches allocation-heavy loops within one
  iteration of the wall limit. The inline counter could carry such a VM-side charge (02 §7.4); no
  charge is added yet.
* Fuel costs of bindings and wrapped builtins are placeholders until `--calibrate-fuel` (WP-1.6);
  there is no replay recording of wall-backstop kills yet (the event carries what the recorder
  needs), and no DAP adapter yet (WP-1.6).
* Wrapping `coroutine.resume` with a plain call means a debugger break inside a nested coroutine
  cannot propagate through it; the DAP adapter must re-add a continuation.
* `wallCheck` tests the 20 ms backstop before the 5 ms client budget, so a client resume preempted for
  more than ≈ 15 ms between two clock reads is reported as `WallBackstop` rather than `WallBudget`. That
  makes `fuel: client profile kills at the wall-time budget …` flaky under heavy load (≈ 1 in 12 runs at
  load 12–15). Follow-up: report `WallBudget` when both limits are past at one read.

## Plan conformance

Plan-Rev: 8

Written to plan revision 8, which is WP-0.10r's own `Plan-Change` to 02 §7.4 and 04 §10.2 (the
Integrator raised this line from 6 when it merged, 09 §5.10.2 D1): WP-0.10r added the `codegen-fornloop-fuel` and `fuel-counter` Luau patches
and made `create()` refuse native codegen on cells and world-script hosts. 02 §7.4 and 04 §10.2, as WP-0.10r
amended them, say the refusal stays after the patch: the patch is its precondition, and lifting it is
02 §8.1's P3 "codegen opt-in on cells" item. World-script hosts run the cell profile; a dedicated profile,
if 05 §1.23 needs one, must keep the refusal.
