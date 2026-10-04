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

It never runs for pull requests. **The repository is public and anyone may fork it.** A pull request from a fork,
and a push to any branch, runs that branch's own workflow files, and such a file can ask for this runner
(`runs-on: [self-hosted, win-gpu]`) before anyone has reviewed it. Nothing in `main` can prevent that; the hook on the
PC is what ends those jobs, so **the runner service stays stopped while the hook is not installed** (steps 5-9).
The layers, from the outside in:

| Layer | What it stops | Where |
|---|---|---|
| Fork approval | GitHub starts no workflow from an outside contributor's fork until you approve the run (step 8). By default GitHub asks only about first-time contributors | GitHub settings |
| Triggers and guards | The workflow has only `schedule` and `workflow_dispatch`, and the job needs `main` and the repository variable `HELIOS_WIN_GPU=enabled`. Every PR runs `tools/ci/check_runner_policy.py` (CTest `lint_runner_policy`), which fails any workflow that could reach this runner and breaks a rule: other triggers, a missing guard, `secrets`, a token broader than `contents: read`, actions not pinned to a commit. It checks what is merged, not what a fork or a branch runs | `.github/workflows/`, `tools/ci/` |
| The job-started hook | A workflow file on a fork or an unreviewed branch decides its own triggers and `runs-on`. The hook, installed on the PC, ends every job that is not a `schedule`, `push` or `workflow_dispatch` run of `refs/heads/main` (a fork's pull request runs as `pull_request` on `refs/pull/<n>/merge`): it stops the runner's worker process (`Runner.Worker.exe`), which runs the job's steps, before the first of them. Failing the hook alone would not be enough: the runner would still run the job's steps marked `if: always()` (or `failure()`, `!cancelled()`) and its actions' `pre:` steps | `D:\helios-ci\hooks\job-started.ps1` |
| The account | Jobs run as `helios-ci`, a standard user with no access to your profile, browser data, SSH keys or Git credentials, nor, once step 4b has closed them, to your folders elsewhere on the drives (Windows opens every folder created at the root of a drive to all accounts) | Windows; step 4b |
| The firewall | `helios-ci` cannot reach the home LAN (other PCs, the router's LAN address, a NAS), LAN multicast and broadcast (mDNS, SSDP, LLMNR) or overlay networks such as Tailscale; the internet stays open, including the router's public address (below) | `tools/ci/runner/firewall.ps1` |
| The wipe | The hook empties `D:\helios-ci\work` before each job, so no job sees an earlier job's files | the hook |

What stays possible (K33's residual risk): code that has passed review and merged runs as `helios-ci` with internet
access. Such code could change what the account itself owns: its profile, its PowerShell profile, the runner
installation (`D:\helios-ci\runner`, including `.env`) and the runner's credentials there. It could switch the hook
off through `.env`, through the PowerShell profile (which the runner loads before the hook), or with
`Set-ExecutionPolicy -Scope CurrentUser Restricted`: the CurrentUser scope takes precedence over the LocalMachine
policy of step 4, so the hook would no longer start, and only an execution policy set by Group Policy prevents that
(the checklist checks the scope and the profiles). It cannot reach your profile or the LAN, nor, after step 4b, your
folders elsewhere on the drives. It cannot reach administrator rights either, **as long as you never run anything from
`D:\helios-ci` in an elevated window once the service has run**: not `config.cmd`, `run.cmd` or a program in `bin`,
and not from a folder that "Rotate" sets aside. The account can change every file there, `config.cmd` included, and
what you run elevated runs with your rights ("Rotate or remove" below never does it). Like every local account, it can
still read what Windows leaves open to all users (the machine-wide tools, which the build needs, `C:\ProgramData` and
whatever step 4b's audit lists as `read`), create files and folders in `C:\ProgramData` and `C:\Windows\Temp` and
folders at the root of a drive, read and change `C:\Users\Public` (Windows lets interactive and service logons write
there, so keep nothing in it that you would mind losing or that you run), and read and write a FAT32 or exFAT drive
(most USB sticks) while one is plugged in. It can reach programs on the PC itself that listen on the network,
including on `localhost` (Windows Firewall does not filter loopback): keep such services (databases, dev servers,
remote-control tools) behind a password, or stop them while the runner is enabled. The firewall blocks private
addresses only, so the router's public (WAN) address stays reachable: many routers show their admin page there to
clients on the LAN, and NAT loopback passes port-forwarded traffic on to the LAN device behind it (a NAS), often with
the router's LAN address as the source. That depends on the router; the checklist tests it, and if the admin page or a
forwarded service answers, turn off the router's remote administration or NAT loopback (or the port forward). If you
suspect misuse, follow "Rotate" below.

Code that ran as `helios-ci` while the runner had no working hook was not reviewed at all, and it could have done all
of the above: that is a runner online before step 9 ("Already done" below), and the jobs behind "`File doesn't exist`"
and "could not end the job" under "Day to day". Changed runner files, a planted profile or a copy of the runner's
credentials leave nothing that a checklist could reliably find, and a job can delete its own log, so treat it as
suspected misuse: "Rotate", which replaces the registration, the account and `D:\helios-ci`. That code could also
change everything else the account could change at the time: before step 4b, every folder that step 4b's audit lists
as `write`. Closing such a folder afterwards keeps what was planted in it; step 4b says what to do.

## Names (binding)

| Item | Value |
|---|---|
| Windows account | `helios-ci`: local, standard user (Users group only) |
| Folders | `D:\helios-ci\runner` (runner), `D:\helios-ci\work` (job work directory), `D:\helios-ci\hooks` (hook) |
| Runner | name `helios-win-gpu`; labels `self-hosted`, `Windows`, `X64` (automatic) and `win-gpu` |
| Service | `actions.runner.*`, running as `.\helios-ci`; startup type Manual until the hook and the firewall are in place (steps 5-9), then Automatic |
| Switch | repository variable `HELIOS_WIN_GPU` = `enabled` (optional: `HELIOS_WIN_GPU_ADAPTER`) |
| Hook | `ACTIONS_RUNNER_HOOK_JOB_STARTED=D:\helios-ci\hooks\job-started.ps1` in `D:\helios-ci\runner\.env` |
| Firewall rules | group `Helios CI runner: LAN block for helios-ci` |

The checker counts every label of the runner: a job whose `runs-on` uses only `self-hosted`, `windows`, `x64` and
`win-gpu` reaches this PC, not only one that names `win-gpu`. If you add labels to the runner, update
`RUNNER_LABELS` in `tools/ci/check_runner_policy.py` in the same change.

## Setup

Steps 1, 2, 3 and 5 are the owner's steps of 2026-10-03; they are repeated here so that this page is complete.
Run the commands in an elevated PowerShell (Run as administrator) unless a step says otherwise.

**Already done on 2026-10-03?** Then the runner `helios-win-gpu` is registered and Idle, without the hook, so a
fork's pull request or a push to any branch can run code on this PC now. **First stop the service and keep it from
starting** (it stays registered; GitHub shows it Offline, and jobs for it wait in the queue):

```powershell
Get-Service actions.runner.* | Stop-Service
Get-Service actions.runner.* | Set-Service -StartupType Manual
Get-Service actions.runner.* | Select-Object Name, Status, StartType      # Stopped, Manual
```

Whatever ran on it since then ran as `helios-ci` without the hook and without the firewall, and nobody reviewed it.
Such code can leave behind what the hook does not undo: a PowerShell profile, which the runner loads before the hook;
changed runner files or a changed `.env` in `D:\helios-ci\runner`, where the account has full control; a copy of the
runner's credentials, with which another machine can take this runner's jobs. The runner writes one
`Worker_<time>-utc.log` per job into `_diag` and keeps them 30 days; list them for your report:

```powershell
# Already done: the jobs that ran on the runner without the hook (one Worker_<time>-utc.log per job, kept 30 days).
Get-ChildItem D:\helios-ci\runner\_diag -Filter 'Worker_*.log' | Select-Object Name, Length, LastWriteTime
```

A job can also delete its own log, so an empty list does not prove that none ran. Start over either way: install Go
and set the execution policy (the `GoLang.Go` and `Set-ExecutionPolicy` lines of step 4), require approval for fork
pull requests (step 8), then follow "Rotate" below from its step 2. It removes the runner with Windows' own tools,
never with `config.cmd` from the old runner folder, which that code could have changed; it replaces the account and
`D:\helios-ci`, installs the hook and the firewall before it registers the runner again from a fresh download in a new
folder, and goes on with step 4b, step 9 (which starts the service), the checklist and step 10.

Until step 4b, that code could also change every folder that step 4b's first audit lists as `write`, including
`C:\VulkanSDK` (step 4 created it open, and the Vulkan loader loads its validation layer into your own validated
runs), and read every folder the audit lists. Closing or moving such a folder keeps whatever was planted in it, so do
step 4b's "After unreviewed code" list for that first audit before you close anything.

### 1. Prerequisites

Windows 10 or 11 Pro with BitLocker, the latest GPU driver (Vulkan 1.3), and a data volume `D:` with about 100 GB free
that holds nothing but `D:\helios-ci` (09 §5.4a: the work directory on its own volume). Windows opens what is created
at the root of a volume to every account (step 4b), so keep your own files off `D:`; if it already holds some, move
them, or close them in step 4b.

### 2. The account

```powershell
New-LocalUser -Name helios-ci -Password (Read-Host -AsSecureString 'Password') -PasswordNeverExpires -AccountNeverExpires
Add-LocalGroupMember -Group (Get-LocalGroup -SID S-1-5-32-545).Name -Member helios-ci   # Users
Get-LocalGroupMember -SID S-1-5-32-544                                                  # Administrators: no helios-ci
```

### 3. Folders, ACLs, BitLocker

Create `D:\helios-ci\runner`, `work` and `hooks` without inherited permissions. Administrators and SYSTEM get full
control of all three; `helios-ci` gets full control of `work`, read and execute only on `hooks`, so that no job can
rewrite the hook, and nothing on `runner`: `config.cmd` (step 5) gives the account full control of `runner` and
`work` through a local group of its own (`GITHUB_ActionsRunner_G...`), and until then no process of the account can
change the runner's files that you run elevated in step 5. `D:\helios-ci` itself would inherit *Modify* for
Authenticated Users from `D:\`, with which the account could rename it while nothing in it is open and put a tree of
its own, `hooks` included, in its place; it gets Administrators and SYSTEM, and Users may only list it. Turn
BitLocker on for `D:` with auto-unlock.

```powershell
New-Item -ItemType Directory -Force -Path D:\helios-ci\runner, D:\helios-ci\work, D:\helios-ci\hooks | Out-Null
icacls D:\helios-ci /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-545:(RX)"
icacls D:\helios-ci\runner /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F"
icacls D:\helios-ci\work /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "helios-ci:(OI)(CI)F"
icacls D:\helios-ci\hooks /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "helios-ci:(OI)(CI)RX"
```

The entries name the account by its SID. An account created again under the same name has a new SID, so "Rotate"
makes these folders again for the new account.

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
alias). The runner service sees the new `PATH` and `VULKAN_SDK` the next time it starts (step 9).

### 4b. Close your own folders to `helios-ci`

The account protects your profile (`C:\Users\<you>`), not the rest of the drives. The root of an NTFS volume (`C:\`,
or a data volume that Windows formatted) gives Authenticated Users *Modify* and Users *Read & execute* on everything
created below it, so on a default installation `helios-ci` can read, change, delete or encrypt `D:\Photos`,
`D:\Backup`, `C:\dev` or `C:\src`. Only the profiles and `D:\helios-ci\runner`, `work` and `hooks` (step 3) are closed
to it. The audit below leaves out Windows' own folders: `helios-ci` can only read most of them, but it can create
files in `C:\ProgramData` and `C:\Windows\Temp` and change `C:\Users\Public` ("What stays possible" above). A FAT32 or
exFAT drive has no permissions at all. The worst case is a clone of this repository outside your profile: merged code
could change it, and the next time you build or validate from it (09 §5.9), that code runs as you. **Keep every clone
of this repository inside your profile**, the one you validate from included (step 6 puts its clone there).

List what `helios-ci` can open at the root of each local drive (elevated; it reads ACLs and changes nothing):

```powershell
# Step 4b audit: files and folders at the root of each local drive that helios-ci can read or change.
$account = Get-CimInstance Win32_UserAccount -Filter "LocalAccount = TRUE AND Name = 'helios-ci'"
if (-not $account) { throw 'There is no local account helios-ci yet: do step 2 first' }
$ci = $account.SID
# helios-ci and the groups that every account's token holds (a service's too).
$anyone = @{ 'S-1-1-0' = 'Everyone'; 'S-1-2-0' = 'LOCAL'; 'S-1-5-6' = 'SERVICE'; 'S-1-5-11' = 'Authenticated Users'
    'S-1-5-15' = 'This Organization'; 'S-1-5-32-545' = 'Users'; 'S-1-5-113' = 'Local account'; $ci = 'helios-ci' }
$write = [int][Security.AccessControl.FileSystemRights]('WriteData, AppendData, WriteExtendedAttributes, ' +
    'WriteAttributes, DeleteSubdirectoriesAndFiles, Delete, ChangePermissions, TakeOwnership') -bor 0x50000000
$read = [int][Security.AccessControl.FileSystemRights]'ReadData' -bor 0x90000000
# (0x50000000: generic all and generic write; 0x90000000: generic all and generic read)
$everyDrive = '$Recycle.Bin', 'System Volume Information', 'pagefile.sys', 'swapfile.sys'
$windowsDrive = 'Windows', 'Windows.old', 'Program Files', 'Program Files (x86)', 'ProgramData', 'Users',
    'Documents and Settings', 'PerfLogs', 'Recovery', 'Boot', 'bootmgr', 'BOOTNXT', 'Config.Msi', 'OneDriveTemp',
    'hiberfil.sys', 'DumpStack.log', 'DumpStack.log.tmp', '$WinREAgent', '$SysReset', '$GetCurrent', '$Windows.~BT',
    '$Windows.~WS'
$found = @(foreach ($disk in Get-CimInstance Win32_LogicalDisk -Filter 'DriveType = 2 OR DriveType = 3') {
    $root = $disk.DeviceID + '\'
    if (-not $disk.FileSystem) {
        Write-Warning "$root has no file system (locked, or no medium): audit it again once it is open"
        continue
    }
    if ($disk.FileSystem -notin 'NTFS', 'ReFS') {
        [pscustomobject]@{ Path = $root; Access = 'write'; Who = "every account ($($disk.FileSystem) has no permissions)" }
        continue
    }
    foreach ($item in Get-ChildItem -LiteralPath $root -Force -ErrorAction SilentlyContinue) {
        if ($item.Name -in $everyDrive -or $item.FullName -eq 'D:\helios-ci' -or
            ($disk.DeviceID -eq $env:SystemDrive -and $item.Name -in $windowsDrive)) { continue }
        try { $acl = Get-Acl -LiteralPath $item.FullName -ErrorAction Stop }
        catch { [pscustomobject]@{ Path = $item.FullName; Access = '?'; Who = $_.Exception.Message }; continue }
        $bits = 0
        $who = @()
        $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
        if ($owner -and $anyone.ContainsKey($owner)) { $bits = $write; $who += "owner: $($anyone[$owner])" }
        foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
            $sid = $rule.IdentityReference.Value
            $mask = [int]$rule.FileSystemRights -band ($write -bor $read)
            if ($rule.AccessControlType -eq 'Allow' -and $anyone.ContainsKey($sid) -and $mask) {
                $bits = $bits -bor $mask
                $who += "$($anyone[$sid]): $($rule.FileSystemRights)"
            }
        }
        if ($bits) {
            $access = if ($bits -band $write) { 'write' } else { 'read' }
            [pscustomobject]@{ Path = $item.FullName; Access = $access; Who = $who -join '; ' }
        }
    }
})
if ($found.Count) { $found | Format-Table -Wrap -AutoSize } else { 'Nothing at the root of a local drive is open to helios-ci' }
```

It leaves out Windows' own folders and `D:\helios-ci`. `write` means that `helios-ci` can change or delete the item (or
its permissions), `read` that it can read it; `Who` names the entries that allow it (Allow entries only: a Deny entry
does not take a line off the list). `?` means the ACL could not be read: check that item with `icacls`. For every
line:

- **Your files, or a clone of a repository**: move it into your profile, or close it. A file at a drive root: move it
  into your profile or into a folder you close. To close a folder, give yourself access first: with User Account
  Control, your membership in Administrators counts only in an elevated window, so once Users and Authenticated Users
  are gone, your everyday access comes from this entry alone (if you elevate with another account's password, set `$me`
  to your own account's SID instead, which `whoami /user` shows in a window that is not elevated):

  ```powershell
  $dir = 'D:\Photos'                                                    # a folder the audit listed
  $me = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value   # you (elevated, it is still your account)
  icacls $dir /inheritance:d                    # turn the inherited entries into the folder's own
  icacls $dir /grant "*${me}:(OI)(CI)F"
  icacls $dir /remove:g '*S-1-1-0' '*S-1-2-0' '*S-1-5-6' '*S-1-5-11' '*S-1-5-15' '*S-1-5-32-545' '*S-1-5-113' helios-ci
  ```

- **A tool that the job uses** (for example `C:\VulkanSDK`, or a tool installed outside `C:\Program Files`):
  `helios-ci` has to read it, so `read` is fine. `write` is not: code that `helios-ci` plants there runs as you the
  next time you use the tool. Keep Users' read and execute and remove the rest:

  ```powershell
  $dir = 'C:\VulkanSDK'
  icacls $dir /inheritance:d
  icacls $dir /remove:g '*S-1-1-0' '*S-1-2-0' '*S-1-5-6' '*S-1-5-11' '*S-1-5-15' '*S-1-5-113' helios-ci
  icacls $dir /grant:r '*S-1-5-32-545:(OI)(CI)RX'
  ```

- **Something you do not need** (for example a driver installer's leftover `C:\AMD` or `C:\NVIDIA`): delete it. An
  item whose `Who` says `owner: helios-ci` was created by the runner account: find out why before you delete it.
- **`D:\helios-ci.old-<time>`**, the old tree that "Rotate" sets aside: leave it until Rotate's step 5 deletes it,
  and never run anything in it.
- **A FAT32 or exFAT drive**: unplug it while the runner is enabled, or keep nothing on it that is private or that you
  run.

**After unreviewed code.** If code that nobody reviewed ran as `helios-ci` before you closed these folders (a runner
online before step 9, as under "Already done", or a suspected misuse), every `write` line of the first audit was open
to it and every line could be read. Closing or moving a folder keeps whatever was planted in it, so before you close
or move anything:

- **A tool listed as `write`** (for example `C:\VulkanSDK`, whose validation layer the Vulkan loader loads into your
  own validated runs): delete the folder without running anything in it, its uninstaller included (that would run
  as you): `cmd /c rd /s /q "\\?\C:\VulkanSDK"`. Then install the tool again (step 4's line; if `winget` still finds
  the old installation, add `--force`) and close the new folder as above.
- **A clone of a repository**: clone it again into your profile instead of moving the old one, and do not build, run
  or open anything from the old clone; delete it with `rd` as above.
- **Secrets in any listed folder**, `read` lines included (a token in a clone's `.git\config`, `.env` files, keys):
  treat them as read, and replace them.
- **Anything else you run** from a `write` folder, from `C:\Users\Public` or from a FAT32 or exFAT drive that was
  plugged in: restore it from a backup made before the runner came online (2026-10-03), or install it again.

Run the audit again until its `write` lines are at most such drives and set-aside trees, and its `read` lines are only
tools the job uses. It reads the top level only: a folder you closed stays closed below, unless something below it
grants access itself. A folder you create at a drive root later starts open again: run the audit after creating one
(the checklist repeats it).

### 5. Register the runner

Register only from a fresh download into the new `runner` folder that step 3, or "Rotate", has just made (it may
already hold step 6's `.env`). You run `config.cmd` elevated, so its folder must never have been open to
`helios-ci`; once the service has run there, the account can change every file in it. To register again, follow
"Rotate", which makes a new folder.

GitHub → Settings → Actions → Runners → New self-hosted runner → Windows x64. In the elevated window, go to
`D:\helios-ci\runner` (in place of the page's `mkdir actions-runner; cd actions-runner`) and run the page's Download
commands there, including the line that checks the download's SHA-256. Then run `config.cmd` interactively with
`--url`, `--token` (from that page), `--name helios-win-gpu`, `--labels win-gpu`, `--work D:\helios-ci\work` and
`--runasservice`, and enter `.\helios-ci` and its password at the prompts. Never paste the token or the password
anywhere else. The registration token is used once and is not stored; the runner keeps its own credentials in
`D:\helios-ci\runner`.

`config.cmd` starts the service at once, and the runner takes a job that waits for it within seconds. On Windows it
never writes `.env`, so in a runner folder without one the service runs with no hook. Where you can, do steps 6 and 7
before this step, as "Rotate" does: the download holds no `.env` either, so the service that `config.cmd` starts then
reads your hook line, and the firewall rules already apply to its account. Either way, stop it and keep it from
starting until step 9:

```powershell
Get-Service actions.runner.* | Stop-Service
Get-Service actions.runner.* | Set-Service -StartupType Manual
```

### 6. Install the job-started hook

Copy the hook from a clone of the repository at `main` (a clone, not a browser download: a downloaded `.ps1` carries a
mark that RemoteSigned refuses; `Unblock-File` removes it):

```powershell
git clone https://github.com/PageMastr/HeliosEngine.git $env:USERPROFILE\src\HeliosEngine   # or git pull in an existing clone
Copy-Item $env:USERPROFILE\src\HeliosEngine\tools\ci\runner\job-started.ps1 D:\helios-ci\hooks\job-started.ps1
notepad D:\helios-ci\runner\.env
```

In `.env` (Notepad offers to create it: the runner never does), add this line (keep any lines you wrote before)
and save. The runner reads `.env` when the service starts (step 9):

```
ACTIONS_RUNNER_HOOK_JOB_STARTED=D:\helios-ci\hooks\job-started.ps1
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
169.254.0.0/16, 100.64.0.0/10 (carrier-grade NAT and overlay networks such as Tailscale), 224.0.0.0/4 and
255.255.255.255 (multicast and broadcast: mDNS, SSDP, LLMNR), fc00::/7, fe80::/10, ff00::/8, and the `LocalSubnet`
keyword (which also covers LAN devices' global IPv6 addresses). Re-running it replaces its own rules, and it checks
its input before it removes anything. Later, for WP-0.4's lab (a Linux host for NS-0.3), allow that host and nothing
else:

```powershell
.\tools\ci\runner\firewall.ps1 -AllowAddress 192.168.1.50
```

With `-AllowAddress` the `LocalSubnet` rule is left out (a block rule cannot be split around a keyword), so devices on
the LAN's global IPv6 prefix are reachable again; the script warns. Name resolution keeps working, because Windows'
DNS Client service sends the queries, not `helios-ci`. If you recreate the account, run the script again: the rules
name the account's SID.

### 8. Require approval for fork pull requests

GitHub → Settings → Actions → General → "Approval for running fork pull request workflows from contributors" (older
pages: "Fork pull request workflows from outside collaborators") → **Require approval for all external contributors**
→ Save. By default GitHub asks only about contributors who have had nothing merged yet. With this setting, no
workflow from a fork runs until you click "Approve and run"; approve a run only after reading its workflow files.
If you do approve one, the hook still ends any of its jobs that ask for this runner before their first step (the last
item of the checklist tests this).

### 9. Start the runner

Only after steps 4b, 6, 7 and 8. The runner runs the hook only if it can see it: when `.env` sets no hook it runs
none, and when the file is missing or `helios-ci` cannot read it, "Set up runner" fails without running the hook, so
nothing ends the job ("`File doesn't exist`" under "Day to day"). PowerShell, in turn, refuses to run the hook when
the execution policy is stricter than RemoteSigned or the file is marked as downloaded, which also fails the step
without ending the job. This block starts the service only when `.env` sets the hook, `helios-ci` can read it, the
file carries no download mark, and Windows PowerShell's machine-wide policy (step 4, or a group policy, which wins)
lets it run; it stops with a message otherwise. It checks Windows PowerShell's policies only: when PowerShell 7 is
installed, the runner starts the hook with `pwsh`, which keeps its own machine-wide policy (RemoteSigned, unless
`$PSHOME\powershell.config.json` or a group policy under PowerShellCore sets another), and neither shell's
CurrentUser policy for `helios-ci` shows here. The checklist asks `helios-ci` for the policy in effect in both:

```powershell
# Step 9: start the runner only if .env sets the hook and helios-ci can read and run it.
& {
    $ErrorActionPreference = 'Stop'
    $hook = 'D:\helios-ci\hooks\job-started.ps1'
    $ci = (Get-LocalUser -Name helios-ci).SID.Value
    if (-not (Test-Path -LiteralPath $hook -PathType Leaf)) { throw "$hook is missing: do step 6" }
    # The runner sets a variable for each name=value line of .env, as written; a later line for a name wins.
    $lines = @(Get-Content -LiteralPath D:\helios-ci\runner\.env |
        Where-Object { $_ -match '^ACTIONS_RUNNER_HOOK_JOB_STARTED=' })
    if ($lines.Count -eq 0 -or $lines[-1] -ne "ACTIONS_RUNNER_HOOK_JOB_STARTED=$hook") {
        throw "the last ACTIONS_RUNNER_HOOK_JOB_STARTED line of D:\helios-ci\runner\.env must name ${hook}: do step 6"
    }
    $read = [int][Security.AccessControl.FileSystemRights]'Read'
    $rules = @((Get-Acl -LiteralPath $hook).GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]))
    if (@($rules | Where-Object { $_.AccessControlType -ne 'Allow' }).Count) {
        throw "$hook has a Deny entry, and step 3 sets none: remove it (icacls $hook shows it)"
    }
    if (-not @($rules | Where-Object {
                $_.IdentityReference.Value -eq $ci -and ([int]$_.FileSystemRights -band $read) -eq $read }).Count) {
        throw "helios-ci ($ci) cannot read $hook (icacls shows the entry of an account deleted since as a bare " +
            '*S-1-5-21-... SID): run the icacls line of step 3 for D:\helios-ci\hooks again'
    }
    # RemoteSigned refuses an unsigned script that carries the mark of a download (a browser's copy).
    if (Get-Item -LiteralPath $hook -Stream Zone.Identifier -ErrorAction SilentlyContinue) {
        throw "$hook is marked as downloaded, so PowerShell would not run it: Unblock-File $hook, or copy it " +
            'from a clone (step 6)'
    }
    # Windows PowerShell runs the hook when PowerShell 7 is not installed. A group policy wins over step 4's setting.
    $gp = Get-ItemProperty -LiteralPath HKLM:\SOFTWARE\Policies\Microsoft\Windows\PowerShell `
        -ErrorAction SilentlyContinue
    $lm = Get-ItemProperty -LiteralPath HKLM:\SOFTWARE\Microsoft\PowerShell\1\ShellIds\Microsoft.PowerShell `
        -ErrorAction SilentlyContinue
    $policy = 'Restricted'                       # what Windows 10 and 11 use when nothing is set
    if ($gp -and $gp.PSObject.Properties['EnableScripts']) {
        if ($gp.EnableScripts -eq 1 -and $gp.PSObject.Properties['ExecutionPolicy']) { $policy = $gp.ExecutionPolicy }
    } elseif ($lm -and $lm.PSObject.Properties['ExecutionPolicy']) {
        $policy = $lm.ExecutionPolicy
    }
    if ($policy -notin 'RemoteSigned', 'Unrestricted', 'Bypass') {
        throw "Windows PowerShell's execution policy is $policy, so the hook would not run: do step 4's " +
            'Set-ExecutionPolicy line (a group policy under Windows PowerShell overrides it)'
    }
    Get-Service actions.runner.* | Set-Service -StartupType Automatic
    Get-Service actions.runner.* | Start-Service
    Get-Service actions.runner.* | Select-Object Name, Status, StartType      # Running, Automatic
}
```

Then check that the hook ends unreviewed code's jobs (the last item of the checklist below) before step 10.

### 10. Switch the jobs on

GitHub → Settings → Secrets and variables → Actions → Variables → New repository variable: `HELIOS_WIN_GPU` =
`enabled`. If the PC has more than one GPU, also set `HELIOS_WIN_GPU_ADAPTER` to part of the discrete GPU's name
(for example `RTX`); the RHI then uses that adapter. Then Actions → win-gpu → Run workflow, on `main`.

If the hook is ever removed or `.env` loses its line, stop the service and set it to Manual (step 5) until steps 6
and 9 are done again. Register the runner again, or create `helios-ci` again, only through "Rotate".

## Verification checklist

Do this after the setup and after any change to the PC, the hook or the firewall.

- [ ] The runner page shows `helios-win-gpu` Idle with the labels `self-hosted`, `Windows`, `X64`, `win-gpu`.
- [ ] `Get-CimInstance Win32_Service -Filter "Name LIKE 'actions.runner.%'" | Select-Object Name, StartName, State, StartMode`
      shows `.\helios-ci`, Running, Auto, and it was started only after the hook and the firewall were in place.
- [ ] Settings → Actions → General shows "Require approval for all external contributors" for fork pull request
      workflows.
- [ ] `Get-LocalGroupMember -SID S-1-5-32-544` does not list `helios-ci`.
- [ ] `Get-ExecutionPolicy -List` shows `RemoteSigned` for `LocalMachine`.
- [ ] `icacls D:\helios-ci\hooks` gives `helios-ci` `(RX)` only and lists no bare `*S-1-5-21-...` SID (a deleted
      account's: see "Rotate"); `.env` has the hook line and no line that you did not write (each line sets a
      variable for the runner); the hash check above is True.
- [ ] `icacls D:\helios-ci` lists Administrators, SYSTEM and `BUILTIN\Users:(RX)` only, and `icacls
      D:\helios-ci\runner` names no account but Administrators, SYSTEM and `config.cmd`'s `GITHUB_ActionsRunner_G...`
      group (step 3).
- [ ] Step 4b's audit lists no `write` line but FAT32 or exFAT drives you accepted and `D:\helios-ci.old-<time>` trees
      that you have not deleted yet, and its `read` lines are only tools the job uses.
- [ ] `Get-NetFirewallRule -Group 'Helios CI runner: LAN block for helios-ci'` lists 3 rules (2 with `-AllowAddress`),
      Enabled, Outbound, Block; `... | Get-NetFirewallAddressFilter` shows the ranges of step 7;
      `Get-NetFirewallProfile | Select-Object Name, Enabled` shows every profile enabled.
- [ ] As `helios-ci`, in a window that does not load its profiles (`runas /user:helios-ci "powershell -NoProfile"`: a
      profile could change what the commands below show):
  - `$PROFILE.AllUsersAllHosts, $PROFILE.AllUsersCurrentHost, $PROFILE.CurrentUserAllHosts,
    $PROFILE.CurrentUserCurrentHost | Where-Object { Test-Path -LiteralPath $_ }` prints nothing, and neither does
    the same command in `pwsh -NoProfile` if PowerShell 7 is installed. The runner loads these profiles before the
    hook, so code in one runs before the hook can refuse a job ("What stays possible" above). Ask `$PROFILE` as
    `helios-ci`, not a path under `C:\Users\helios-ci`: the account can move its own Documents folder. If one exists
    and you did not make it, follow "Rotate";
  - `Get-ChildItem C:\Users\<you>` fails with access denied;
  - for a folder that step 4b closed, `Get-ChildItem D:\Photos` and `Set-Content D:\Photos\probe.txt x` fail with
    access denied, and for a tool folder, `Set-Content C:\VulkanSDK\probe.txt x` does too;
  - `Test-NetConnection <your router's IP> -Port 80` fails, `Test-NetConnection github.com -Port 443` succeeds;
  - `Test-NetConnection <your public IP address> -Port 80` and `-Port 443` (the router's status page shows the
    address) fail, or reach nothing you would mind `helios-ci` using (see "What stays possible" above);
  - `git --version; cmake --version; python --version; go version; $env:VULKAN_SDK` all answer;
  - `Get-ExecutionPolicy` (the policy in effect for `helios-ci`) answers `RemoteSigned`, `Unrestricted` or `Bypass`,
    and `Get-ExecutionPolicy -Scope CurrentUser` answers `Undefined` (anything else overrides step 4's policy for
    `helios-ci`; `Restricted` there would keep the hook from running). If PowerShell 7 is installed, the same two
    answers come from `pwsh -NoProfile -c Get-ExecutionPolicy` and `pwsh -NoProfile -c Get-ExecutionPolicy -Scope
    CurrentUser`: it keeps its own settings, and the runner starts the hook with `pwsh` when it finds it.
- [ ] A dispatched run on `main`: "Set up runner" prints `job-started hook: workflow_dispatch job on refs/heads/main;
      removed N entries ...`; "Runner isolation" and "LAN egress blocked" pass (the latter says how many connects the
      firewall denied); the job builds; "The goldens and the bench ran on a hardware GPU" names your GPU.
- [ ] The run's `results-win-gpu` artifact holds `run.json`, `host.json`, `ctest.xml`, `gates/`, `inventory.json`,
      `hnoise-win-gpu.json` and `rendertest/`.
- [ ] The hook ends unreviewed code's jobs. Push a throwaway branch whose only change is a workflow
      `.github/workflows/hook-test.yml`:

      ```yaml
      on: push
      jobs:
        hook-test:
          runs-on: [self-hosted, win-gpu]
          steps:
            - if: always()
              run: echo this must not run
      ```

      Its run must fail at "Set up runner", whose log shows `refused: ref 'refs/heads/<branch>'` and `ending the job`
      (if the log is cut short there, that is the hook stopping the worker). No later step may run, and `this must
      not run` must not appear anywhere in the log. Delete the branch afterwards. If the echo does appear, stop the
      service at once (step 5) and report it: the hook did not end the job. (The `if: always()` is the point: a step
      without it is skipped after any failed step, so it would pass even without the hook's stop. A run that is
      merely skipped by a job-level `if:` does not test the hook.)

## Day to day

- **Pause**: set `HELIOS_WIN_GPU` to anything else, or delete it: the job is skipped, not queued. Stopping the service
  also works, but scheduled jobs then queue for 24 h and fail.
- **Schedule**: change the cron in `win-gpu.yml` through a PR.
- **`refused:` at "Set up runner"**: the hook ended a job that is not an allowed run of `main`, for example a push
  to another branch whose workflow asks for this runner. It stops the job's worker process, so the run fails there,
  possibly with a message about the runner's worker; the runner stays online. Look at which branch or fork started it.
- **"could not end the job"** at "Set up runner": the hook refused a job but found no worker process to stop, or could
  not stop it, so that job's `if: always()` and `pre:` steps may have run. Stop the service (step 5), report it, and
  follow "Rotate" (and step 4b's "After unreviewed code") before the runner runs again: that code was not reviewed. A
  line saying that the parent process is not the runner's `Runner.Worker.exe`, or that its lookup failed, followed by
  "ending the job: stopping every Runner.Worker.exe", means the hook found the worker by name instead; the job was
  ended, but report it too.
- **`File doesn't exist`** at "Set up runner", after "A job started hook has been configured by the self-hosted runner
  administrator": the runner could not see the hook. The file is missing, `.env` names another path, or `helios-ci`
  cannot read it (for example because the account was created again without "Rotate"). The hook did not run, so it did
  not end the job: the job's `if: always()` and `pre:` steps may have run. Stop the service (step 5) and report it
  with the run's branch or fork. If the job was not a run of `main`, its code was not reviewed: follow "Rotate" (and
  step 4b's "After unreviewed code"). Otherwise fix step 6 (and step 3's `icacls` line for `hooks`) and start the
  service again with step 9's block.
- **"entries survived the wipe"** at "Set up runner": a leftover process holds files in `D:\helios-ci\work`. Reboot
  (or end `helios-ci`'s processes); the next job wipes again.
- **`rhi_triangle_smoke`** opens a window. A service runs without a desktop, so it may fail on this runner; that is a
  finding to report, not a reason to run the runner interactively.
- **Golden images**: the goldens were blessed on lavapipe (`golden/vulkan-llvmpipe/`); a real GPU may differ beyond
  the ꟻLIP tolerance. A failure is a finding for WP-0.12 (per-driver goldens), not a setting to relax.
- **"did not run on a hardware GPU"**: the goldens or the bench used a software rasterizer, or saw no GPU at all.
  With two GPUs, set `HELIOS_WIN_GPU_ADAPTER` (step 10). If the log says `no Vulkan physical devices` or `GPU:
  unavailable`, the driver does not offer Vulkan to a service's session; that is a finding to report (the options
  are the GPU-P VM of 09 §5.4a or a different runner setup, the owner's decision), not a reason to run the runner
  from an interactive logon.

## Rotate or remove

Rotate periodically, and at once if you suspect that a job misbehaved or that code nobody reviewed ran while the
runner had no working hook. Every rotation is the same: it removes the runner without running anything from its
folder, replaces the account and `D:\helios-ci`, installs the hook and the firewall rules for the new account, and
only then registers the runner again, from a fresh download in the new `runner` folder.

**Never run `config.cmd remove`**, the removal command that GitHub's page shows, and never run anything else from
`D:\helios-ci`, or from a folder set aside from it, in an elevated window. Removing and registering a runner service
both need administrator rights, and `config.cmd` runs `powershell.exe` over its folder and then
`bin\Runner.Listener.exe`, all of which `helios-ci` can change: it has full control of the runner's folder, and the
runner itself replaces `bin` when it updates. `config.cmd` is a plain batch file, so one line added to it would run
with your rights. Its `remove` also stops and deletes whatever service the `.service` file there names.

1. Set `HELIOS_WIN_GPU` to `disabled`.
2. Remove the runner. On the PC, elevated, stop and delete the runner's service and the local group through which
   `config.cmd` gave the account the runner's folders. `config.cmd` reuses a group of the same name, so a group left
   behind would give the next account the folders that this rotation sets aside. (Like the rest of this runbook, the
   block assumes that this PC runs no other runner.)

   ```powershell
   # Rotate: delete the runner's service and config.cmd's group with Windows' own tools, not with config.cmd.
   & {
       $ErrorActionPreference = 'Stop'
       foreach ($service in @(Get-Service -Name actions.runner.*)) {
           Stop-Service -Name $service.Name -Force
           sc.exe delete $service.Name
           if ($LASTEXITCODE -ne 0) { throw "sc.exe could not delete the service $($service.Name)" }
       }
       Get-LocalGroup -Name 'GITHUB_ActionsRunner_G*' | Remove-LocalGroup
       'What is left (nothing below this line):'
       Get-Service -Name actions.runner.*
       Get-LocalGroup -Name 'GITHUB_ActionsRunner_G*'
   }
   ```

   Then GitHub → Settings → Actions → Runners → `helios-win-gpu` → Remove → **Force remove this runner**, not the
   command that the dialog shows. This deletes the registration, so the runner's credentials stop working, and so
   does any copy of them.
3. Replace the account and `D:\helios-ci`, and install the hook and the firewall rules for the new account
   **before** you register the runner again. `config.cmd` starts the service at once, jobs that asked for this runner
   while it was stopped or removed still wait in the queue (up to 24 h), and the new runner takes one within seconds:
   without the hook and the firewall rules, that job would run all of its steps, with the LAN open. Run blocks a to d
   in one elevated window, in this order.

   **a.** List what `helios-ci` owns outside its profile (it reads ACLs only, and may take a few minutes). Once the
   account is deleted, Windows shows these items' owner as a bare SID, and step 4b's audit no longer labels them
   `owner: helios-ci`. An `Owner` that starts with `?` means that the item's ACL could not be read, which the account
   can arrange for what it owns: look at it with `icacls`, after `takeown /f <path>` if need be. Keep the list for
   your report, and find out what each item is before you delete it: it may have been left for you, or for a program
   you use, to run. Windows PowerShell 5.1 follows directory links when it lists recursively, so a link that the old
   account left in `ProgramData` or `Public` (to `C:\`, or in a loop) can keep this block busy for a very long time,
   though it only reads. If it has not finished after half an hour, press Ctrl+C, look for such links (an `l` in the
   `Mode` column of `Get-ChildItem -Force`) and note them for your report.

   ```powershell
   # Rotate: what helios-ci owns outside its profile, listed before the account is deleted.
   $old = (Get-LocalUser -Name helios-ci -ErrorAction Stop).SID.Value
   "helios-ci's SID: $old"
   $roots = @(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType = 2 OR DriveType = 3' |
       ForEach-Object { $_.DeviceID + '\' })
   $items = @(Get-ChildItem -LiteralPath $roots -Force -ErrorAction SilentlyContinue) +
       @(Get-ChildItem -LiteralPath $env:ProgramData, $env:PUBLIC, "$env:SystemRoot\Temp" -Recurse -Force `
           -ErrorAction SilentlyContinue)
   $items | ForEach-Object {
       $item = $_
       try {
           $owner = (Get-Acl -LiteralPath $item.FullName -ErrorAction Stop).GetOwner(
               [Security.Principal.SecurityIdentifier]).Value
       } catch { $owner = "? $($_.Exception.Message)" }
       if ($owner -eq $old -or "$owner".StartsWith('?')) {
           [pscustomobject]@{ Owner = $(if ($owner -eq $old) { 'helios-ci' } else { $owner })
               Path = $item.FullName; LastWriteTime = $item.LastWriteTime }
       }
   } | Format-Table -AutoSize -Wrap
   ```

   **b.** End the account's processes, then delete its profile and the account. A process that the old account left
   running keeps its rights after the account is gone, and block d moves the firewall rules to the new account.
   `Remove-CimInstance` deletes the profile's folder and its registry entry; a folder deleted by hand leaves the
   entry, and the new account would get the same folder back. If it says that the profile is in use, restart the PC,
   run block a again (it sets `$old`), then this block.

   ```powershell
   # Rotate: end helios-ci's processes, then delete its profile (folder and registry entry) and the account.
   & {
       $ErrorActionPreference = 'Stop'
       if (-not $old) { throw 'no $old: run block a first, it sets it' }
       Get-Process -IncludeUserName | Where-Object { $_.UserName -eq "$env:COMPUTERNAME\helios-ci" } |
           Stop-Process -Force
       Get-CimInstance Win32_UserProfile -Filter "SID = '$old'" | Remove-CimInstance
       Remove-LocalUser -SID $old
       "Deleted helios-ci ($old), its processes and its profile"
   }
   ```

   Then create the account again (step 2).

   **c.** Set the old `D:\helios-ci` aside, with its `runner`, `work` and `hooks`, and make step 3's folders again for
   the new account: a new `runner` that only Administrators and SYSTEM can change until `config.cmd` has run, and a
   new `hooks`, which the old account cannot have touched (the setup of 2026-10-03 gave it full control of `hooks`).
   Renaming `D:\helios-ci` changes no permissions on anything inside it. If `Rename-Item` says that the folder is in
   use, close what has it open (an Explorer window, or a window whose current folder is inside it) or restart the PC,
   then run the block again. The old tree keeps the evidence for your report: `D:\helios-ci.old-<time>\runner\_diag`
   holds the runner's log and one `Worker_*.log` per job, beside the old `.env` and the runner's files. Nothing in it
   is run again, and step 5 deletes it.

   ```powershell
   # Rotate: set the old D:\helios-ci aside, and make step 3's folders again for the new helios-ci.
   & {
       $ErrorActionPreference = 'Stop'
       if (-not $old) { throw 'no $old: run block a first, it sets it' }
       if ((Get-LocalUser -Name helios-ci).SID.Value -eq $old) {
           throw 'helios-ci is still the old account: do block b and step 2 first'
       }
       if (@(Get-Service -Name actions.runner.*).Count -or @(Get-LocalGroup -Name 'GITHUB_ActionsRunner_G*').Count) {
           throw "the runner's service or config.cmd's group is still there: do Rotate's step 2 first"
       }
       Rename-Item -LiteralPath D:\helios-ci -NewName "helios-ci.old-$(Get-Date -Format yyyyMMdd-HHmmss)"
       New-Item -ItemType Directory -Path D:\helios-ci\runner, D:\helios-ci\work, D:\helios-ci\hooks | Out-Null
       icacls D:\helios-ci /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-545:(RX)"
       if ($LASTEXITCODE -ne 0) { throw 'icacls failed on D:\helios-ci' }
       icacls D:\helios-ci\runner /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F"
       if ($LASTEXITCODE -ne 0) { throw 'icacls failed on D:\helios-ci\runner' }
       icacls D:\helios-ci\work /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "helios-ci:(OI)(CI)F"
       if ($LASTEXITCODE -ne 0) { throw 'icacls failed on D:\helios-ci\work' }
       icacls D:\helios-ci\hooks /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "helios-ci:(OI)(CI)RX"
       if ($LASTEXITCODE -ne 0) { throw 'icacls failed on D:\helios-ci\hooks' }
   }
   ```

   **d.** Install the hook from your clone at `main`, write `.env` with the hook line, and add the firewall rules for
   the new account (steps 6 and 7; the runner reads this `.env` when `config.cmd` starts it). If you ran
   `firewall.ps1` with `-AllowAddress` before, add the same `-AllowAddress` to its line.

   ```powershell
   # Rotate: before registering, install the hook, .env and the firewall rules for the new helios-ci (steps 6 and 7).
   & {
       $ErrorActionPreference = 'Stop'
       $repo = "$env:USERPROFILE\src\HeliosEngine"
       if (-not (Test-Path -LiteralPath $repo)) {
           git clone https://github.com/PageMastr/HeliosEngine.git $repo
           if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }
       }
       git -C $repo switch main
       if ($LASTEXITCODE -ne 0) { throw "git switch main failed in $repo" }
       git -C $repo pull --ff-only
       if ($LASTEXITCODE -ne 0) { throw "git pull failed in $repo" }
       Copy-Item -LiteralPath "$repo\tools\ci\runner\job-started.ps1" -Destination D:\helios-ci\hooks\job-started.ps1
       Set-Content -LiteralPath D:\helios-ci\runner\.env -Encoding Ascii `
           -Value 'ACTIONS_RUNNER_HOOK_JOB_STARTED=D:\helios-ci\hooks\job-started.ps1'
       & "$repo\tools\ci\runner\firewall.ps1"
       icacls D:\helios-ci\hooks\job-started.ps1      # <PC>\helios-ci:(I)(RX), Administrators and SYSTEM only
   }
   ```

   Last, cancel the runs that still wait for this runner: Actions → filter `is:queued` → each run whose job waits for
   a runner with the `win-gpu` label → Cancel workflow. The hook would end them too, but this way nothing rests on it.
   Go on with step 4: the service that `config.cmd` starts runs the hook, and the firewall rules apply to its account.
4. Register again (step 5: a fresh download into the new `runner` folder, then `config.cmd`; it ends with stopping
   the service), run step 4b's audit (and its "After unreviewed code" list, if that is why you rotate), start the
   service with step 9's block (it refuses while `.env` does not set the hook or `helios-ci` cannot read it), go
   through the checklist, and set `HELIOS_WIN_GPU=enabled`.
5. Delete the set-aside tree once your report no longer needs it, without running anything in it:
   `cmd /c rd /s /q "\\?\D:\helios-ci.old-<time>"`. Like the hook's wipe, `rd` removes a link inside without following
   it, whereas Windows PowerShell 5.1's `Remove-Item -Recurse` can follow a directory link that the old account left
   there into its target, with your administrator rights.

Remove for good: delete the `HELIOS_WIN_GPU` variable; do Rotate's step 2 (the service, `config.cmd`'s group and the
registration, without `config.cmd`); remove the firewall rules from your clone with
`& "$env:USERPROFILE\src\HeliosEngine\tools\ci\runner\firewall.ps1" -Remove` (it also works after the account is
gone); run Rotate's blocks a and b (what the account owns, then its processes, its profile and the account); then
delete the folders without running anything in them: `cmd /c rd /s /q "\\?\D:\helios-ci"`, and the same for every
`D:\helios-ci.old-<time>`.

## Not covered yet (WP-0.4)

- **NS-0.3's cross-host half** needs a Linux lab host on this LAN (`helios-cell` and `helios-gateway`), its address in
  `firewall.ps1 -AllowAddress`, and a job; until then it stays unmeasured (`scorecard.jsonc`).
- **The nightly scorecard does not read `results-win-gpu` yet**: the results exist as artifacts only.
- **Hyper-V GPU-P VM isolation** (09 §5.4a, preferred when it works) has not been tried; the account isolation above
  is the default until a WP-0.4 ADR records that decision.
- **The REF/MIN/SERVER purchase list** (WP-0.4) is a separate deliverable.
