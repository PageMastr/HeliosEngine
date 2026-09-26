# tools/milestone — milestone validation on Windows (WP-0.3)

`validate.ps1` is the script of 09 §5.9. The user runs it on their Windows machine at each milestone
and pastes back the report it writes. It can start from plain PowerShell:

```bat
git pull
powershell -ExecutionPolicy Bypass -File tools\milestone\validate.ps1 -Milestone M0
```

What it does:

1. **Developer shell.** Unless `cl.exe` is already on `PATH`, it finds the newest Visual Studio in
   `[17.14, 19.0)` with `vswhere` and enters its developer shell (`Enter-VsDevShell`, x64).
2. **Build and test.** `cmake --preset windows-msvc-release`, `cmake --build --preset …` and
   `ctest --preset … --output-junit`. The build is incremental, so a stale or missing tree is rebuilt
   and an up-to-date one costs seconds.
3. **Toolset.** MSVC 19.44 (VS 2022 17.14, the floor) or any 19.5x (VS 2026, the primary) passes. Anything
   else stops the script with the install fix. CMake records the version in
   `build\windows-msvc-release\CMakeFiles\<cmake-version>\CMakeCXXCompiler.cmake`, not in `CMakeCache.txt`
   as 09 §5.9 says, so the script reads the cache first and then that file.
4. **Scripted checks.** It builds the Go services into `build\go\` and runs `helios-backend.exe run --seed dev`
   with a fresh data directory under `build\milestone\`. It measures the first-run and warm start times and
   the idle working set including PostgreSQL against 05 BE-A1 (≤ 30 s, ≤ 5 s, ≤ 500 MB), logs in as `dev1`,
   starts `helios-cell.exe` and `helios-gateway.exe` against the backend, waits for a zone tick line and the
   gateway's listening line, and asks the session service for a connect token. Everything it starts is
   stopped at the end. Logs are in `build\milestone\logs\`.
5. **Interactive part.** It opens the launcher when one is built (WP-0.17) and lists the milestone's steps
   from 09 §5.9's table, for the user to answer in the pasted report.
6. **Scorecard.** With Python 3 installed, it runs the doctest XML collection and `tools/scorecard/report.py`
   for the milestone's phase over this machine's results. The report shows Linux as unmeasured, since only
   Windows results exist here.
7. **Report.** `build\milestone-<M>.txt` holds the toolset, every check as PASS, FAIL or SKIP with its
   measurement, the interactive steps and the scorecard. The Integrator commits the pasted report as
   `docs/evidence/milestone-<M>.txt`, the M-class record that `scorecard.jsonc`'s
   `EXIT-0.windows-validation` item looks for.

Options: `-NoInteractive` (do not open the launcher) and `-SelfTest` (the script's own checks, no build).
The exit code is 1 when any check fails.

The script is ASCII-only and uses no PowerShell 7 syntax, because the user runs it under Windows PowerShell
5.1, which reads a script without a byte-order mark in the ANSI code page. CTest `lint_milestone_selftest`
(label `lint`) runs `-SelfTest` under Windows PowerShell on Windows and `pwsh` elsewhere. The self-test
covers the toolset rule, the version lookup, the JUnit summary, the report format and the encoding. The
build, the service checks and the interactive part run only on a real Windows machine.

## Plan conformance

Plan-Rev: 6
