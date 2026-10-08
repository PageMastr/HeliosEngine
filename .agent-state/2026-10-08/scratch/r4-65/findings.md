# review-65
score: 8
verdict: CHANGES REQUESTED

## BLOCKING 1: CONF-12 no longer sees external definitions inside an extern "C" { } block in a gate file (regression from the round-2 scanner change)
**location**: tools/conformance/internal/conformance/conf11_12.go:979 (call with codeLines(..., true)), :928 (externCRE), :1064 (use)

**why**: Round 2 made checkGateSymbols scan lines with string literals blanked. That turns `extern "C" {` into `extern " " {`, which no longer matches externCRE `^extern\s*"C"$`. The brace therefore falls to the default branch: it is treated as a function body (kind=func, depth++), and everything up to the matching `}` is skipped. A non-static variable or function defined inside a linkage block in any gate file is now invisible to CONF-12's symbol rule. The gate's own header uses that pattern (engine/core/src/cpugate/cpu_gate.h:25), and so does the good fixture's header (testdata/CONF-12/good/.../cpu_gate.h:4). That fixture still passes, but now only because its block is skipped. No bad fixture puts a definition inside a block, so go test does not notice. This silently weakens a conformance rule that this WP closes. Today's tree is not affected, and check 2 would still catch a real symbol at object level.

**evidence**: Scratch tree with engine/core/src/cpugate/cpu_gate.h = `#ifdef __cplusplus / extern "C" { / #endif / int hcg_leak_in_header; / int helios_cpu_gate_run(void); / ...` and cpu_gate.c with `int hcg_leak_in_source = 1; unsigned hcg_leak_fn(void) { return 0; }` inside the same kind of block. `go run ./cmd/helios-conformance -root <tree> -rules CONF-12 -strict` at 333d148: "0 failing". The same tree with 11c4075's conf11_12.go: 3 findings (cpu_gate.c:5 hcg_leak_in_source, cpu_gate.c:6 hcg_leak_fn, cpu_gate.h:6 hcg_leak_in_header). Candidate change verified: externCRE = `^extern\s*"(?:C|\s*)"$` gives the 3 findings, and `go test ./...` stays ok.

**fix**: Recognise the blanked linkage spec: externCRE = `^extern\s*"(?:C|\s*)"$` (a blanked "C" or "C++" is only spaces between the quotes). Alternatively, test the head of the unblanked line at the same columns, since codeLines keeps positions. Add a seeded case so this cannot regress: e.g. tools/conformance/testdata/CONF-12/bad/engine/core/src/cpugate/linkage.h = `#ifdef __cplusplus` / `extern "C" {` / `#endif` / `int hcg_in_linkage_block;` / `#ifdef __cplusplus` / `}` / `#endif`, with `engine/core/src/cpugate/linkage.h:4: CONF-12` in bad/expect.txt. Keep the good header's block holding only declarations.


### Skeptic verdicts on: CONF-12 no longer sees external definitions inside an extern "C" { } block in a gate file (regression from the round-2 scanner change)
- refuted=False classification=blocking: I reproduced the finding at the #65 head, 333d148. Round 2 (commit 56d0d58) changed the CONF-12 symbol scan at conf11_12.go:979 to read codeLines(p.Tree.Lines(f), true). Lines 74-87 of rules.go show that with blankStrings set, codeLines replaces everything between the quotes with spaces. So `extern "C" {` becomes `extern " " {`.

That text no longer matches externCRE `^extern\s*"C"$` at line 928, so the switch at :1064 does not open a linkage block. aggregateRE does not match it and there is no '=', so it reaches the default branch. gateDefinition then reports nothing, because gateExternRE strips `extern ` and leaves `" "`, which contains no identifier. The scan sets kind="func" and depth++, so everything up to the matching `}` is skipped as if it were a function body. This contradicts the function's own doc comment at :1035, which says "statements at file scope (inside an `extern "C" {` block too)".

I tried to refute it in four ways and could not:
(a) #67 (1a86561) does not fix it. It still passes the unblanked `lines` (its line 977), so only #65 has the regression, and #65's branch head is still 333d148.
(b) No fixture covers it. The only gate-file `extern "C" {` blocks are in engine/core/src/cpugate/cpu_gate.h:25 and the good fixture's cpu_gate.h:4, and neither defines anything inside the block. No bad fixture puts a definition in such a block, so `go test ./...` passes at the head.
(c) Today's tree is not affected. Running the head binary over the real gate files reports 0 failing.
(d) The object-level backstop is only partial. Audit check 2 in isa_audit.cmake covers object symbol lists, but its own comment (lines 51-52) says the MSVC/dumpbin form of check 2 is run by hand in tools/ci/msvc_gate_audit.ps1 and "no CI job calls it". So on the primary platform, CI does not cover this gap.

