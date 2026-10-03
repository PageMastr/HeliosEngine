# Runbook: the `win-gpu` self-hosted runner (the owner's Windows PC)

WP-0.4; [09 §5.4a](../plan/09-roadmap-and-process.md) (the policy), K33 (the risk). This runbook is for the
repository owner. It covers the machine setup, the hook and firewall from `tools/ci/runner/`, switching the jobs on,
checking the result, and removing or rotating the runner.

## What runs on the PC, and what protects it

[`.github/workflows/win-gpu.yml`](../../.github/workflows/win-gpu.yml) runs one job every day at 07:37 UTC and when
you start it from the Actions tab ("Run workflow" on `main`). A queued job waits up to 24 h, so a PC that is off at
07:37 UTC runs the job when it comes back on. The job:

1. checks that the runner account is not an administrator and cannot list other users' profile folders, and that
   it cannot open a TCP connection to the LAN's default gateway;
2. builds the `windows-vs2022` preset (MSBuild, RelWithDebInfo) and runs the `gpu`-labelled CTests on the real GPU:
   the Vulkan golden images (`rendertest.vulkan.*`, Khronos-validated), `rhi_tests_gpu`, `pcg_gpu_tests` and
   `rhi_triangle_smoke`;
3. runs ADR-0.9c's `pcg_hnoise_bench --hardware-gpu` and a strict `net_bench --gate` (NS-0.2's re-test on fixed
   hardware), then fails if the goldens or the bench ran on a software rasterizer or found no GPU;
4. uploads the `results-win-gpu` artifact (30 days).

It never runs for pull requests. The layers, from the outside in:

