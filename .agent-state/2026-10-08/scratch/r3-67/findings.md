# review-67
score: 9
verdict: APPROVE

## NIT 1
1. New data since the PR's 07:35 UTC snapshot. Today's Nightly 37449776267 on fb9517f (attempt 1, from 10:26 UTC) and win-gpu run 37473635682 answer statements the PR leaves open: 09:1388 "no nightly has run yet on a main that includes #62", 09:1445 "the first hosted nightly on 2bc2d85 shows whether hosted Windows still measures below 100k", and in the WP-0.13 row (09:1421) "no hosted nightly has run on it yet" and "failed twice on hosted Windows". Also PLAN.md:1055 ("none of them after #62", NS-0.7 "failed twice") and §8.2's "editorui_ed15 ... once" (09:1443). What the new runs show: hosted Windows NS-0.2 stack 125,630 (strict pass), but the step failed on NS-0.7 (19,696 pps, 1.5222 % drops; its third hosted-Windows failure, not in §8.2's P0 list); hosted Linux 142,749; linux-gcc perf failed only on pcg_tests_perf; linux-asan failed on schemac_sql_postgres and an editorui_ed15 timeout (its second); win-gpu 37473635682 passed strictly at 119,214. My decision: not a defect, because every statement was true at the dated snapshot and 09:1386-1388 names which runs were read. Recommendation: before merge, either add a dated sentence citing attempt 1 explicitly (attempt 2 of the three failed jobs has been running since 14:23 UTC), or qualify 09:1388 as "by 07:35 UTC" and leave the data to the next refresh.

## NIT 2
2. 09:1421: WP-0.13's status cell still reads "**Done** … except NS-0.4's Windows half". With the approval lapsed and hosted Windows short of a 3-night streak, NS-0.2 is not green either; add it to the "except" clause.

## NIT 3
3. docs/evidence/ns-0.2-owner-approval-2026-09-30.md:1 and :153-162 still read as an approval in force, and the nightly's advisory line links to this record every night. A one-line dated status note (lapsed 2026-10-04 under 09 §5.6, run 37231118477; owner's answer pending, 09 §8.1) would keep the record consistent without any registry or workflow change. The PR body already discloses this.

## NIT 4
4. The PR title "status refresh after #57–#66" names the unmerged #65 and omits the 12 earlier PRs. For the squash subject use e.g. "after #51 (22 PRs)".

## NIT 5
5. 09:1411: the run ordinals ("Run 5" = 37391930751) differ from GitHub's run numbers: 37391930751 is #6, and #5 is 37347400339, a skipped dispatch on agent/claude/wp-0.13r-ns02-headroom. Every ordinal carries its ID, so this is cosmetic; a note that skipped runs are not counted would help readers of the Actions UI.

## verified
Head 1a86561, in my own worktree /home/user/wt/review-67-r2 (now removed). The full diff fb9517f..1a86561 is 24 files, all Markdown plus PLAN-REV (14 → 15), with no code. This round, 76068f1..1a86561, touches 8 files.

Both round-1 blocking findings are fixed everywhere the statement appears:
- 09 §8.1: WP-0.4 row :1411, WP-0.13 row :1421, and the intro :1383-1395, which supersedes the fired-risks row :1430.
- 09 §8.2: :1444-1446, row 8 at :1463, and the owner list at :1470-1471.
- PLAN.md :1042-1055; engine/net/README.md :230-233 and :238; CONSISTENCY :1958.
- A grep finds no leftover \"approval in force\", \"re-test passed\" or \"passes on hosted Linux on\" statement outside the superseded fired-risks row. That row sits between two rows #65 rewrites (:1429, :1431), so leaving it is justified.
- The lapse follows from 09 §5.6 (:998-1000) and the record (:155-158). Owner question 1 states the Director's reading of \"the re-test\" as a reading for the owner to confirm or reject.

PR list: `git log --first-parent 14e46dd..fb9517f` gives 23 commits: 21 squash PRs, #56's merge commit 5bf99e6 and the owner's 1dac3d1. That is the 22 PRs at 09:1383-1384; the five PRs after f5f2729 match too.