The candidate fix works: externCRE `^extern\s*"(?:C|\s*)"$` restores all the findings, still reports 0 on the real tree, and `go test ./...` stays ok.

I classify it as blocking because the PR silently weakens a lint rule this WP is closing, against the rule's documented behavior and with no test that would catch it. CLAUDE.md forbids weakening lints, and the fix is one line plus a seeded bad fixture. Today's real-world impact is low: no current definition sits in such a block, and a definition there would be unusual.
- refuted=False classification=blocking: The finding holds, and it should block the merge.

1. **The PR caused it.** Main (fb9517f) passed the unblanked lines to checkGateSymbols, so its `extern "C" {` handling worked. Round 2 of this PR (56d0d58, kept at head 333d148) now passes `codeLines(..., true)`. That function blanks literal contents and keeps the quotes, so `extern "C"` becomes `extern " "`. externCRE `^extern\s*"C"$` no longer matches, and the externCRE case is now dead code. The brace falls to the default branch: gateDefinition is called on the head itself, then kind is set to "func" and the whole linkage block is skipped as a function body. The doc comment on checkGateSymbols ("statements at file scope (inside an `extern "C" {` block too)") is now false.

2. **The rule is one this WP owns.** WP-0.5r's acceptance includes "CONF-12 closed", and the PR changes this exact scanner.

3. **The PR does not disclose it.** "Not verified / known limitations" does not mention it, and the round-2 table describes the change only as a fix. No bad fixture puts a definition inside a linkage block, so `go test` stays green.

4. **It is not a style point or outside the WP's scope.** It is a correctness regression that silently weakens a conformance gate. CLAUDE.md says never to weaken lints, and this does so, even if by accident.

**What softens it, without making it non-blocking:**
- Today's tree is unaffected: cpu_gate.h's block holds only declarations.
- ISA-audit check 2 (nm on the gate objects) would still catch a real leaked symbol at object level.

That makes it "real but contained" (7–8 under the scoring section), and a score of 9 or more needs zero blocking findings. The fix is one line plus a seeded fixture.

## NIT 1
Nothing checks that the real clang-cl gate compile carries HELIOS_ISA_GATE_CLANG_CL. The COFF tests read the variable directly (tools/lint/lint_tests.cmake:225). Check 1 requires only /GS- of a clang-cl gate unit (tools/lint/isa_audit.cmake:846). No CI job runs check 2 on clang-cl's objects. A regression in the branch at cmake/HeliosIsa.cmake:120 would therefore pass every test. Suggestion: check 1 requires the three /clang:-fno-* options on clang-cl gate units, with a fixture.

## NIT 2
cl gap (acknowledged in the PR): putting `#pragma loop(no_vector)` before the gate's byte loops (e.g. engine/core/src/cpugate/cpu_gate.c:226, the brand-sanitizing loop) under `_MSC_VER && !__clang__` would apply the same rule to cl's objects now, instead of waiting for WP-0.2r part 2's strict script to find `__xmm@` pools.

## NIT 3
When clang, llvm-nm or llvm-objdump is missing, the COFF tests are simply not registered; only a STATUS line says so (tools/lint/lint_tests.cmake:244, :251). That is correct: they do not falsely pass. But a runner-image change would drop them from CI without any red signal. Consider a cache option, set by CI, that makes missing tools a configure error.

## NIT 4
engine/core/src/platform_init.cpp:14-16 and the PR body say platformInit() allocates nothing before log::flush(). A failed os::pipeWrite returns an Error built by errnoError/lastError (a std::string), so the error path of the first write can allocate. This is negligible; only the wording is imprecise.

## NIT 5
Carried from rounds 1 and 2: ADR-0.6c §3 item 3 is listed under WP-0.2r part 2 in 09 §5.10.4 (b), while WP-0.5r's §2 row still names it. This is the Director's call, as the implementer's comment says.

