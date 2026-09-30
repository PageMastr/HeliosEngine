**Adversarial review round 3 (delta since 6e3e311): score 9/10 — APPROVE**

Reviewed head `0dbdc33614d3251eeb7b5bf85c6d961c543f0fdf`. There are two new commits since round 2:
- `9706f95` changes comments and docs only, in `grid.cpp`, `grid.h` and the README;
- `0dbdc33` is a bare merge of main 5bc959f.

Round 2's B1 is fixed. The nit is taken, and the new wording is true against Jolt's source. One copy of the unqualified claim remains, in a test comment (nit 1).

## Blocking findings

None.

## Non-blocking nits

1. **`engine/physics/tests/test_grid.cpp:357-358` still has the unqualified claim.**
   - It reads: "...and never clamped a kinematic one: Jolt clamps only dynamic bodies while stepping."
   - This is the third copy of the claim that round 2's nit covered. Round 2 listed only `grid.h` and the README, so its absence from `9706f95` is my omission, not the implementer's.
   - It is true while every Helios body uses discrete motion quality. For consistency, it could end: "Jolt clamps only dynamic bodies while stepping (with discrete motion quality)".
2. **Optional wording in `grid.h:173-174`.**
   - It reads: "with discrete motion quality (every Helios body), Jolt never clamps a kinematic body later".
   - The CCD clamp depends on the motion quality of the *other* body: a dynamic `LinearCast` body hits the kinematic one. The kinematic body's own quality plays no part. The parenthetical makes the condition global, so the sentence is true.
   - A later reader who gives only dynamic ships `LinearCast` could still take "with discrete motion quality" to be the kinematic body's. The README's wording is exact ("a kinematic body hit by a `LinearCast` (CCD) body"). "while no Helios body uses `LinearCast` (CCD)" would say the same thing in `grid.h`.

## What I verified

**1. The new `grid.cpp:289-291` comment matches Jolt's source.**
- "debug builds assert":
  - `Body::ApplyBodyCreationSettings` calls the asserting setters (`Body.cpp:424-425`);
  - they assert at `MotionProperties.h:48` (linear) and `:61` (angular).
- "Release builds clamped a dynamic body only at its first step". Both per-step clamps are gated on `body.IsDynamic()`:
  - gravity and drag: `PhysicsSystem.cpp:778-786` calls `ApplyForceTorqueAndDragInternal`, which clamps at `MotionProperties.inl:146-148`;
  - `JobIntegrateVelocity`: `PhysicsSystem.cpp:1623-1628`, before `AddRotationStep` and `delta_pos`.

  Nothing clamps at creation in release: `SetLinearVelocity` only asserts and stores.
- "never a kinematic one":
  - `MoveKinematic` is unclamped;
  - the only other path is CCD. `sSolveCCDContact<EMotionType::Kinematic>` writes body 2 back through `Set*VelocityClamped` (`PhysicsSystem.cpp:2179-2184`, dispatched at `:2304-2309`).
  - It runs only when a dynamic, non-sensor body has `LinearCast` quality (`:1655-1657`). No Helios body has, so the past-tense claim about Helios holds.
- "as setVelocity() does through BodyInterface": `grid.cpp:459` calls `SetLinearAndAngularVelocity`, which clamps every non-static body (`BodyInterface.cpp:561-570`).

**2. The qualified claims in `grid.h:172-174` and `README.md:60-65` are true.**
- `grep -rn 'MotionQuality\|LinearCast'` over the whole repository, excluding `third_party/`, finds only the new README sentence. There are no hits in `engine/` or `apps/`, so nothing sets `mMotionQuality`.
- `BodyCreationSettings.h:104` defaults to `EMotionQuality::Discrete`.
- "every Helios body uses discrete motion quality" therefore holds, and so does the README's "its one other clamp ... is unreachable".

**3. No stale copy of round 1's claim remains.**
- I grepped the PR's 4 files and `engine/physics` for "first step", "unchanged" and "clamp". I also grepped the repository (outside `third_party/`) for "clamp(s|ed) at the first step", "stepped behaviour is unchanged", "clamped before integrating" and "release builds clamp".
- There are no "unchanged" hits.
- Every "first step" hit is now scoped to dynamic bodies, with kinematic bodies stated as never clamped: `grid.cpp:290`, `README.md:61-62` and `test_grid.cpp:357`.
- The remaining "clamp" hits are accurate: `grid.cpp:269`, `grid.h:85-86` and `:172-174`, `README.md:58-65`, and `test_grid.cpp:355`, `:379` and `:395`. The one exception is the qualifier in nit 1.

**4. `0dbdc33` is a clean bare merge.**
- Its parents are `9706f95` and main `5bc959f`. `git merge-tree --write-tree 9706f95 5bc959f` gives `3f39956`, exactly `0dbdc33^{tree}`.
- `git diff origin/main 0dbdc33` touches only #36's 4 files (+67/−5): `README.md`, `grid.h`, `grid.cpp` and `test_grid.cpp`.
- Between `6e3e311` and `0dbdc33`, the only non-comment change is main's #37 hunk in `engine/pcg/bench/hnoise_bench.cpp`, which came in through the merge.

**5. Merge precondition.**
- main is still **5bc959f** (`git ls-remote`).
- `git merge-tree --write-tree origin/main 0dbdc33` = `3f39956` = the head tree, so a squash onto main gives exactly the tree that was tested.

**6. Tests.** The delta's own code is comments only, but the merge brought in a code change. So I built `physics_tests` anyway: gcc 13.3, RelWithDebInfo, own directory, `-j2`.
- 0 build warnings.
- `ctest -R '^physics_tests'` passes **twice**: `physics_tests` and `physics_tests_perf`.
- A direct run passes 41/41 cases and 44,510 assertions, the same as round 2.

**7. Lints and hygiene.**
- `cmake -P tools/ci/run_lints.cmake`: all lints passed.
- No closing keywords in the two new commits, the PR's other commits, the PR title or the PR body.
- The trailers are present on both new commits (WP-0.9, Criteria, Plan-Rev 11). No Plan-Change is needed, since docs/plan did not change.
- The PR body's round-3 section describes the delta accurately, and it asks for the corrected squash wording.

**8. CI on `0dbdc33`.** CI_STATE_PLACEHOLDER

## Not verified

- I did not rebuild Debug+ASan this round, because the PR's delta is comments only. The PR body reports it passing twice.
- I did not compile MSVC, clang-cl or MinGW locally. I rely on PR CI.
- The CCD path is from reading the source only. Helios has no API that enables `LinearCast`, so I could not exercise it.

---
_Generated by [Claude Code](https://claude.ai/code)_
