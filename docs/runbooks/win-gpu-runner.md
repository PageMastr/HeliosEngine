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
(the checklist checks the scope). It cannot reach your profile, administrator rights or the LAN, nor, after step 4b,
your folders elsewhere on the drives. Like every local account, it can still read what Windows leaves open to all
users (the machine-wide tools, which the build needs, `C:\ProgramData` and whatever step 4b's audit lists as `read`),
create files and folders in `C:\ProgramData` and `C:\Windows\Temp` and folders at the root of a drive, read and change
`C:\Users\Public` (Windows lets interactive and service logons write there, so keep nothing in it that you would mind
losing or that you run), and read and write a FAT32 or exFAT drive (most USB sticks) while one is plugged in. It can
reach programs on the PC itself that listen on the network, including on `localhost` (Windows Firewall does not filter
loopback): keep such services (databases, dev servers, remote-control tools) behind a password, or stop them while the
runner is enabled. The firewall blocks private addresses only, so the router's public (WAN) address stays reachable:
many routers show their admin page there to clients on the LAN, and NAT loopback passes port-forwarded traffic on to
the LAN device behind it (a NAS), often with the router's LAN address as the source. That depends on the router; the
checklist tests it, and if the admin page or a forwarded service answers, turn off the router's remote administration
or NAT loopback (or the port forward). If you suspect misuse, follow "Rotate" below.

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

Then: narrow the `hooks` ACL (the `icacls` line in step 3), install Go and set the execution policy (the `GoLang.Go`
and `Set-ExecutionPolicy` lines of step 4), close your folders to `helios-ci` (step 4b), then steps 6 to 10 and the
verification checklist. Start the service only at step 9.

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

Create `D:\helios-ci\runner`, `work` and `hooks`. Remove inheritance and give `helios-ci`, Administrators and
SYSTEM full control on `runner` and `work`. Turn BitLocker on for `D:` with auto-unlock. For `hooks`, give
`helios-ci` read and execute only, so that no job can rewrite the hook:

```powershell
icacls D:\helios-ci\hooks /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "helios-ci:(OI)(CI)RX"
```

The entry names the account by its SID. An account created again under the same name has a new SID, so run this line
again then ("Rotate"): `config.cmd` gives the account access to `runner` and `work` itself, but not to `hooks`.

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
- **A FAT32 or exFAT drive**: unplug it while the runner is enabled, or keep nothing on it that is private or that you
  run.

Run the audit again until its `write` lines are at most such drives and its `read` lines are only tools the job uses. It reads the top
level only: a folder you closed stays closed below, unless something below it grants access itself. A folder you
create at a drive root later starts open again: run the audit after creating one (the checklist repeats it).

### 5. Register the runner

GitHub → Settings → Actions → Runners → New self-hosted runner → Windows x64. Download the runner into
`D:\helios-ci\runner`, then run `config.cmd` interactively with `--url`, `--token` (from that page), `--name
helios-win-gpu`, `--labels win-gpu`, `--work D:\helios-ci\work` and `--runasservice`, and enter `.\helios-ci` and its
password at the prompts. Never paste the token or the password anywhere else. The registration token is used once and
is not stored; the runner keeps its own credentials in `D:\helios-ci\runner`.

`config.cmd` starts the service at once, without the hook. Stop it and keep it from starting until step 9:

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

In `.env`, add this line (keep any existing lines) and save. The runner reads `.env` when the service starts
(step 9):

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
nothing ends the job ("`File doesn't exist`" under "Day to day"). This block starts the service only when `.env` sets
the hook and `helios-ci` can read it, and stops with a message otherwise:

```powershell
# Step 9: start the runner only if .env sets the hook and helios-ci can read it.
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
            '*S-1-5-21-... SID): run the icacls line of step 3 again'
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

If the hook is ever removed, `.env` loses its line, the runner is registered again or `helios-ci` is created again,
stop the service and set it to Manual (step 5) until steps 3, 6 and 9 are done again.

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
      account's: see "Rotate"); `.env` has the hook line; the hash check above is True.
- [ ] Step 4b's audit lists no `write` line but FAT32 or exFAT drives you accepted, and its `read` lines are only tools
      the job uses.
- [ ] `Get-NetFirewallRule -Group 'Helios CI runner: LAN block for helios-ci'` lists 3 rules (2 with `-AllowAddress`),
      Enabled, Outbound, Block; `... | Get-NetFirewallAddressFilter` shows the ranges of step 7;
      `Get-NetFirewallProfile | Select-Object Name, Enabled` shows every profile enabled.
- [ ] As `helios-ci` (`runas /user:helios-ci powershell`):
  - `Get-ChildItem C:\Users\<you>` fails with access denied;
  - for a folder that step 4b closed, `Get-ChildItem D:\Photos` and `Set-Content D:\Photos\probe.txt x` fail with
    access denied, and for a tool folder, `Set-Content C:\VulkanSDK\probe.txt x` does too;
  - `Test-NetConnection <your router's IP> -Port 80` fails, `Test-NetConnection github.com -Port 443` succeeds;
  - `Test-NetConnection <your public IP address> -Port 80` and `-Port 443` (the router's status page shows the
    address) fail, or reach nothing you would mind `helios-ci` using (see "What stays possible" above);
  - `git --version; cmake --version; python --version; go version; $env:VULKAN_SDK` all answer;
  - `Get-ExecutionPolicy -Scope CurrentUser` answers `Undefined` (anything else overrides step 4's policy for
    `helios-ci`; `Restricted` there would keep the hook from running), and so does
    `pwsh -NoProfile -c Get-ExecutionPolicy -Scope CurrentUser` if PowerShell 7 is installed (it keeps its own
    setting, and the runner starts the hook with `pwsh` when it finds it).
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
- **"could not end the job"** at "Set up runner": the hook refused a job but found no worker process to stop, or
  could not stop it, so that job's `if: always()` and `pre:` steps may have run. Stop the service (step 5) and report
  it. A line saying that the parent process is not the runner's `Runner.Worker.exe`, or that its lookup failed,
  followed by "ending the job: stopping every Runner.Worker.exe", means the hook found the worker by name instead;
  the job was ended, but report it too.
- **`File doesn't exist`** at "Set up runner", after "A job started hook has been configured by the self-hosted
  runner administrator": the runner could not see the hook. The file is missing, `.env` names another path, or
  `helios-ci` cannot read it (for example because the account was created again and step 3's `icacls` line was not
  run again). The hook did not run, so it did not end the job: the job's `if: always()` and `pre:` steps may have run.
  Stop the service (step 5), fix steps 3 and 6, start it again with step 9's block, and report it with the run's
  branch or fork.
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

Rotate (periodically, or at once if you suspect a job misbehaved):

1. Set `HELIOS_WIN_GPU` to `disabled`.
2. GitHub → Settings → Actions → Runners → `helios-win-gpu` → Remove; copy the removal token. On the PC:
   `cd D:\helios-ci\runner; .\config.cmd remove --token <token>` (this also removes the service).
3. No suspected misuse: reset the account's password,
   `Set-LocalUser helios-ci -Password (Read-Host -AsSecureString)`, and go on with step 4.

   Suspected misuse: replace the account. First list what it owns outside its profile (elevated; it reads ACLs only,
   and may take a few minutes). Once the account is deleted, Windows shows these items' owner as a bare SID, and step
   4b's audit no longer labels them `owner: helios-ci`. Keep the list for your report, and find out what each item is
   before you delete it: it may have been left for you, or for a program you use, to run.

   ```powershell
   # Rotate: what helios-ci owns outside its profile, listed before the account is deleted.
   $old = (Get-LocalUser -Name helios-ci -ErrorAction Stop).SID.Value
   "helios-ci's SID: $old"
   $roots = @(Get-CimInstance Win32_LogicalDisk -Filter 'DriveType = 2 OR DriveType = 3' |
       ForEach-Object { $_.DeviceID + '\' })
   $items = @(Get-ChildItem -LiteralPath $roots -Force -ErrorAction SilentlyContinue) +
       @(Get-ChildItem -LiteralPath $env:ProgramData, $env:PUBLIC, "$env:SystemRoot\Temp" -Recurse -Force `
           -ErrorAction SilentlyContinue)
   $items | Where-Object {
       try { $acl = Get-Acl -LiteralPath $_.FullName -ErrorAction Stop } catch { return $false }
       $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -eq $old
   } | Select-Object FullName, LastWriteTime | Format-Table -AutoSize -Wrap
   ```

   Then remove the account and its profile (`Remove-LocalUser helios-ci`, then delete `C:\Users\helios-ci`), create
   it again (step 2), and delete everything in `D:\helios-ci\runner` and `D:\helios-ci\work`. The new account has a
   new SID, and the `hooks` ACL still names the old one: without the next block `helios-ci` cannot read the hook, and
   the runner then runs jobs without it ("`File doesn't exist`" under "Day to day"). In the same window:

   ```powershell
   # Rotate: give the new helios-ci step 3's access to the hook, and drop the deleted account's entry.
   icacls D:\helios-ci\hooks /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" `
       "helios-ci:(OI)(CI)RX"
   if ($old) { icacls D:\helios-ci\hooks /remove:g "*$old" }
   icacls D:\helios-ci\hooks\job-started.ps1      # <PC>\helios-ci:(I)(RX), and no bare *S-1-5-21-... SID
   ```
4. Register again (step 5, which ends with stopping the service), add the hook line to `.env` again (step 6),
   re-run `firewall.ps1` (step 7: a new account has a new SID) and step 4b's audit, start the service with step 9's
   block (it refuses while `helios-ci` cannot read the hook), go through the checklist, and set
   `HELIOS_WIN_GPU=enabled`.

Remove for good: delete the `HELIOS_WIN_GPU` variable, `config.cmd remove --token <token>`,
`.\tools\ci\runner\firewall.ps1 -Remove` (it also works after the account is gone), `Remove-LocalUser helios-ci`,
and delete `C:\Users\helios-ci` and `D:\helios-ci`.

## Not covered yet (WP-0.4)

- **NS-0.3's cross-host half** needs a Linux lab host on this LAN (`helios-cell` and `helios-gateway`), its address in
  `firewall.ps1 -AllowAddress`, and a job; until then it stays unmeasured (`scorecard.jsonc`).
- **The nightly scorecard does not read `results-win-gpu` yet**: the results exist as artifacts only.
- **Hyper-V GPU-P VM isolation** (09 §5.4a, preferred when it works) has not been tried; the account isolation above
  is the default until a WP-0.4 ADR records that decision.
- **The REF/MIN/SERVER purchase list** (WP-0.4) is a separate deliverable.