## verified
Fetched origin (main fb9517f); worktree /home/user/wt/review-65-r3 at 333d148 (removed at the end, primary checkout untouched). Read both prior reviews, both implementer comments, the PR body's round-2 table, 09 §2 WP-0.5r, 02 §1.1 gate rules/check 2, 09 §5.3, and `git diff 11c4075 333d148` in full.
Round-2 blocking items: (1) tools/conformance/README.md:60 CONF-12 row now reads 'passes (...)'. (2) tools/ci/msvc_gate_audit.ps1:71-77 is strict: `$literalPools` and its exemption are removed, so only the three exports may be External; the import list is unchanged. Not run: no PowerShell or dumpbin here.
clang-cl flags: applied only in the gate branch of helios_apply_isa_level for MSVC-frontend Clang (HeliosIsa.cmake:120), PRIVATE C/C++ genex; cl keeps /GS- only, so no /clang: option reaches cl. Check 1 strips /clang: and ignores -fno-vectorize, and lint_isa_audit passed in the Windows clang-cl CI job. The deviation is recorded in cpu_gate.h:15-17, tools/lint/README.md, cmake/README.md, engine/core/README.md and 09 §5.10.4 (b) (lines 1246, 1248).
clang-cl 18 by hand on the probe and the hook (shipping and Sandy Bridge) at /Od, /O1, /O2 /Ob1 /Zi, /Ox, each with and without -ffreestanding: externals are only the three exports plus allowlisted __imp_ imports (the test's -ffreestanding hides no memset/memcpy import). A seeded /arch:AVX2 COFF object fails check 2 with VEX findings (the COFF disassembly path works). A missing object fails ('does not exist').
COFF test mutations, all via ctest and all reverted: string literal back in cpu_gate.c → probe fails with a ??_C@ export; L"HELIOS_CPU_GATE_SILENT" back in the hook → hook and hook_snb fail; -fno-vectorize/-fno-slp-vectorize removed → probe fails with 3 __xmm@ splats; -fno-unroll-loops removed → probe fails with __xmm@00203a64... (\"CPU supported: \"); restored → 4/4 pass. A one-character literal folds into an immediate and correctly passes.
CONF-12: go vet/test ok. The 11c4075 scanner fails the new good and bad fixtures (claim confirmed). Found the extern "C" regression (blocking 1) and checked the candidate fix.
platformInit(): static string_view text, noexcept, three writes, exit 70. Feature-name table matches the enum bit order (19 entries; CMPXCHG16B fits [11]).
linux-gcc (GCC 13, -j2): full build, 0 warnings. xvfb-run -a ctest -LE perf -j2 twice: 538/538 both times (Skipped: shaderc.spirv-val (no spirv-val here), rendertest.validation-layer, server_tests_ns03_remote). The 4 lint_isa_coff_gate_* tests pass. ctest -L lint -j2: 463/463. lint_isa_audit: 1372 units (1307 avx2, 62 base, 3 CPU-gate), 9 gated executables. core_tests -tc="cpu gate*": 15 cases, 109 assertions. Children: child exit 0, snb exit 78 with the message, nohook prints static-init then 'Helios: CPU gate did not run…' and exits 70. nm on the ELF gate objects: only the 2 probe exports; hooks import write/_exit/sigaction/helios_cpu_gate_run.
cross-mingw (gate targets + core_tests, 0 warnings): AddressOfCallBacks == helios_cpu_gate_tls_entry, right after __xl_a and ahead of the child's .CRT$XLB slot and __xl_c/d/f/z, in child, snb and snb_gui (GUI subsystem 2); in nohook the .CRT$XLB slot is first. MinGW gate objects' externals are the three exports plus allowlisted imports and _tls_used. The shipping hook has 0 test-hook strings.
cmake -P tools/ci/run_lints.cmake: all passed. helios-conformance -strict (full): 12 rules, 0 failing, 1 suppressed. -strict -rules CONF-12: 0 failing (1905 files).
CI on 333d148: 14/14 green (run 37428588038). The linux-gcc job registered and passed lint_isa_coff_gate_probe and _detects_literal_pool ('no MinGW-w64 windows.h' status line). The clang-cl job ran lint_isa_audit and core_tests.
Commit trailers: WP, Criteria and Plan-Rev present; cb88107 carries Plan-Change. No closing keywords.

## not_verified
No MSVC/cl or Windows SDK here: cl's own gate objects, msvc_gate_audit.ps1 (no PowerShell/dumpbin, and no CI job runs it) and all Windows runtime behaviour are proven only by Windows CI. The COFF tests use clang 18; the LLVM version bundled with the Windows clang-cl job was not checked, and that job does not run check 2. linux-clang and linux-dev (modular) were not rebuilt locally this round (CI green). MinGW binaries could not be run (no Wine); TLS order was checked statically. Test counts differ from the PR body's by one (538 vs 539, 463 vs 464 lint), apparently an environment-dependent registration; not investigated. tools/ci/check_owner_lines.sh is not in the tree, so trailers were checked by hand.

## notes_for_lead
- **The one blocking finding is a contained regression from round 2's `56d0d58`.** It is a one-line regex change plus one fixture file, and I checked the regex locally (3 findings, and `go test` stays ok). Today's tree is unaffected: nothing is defined inside an `extern "C"` block in the gate files. Once it is fixed with the fixture, I expect a 9 to 10.
- **Director items:**
  - ADR-0.6c §3 item 3 is listed under both WP-0.2r part 2 (09 §5.10.4 (b)) and WP-0.5r's §2 row. Reconciling them needs a §2.1 contract change, which only the Director can make.
  - Neither RT-09 nor CL-17 (early) has a `scorecard.jsonc` entry (09 §5.3 DoD 8). This predates the PR (WP-0.2r part 1 did not add RT-09 either), so I did not count it against this PR. Someone should decide which WP registers them.
- **cl objects stay unverified until WP-0.2r part 2 wires `msvc_gate_audit.ps1` into CI.** The strict script may then report cl auto-vectorizer pools. The PR body records that this would be a finding against the gate objects, and `#pragma loop(no_vector)` is the likely remedy.
- **Cleanup:** I removed my worktree and both build dirs. The primary checkout is unchanged and clean. Scratch files are under `scratchpad/review-65-r3`. Nothing was posted to GitHub.
# challenge-65

## BLOCKING 1: The round-2 CONF-12 change no longer scans inside `extern "C" {` blocks, so external definitions there are missed
**location**: tools/conformance/internal/conformance/conf11_12.go:979 (new call codeLines(p.Tree.Lines(f), true)), :928 (externCRE = ^extern\s*"C"$), :1064 (where it is used), :1035 (doc comment that still claims `extern "C" {` blocks are covered)

**why**: checkGateSymbols now gets lines with the contents of string literals blanked, so `extern "C" {` arrives as `extern " " {`. externCRE no longer matches it, and the `{` branch falls to `default:`. That branch calls gateDefinition (no name is found, so nothing is reported) and sets kind="func", so the whole block is skipped as if it were a function body. Any non-static definition inside the usual `#ifdef __cplusplus / extern "C" { / #endif` wrapper of a gate file is no longer reported. cpu_gate.h uses that wrapper, and so does the good fixture's cpu_gate.h. On fb9517f these definitions are reported, so this PR weakens a gate lint without saying so, against CLAUDE.md's rule never to weaken lints. Audit check 2 can still catch a definition in a compiled gate object, but CONF-12's documented source-level contract (conf11_12.go:1035) is now false. No bad fixture covers this case, which is why TestFixtures stays green.

**evidence**: I wrote a scratch Go test (not committed) that runs Run(Options{Rules:[CONF-12],Strict:true}) on a temp tree. Its engine/core/src/cpugate/cpu_gate.c has `#ifdef __cplusplus` / `extern "C" {` / `#endif` / `int hcg_leak_in_externc = 1;` / `unsigned hcg_leak_fn_in_externc(void) { return 0; }` / `#ifdef __cplusplus` / `}` / `#endif`, followed by literal edge cases (a declaration after a string, '"', escaped quotes, a line continuation, '\'', a digit separator). On fb9517f the tool reports cpu_gate.c:5 and :6 ("external symbol hcg_leak_in_externc …", "… hcg_leak_fn_in_externc …") plus the edge cases, 10 failing. On 333d148 lines 5 and 6 are gone (7 failing); every literal edge case is still reported, so the blanking itself is fine. With externCRE changed to `^extern\s*"[C ]"$` (accepting the blanked form), both lines are reported again and `go test ./internal/conformance -run TestFixtures` still passes.

**fix**: Make the extern-"C" test see through blanking: externCRE = regexp.MustCompile(`^extern\s*"[C ]"$`), or match the head against the unblanked line. Add a case to testdata/CONF-12/bad/…/cpu_gate.c (or a gate header) with a non-static definition inside an `#ifdef __cplusplus extern "C" { #endif … }` block, so the old and new scanners differ on a fixture. Optionally state in tools/conformance/README.md's CONF-12 paragraph (around :313) that extern "C" blocks are covered.


## BLOCKING 2: The new MSVC-ABI check-2 tests compile with /clang:-ffreestanding, which the gate never builds with and which hides compiler-generated memset/memcpy imports
**location**: tools/lint/lint_tests.cmake:225 (the flag); :201 and :205 (comment: "with the gate level's clang-cl flags", whose only named deviation is -fno-ms-compatibility, "changes parsing, not code generation"); tools/lint/README.md:224, docs/plan/09-roadmap-and-process.md:1246 and the PR body's round-2 table describe the same flags

**why**: lint_isa_coff_gate_* is the PR's only evidence that clang-cl's MSVC-ABI gate objects define and import only the allowlisted symbols. msvc_gate_audit.ps1 runs in no job, and the Windows clang-cl job does not run check 2. But the test command adds `/clang:-ffreestanding`, which neither HELIOS_ISA_GATE_CLANG_CL nor the windows-clang-cl preset passes. -ffreestanding implies -fno-builtin, which stops LLVM from turning loops into memset/memcpy library calls. So the test can pass a gate unit that the real clang-cl build compiles with a `memset` import: a CRT call before the CRT exists, not on HELIOS_ISA_GATE_ALLOWED_IMPORTS, against 02 §1.1. The test therefore checks a weaker configuration than the build it vouches for, and the PR body, the lint README and 09 §5.10.4 (b) all say it uses the gate level's clang-cl flags.

**evidence**: Scratch file memset_demo.c: `int helios_cpu_gate_run(char* buf, uint32_t n){ for(i=0;i<n;++i) buf[i]=0; return 1; }`. Compiled with the lint's exact command (clang-cl-18 --target=x86_64-pc-windows-msvc /O2 /Ob2 /DNDEBUG $HELIOS_ISA_GATE_CLANG_CL /X /clang:-ffreestanding), then `cmake -DMODE=object … -P isa_audit.cmake` (333d148's audit and allowlist) prints "is clean". The same command without /clang:-ffreestanding gives `U memset`, and the audit fails: "object: references 'memset', which is not on HELIOS_ISA_GATE_ALLOWED_IMPORTS". The flag is also unnecessary: the real cpu_gate.c and win32/cpu_gate_hook.c (shipping and HELIOS_CPU_GATE_TEST_EMULATE_SANDY_BRIDGE, with -imsvc MinGW headers and /clang:-fno-ms-compatibility) compile without it here, and check 2 reports all three objects clean. The current gate sources are therefore fine; the defect is the test's fidelity. Also checked: the PR's mutation claims hold (the 11c4075 sources give 32 and 6 `??_C@` exports; the head probe without -fno-vectorize/-fno-slp-vectorize gives the three `__xmm@` 3f/5e/e0 splats).

**fix**: Drop `/clang:-ffreestanding` from the custom command at lint_tests.cmake:225 (verified to build and pass here). Better still, add a seeded fixture with a variable-length zeroing loop that must fail with "references 'memset'", so the libcall class is covered. Update the PR body's round-2 row if any non-gate flag stays in the command.


### Skeptic verdicts on: The round-2 CONF-12 change no longer scans inside `extern "C" {` blocks, so external definitions there are missed
- refuted=False classification=blocking: I reproduced the finding at #65 head 333d148 and it holds. The only change to conf11_12.go since fb9517f is at line 979: `checkGateSymbols(p, f, lines)` became `checkGateSymbols(p, f, codeLines(p.Tree.Lines(f), true))`. With blankStrings=true, codeLines (rules.go:33) replaces everything inside a string literal with spaces and keeps the quotes, so `extern "C" {` reaches the scanner as `extern " " {`.

At the `{`, the head is `extern " "`. That does not match `externCRE = ^extern\s*"C"$` (line 928), is not an aggregate and contains no `=`, so it falls to `default:`. There, `gateDefinition(..., body=true)` strips `extern ` with gateExternRE and is left with `" "`, which has no identifier, so it reports nothing. The scanner then sets kind="func" and skips the whole block as if it were a function body. The doc comment at line 1035 ("inside an `extern "C" {` block too") is therefore false at 333d148.

Nothing else in the PR handles this. The bad fixture has no extern "C" block in a gate file; the only extern "C" in the bad fixtures is `apps/tool/dispatch.cpp`, which is not a gate file. That is why TestFixtures still passes at 333d148. PR #67 (1a86561) does not touch this either: it still has the pre-change `checkGateSymbols(p, f, lines)` at line 977. The real cpu_gate.h (line 25) and the good fixture's cpu_gate.h (line 4) both use the `#ifdef __cplusplus / extern "C" {` wrapper. Today they only hold declarations, enums and typedefs, so nothing real is missed yet. The problem is lost coverage, not a missed violation.

The impact is partly offset by ISA audit check 2: the lint-labelled CTest checks the symbols of the compiled gate objects (ELF) for unlisted exports, so a leaked definition would most likely still be caught at object level. Even so, this PR quietly turns off part of a source-level gate lint and makes its own documented contract false. CLAUDE.md says lints must never be weakened. The fix is one line plus a fixture case, so I classify it as blocking, not a nit.
- refuted=False classification=blocking: The finding is correct, and it should block. Round 2 changed checkGateSymbols to receive `codeLines(p.Tree.Lines(f), true)`, which blanks the contents of literals, so `extern "C" {` becomes `extern " " {`. `externCRE` (`^extern\s*"C"$`, conf11_12.go:928) no longer matches it. The `{` then falls to `default:`. There gateDefinition finds no name, `kind` is set to "func", and the whole block is skipped as if it were a function body.

As a result, any non-static definition inside the usual `#ifdef __cplusplus / extern "C" { / #endif` wrapper is no longer reported. The real gate header uses that wrapper (engine/core/src/cpugate/cpu_gate.h:25), and so does the good fixture's cpu_gate.h.

Why it is blocking under the project rules:
1. **Correctness defect in a lint this WP owns.** Closing CONF-12 is one of WP-0.5r part 1's stated acceptance criteria, and the PR edited this scanner itself.
2. **Undisclosed weakening of a lint.** CLAUDE.md says never to weaken lints. The PR checklist says "No tests, lints or CI jobs skipped, weakened or disabled". The round-2 table describes the change only as "ignores ';' and braces inside literals". The narrowing is unintended and not disclosed, so that checklist claim is now false.
3. **False documented contract.** The doc comment at conf11_12.go (checkGateSymbols: "statements at file scope (inside an `extern "C" {` block too)") is no longer true.

It is not covered by any disclosed known limitation, and it is not outside the WP's scope. Audit check 2 still checks the compiled objects, which limits the impact (the real tree is clean today), but that does not make the source-level rule correct. Under reviewer-rules this is "a real but contained defect" (7–8 band), which rules out a score of 9 or more. The fix is one line plus a fixture case.

### Skeptic verdicts on: The new MSVC-ABI check-2 tests compile with /clang:-ffreestanding, which the gate never builds with and which hides compiler-generated memset/memcpy imports
- refuted=False classification=blocking: I could not refute the finding. At 333d148, tools/lint/lint_tests.cmake:225 builds every lint_isa_coff_gate_* object with `/X /clang:-ffreestanding`. That flag appears nowhere else in the repo outside third_party. The real gate build does not use it. helios_apply_isa_level("gate") with clang-cl adds only HELIOS_ISA_GATE_CLANG_CL (`/GS- -fno-sanitize=all /clang:-fno-vectorize /clang:-fno-slp-vectorize /clang:-fno-unroll-loops`, HeliosIsa.cmake:93). engine/core/CMakeLists.txt and the windows-clang-cl preset add nothing like it.

The docs say otherwise. Line 201's comment, tools/lint/README.md ("with the gate level's clang-cl flags and /O2 /Ob2"), 09 §5.10.4 (b) and the PR body's round-2 row ("with /O2 /Ob2 and HELIOS_ISA_GATE_CLANG_CL") all describe the test as using the gate's flags. Neither the PR body nor commit cb88107 ever mentions -ffreestanding.

-ffreestanding implies -fno-builtin, which stops LLVM from turning loops into libcalls. memset is not on HELIOS_ISA_GATE_ALLOWED_IMPORTS, and 02 §1.1 check 2 requires every undefined symbol to be on the allowlist. posix/cpu_gate_hook.c:112 shows the project already avoids memset in the gate on purpose.

These COFF tests are also the only check-2 coverage of MSVC-ABI gate objects. lint_isa_audit and the coff tests are guarded `NOT WIN32`, ci.yml never calls msvc_gate_audit.ps1, and the Windows clang-cl job only runs ctest. So a future gate change that makes clang-cl emit `memset` would pass every CI job. The test checks a weaker build than the one it vouches for.

The current gate sources are clean with or without the flag, so this is a gap in what the test catches, not a shipped bug. The fix is a one-token removal.
- refuted=False classification=blocking: The technical claim holds. At head 333d148, tools/lint/lint_tests.cmake:225 compiles the COFF gate objects with `/X /clang:-ffreestanding` on top of HELIOS_ISA_GATE_CLANG_CL. Neither the gate level (cmake/HeliosIsa.cmake:93,121) nor any preset passes -ffreestanding or -fno-builtin; `git grep` finds the flag nowhere else. The flag changes code generation in exactly the area check 2 audits. It turns off LLVM's loop-idiom libcalls, and the memset or memcpy those would produce is a forbidden import, since neither is on HELIOS_ISA_GATE_ALLOWED_IMPORTS. The flag is also unneeded: the real probe and both hook builds compile without it and audit clean.

Why this blocks under reviewer-rules.md:
(1) It is an undisclosed, materially misleading claim about what was verified. The lint comment (:201), tools/lint/README.md, the plan text in 09 §5.10.4 (b) and the PR body all say these tests build "with the gate level's clang-cl flags". The comment at :205 discloses the only extra flag it mentions as one that "changes parsing, not code generation", which suggests the build matches the real one's code generation. The undisclosed -ffreestanding does change code generation and hides a class of check-2 violations. This conflicts with CLAUDE.md's "Report exactly what you verified" and with the PR's statement that "clang-cl's objects are checked here and on the Linux CI jobs".
(2) The checklist in reviewer-rules.md asks for tests that "don't exercise the real path", and this test does not build the real configuration.
(3) It is inside the WP's scope and not a known limitation. The COFF tests are new in this PR, live in tools/lint, and are the PR's only evidence for 02 §1.1 check 2 on clang-cl MSVC-ABI objects. The "Not verified" section mentions -fno-ms-compatibility, not -ffreestanding, and names no owner for it.
(4) The calibration matches earlier rounds. Rounds 1 and 2 both treated inaccurate docs or claims about gate and conformance verification as blocking. This is a stronger case of the same kind, because the inaccuracy hides a real class of violations.

Things that make it smaller but do not cancel it:
- Today's gate sources are clean under the true flags, so nothing that ships is wrong.
- In CI, a memset added to the shared cpu_gate.c would still be caught by ELF check 2 in the real clang and gcc builds.
- The hook COFF tests do not run in CI anyway, because the runners have no MinGW headers.

So this is a contained defect (7–8 territory) with a one-line fix: drop `/clang:-ffreestanding`, or disclose and justify it everywhere the flags are described. The seeded memset fixture the finding suggests is optional hardening; merging should not wait for it.

## NIT 1
tools/lint/lint_tests.cmake:217 says the COFF objects use "the Release flags of the MSVC-family presets (/O2 /Ob2)", but every MSVC-family preset builds RelWithDebInfo (/O2 /Ob1; CMakePresets.json windows-msvc-release and windows-clang-cl). I compiled the gate with /Ob1 and it is clean too. Either fix the comment or build both /Ob1 and /Ob2.

## NIT 2
The PR body's Windows-placement row says the MinGW TLS order was "checked by hand … on six gated images (below)", but "How it was tested" names three: core_cpugate_child, core_cpugate_child_snb and core_cpugate_child_snb_gui.

## NIT 3
platformInit() (engine/core/src/platform_init.cpp:41-47) allocates nothing only when the writes succeed. os::pipeWrite's error path builds an Error with a std::string (win32_process.cpp:156 lastError("WriteFile(pipe)"); posix_process.cpp:171 errnoError), for example when stderr is a closed pipe. The round-2 note that it "allocates nothing before log::flush()" is a little too strong. Low risk; a raw write would remove it.

## NIT 4
cl has no command-line switch that turns off auto-vectorization, but `#pragma loop(no_vector)` exists per loop. The gate's byte loops (cpu_gate.c:226-229 brand sanitizer, hcg_put, the hook's hcg_append and widening loop) could be kept free of cl's `__xmm@` pools by construction, instead of waiting for msvc_gate_audit.ps1 in WP-0.2r part 2. Optional.