All 8 nits are fixed:
- C4268: 81 lines, 78 distinct (79 in jobs.h, 2 in engine/core/tests/tu_isolation.h); the other nine codes match too.
- NS-0.4: 14 non-cancelled Nightly runs (9 scheduled, 5 dispatched, 3 on agent branches), with 5/5 libFuzzer jobs successful in each.
- Hosted Windows has three strict passes, one of them scheduled; the streak rule is at tools/scorecard/README.md:224-226.
- No \"the project's schema.lock\" remains.
- WP-1.23's row is at 09:165.
- The normative `.hman` clause is in 05 §7 and engine/patch/README.md:101-102.
- CONSISTENCY :1944 wording and test column are corrected.
- No added non-table line is over 110 columns (checked with Python, counting characters).

Figures read from downloaded job logs:
- win-gpu job 111864907749: stack 85,054, raw 140,969, GPU 14.59/19.52, overhead 0.197, AVX2 1.665/1.819, SSE4.2 5.912, scalar 20.955, NS-0.7 0.260/0.133.
- win-gpu job 112038791194: 118,253, 8.46 µs, raw 215,488, NS-0.7 0.198/0.103, hnoise run 5.
- Hosted jobs: 111172561149 (101,410; NS-0.7 19,816 pps at 0.9222 %), 111205901391 (152,329), 111410629923 (84,989, raw 249,027), 111721847147 (84,730, raw 250,980), 111410630005 (89,518 at 10:30 UTC), 111721847100 (113,289).
- Run 37231118477 started at 20:11 UTC; 37340158971 is the scheduled run. The amber/red bands are at 03:956-957.

D1: every anchor in 1a86561's Plan-Change trailer has a revision-15 row in CONSISTENCY §43.

D3: run through map.jsonc, #65's paths hit 02 §1.1 (HeliosModule.cmake) and 02 §1.4 (HeliosModular.cmake), and the delta is empty.

Links: 23 changed Markdown files, 210 relative links, 0 broken. All of docs/ plus README.md: 228 links, 0 broken. I self-tested the checker on a planted bad anchor and a missing file.

Lints and tests:
- `cmake -P tools/ci/run_lints.cmake`: all passed, twice (D6 snapshot 61 modules and PLAN-REV 15; conformance 0 failing, CONF-12 ×8 known failing).
- `scorecard.py check`: OK twice (19 criteria, 1 approved).
- On a configure-only linux-gcc tree (deleted afterwards), 31 lint CTests passed 31/31, twice: lint_status_unittest, lint_scorecard_unittest, lint_run_lints_status_python, lint_ip_names and its 12 fixtures, lint_licenses and its 9 fixtures, lint_test_namespaces, lint_conformance, lint_runner_policy and lint_runner_policy_unittest.

