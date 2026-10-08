# Owner answers, 2026-10-06 (verbatim, from the AskUserQuestion results in the lead's session)

The owner (the repository owner, the user) answered the Director's questions on 2026-10-06. The questions
were put by the lead (Director) at 14:24 UTC and 14:27 UTC; the answers are quoted exactly as the tool
returned them. "Selected" means the owner picked that option; the option's label and description are the
text the owner saw and chose.

## 1. NS-0.2 (answered 14:27 UTC)

Question as asked:
> NS-0.2's 2026-09-30 approval for hosted Linux lapsed on 2026-10-04. The first fixed-hardware re-test on
> win-gpu measured 82,842 packets per core, under the 100k bar. After the #62 rework, win-gpu passed strictly
> at 118,253. Hosted Linux swings between about 88k and 110–132k depending on the host it lands on. What
> should the record say?

Selected: **"(d) Re-test = post-#62 run"**
Option description (as shown to the owner):
> You meant 'the re-test' to be the run after the second pass, not the first win-gpu run. The approval never
> lapsed, the strict pass at 118,253 satisfies it, and the rows are reworded to match.

(The other options were: (a) Re-confirm as is; (b) Re-confirm with a floor; (c) Leave it lapsed.)

## 2. Harrow High

First ask (14:24 UTC), question:
> Harrow High: 02 §5.5 keeps the station interior a container in zone 'tallis' (Phase 1). ED-20, NS-3.11 and
> NS-4.6 treat Harrow High as its own single-cell zone. Which should the plan say?

Owner's answer: **"explain this with more detail and re-ask me"**

Re-ask (14:27 UTC), question:
> Harrow High is the orbital station over planet Harrow. Its concourse is BENCH-1: 200 avatars, 60 NPCs, 40
> screens. M1 'Descent' starts with undocking a Kestrel from it.
>
> Today the plan describes it two ways:
> • As a container (02 §5.5, Phase 1). Tallis space and Harrow's surface are one zone, `tallis`, run by one
> cell. The station interior is an object container inside it, like Saltmarch. That is what makes the
> orbit-to-interior descent (BENCH-2) seamless: there is no server handoff in Phase 1.
> • As a zone of its own, in three later criteria. ED-20 (Ph3, editor collaboration) runs "a second zone
> session (Harrow High)"; a zone session exists only for a zone. NS-3.11 (Ph3, live server migration)
> migrates "a 500-bot, 20k-entity single-cell hub (Harrow High)" 100 times. NS-4.6 (Ph4, crash recovery)
> kills the cell of "single-cell Harrow High" 10 times under 500 bots.
>
> The question: does the hub get its own zone (and server cell) once the engine can hand players between
> zones (Ph2–3), or does it stay a container in `tallis` forever? Making it a zone from Phase 1 isn't viable:
> zone handoff doesn't exist until Phase 2, so M1's undock would break.

Selected: **"(A) Container, zone from Ph3 (Recommended)"**
Option description (as shown to the owner):
> Phase 1 is unchanged. From Phase 3 the station is its own single-cell zone, so the densest social space
> gets a dedicated server and scales on its own, and the three criteria stand as written. Cost: in Phase 3
> its files move to their own zone folder, and docking or undocking becomes a zone transition (the client
> prefetches the destination; the handoff hitch budget applies).

Option preview (as shown to the owner):
```
Ph1-2:  zone `tallis` = space + Harrow surface
          ├─ container: Harrow High interior
          └─ container: Saltmarch
Ph3+:   zone `tallis`      (orbit, 4 cells in NS-3.11/4.6)
          └─ container: Saltmarch
        zone `harrow-high` (1 cell, the hub)

ED-20, NS-3.11 (a), NS-4.6 (b): unchanged
02 §5.5: + "Harrow High becomes its own zone in Ph3"
WP impact: a Ph3 content move + dock = zone transition
```
Owner's follow-up message: "You can now continue with these answers in mind."

## 3. #66's starter-art proposals (answered 14:27 UTC)

Question as asked:
> Which of #66's starter-art proposals should become work items? (Pick any; none is required.)

Options shown: "Third-party origin", "Size check", "Rename speederD", "LFS server check" (the Saltmarch
light prop could be noted under Other).

Owner's answer: **"[No preference]"** (no option selected; none becomes a work item now).

## 4. Provenance origin for agent-written content (answered 14:27 UTC)

Question as asked:
> How should agent-written sample content (text, data and other non-code assets) be recorded in its
> provenance .meta?

Selected: **"Keep 'original'"**
Option description (as shown to the owner):
> Today's choice: origin 'original', author 'Claude Code, agent-authored', MIT, the same as agent-written
> code.

## 5. Nightly re-run (owner message, 14:24 UTC, verbatim)

> i am re-running the failed nightly jobs, as sometimes they fail due to resources not being available, wait
> for that run then we will re-evaluate if the nightly job is actually a fail, or was resource starved.

Result (read by the lead from the GitHub API, Nightly run 37449776267 on fb9517f):
- Attempt 1 (scheduled, 10:26 UTC): 3 of 14 jobs failed: windows-vs2026 (net_bench --gate: NS-0.7 trunk
  19,696 pps, 1.5222 % drops; both NS-0.2 halves passed strictly, stack 125,630), linux-asan
  (schemac_sql_postgres: Unix-socket path 108 bytes > 107; editorui_ed15 timeout at 600 s), linux-gcc perf
  (pcg_tests_perf, by design K5b; assetpipe_tests_perf in the doctest --perf pass).
- Attempt 2 (owner's re-run, 14:23 UTC): windows-vs2026 job 112320174721 **passed** (NS-0.7 passed);
  linux-asan job 112320174603 failed only schemac_sql_postgres (527/528; editorui_ed15 passed; the UBSan
  report at tools/schemac/src/sema.cpp:757 recurs, non-fatal); linux-gcc job 112320176183 failed only
  pcg_tests_perf (avx2 2.50529 ms vs 0.5 ms); assetpipe_tests_perf passed; hosted-Linux NS-0.2 stack
  114,640 packets per core, NS-0.7 20,000 pps, 0.0000 % drops, gates passed.
- Attempt 3 (the owner re-ran linux-asan again, 15:29 UTC): in progress when this file was written.
- Lead's reading: NS-0.7 on hosted Windows, editorui_ed15's timeout and assetpipe_tests_perf were
  resource-starved (each passed on re-run). schemac_sql_postgres is a real, deterministic failure (P0, fix
  PR to come from the lead); pcg_tests_perf is by design (K5b).