## NIT 5
Forward compatibility: core_cpugate_child is now a helios_executable() gated image (in HELIOS_CPU_GATE_TARGETS), and it carries its own `.CRT$XLB` TLS callback, helios_cpugate_child_xlb_entry (tests/support/cpugate_child.cpp:72-85). That entry is not in cmake/pre_main_allowlist.cmake:12-23, so WP-0.2r part 2's Windows check 3 will need an entry or a test-image exclusion. Worth a line in 09 §5.10.4 (b).

## NIT 6
tools/lint/lint_tests.cmake:208-209: find_program takes any `clang` on PATH, and the COFF objects are part of ALL. A host whose clang is too old for /clang: options would fail the whole linux-gcc build, where today it would only skip the tests. Optional: check the version, or skip with a STATUS message.

## checked
Static review of PR #65 at 333d148 against origin/main fb9517f (fetched; the merge-base is fb9517f). I did not build the project, and I changed nothing in /home/user/HeliosEngine: no checkout, no commit, nothing posted. I read the whole diff and these files at 333d148: win32/posix cpu_gate_hook.c, cpu_gate.c/.h, cpu.cpp, platform_init.h/.cpp, cpugate_child*.cpp, test_cpu.cpp, engine/core/CMakeLists.txt, top-level CMakeLists.txt, HeliosIsa/HeliosModule/HeliosModular.cmake, isa_allowlist.cmake, msvc_gate_audit.ps1, isa_audit.cmake (checks 1 and 2), lint_tests.cmake, the layering fixtures, the conformance conf11_12.go and rules.go codeLines, the CONF-12 fixtures, 02 §1.1 and the WP-0.5r rows in 09 §2 and §5.10.4 (b).

