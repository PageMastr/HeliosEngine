# Helios build loop: paused state (2026-09-30)

The owner paused all work on 2026-09-30 at about 17:30 UTC ("stop all tasks and save their progress"). This
file records where everything stands so the loop can resume. Nothing below is merged unless it says so.

- **main:** `5bc959f` (PR #37). PLAN-REV **11**.
- **Rules:** the binding agent rules are in `implementer-rules.md` and `reviewer-rules.md` next to this file.
  They include the rule added on 2026-09-30: never write a closing keyword ("fix #N", "fix: #N",
  "fixes/closes/resolves #N") next to a PR or issue number. PR #21's body closed #23, #24, #31 and #32 that
  way; they were reopened.
- **Merge rule:** main's `merge-policy` check requires a WP-class squash tree to equal the PR head tree. So
  before each squash, the PR must contain current main: do a bare `git merge origin/main` (a merge commit
  with `Plan-Rev: 11` and nothing else), then CI green, then squash with `expectedHeadSha`. Merge one PR at a
  time; each merge moves main.

## Merged in this run (squash SHA on main)

| PR | SHA |
|---|---|
| #12 WP-0.19 | cf02817 |
| #10 WP-0.3 1/3 | 77e4e54 |
| #9 WP-0.10r | 3e39fb3 |
| #8 WP-0.15r | 8dc2a4d |
| #15 WP-0.5 async-io | 43c40b3 |
| #7 WP-0.1 merge-policy (Codex) | 04816a1 |
| #11 WP-0.3 2/3 | 1c0b128 |
| #13 WP-0.3 3/3 | 0a323aa |
| #14 WP-1.1a, M1 by owner decision (PLAN-REV 10) | eddb29a |
| #16 WP-0.14 | 13cb382 |
| #17 WP-0.1 doctest TU isolation | a61d49c |
| #20 WP-0.15 NATS flush | 5792aa8 |
| #18 WP-0.12 | 1425608 |
| #19 WP-0.7 schemac lock | 5f88be7 |
| #21 Director D6 plus 04 §3.2 (PLAN-REV 11) | cfd2e15 |
| #37 WP-0.9 pcg parseU32 | 5bc959f |

## Open PRs and the next action for each

### Ready to merge (approved at 9/10 or better, bare-merged onto 5bc959f)
Before squashing, confirm that main is still 5bc959f and CI is green on the head.

| PR | Head | Approval | CI |
|---|---|---|---|
| **#31** WP-0.1 nightly MSBuild configure (ISA image list per config) | `08e2a0375d24978fd90f87ea5d73cc6fad76b156` | r1 9/10 | run 36746696675 green |
| **#23** WP-0.1 nightly Go login limit | `d6e13c2c652dd556117053be5e49a2e0aa84de3a` | r1 9/10 | run 36746702547 green |
| **#24** WP-0.10 script ASan (Luau patch 0003 plus lvmexecute -O1 in Debug-sanitizer) | `102aa3bbab4f95a13e5b80a70cb9119a1fe18743` | r1 9/10 | check it |

### Approved, but they need a bare merge of the then-current main first
| PR | Head | Approval |
|---|---|---|
| **#34** WP-1.1a ecs parity digest (flecs debug mode) | `52ae44c` | r2 9/10 |
| **#38** WP-0.11 rhi_triangle LSan-clean without suppressions | `5e51cad` | r2 9/10 |

### In review
- **#36** WP-0.9 physics velocity clamp. Head `0dbdc33`, which already contains 5bc959f. The round-3 review
  (comment-only delta) was in progress at the pause; see "Round-3 #36" below.
- **#32** WP-0.3 nightly doctest-XML label filter. Round 1 was 8/10. The test fix is at `4fcaf90`, which
  contains 5bc959f. It **needs a round-2 review**; its CI was still running.
- **#39** (new) WP-0.3 nightly: don't run perf-labelled tests under ASan. This is the owner's decision of
  2026-09-30, "Drop speed tests there". Head `9568bc4`. It **needs a round-1 review**; CI was still running.

### WP-0.7b stack (bottom-up: #22 → #30 → #33 → #35)
The review bodies are posted on each PR.

| PR | Head | Review | Blocking findings to fix |
|---|---|---|---|
| **#22** luau | `95aa7ca` | r2 8/10 | Nil elements bypass the per-call value cap: `Optional` returns nullopt without `take(c, 1)`, and `reserve` doesn't deduct. Fix and add a hostile test. |
| **#30** sql | `cac545f` | r1 7/10 | (1) A new type can take over another type's table: take the baseline by lock id, and reject a table owned by another entry. (2) A lone CR in a `///` doc comment injects SQL: reject control characters in doc comments in the lexer. This also affects the C++ and luau emitters' `//` comments. |
| **#33** repl | `180b275` | r1 8/10 | The protocol hash ignores rpc and event payload layouts: include argument and field ids and signatures. |
| **#35** lint | `9df9ee6` | Not posted; the partial draft (would be 7/10) is in `pr35-lint-partial-review.md` here | (1) `size.unbounded` misses unboundable element types (`string[4]`, map keys, list elements). (2) The lint isn't a gate: add a `lint`-labelled CTest running `--emit lint --Werror` over `schemas/gameplay`, or record report-only status and the follow-up in the README and 09 §8.1. |

The implementer's worktree was `/home/user/wt/wp-0.7b`. After fixing #22, merge it up the stack with
merge commits.

### WP-0.2 stack (bottom-up: #25 → #26 → #27 → #28 → #29)
The round-1 bodies are posted on each PR. Scores: #25 8/10, #26 8/10, #27 7/10, **#28 9/10 approved**,
#29 7/10.

**Director decisions already made:**
- Syntactic scanners are accepted for Phase 0, instead of go/analysis and ast-grep, with per-rule limits in
  the conformance README. The 09 §5.10.3 Plan-Change is still to do.
- The services/go.sum licence scan is an owned follow-up, not blocking.

**Where it stands:**
- **#25:** its fixes are done on the **WIP branch `agent/claude/wip/wp-0.2-25` at `44c14b9`**. Its parent
  `e02526e` merges 5bc959f into #25. The PR branch is still `bee7a93`.
  - Still to do: `ctest -L lint` twice, `run_lints.cmake`, and re-running the reviewer's `seed25.sh`, only
    on committed work because it runs `git checkout`/`git clean`.
  - Then tick the Deviation box, write the PR body notes, and fast-forward the PR branch to the WIP (or
    re-commit it).
- **#26 (`a0bec9e`), not started:**
  - a `framework/known_scope` fixture;
  - the CONF-09 fixtures, flow-style steps and GOTOOLCHAIN;
  - CONF-10 `.cppm`/`.ixx`, splices and `#if 0`;
  - a records-to-scorecard-gap check;
  - `walkFiles` skipping only the top-level build/;
  - the README limits and the deviation note.
- **#27 (`2b26efe`), not started:**
  - C++ string-constant resolution and multi-line calls, with unresolved buckets or keys failing closed;
  - `engine/server/**` and `apps/cellserver/**` in CONF-04's map;
  - the nits.
- **#28 (`2b6e24c`):** approved. It takes merges from below only; its nits go to a follow-up.
- **#29 (`0ee98f6`), not started:**
  - known-failing records pinned by fingerprint;
  - `conformance:allow` at `lint_tests.cmake:94`;
  - CONF-11 following `${HELIOS_ISA_AVX2}` and per-target lists everywhere;
  - the nits.

### NS-0.2 owner approval (Director PR, not yet opened)
The owner's decision (2026-09-30), verbatim: "go ahead and pass it for now. And note the reason is due to
inconsistent resource availablity causing occasional skewed results. and that it passes everywhere else. but
that at a later date testing will be completed with fixed hardware and if more work is needed it will get a
second pass then, but that as of right now, human review approves it for passing."

The WIP is on branch `agent/claude/director-ns02-owner-pass` at `a8558fb`.
- **Done:**
  - the scorecard registry mechanism: `owner_approval`, `advisory` refs, `follow_ups`, and an `approved`
    status;
  - `net_bench --gate --advisory ns02-stack` with `engine/net/bench/ns02_gate.h` and a doctest.
- **Left:**
  - `docs/evidence/ns-0.2-owner-approval-2026-09-30.md`;
  - the scorecard.jsonc NS-0.2 entry;
  - unit tests for the new registry mechanism;
  - the tools/scorecard and engine/net README updates;
  - `--advisory ns02-stack` on the two nightly net_bench steps;
  - status text in 09 §8.1 (WP-0.13 and fired risks), §8.2 and PLAN.md §11;
  - two 10-minute gate runs;
  - the checks, then the PR.
- **Notes for the Director:**
  - NS-0.7 also failed once on hosted Windows (19,690 pps, 1.55 % drops) and isn't covered by the
    approval.
  - One dev-VM 10 s stack run lost 64 packets.
  - Consider a 09 §5.6 Plan-Change that defines owner approval, given 09 §0 principle 4, "Nothing is
    waived".

### Round-3 #36
See the addendum at the end of this file, if the reviewer reported before the pause completed.

## Owner decisions this run
- **ADR-004a M1 (2026-09-27):** VM-stall runs are discarded; M1 is met. Recorded in ADR-004a §7 (#14).
- **NS-0.2 (2026-09-30):** passes on owner approval, verbatim as above. The PR is not yet open.
- **Perf tests under ASan (2026-09-30):** "Drop speed tests there". PR #39.
- **Director (lead) decisions:** NS-0.3 is class N+H (#16); the lint threat model is best-effort against
  accidental omissions (#17, #18); the syntactic conformance scanners are accepted (WP-0.2).

## Owner actions pending
- Delete the remote branch `agent/claude/fix-nightly-verify-all`. It was a temporary verification branch,
  must not be merged, and the agent proxy refused the delete with a 403.
- WP-0.4: register the Windows PC as the `win-gpu` runner. This unblocks the NS-0.3 H half and NS-0.2's
  fixed-hardware re-test.
- Optional: decide whether to vendor SPIRV-Reflect for 03 §1.7's cross-check (recommended: not now).

## Follow-up backlog (not started)
- **WP-0.15r:** the client_ip PII follow-ups.
- **The memory budget-crossing test under TSan.**
- **WP-0.3 and WP-0.1 round-5 nits.**
- **WP-1.1a round-2 nits:** a World-level owner-check test, ADR wording, and `EntityRegistry::reserve`.
- **services:** NATSHandle ctx; the trivially passing negative check at `stack_test.go:198-203`; the non-ASCII
  matcher rows.
- **WP-0.12 round-4 nits:**
  - WL6;
  - the rendertest README behaviour note;
  - fake-layer `add_dependencies`;
  - `CAPTURE`;
  - MSVC C4456;
  - `.tpp`;
  - `shaderDemoteToHelperInvocation`, owned by WP-0.11;
  - Windows layer detection, owned by WP-0.4.
- **schemac lock round-2 follow-ups:** the takeover-window reset (a diff was in the review), probes B–E, and
  docs.
- **Plan follow-ups from #21's review:**
  - the 04 §8 stage-4 wording;
  - a §5.2a Director branch class;
  - the §2.1 "working tree" mentions;
  - the stale `zone_clock.h:12` comment;
  - the engine/server layer row in 02 §1.1.
- **Director:** record the WP-0.7b tagged-userdata deferral (to WP-1.6) in the 09 §2 rows.
- **ASan follow-ups:**
  - a positive control for the Luau poison test on GCC;
  - a byte-based C-stack guard;
  - dispatch the nightly after the ASan PRs land.
- **ASan-fix nits:** for #34, the tighter empty-table rule, a test of the `#if` gate, and README l.204; for
  #38, `message(WARNING)`, the sticky FORCE, the 111-column line and dbus fd-limit noise.
- **WP-0.2:** the §5.10.3 Plan-Change, #28's nits, and the go.sum licence scan.
- **MSBuild warnings to triage** (WP-0.1): C4268, C4127, C4458.
- **render_tests_perf** hit 0.3028 ms against its 0.3 ms gate once on a hosted runner.

## Local worktrees at the pause (they may not survive a container restart)
All work that matters is pushed: see the branches above.

| Worktree | Branch |
|---|---|
| /home/user/wt/fix-asan | #36's branch (build dirs: ASan, gcc) |
| /home/user/wt/fix-script-asan | #24 |
| /home/user/wt/wp-0.7b | the #35 branch |
| /home/user/wt/wp-0.2 | `agent/claude/wip/wp-0.2-25` (build: gcc) |
| /home/user/wt/fix-nightly-{msbuild-isa,doctest-gpu,go-login-limit,asan-no-perf} | the nightly-fix PRs |
| /home/user/wt/ns02-pass | `agent/claude/director-ns02-owner-pass` (build: gcc) |

## Addendum: round-3 #36 (reported after the pause)
- **Provisional score:** 9/10, APPROVE, with no blocking findings, on head `0dbdc33614d3251eeb7b5bf85c6d961c543f0fdf`.
  Not yet posted.
- **Draft:** `pr36-r3-review-draft.md` in this folder. Replace the `CI_STATE_PLACEHOLDER` line under
  "8. CI on `0dbdc33`" with the final CI result, then post it as a COMMENT review with commit_id `0dbdc33…`.
- **CI at 17:40 UTC:** 5 of 10 jobs green; MSVC primary, clang-cl, linux-gcc, linux-clang and headless still running.
- **Merge precondition holds** while main is 5bc959f: the squash tree `3f39956` equals the head tree.
- **Optional nits:**
  - `test_grid.cpp:357-358`: add "(with discrete motion quality)";
  - `grid.h:173-174`: prefer "while no Helios body uses LinearCast (CCD)".
