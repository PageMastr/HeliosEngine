# NS-0.2: passed on the repository owner's approval (2026-09-30)

| | |
|---|---|
| Criterion | **NS-0.2** (04 §11.4): "Loopback 100k pps/core without loss". Class N (09 §5.6), Phase 0, owner WP-0.13 |
| Approved by | The repository owner (the user), on 2026-09-30 |
| Recorded by | The Director (Claude Code agent), on 2026-10-03, under 09 §5.6 "Owner approval" (plan revision 12) |
| Clause covered | The encrypted HTP stack's 100k packets per core, on the hosted nightly runners only |
| Re-test owed | `net_bench --gate`, without `--advisory`, on fixed hardware: WP-0.4's `win-gpu` runner and a Linux lab host (owners WP-0.4 and WP-0.13) |
| Registry | `scorecard.jsonc`, entry NS-0.2: status `approved`, `owner_approval` 2026-09-30, `advisory` `net.ns02.stack_packets_per_core`, one follow-up |

## The owner's decision, verbatim

> go ahead and pass it for now. And note the reason is due to inconsistent resource availablity causing occasional skewed results. and that it passes everywhere else. but that at a later date testing will be completed with fixed hardware and if more work is needed it will get a second pass then, but that as of right now, human review approves it for passing.

The quotation keeps the owner's spelling.

## What the approval covers, and what it does not