Windows/MSVC: the hook's const_seg(\".CRT$XLA0\") and the /INCLUDE pragmas are correct for x64. I compiled the hook with clang-cl-18 for x86_64-pc-windows-msvc with real-like flags: `.CRT$XLA0` is 8-byte aligned and read-only, holds an ADDR64 relocation to hcg_tls_callback, and the .drectve carries `/INCLUDE:_tls_used /INCLUDE:helios_cpu_gate_tls_entry`. The only externals are the slot and the allowlisted `__imp_` imports. ARM64 and other non-x86 targets compile and record PASS (HCG_HAVE_X86 0). The `/RTC` strip changes only engine/core's CMAKE_C_FLAGS_DEBUG, whose only C units are the three gate units; C++ is untouched, no subdirectories inherit it, and CMP0184 stays OLD under cmake_minimum_required 3.28. The /clang: options reach only clang-cl (the C compiler ID test) and the Linux lint; never cl.exe. The MSVC ASan genex skips gate-level targets. /EXPORT:…,DATA is applied only under WIN32 modular, and MinGW modular is rejected at configure. The loader-lock failure path matches 02 §1.1. platformInit()/checkCpuGateVerdict: exit 70 vs 78, the record bit is never passed by cpu.cpp, and the verdict is read in the same image that records it, in both link flavours.