Merge: `git merge-tree --write-tree origin/agent/claude/wp-0.5r-cpu-gate 1a86561` (#65 head 333d148) is clean.

Hygiene: the owner's README.md:381 line from 1dac3d1 is intact, and neither the diff nor the PR body has a model identifier or a closing keyword.

CI on 1a86561: 14/14 green (run 37430683550).

## not_verified
- lint_scorecard and the other binary-dependent lint CTests: they need the built doctest inventories, so lint_scorecard failed in the configure-only tree as expected. CI ran them green.
- No MSVC, clang-cl or MinGW build: Markdown only, and CI built them.
- The final results of Nightly 37449776267's attempt 2 and of the scheduled win-gpu run 37478933707: both were still running at review time.
- Why the scheduled win-gpu run measured lower: unknown, as the ADR says.
- The owner-lines script (check_owner_lines.sh) is not in this container, so I checked the owner's commit 1dac3d1 by hand: its README line is unchanged.
- The PR's own full `ctest -L lint` 455/455: not reproduced, since that needs a full build.

## notes_for_lead
**Verdict.** 9/10 APPROVE with no blocking findings. Nothing was posted to GitHub; the review body is ready for you to post as a COMMENT review.

**The new data, which you asked me to rule on.** It is a nit, not a defect: every affected statement was true at the dated snapshot, and 09:1386-1388 names exactly which runs were read. But the plan text dates the refresh by day only, and today's Nightly 37449776267 ran on fb9517f itself.

- **Correction to the task text:** the windows-vs2026 `net_bench --gate` step failed on NS-0.7 (1.5222 % drops; its third hosted-Windows failure), not on NS-0.2. Hosted Windows NS-0.2 passed strictly at 125,630.
- **Caveat on that pass:** net_bench's cross-check WARNed that 35 % of the CPU per packet ran outside the measuring thread, against its 25 % limit. The gate still uses the thread figure. engine/net/README.md:253 calls whether NS-0.2 should count that work "the owner's call", so it is worth surfacing with owner question 1.
- **Rest of today's nightly:**
  - linux-gcc: stack 142,749; the perf step failed only `pcg_tests_perf` (K5b); `assetpipe_tests_perf` passed.
  - linux-asan: `schemac_sql_postgres` failed, and `editorui_ed15` timed out for the second time.
  - windows-go passed.
- **Re-run in progress:** someone re-ran the three failed jobs as attempt 2 at 14:23 UTC. If you want the addendum, wait for it or cite attempt 1 explicitly.
- **`win-gpu`:** dispatch 37473635682 on fb9517f passed every step: stack 119,214, NS-0.7 0 drops, hnoise AVX2 1.670 ms, GPU 19.66/22.41 tiles per 0.8 ms, overhead 0.122 ms. That overhead is just below the ADR's quoted 0.124–0.197 range, if you add this run. The scheduled `win-gpu` run 37478933707 was still running.
- **Recommendation:** a quick dated addendum before merge, or qualify 09:1388 "by 07:35 UTC" and leave the data to the next refresh. The addendum would cover the WP-0.13 row, §8.2's P0 list (NS-0.7 on hosted Windows; `ed15` twice) and PLAN.md §11. Your call; it does not change my score.

**Merge order with #65.** `git merge-tree` against #65 at 333d148 is clean.

- **If #67 lands first:** #65 needs only `Plan-Rev: 15` on its next commit (D3: `cmake/HeliosModule.cmake` maps to 02 §1.1 and `HeliosModular.cmake` to 02 §1.4; empty delta).
- **If #65 lands first:** these lines of #67 go stale and should be edited on rebase:
  - the §8.1 intro at 09:1383-1395 ("the open WP-0.5r PR (#65) rewrites or borders them…");
  - 09:1450 and §8.2 row 1 ("#65, in review");
  - the §5.10.4 (c) row at 09:1276;
  - PLAN.md:1025 ("In progress: #65");
  - the PR body's CONF-12 ×8 note.
- **Either order:** the fired-risks row at 09:1430 keeps "Owner approvals in force: NS-0.2" and K5b "armed" until the refresh after #65. #67's intro supersedes it, and #65 does not edit that row.

**Housekeeping.**
- The owner-lines script (check_owner_lines.sh) is not in this container, so I checked by hand that 1dac3d1's README line is intact.
- The worktree /home/user/wt/review-67-r2 and its build directory are removed, and the primary checkout is untouched.
- Downloaded job logs are under /tmp/claude-0/-home-user-HeliosEngine/28b618d3-b602-4e0c-963a-809be3b7f518/scratchpad/review-67-r2/logs/ if you want to re-grep them.
# challenge-67

## BLOCKING 1: 02 §1.1's new Deps key is contradicted by editorui's links, so the PR body's 'no deviation under that reading' is false
**location**: docs/plan/02-engine-runtime.md:47-51 (key) and :83 (toolsfw/editorui/edtools row); docs/plan/CONSISTENCY.md:1944; PR body section 2 item 6

**why**: Revision 15 makes the Deps column normative: 'Deps are the Helios modules a module may link, and the code may link fewer'. engine/editorui links helios::rhi and helios::render. Its row allows only 'world, assetpipe', and neither of those reaches rhi or render, even counting their own Deps transitively (world → ecs, records, physics, anim, nav, pcg, script; assetpipe → asset, records, pcg, physics, anim, nav). So the amended key is false for an existing module. That contradicts the PR body's 'No deviation under that reading' and the CONSISTENCY row's claim that this amends the plan to the code. The key also does not say whether Deps include what the listed modules reach. Read literally, records → core, ecs → math, net → math and gameplay → core/math/reflect also link modules their rows do not list.

**evidence**: git show fb9517f:engine/editorui/CMakeLists.txt: `DEPS helios::toolsfw helios::rhi helios::render helios::core helios::reflect`. engine/editorui/README.md:4-5: 'It depends on `toolsfw`, `rhi`, `render`, `core` and `reflect`'. 02:83 at 1a86561: `| 4 | toolsfw, editorui, edtools/* | … | ED | world, assetpipe | 07 |`. 02:429 already says the editor 'draws ImGui through the RHI'. engine/CMakeLists.txt:52 declares only `PEERS toolsfw` for editorui, and HeliosLayering checks only layer and peers, so configure passes, and no lint catches the contradiction.

**fix**: In the same 02 §1.1 amendment, add the graphics modules editorui links to the toolsfw/editorui/edtools row's Deps (e.g. 'world, assetpipe, render' with rhi, or a separate editorui entry). Also say in the key whether Deps include the lower-layer modules reachable through the listed ones, e.g. '…may link, directly or through the modules listed'. Then update CONSISTENCY.md:1944's description (it currently says asset, records and patch 'link within their rows' as the whole check) and PR body item 6.


## BLOCKING 2: CONSISTENCY §43's 04 §11.1 row says the Luau patch lines now name their files, but only the Jolt line changed
**location**: docs/plan/CONSISTENCY.md:1952; docs/plan/04-networking-and-servers.md:1852-1853

**why**: The D1 change record says 'The Luau and Jolt patch lines name their files'. The diff to 04 §11.1 only rewrites the Jolt line (to third_party/jolt/patches/0001-stable-order.patch). The Luau lines still say `third_party/luau/patches/codegen-fornloop-fuel, fuel-counter` (and det-math), not the vendored files 0001-codegen-fornloop-fuel.patch and 0002-fuel-counter.patch. So the record claims a change this PR did not make, and the plan's change log (D1) is the audit trail reviewers rely on.

**evidence**: git diff fb9517f 1a86561 -- docs/plan/04-networking-and-servers.md changes only the engine/server, apps/cellserver, apps/gateway and Jolt patch lines. At 1a86561, 04:1853 reads `third_party/luau/patches/codegen-fornloop-fuel, fuel-counter   native-code fuel parity; inline fuel counter (§10.2)`. git ls-tree fb9517f third_party/luau/patches/ lists 0001-codegen-fornloop-fuel.patch, 0002-fuel-counter.patch and 0003-asan-unpoison-freed-page.patch.

**fix**: Either rename the Luau line in 04 §11.1 to the two file names (det-math stays as a planned name), or change CONSISTENCY.md:1952 to 'The Jolt patch line names its file'.


### Skeptic verdicts on: 02 §1.1's new Deps key is contradicted by editorui's links, so the PR body's 'no deviation under that reading' is false
- refuted=False classification=blocking: The core claim holds at #67 head 1a86561. Revision 15 adds a normative definition to the 02 §1.1 key (02:48-51): "**Deps** are the Helios modules a module may link, and the code may link fewer". That makes the Deps column an upper bound. engine/editorui links helios::rhi and helios::render, but its row (02:83, "toolsfw, editorui, edtools/*") allows only "world, assetpipe", plus the same-row peer toolsfw.

Neither listed module reaches rhi or render, even if Deps are read transitively through the table:
- world → ecs, records, physics, anim, nav, pcg, script, with script → core, reflect, asset
- assetpipe → asset, records, pcg, physics, anim, nav
All of these are HEADLESS-side modules. So the amended key is false for an existing module even under the most lenient reading.

Nothing in the PR records this as a deviation. CONSISTENCY.md:1944 (the rev-15 02 §1.1 row) says "Deviation: None" and checks only asset, records and patch. editorui's README and 09 §8.1's WP-0.18 row describe the shell "on the Helios RHI", but neither lists the link as a 02 §1.1 deviation. #65 (333d148) does not touch engine/editorui, engine/CMakeLists.txt or the §1.1 table; its only 02 edit is the gate-history wording at about line 163. So nothing in flight fixes it.

The secondary point about ambiguity also holds. Read literally, without transitive closure, these modules link things their rows omit:
- records → core
- ecs → math
- net → math
- script → math
- gameplay → core, math, reflect
Other rows list transitive deps redundantly (pcg lists core, math, reflect, asset), so the table gives no consistent convention. The key does not say which reading applies.

One part of the finding is overstated. PR body item 6's "No deviation under that reading" is scoped to the asset/records row, and for those two modules it is true. The substantive error is elsewhere: the key is a general normative statement, it is contradicted by editorui, and the ledger records that 02 §1.1 anchor as having no deviation.

09 §5.10.1 makes table constants and "only"/"must"-type rules normative. Configure does not enforce the Deps column (engine/CMakeLists.txt:52 gives editorui only `PEERS toolsfw`), so no gate catches the contradiction. A plan-consistency PR that adds a normative rule existing code already breaks, and records "None", leaves a false entry in the ledger. That is why I classify it as blocking, even though the fix is a small doc edit.
- refuted=False classification=blocking: This should block the merge, but the finding is broader than the defect. The core is real and inside the PR's scope; two parts are overstated.

**The real defect.** Revision 15 adds new normative text to 02 §1.1's key, written by this PR: "Deps are the Helios modules a module may link, and the code may link fewer." That makes the Deps column an upper bound. Merged code breaks it:
- `engine/editorui` links `helios::rhi` and `helios::render`.
- Its public header `imgui_renderer.h` includes them, so the use is real, not a stale link.
- Its row, `toolsfw, editorui, edtools/*`, allows only `world, assetpipe`. Neither reaches rhi or render, even counting each listed row's own Deps transitively.
- The plan's own 02:429 says the editor "draws ImGui through the RHI", so after the PR the plan contradicts itself too.

Why this blocks rather than being a nit:
- The PR's deliverable is plan text, and its criterion is accuracy. Its CONSISTENCY §43 row for the changed anchor 02 §1.1 calls the change "amendments of the plan to the code". It records "Rework WPs: None" after checking only `asset`, `records` and `patch`.
- So the amended anchor now leaves an existing module in conflict with the plan. That conflict is not recorded as a deviation, is not named as rework, and the row was not fixed. That is a D1/D3 record miss (09 §5.10.2).
- The PR's "Deviation from the plan … none introduced" checkbox is therefore inaccurate under the PR's own new reading.
- The PR does not disclose this anywhere: not in its known limitations, not in CONSISTENCY, not in a README. No other WP owns it, and it is not style.

**What is overstated.**
1. The title calls the body's "No deviation under that reading" false. In context, that sentence is item 6's verdict on `asset` and `records`, and for those two it is true. The false statements are the key's table-wide normative claim and CONSISTENCY's "None".
2. The sub-point about `records → core`, `ecs → math`, `net → math` and `gameplay → core/math/reflect` holds only under a strict direct-link reading. The table's convention is plainly transitive: `world` lists `ecs`, not `core`. Under that reading all of these resolve. Saying "directly or through the modules listed" in the key is a wording nit, not a blocker.

**Mitigating facts.**
- The editorui row mismatch existed before this PR (old row `world, assetpipe`, same links). The #63 audit did not list it.
- Configure does not check Deps, so no build breaks; the PR's CONSISTENCY test column says so honestly.
- The fix is small: add `rhi, render` to the editorui row's Deps, or give editorui its own entry, then update the CONSISTENCY row's check list and the PR body. That supports a contained 7–8 defect, not a ≤ 6.

### Skeptic verdicts on: CONSISTENCY §43's 04 §11.1 row says the Luau patch lines now name their files, but only the Jolt line changed
- refuted=False classification=blocking: The finding is true at the reviewed head. I could not refute it. The PR's D1 change record for 04 §11.1 (CONSISTENCY.md:1952 at 1a86561) says "The Luau and Jolt patch lines name their files". In 04 §11.1 the PR only changes the Jolt line, from `stable-order` to `0001-stable-order.patch`. The Luau lines are byte-identical to the base fb9517f and to every commit on the PR branch (76068f1, 48c17e1, 1a86561). They still use the short names `codegen-fornloop-fuel, fuel-counter` (and `det-math`), not the vendored files.

I tested three ways to refute it, and none holds:
1. **The short names already count as file names.** The PR's own standard rules this out. The Jolt line used the same short form (`stable-order`), and the PR rewrote it to the file name. The PR's 02 §7.1 row (CONSISTENCY.md:1950) also calls that rewrite "04 §11.1's layout names the patch file". So by the PR's own usage, the short form does not name the file.
2. **The Luau files are named elsewhere.** 04 §10.2 (lines 1715 and 1720) does name `0001-codegen-fornloop-fuel.patch` and `0002-fuel-counter.patch`. That text predates this PR (the revision-8 row for 04 §10.2 records "the patch file names"). The 04 §11.1 row is about the §11.1 layout lines, so §10.2 does not make the claim true.
3. **A later commit on the branch fixed it.** No. The branch head is still 1a86561.

So the merged-changes log (§43, the audit trail the Integrator and reviewers rely on) would record a change to 04 §11.1 that the PR does not make. This is low severity: no normative rule, code or gate is affected, and either fix in the finding is a one-line edit. But this docs-only PR introduces a false statement into the plan's own consistency record, so it should be corrected before merge.
- refuted=True classification=nit: The inaccuracy is real. The finding describes the diff correctly: in 04 §11.1, PR #67 rewrote only the Jolt patch line. The Luau lines still name `det-math` (a planned patch with no file yet) and `codegen-fornloop-fuel, fuel-counter`, and the vendored files are `0001-codegen-fornloop-fuel.patch` and `0002-fuel-counter.patch`. So the last sentence of CONSISTENCY §43's 04 §11.1 row, "The Luau and Jolt patch lines name their files", overstates what this PR changed.

It is not merge-blocking under reviewer-rules.md's scoring:
1. **The normative plan text is right.** 04 §11.1 at head is internally consistent. The Luau lines identify the patches by their base names, which map one-to-one onto the vendored files, and 04 §10.2 (lines 1715 and 1720) already gives the full file names. The error sits only in the descriptive column of a change-log row.
2. **D1's required content is correct.** D1 (09:1112-1115) requires one CONSISTENCY line per anchor, naming the rework WPs the change opened. The 04 §11.1 row exists and covers the anchor. Its rework column ("None (as for 02 §1.1)") and test column are correct whether or not the Luau line changed.
3. **The PR description makes no false claim.** Its claims about 04 §11.1 are "an 04 §11.1 line" (item 8, engine/server) and "the Jolt patch line (111) shortened too" (round-1 nit 8). Both are true. It never says the Luau lines were changed, so this is not gate-gaming or a false statement about what the PR did or tested.
4. **No downstream effect.** It opens no rework WP, changes no gate, threshold or registry, and the fix is a one-word edit.
5. **Precedent.** On this same PR, round 1 classed a comparable overstatement in a CONSISTENCY §43 row as non-blocking nit 7: the 02 §1.1 row's test column said the layering check "enforces the rows". The blocking round-1 findings were wrong status facts: the NS-0.2 approval lapse and the list of merged PRs.

It should be fixed, either by changing the sentence to "The Jolt patch line names its file" or by renaming the Luau line to the two vendored file names. But it is a nit and does not stop a ≥ 9 score.

## NIT 1
03 §8.4 (docs/plan/03-rendering.md:1848-1850) says 'The lavapipe and Null goldens that every PR job compares are small (320 × 180 to 640 × 360) and are committed as ordinary files under `tools/rendertest/golden/`'. PR-tier gpu CTests also compare committed goldens elsewhere, at other sizes: engine/rhi/tests/golden/{triangle,textured_quad}.png (256 × 256), engine/editorui/tests/golden/*.png (ED-15, 1920 × 1080 and 3840 × 2160) and the Null trace goldens in engine/render/tests/golden/. The §9.1 layout (03:1904) also drops `tests/golden/` without naming these. Scope the sentence to helios-rendertest's goldens (e.g. 'helios-rendertest's lavapipe and Null goldens'). The tools/rendertest/golden PNGs are all 320 × 180, so the sentence is true for them.

## NIT 2
engine/physics/README.md:142-144 still says 'stock Jolt orders by `BodyID` in a fourth place the plan does not list'. After revision 15, 02 §7.1 (02:1903-1906) lists it. CONSISTENCY.md:1950 cites this README ('its README already recorded the fourth order'), and the PR updated the analogous notes in the pcg, reflect, schemac, patch and rendertest READMEs because leaving them 'would have made them false'. The physics one was missed.

## NIT 3
services/pkg/patchcdn/layout.go:141 ('creating a level-19 encoder per chunk') and :157 ('one zstd frame at level 19 (05 §7)') now contradict the amended 05 §7 (SpeedBestCompression, about level 11; the code at :144 uses SpeedBestCompression). 09 §5.10.4 (c) (09:1276) and the PR body list only engine/pcg/include/helios/pcg/kernel.h:12-14 and engine/CMakeLists.txt:45-48 as code comments left for follow-up. Add layout.go to that list.

## NIT 4
tools/schemac/README.md:231 says the lock is 'one per schema package', but 02 §3.4 (02:998-999) says 'several packages may share one lock', and schemas/sample/schema.lock.jsonc is shared by sample.common, sample.items and sample.ship (engine/toolsfw/CMakeLists.txt helios_schema(helios_toolsfw_samples … LOCK schemas/sample/schema.lock.jsonc)). Say 'one per schema package or package set'.

## NIT 5
Post-snapshot data (the PR discloses its 07:35 UTC snapshot). CI run 37416153106 on fb9517f was re-run: attempt 2 (13:48 UTC) passed all 12 jobs, including 'Backend services (Go, windows-latest)'. Attempt 1's job 112115095456 failed. PLAN.md:995-996 ('passed 11 of its 12 jobs'), PLAN.md:1040, 09:1424 (WP-0.16 row) and 09 §8.2:1442 describe attempt 1 only; consider '(attempt 1; passed on re-run)', as the same row already says for #63. Since the snapshot, two more win-gpu runs on fb9517f passed strictly: 37473635682 (workflow_dispatch: stack 119,214, raw 217,687; GPU full detail 19.66 and level-adaptive 22.41 tiles per 0.8 ms; CPU avx2 1.670 ms; NS-0.7 pass, cell 0.180 and gateway 0.101 cores) and 37478933707 (schedule: stack 115,685; NS-0.7 pass). Nightly 37449776267 (schedule, fb9517f, the first to include #62) was still in progress. 'Five win-gpu runs' (ADR-0.9c, 09 §7 K5b, WP-0.4/0.13 rows, PLAN.md) and 'no nightly has run yet on a main that includes #62' will be stale once these are counted. No figure in the PR is wrong for its snapshot.

## checked
Static review of `git diff fb9517f 1a86561` (24 Markdown files, +375/−307; origin/main confirmed at fb9517f after fetch). Files were read with git show only: no worktree, no checkout, no build, and nothing written outside the scratchpad. Nothing was posted to GitHub.

(1) Plan-vs-code amendments: each amended text was checked against the code at fb9517f/1a86561.
- 02 §1.1: the key was checked against every engine/*/CMakeLists.txt helios_module DEPS/PRIVATE_DEPS and engine/CMakeLists.txt's helios_declare_module table. asset links only core (+zstd), records links core/reflect/hxl, patch links core → monocypher+zstd, server links core/net/ecs/authority → natsc+yyjson, and server is L4 HEADLESS with PEERS authority. All of these match. editorui does not match: it links rhi and render (blocking 1).
- engine/server: ZoneHost, CellServer, GatewayServer, TickGraph, OrchestratorClient and ZoneInstance all exist in engine/server/include. WP-1.2 owns ZoneInstance (09:144).
- 02 §3.1 and 06 §1.1: the TagDef record in schemas/gameplay/tags.hschema has a `replicate: Audience` field. The records cook builds the tag table in byte-wise name order with ancestors (engine/records/src/cook.cpp:1002), and nothing reads .htags.
- 02 §3.4: the committed locks are schemas/gameplay and schemas/sample (shared by three packages). The helios_schema LOCK default and salting (tools/schemac/src/lock.cpp:214) match. TypeRegistry::publish refuses a duplicate id (engine/reflect/src/registry.cpp:56-60), tested in engine/reflect/tests/test_walker.cpp:228-230, and hosts abort on the error (apps/editor/main.cpp:68-71).
- 02 §3.6: the namespace is helios::refl, with headers in helios/reflect. 867 C++ lines contain `refl::`, which matches 'about 870'.
- 02 §5.8: initializePcgModule, the cpuGate fallback with a warning (engine/pcg/src/kernel.cpp:81-101), lazy init in activeKernel, the measurement guard in test_perf.cpp:37-38 and the bench's exit 2 all match.
- 02 §7.1: the patch file name, 389 lines (wc -l) and the fourth order (ContactConstraintManager pair order, ContactListener.h hunks) match. The every-build refusal of key 0 and duplicates is in engine/physics/src/grid.cpp:234, 262, 375, 504 and 517. The MANIFEST lists the patch beside Luau's.
- 03 §8.4/§9.1: tools/shaderc and tools/rendertest exist. tools/rendertest/golden/{null,vulkan-llvmpipe} hold 8+8 files, the PNGs are 320×180, and there is no LFS attribute (other goldens: nit 1).
- 04 §11.1: correct except the Luau line (blocking 2).
- 05 §7: SpeedBestCompression is in services/pkg/patchcdn/layout.go:144, kHeaderSize = 352 in engine/patch/include/helios/patch/manifest.h:51, the README heading 'The `.hman` v0 format' exists, and services/testdata/vectors/hman/ holds the vectors.
- 08 §2.5, 09 §2.3a WP-2.16b/WP-2.16c2, 09 §2 WP-1.23 and 09 §5.2a: the wording changes match.
- K5b: the 09 §7 row, the §7 intro and the ADR-0.9c Fired row, §3 and §6 item 1 match. ADR table figures are consistent with the status line (14.59–19.65 → 14.6–19.7).
- Every test name cited in the CONSISTENCY §43 rows exists via git grep, including records, schemac lock, pcg kernels, physics grid/determinism/stable-order, patchcdn TestChunkObjects/Windows and rendertest CTests. ecs type_key.h:18-21 matches the 02 §1.4 wording.

(2) NS-0.2 consistency: I read 09 §8.1 (intro, WP-0.4, WP-0.13 and fired-risks rows), §8.2 (P0 paragraph, row 8, owner list), §5.6, §7, PLAN.md status and §11, engine/net/README.md:219-238, CONSISTENCY §43, ADR-0.9c and the approval record (docs/evidence/…:153-162).
- Every edited place says the approval lapsed on 2026-10-04 after run 37231118477 (82,842), that runs 2–4 failed, that #62 was the second pass, that 37391930751 passed strictly (118,253), that the Linux half has not run, and that re-confirmation is the owner's decision.
- The only remaining 'in force' text is the fired-risks row (09:1430). The §8.1 intro (09:1392-1394) explicitly supersedes its K5b and NS-0.2 sentences, 'Owner approvals in force' included.
- Figures agree across files: hosted Windows below 100k in 4 of 7 runs, raw rates, NS-0.7 cell/gateway cores, NS-0.4 at 14 runs (9 scheduled and 5 dispatched), C4268 79+2=81.
- scorecard.jsonc, the evidence record and tools/scorecard/README.md were left unchanged, as the PR discloses.
- The 22-PR list matches `git log --first-parent 14e46dd..fb9517f` (23 commits = 22 PRs including #56, plus 1dac3d1).

(3) D1 trailer: I mapped every changed hunk of docs/plan/02–09 to its heading with a script. Changed: 02 §1.1, §1.4, §3.1, §3.4, §3.6, §3.8, §5.8, §7.1; 03 §8.4, §9.1; 04 §11.1; 05 §7; 06 §1.1; 07 §1.8.2, §2.2; 08 §2.5; 09 §2.2 (WP-1.23), §2.3a, §5.2a, §5.10.4, §7, §8.1, §8.2. This equals the head commit's Plan-Change list exactly, plus ADR-0.9c, PLAN.md, CONSISTENCY and PLAN-REV (14→15). Every anchor has a §43 row.

(4) No closing keyword (fix/fixes/fixed/close(s|d)/resolve(s|d) #N) appears in the added lines, the 11 commit messages or the PR body. No model identifier appears in the changed files or the PR body. The commits carry only the repository's standard co-author attribution trailer, which is on 86 commits already on main.

Also checked: git merge-tree of 1a86561 with #65's head 333d148 is clean. Actions API: run 37416153106 attempt history, plus the logs of the new win-gpu runs 37473635682 and 37478933707 via get_job_logs.

Not done: no build or ctest. The PR is Markdown-only and the main reviewer covers tests. I did not verify content/README fixture counts or the per-run hosted-nightly figures from job logs.