It covers one clause: the rate of encrypted HTP packets through the full stack (`net_bench`'s "NS-0.2 HTP
stack" line, perf metric `net.ns02.stack_packets_per_core`), as measured by the hosted nightly runners
(`linux-gcc` on `ubuntu-24.04`, `windows-vs2026` on `windows-latest`). On those runs the nightly passes
`net_bench --gate --advisory ns02-stack`, which prints that rate with an `NS-0.2 advisory:` line instead of
failing on it. The threshold stays 100k.

It does not cover, and the gate still fails on:
- raw loopback datagrams below 100k per core (`net_bench`'s "NS-0.2 socket" line, and `net_tests`'
  `perf: NS-0.2: loopback 100k pps per core without loss`);
- **any loss**, of raw datagrams or of encrypted packets: "without loss" is not approved;
- an encrypted stack that does not connect or sends nothing;
- **NS-0.7**, which the same `net_bench --gate` run measures (see below);
- any run other than the hosted nightly: `net_bench --gate` on a developer machine, the lab or `win-gpu`
  gates every clause, the stack's rate included.

## The measurements behind it

### Hosted nightly runners, 2026-09-27 to 2026-10-02

Every `net_bench --gate` run the hosted runners completed, read from the job logs. "Stack" is encrypted
packets delivered per core; every run delivered every packet it sent. NS-0.7 is 600 s at 20,000 pps of
1,200 B datagrams.

| Date (UTC) | Run | Event, commit | Raw datagrams per core | Stack per core | NS-0.7 | Gate |
|---|---|---|---|---|---|---|
| **Linux** (`ubuntu-24.04`, GCC) | | | | | | |
| 2026-09-27 09:37 | 36309067888 | schedule, 13cb382 | 200,641 | 88,248 | 20,000 pps, 0 drops | fail (NS-0.2) |
| 2026-09-27 19:26 | 36343324805 | dispatch, 23dec29 | 200,195 | 88,344 | 20,000 pps, 0 drops | fail (NS-0.2) |
| 2026-09-27 20:32 | 36344479712 | dispatch, 6ea95be | 451,382 | 132,039 | 20,000 pps, 0 drops | pass |
| 2026-09-28 10:06 | 36406371574 | schedule, cfd2e15 | 254,942 | 110,536 | 20,000 pps, 0 drops | pass |
| 2026-09-29 10:12 | 36552271589 | schedule, cfd2e15 | 200,668 | 88,316 | 20,000 pps, 0 drops | fail (NS-0.2) |
| 2026-09-30 10:03 | 36698342872 | schedule, cfd2e15 | 198,969 | 88,405 | 20,000 pps, 0 drops | fail (NS-0.2) |
| 2026-09-30 17:47 | 36747179407 | dispatch, 9568bc4 | 200,785 | 88,324 | 20,000 pps, 0 drops | fail (NS-0.2) |
| 2026-10-01 10:30 | 36847862239 | schedule, 5bc959f | 201,174 | 89,107 | 20,000 pps, 0 drops | fail (NS-0.2) |
| 2026-10-02 10:07 | 36992204809 | schedule, 5bc959f | 414,908 | 120,732 | 20,000 pps, 0 drops | pass |
| **Windows** (`windows-latest`, VS 2026) | | | | | | |
| 2026-09-27 19:33 | 36343324805 | dispatch, 23dec29 | 249,027 | 85,399 | 20,000 pps, 0 drops | fail (NS-0.2) |
| 2026-09-27 20:52 | 36344479712 | dispatch, 6ea95be | 223,776 | 84,970 | **19,690 pps, 1.55 % drops** | fail (NS-0.2 and NS-0.7) |

The scheduled Windows nightlies before #31 could not configure MSBuild, so they never ran `net_bench`.

What the table shows:
- **The level follows the host, not the code.** The same commit measured both sides of 100k: cfd2e15 gave
  110,536 and then 88,316 and 88,405, and 5bc959f gave 89,107 and then 120,732. On Linux the runs fall into
  two groups that the raw rate tracks too: about 200k raw with about 88k encrypted (6 runs), and 255k–451k
  raw with 110k–132k encrypted (3 runs). This is the inconsistent resource availability the owner names.
- **Below 100k is not rare on hosted runners.** It was 6 of 9 Linux runs and both Windows runs, not
  occasional ones; the record states it as measured.
- Raw datagrams passed on every hosted run (198,969–451,382 per core), and no hosted run lost a packet.

### The development container ("passes everywhere else")

The shared 4-vCPU dev VM, GCC 13 RelWithDebInfo:
- 2026-09-25 (`engine/net/README.md`): raw 478,863 datagrams per core; encrypted stack 1,162,752 of
  1,162,752 delivered, 117,236 packets per core; NS-0.7 600 s at 20,000 pps with 0 drops. Gate passed.
- 2026-09-30: one 10 s stack run **lost 64 packets** (reported in the Director's paused-state notes; the
  run's other numbers were not kept). A loss is not covered by this approval: such a run fails the gate with
  or without `--advisory`.
- 2026-10-03: the two 10-minute gate runs below, on this record's branch.

### Two 10-minute gate runs on the dev VM, 2026-10-03

Built from this record's branch (GCC 13 RelWithDebInfo, `build/<task>-gcc`) on the shared 4-vCPU dev VM
while other agents compiled on it (two to four `cc1plus` processes at about 90 % CPU each), which is the
resource contention the owner names. The runs were serial, with nothing else of the recording agent's
running except a once-a-minute `/proc/loadavg` sampler.

| | Run 1: `net_bench --gate` (what a local run gates) | Run 2: `net_bench --gate --advisory ns02-stack` (the nightly's command) |
|---|---|---|
| Time (UTC) | 01:42:55–01:53:08 | 01:56:15–02:06:28 |
| Load average before (1, 5, 15 min) | 5.98, 6.09, 4.93 | 3.90, 4.27, 4.70 |
| Load average after | 4.16, 4.83, 4.94 | 5.29, 4.70, 4.77 |
| 1-minute load during (sampled each minute) | 3.74–6.56 | 3.15–6.20 |
| Raw datagrams | 1,000,000 of 1,000,000, 518,573 per core | 1,000,000 of 1,000,000, 512,424 per core |
| Encrypted stack (10 s) | 1,137,664 of 1,137,664, **120,504** per core | 1,087,232 of 1,087,232, **117,594** per core (the `NS-0.2 advisory:` line says the rate was not gated) |
| NS-0.7 (600 s) | 11,999,997 of 11,999,997, 20,000 pps, 0 drops, cell 0.24 and gateway 0.26 cores | 11,999,992 of 11,999,992, 20,000 pps, 0 drops, cell 0.24 and gateway 0.26 cores |
| Verdict | `gates PASSED`, exit 0, 612 s | `gates PASSED (NS-0.2's HTP stack rate advisory)`, exit 0, 612 s |

Both runs passed every clause, the strict one included, and neither lost a packet. They do not reproduce the
hosted shortfall: this VM's raw rate (about 515k per core) is that of the hosted runs that passed, not of the
about-200k hosts that measured 88k encrypted.

## NS-0.7 is not covered

NS-0.7 (one trunk connection, 20k pps of 1,200 B for 10 min, < 0.1 % drops, ≤ 1 core per side) failed once
on hosted Windows: run 36344479712, 2026-09-27, delivered 11,813,909 of 11,999,992 datagrams (19,690 pps,
1.5507 % drops; cell thread 0.26 cores, gateway thread 0.15). It passed on the other Windows run that day and
on every Linux run. Nothing in this approval relaxes it: `--advisory ns02-stack` leaves NS-0.7's verdict as it
was, and because NS-0.7 shares the `net_bench --gate` run with NS-0.2, an NS-0.7 failure still fails that
night's run for both criteria. Whether the Windows failure repeats will show once the Windows nightly runs
`net_bench` again; 04 §2.6's fallback is for a failure that persists after profiling.

## The re-test owed

When WP-0.4 registers the owner's Windows PC as the `win-gpu` runner (and a Linux lab host is on its network),
the nightly gains a `net_bench --gate` step there **without** `--advisory`. If the encrypted stack measures
below 100k per core on that fixed hardware, the approval lapses: NS-0.2 is failing again and the stack gets a
second pass (WP-0.13). The follow-up is listed in `scorecard.jsonc` and in 09 §8.1 until it is done.

## How it is enforced

- `engine/net/bench/net_bench.cpp` and `ns02_gate.h`: `--advisory` takes only `ns02-stack` and only with
  `--gate` (exit 2 otherwise), and it relaxes only the encrypted stack's rate. `net_tests`' doctest
  `NS-0.2 stack verdict: the rate gates by default and only the rate can be advisory` and the
  `net_bench_advisory_*` CTests check both.
- `.github/workflows/nightly.yml`: the two hosted `net_bench --gate` steps pass `--advisory ns02-stack`.
- `scorecard.jsonc`: NS-0.2 cites this record. The checker (`tools/scorecard/scorecard.py`) requires a dated
  record under `docs/evidence/`, a test that still runs and a follow-up. The nightly report reads "passed
  (owner approval 2026-09-30, evidence docs/evidence/ns-0.2-owner-approval-2026-09-30.md)" and shows the
  stack's level each night.
- `tools/scorecard/test_nightly_workflow.py` pins the scope: the only advisory in the nightly is
  `ns02-stack` on those two steps, and NS-0.2 is the only criterion with an approval.
