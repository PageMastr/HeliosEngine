# tools/status — the 09 §8.1 status snapshot (D6, D7; WP-0.3)

`snapshot.py` is the tool 09 §5.10.2 D6 calls `tools/status/snapshot`. It absorbed
`check_status.cmake`, which is kept as a thin entry point that runs `snapshot.py check`.

```
python3 tools/status/snapshot.py                    # the D6/D7 check (same as `check`); exit 1 when stale
cmake -P tools/status/check_status.cmake            # the same check, the round-audit command the plan names
python3 tools/status/snapshot.py inventory          # the "Tree inventory (D6 check)" module list
python3 tools/status/snapshot.py inventory --write  # ... written into 09 §8.1 (that row only)
python3 tools/status/snapshot.py report --inventory inventory.json   # Markdown snapshot for the round
```

**`check`** fails when:
- a module directory under `engine/`, `apps/`, `tools/`, `services/cmd/`, `services/internal/` or
  `services/pkg/` is not named in 09 §8.1 (D6);
- such a directory has no WP in §8.1's "Tree inventory (D6 check)" row (named elsewhere, or `?`);
- §8.1 has no row whose first cell is "Tree inventory (D6 check)" (a renamed row would otherwise turn
  the WP check off);
- a module README in those directories, or `services/README.md`, has no `Plan-Rev: <n>` line, or one
  above `docs/plan/PLAN-REV` (D7). A README that is not UTF-8 counts as having none; a BOM is fine.

Hidden directories and `__pycache__` are not modules. Directories without a README must be named but
need no `Plan-Rev`. Modules the row names that are not in the tree (work in progress on other branches)
are reported as a note, as is a row that is not in the generated form. A missing §8.1 or PLAN-REV, or a
PLAN-REV that does not start with a number, is an error (exit 2), not a stale status (exit 1).

`check_status.cmake` looks for Python 3.10+ in every `PATH` directory for each name in turn (Windows:
`py -3`, `python`, `python3`; elsewhere `python3`, `python`) and runs each candidate before using it, so
the Microsoft Store aliases are skipped; `-DHELIOS_STATUS_PYTHON=<path>` overrides the search. It
reports exit 1 as "stale" and any other exit as "snapshot.py failed".

**`inventory`** regenerates the row from the tree, keeping the WP the row gives each module and `?` for a
new one, so that the Director fills it in; groups outside `engine/`, `apps/`, `services/` and `tools/`
that the row names (e.g. `content/…`) are kept after them. **`report`** adds what D6 asks the Director to refresh: test
counts per module and toolchain (from `tools/scorecard/scorecard.py inventory` files, which the
nightly publishes), the jobs of every workflow, WP branches in the local git refs (fetched; merged or not; no network) and
the rework WPs 09 §2 defines (open or done: §8.1 says which). Doctest binaries named after no module are
counted in an `(other)` row. The prose of §8.1 stays the Director's, and so do PLAN.md §11 and the §5.8
ratchet state, which D6 also names: they are not generated yet (see the WP-0.3 row of §8.1).

CTest `lint_status_unittest` (label `lint`) runs the seeded-fixture tests in `test_snapshot.py`. The
check itself runs at the round audit. Registering it with the build-independent lints
(`tools/ci/run_lints.cmake`) is WP-0.2's, once the in-progress modules are named, so that it does not fail
parallel work mid-round (`CONSISTENCY.md`, round 5).

## Plan conformance

Plan-Rev: 6