Lint tests: when tools are missing they print a STATUS line and are not registered; none passes silently. The PR body discloses that the hook COFF test is absent where MinGW headers are missing. The CI linux-gcc job log for 333d148 shows lint_isa_coff_gate_probe and _detects_literal_pool passed and the hook tests not registered, as claimed. All 14 checks on 333d148 are green (run 37428588038).

CONF-12: I ran Go probes on scratch copies of tools/conformance at fb9517f and 333d148 (go1.27.1 toolchain from the cache). The literal edge cases are handled; the extern \"C\" block is the regression in blocking 1. The old scanner fails the new good/bad fixtures, as the PR claims. Also verified from the PR body: 56d0d58→333d148 is comment-only; there are 8 gated mains; known_failing.jsonc is empty; the README/map/scorecard/09 text matches; 15 non-Windows \"cpu gate\" test cases; commit trailers are present and there are no closing keywords.

Not verified: any Windows run or cl.exe objects (no MSVC here); MSVC Debug and ASan builds; behaviour of the VS generator; the full ctest counts (left to the build reviewer). Scratch is under /tmp/claude-0/-home-user-HeliosEngine/28b618d3-b602-4e0c-963a-809be3b7f518/scratchpad/pr65-static/ (zz_probe_test.go, memset_demo.c); larger copies deleted.