| Layer | What it stops | Where |
|---|---|---|
| Triggers and guards | The workflow has only `schedule` and `workflow_dispatch`, and the job needs `main` and the repository variable `HELIOS_WIN_GPU=enabled`. Every PR runs `tools/ci/check_runner_policy.py` (CTest `lint_runner_policy`), which fails any workflow that could reach this runner and breaks a rule: other triggers, a missing guard, `secrets`, a token broader than `contents: read`, actions not pinned to a commit | `.github/workflows/`, `tools/ci/` |
| The job-started hook | A workflow file on an unreviewed branch decides its own triggers: a push to such a branch could ask for this runner before any review. The hook, installed on the PC, fails every job that is not a `schedule`, `push` or `workflow_dispatch` run of `refs/heads/main`, before any step runs | `D:\helios-ci\hooks\job-started.ps1` |
| The account | Jobs run as `helios-ci`, a standard user with no access to your profile, browser data, SSH keys or Git credentials | Windows |
| The firewall | `helios-ci` cannot reach the home LAN (other PCs, the router's admin page, a NAS); the internet stays open | `tools/ci/runner/firewall.ps1` |
| The wipe | The hook empties `D:\helios-ci\work` before each job, so no job sees an earlier job's files | the hook |

What stays possible (K33's residual risk): code that has passed review and merged runs as `helios-ci` with internet
access. Such code could change what the account itself owns: its profile, its PowerShell profile, the runner
installation (`D:\helios-ci\runner`, including `.env`) and the runner's credentials there. It cannot reach your
account, your files, administrator rights or the LAN. If you suspect it happened, follow "Rotate" below.

## Names (binding)

| Item | Value |
|---|---|
| Windows account | `helios-ci`: local, standard user (Users group only) |
| Folders | `D:\helios-ci\runner` (runner), `D:\helios-ci\work` (job work directory), `D:\helios-ci\hooks` (hook) |
| Runner | name `helios-win-gpu`; labels `self-hosted`, `Windows`, `X64` (automatic) and `win-gpu` |
| Service | `actions.runner.*`, running as `.\helios-ci` |
| Switch | repository variable `HELIOS_WIN_GPU` = `enabled` (optional: `HELIOS_WIN_GPU_ADAPTER`) |
| Hook | `ACTIONS_RUNNER_HOOK_JOB_STARTED=D:\helios-ci\hooks\job-started.ps1` in `D:\helios-ci\runner\.env` |
| Firewall rules | group `Helios CI runner: LAN block for helios-ci` |

The checker counts every label of the runner: a job whose `runs-on` uses only `self-hosted`, `windows`, `x64` and
`win-gpu` reaches this PC, not only one that names `win-gpu`. If you add labels to the runner, update
`RUNNER_LABELS` in `tools/ci/check_runner_policy.py` in the same change.

## Setup

Steps 1, 2, 3 and 5 are the owner's steps of 2026-10-03; they are repeated here so that this page is complete.
Run the commands in an elevated PowerShell (Run as administrator) unless a step says otherwise.

**Already done on 2026-10-03?** Then the runner `helios-win-gpu` is registered and Idle, and what is left is:
narrow the `hooks` ACL (the `icacls` line in step 3), install Go and set the execution policy (the `GoLang.Go` and
`Set-ExecutionPolicy` lines of step 4, then restart the service), then steps 6, 7 and 8 and the verification
checklist.

### 1. Prerequisites

Windows 10 or 11 Pro with BitLocker, the latest GPU driver (Vulkan 1.3), and a data volume `D:` with about 100 GB free.

### 2. The account

```powershell
New-LocalUser -Name helios-ci -Password (Read-Host -AsSecureString 'Password') -PasswordNeverExpires -AccountNeverExpires
Add-LocalGroupMember -Group (Get-LocalGroup -SID S-1-5-32-545).Name -Member helios-ci   # Users
Get-LocalGroupMember -SID S-1-5-32-544                                                  # Administrators: no helios-ci
```

### 3. Folders, ACLs, BitLocker

Create `D:\helios-ci\runner`, `work` and `hooks`. Remove inheritance and give `helios-ci`, Administrators and
SYSTEM full control on `runner` and `work`. Turn BitLocker on for `D:` with auto-unlock. For `hooks`, give
`helios-ci` read and execute only, so that no job can rewrite the hook:

```powershell
icacls D:\helios-ci\hooks /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "helios-ci:(OI)(CI)RX"
```

### 4. Tools, machine-wide

Every tool must be installed for all users, because the runner service runs as `helios-ci`:

```powershell
winget install -e --id Git.Git --scope machine
winget install -e --id Kitware.CMake --scope machine
winget install -e --id Python.Python.3.12 --scope machine
winget install -e --id GoLang.Go --scope machine          # configure requires Go when CI is set (tools/conformance)
winget install -e --id KhronosGroup.VulkanSDK              # sets VULKAN_SDK for all users; the goldens need its layer
winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
Set-ExecutionPolicy -Scope LocalMachine RemoteSigned       # Windows PowerShell's Restricted default blocks the hook and every step
```

Ninja is not needed: `win-gpu.yml` uses the Visual Studio generator, so no third-party action has to set up the MSVC
environment. Go is needed only so that configure succeeds; the job does not run Go. `python` must be on the machine
`PATH` (the installer's "Add python.exe to PATH"; the job fails when `python` is missing or is only the Microsoft Store
alias). Restart the runner service (or the PC) after installing, so that it sees the new `PATH` and `VULKAN_SDK`.

### 5. Register the runner

GitHub → Settings → Actions → Runners → New self-hosted runner → Windows x64. Download the runner into
`D:\helios-ci\runner`, then run `config.cmd` interactively with `--url`, `--token` (from that page), `--name
helios-win-gpu`, `--labels win-gpu`, `--work D:\helios-ci\work` and `--runasservice`, and enter `.\helios-ci` and its
password at the prompts. Never paste the token or the password anywhere else. The registration token is used once and
is not stored; the runner keeps its own credentials in `D:\helios-ci\runner`.

### 6. Install the job-started hook

Copy the hook from a clone of the repository at `main` (a clone, not a browser download: a downloaded `.ps1` carries a
mark that RemoteSigned refuses; `Unblock-File` removes it):

```powershell
git clone https://github.com/PageMastr/HeliosEngine.git $env:USERPROFILE\src\HeliosEngine   # or git pull in an existing clone
Copy-Item $env:USERPROFILE\src\HeliosEngine\tools\ci\runner\job-started.ps1 D:\helios-ci\hooks\job-started.ps1
notepad D:\helios-ci\runner\.env
```

In `.env`, add this line (keep any existing lines), save, then restart the service:

```
ACTIONS_RUNNER_HOOK_JOB_STARTED=D:\helios-ci\hooks\job-started.ps1
```

```powershell
Get-Service actions.runner.* | Restart-Service
```

The hook is not updated by merges: when `tools/ci/runner/job-started.ps1` changes on `main`, copy it again. To check
that the installed copy is current:

```powershell
(Get-FileHash D:\helios-ci\hooks\job-started.ps1).Hash -eq (Get-FileHash $env:USERPROFILE\src\HeliosEngine\tools\ci\runner\job-started.ps1).Hash
```

### 7. Block the LAN for `helios-ci`

```powershell
cd $env:USERPROFILE\src\HeliosEngine
.\tools\ci\runner\firewall.ps1 -WhatIf     # shows the rules it would add
.\tools\ci\runner\firewall.ps1
```

It adds outbound block rules that apply only to `helios-ci`'s processes: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16,
169.254.0.0/16, fc00::/7, fe80::/10, and the `LocalSubnet` keyword (which also covers LAN devices' global IPv6
addresses). Re-running it replaces its own rules. Later, for WP-0.4's lab (a Linux host for NS-0.3), allow that host
and nothing else:

```powershell
.\tools\ci\runner\firewall.ps1 -AllowAddress 192.168.1.50
```

With `-AllowAddress` the `LocalSubnet` rule is left out (a block rule cannot be split around a keyword), so devices on
the LAN's global IPv6 prefix are reachable again; the script warns. Name resolution keeps working, because Windows'
DNS Client service sends the queries, not `helios-ci`. If you recreate the account, run the script again: the rules
name the account's SID.

### 8. Switch the jobs on

GitHub → Settings → Secrets and variables → Actions → Variables → New repository variable: `HELIOS_WIN_GPU` =
`enabled`. If the PC has more than one GPU, also set `HELIOS_WIN_GPU_ADAPTER` to part of the discrete GPU's name
(for example `RTX`); the RHI then uses that adapter. Then Actions → win-gpu → Run workflow, on `main`.

## Verification checklist

Do this after the setup and after any change to the PC, the hook or the firewall.

- [ ] The runner page shows `helios-win-gpu` Idle with the labels `self-hosted`, `Windows`, `X64`, `win-gpu`.
- [ ] `Get-CimInstance Win32_Service -Filter "Name LIKE 'actions.runner.%'" | Select-Object Name, StartName, State`
      shows `.\helios-ci`, Running.
- [ ] `Get-LocalGroupMember -SID S-1-5-32-544` does not list `helios-ci`.
- [ ] `Get-ExecutionPolicy -List` shows `RemoteSigned` for `LocalMachine`.
- [ ] `icacls D:\helios-ci\hooks` gives `helios-ci` `(RX)` only; `.env` has the hook line; the hash check above is True.
- [ ] `Get-NetFirewallRule -Group 'Helios CI runner: LAN block for helios-ci'` lists 3 rules (2 with `-AllowAddress`),
      Enabled, Outbound, Block; `Get-NetFirewallProfile | Select-Object Name, Enabled` shows every profile enabled.
- [ ] As `helios-ci` (`runas /user:helios-ci powershell`):
  - `Get-ChildItem C:\Users\<you>` fails with access denied;
  - `Test-NetConnection <your router's IP> -Port 80` fails, `Test-NetConnection github.com -Port 443` succeeds;
  - `git --version; cmake --version; python --version; go version; $env:VULKAN_SDK` all answer.
- [ ] A dispatched run on `main`: "Set up runner" prints `job-started hook: workflow_dispatch job on refs/heads/main;
      removed N entries ...`; "Runner isolation" and "LAN egress blocked" pass; the job builds; "The goldens and the
      bench ran on a hardware GPU" names your GPU.
- [ ] The run's `results-win-gpu` artifact holds `run.json`, `host.json`, `ctest.xml`, `gates/`, `inventory.json`,
      `hnoise-win-gpu.json` and `rendertest/`.
- [ ] The hook refuses unreviewed code. Push a throwaway branch whose only change is a workflow
      `.github/workflows/hook-test.yml` with `on: push` and one job (`runs-on: [self-hosted, win-gpu]`, one step
      `run: echo this must not run`). Its run must fail at "Set up runner" with `refused: ref 'refs/heads/<branch>'`,
      and the echo must not appear. Delete the branch afterwards. (A run that is merely skipped by an `if:` does not
      test the hook.)

## Day to day

- **Pause**: set `HELIOS_WIN_GPU` to anything else, or delete it: the job is skipped, not queued. Stopping the service
  also works, but scheduled jobs then queue for 24 h and fail.
- **Schedule**: change the cron in `win-gpu.yml` through a PR.
- **"entries survived the wipe"** at "Set up runner": a leftover process holds files in `D:\helios-ci\work`. Reboot
  (or end `helios-ci`'s processes); the next job wipes again.
- **`rhi_triangle_smoke`** opens a window. A service runs without a desktop, so it may fail on this runner; that is a
  finding to report, not a reason to run the runner interactively.
- **Golden images**: the goldens were blessed on lavapipe (`golden/vulkan-llvmpipe/`); a real GPU may differ beyond
  the ꟻLIP tolerance. A failure is a finding for WP-0.12 (per-driver goldens), not a setting to relax.
- **"did not run on a hardware GPU"**: the goldens or the bench used a software rasterizer, or saw no GPU at all.
  With two GPUs, set `HELIOS_WIN_GPU_ADAPTER` (step 8). If the log says `no Vulkan physical devices` or `GPU:
  unavailable`, the driver does not offer Vulkan to a service's session; that is a finding to report (the options
  are the GPU-P VM of 09 §5.4a or a different runner setup, the owner's decision), not a reason to run the runner
  from an interactive logon.

## Rotate or remove

Rotate (periodically, or at once if you suspect a job misbehaved):

1. Set `HELIOS_WIN_GPU` to `disabled`.
2. GitHub → Settings → Actions → Runners → `helios-win-gpu` → Remove; copy the removal token. On the PC:
   `cd D:\helios-ci\runner; .\config.cmd remove --token <token>` (this also removes the service).
3. Suspected misuse: remove the account and its profile (`Remove-LocalUser helios-ci`, then delete
   `C:\Users\helios-ci`), create it again (step 2), and delete everything in `D:\helios-ci\runner` and
   `D:\helios-ci\work`. Otherwise just reset its password: `Set-LocalUser helios-ci -Password (Read-Host -AsSecureString)`.
4. Register again (step 5), add the hook line to `.env` again (step 6), re-run `firewall.ps1` (step 7: a new account
   has a new SID), go through the checklist, and set `HELIOS_WIN_GPU=enabled`.

Remove for good: delete the `HELIOS_WIN_GPU` variable, `config.cmd remove --token <token>`,
`.\tools\ci\runner\firewall.ps1 -Remove`, `Remove-LocalUser helios-ci`, and delete `C:\Users\helios-ci` and
`D:\helios-ci`.

## Not covered yet (WP-0.4)

- **NS-0.3's cross-host half** needs a Linux lab host on this LAN (`helios-cell` and `helios-gateway`), its address in
  `firewall.ps1 -AllowAddress`, and a job; until then it stays unmeasured (`scorecard.jsonc`).
- **The nightly scorecard does not read `results-win-gpu` yet**: the results exist as artifacts only.
- **Hyper-V GPU-P VM isolation** (09 §5.4a, preferred when it works) has not been tried; the account isolation above
  is the default until a WP-0.4 ADR records that decision.
- **The REF/MIN/SERVER purchase list** (WP-0.4) is a separate deliverable.
