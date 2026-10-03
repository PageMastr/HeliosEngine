# job-started.ps1: the win-gpu runner's job-started hook (docs/plan/09-roadmap-and-process.md section 5.4a; WP-0.4;
# K33). ASCII only: Windows PowerShell 5.1 reads a file without a BOM as ANSI.
#
# Installed as D:\helios-ci\hooks\job-started.ps1 and named by ACTIONS_RUNNER_HOOK_JOB_STARTED in
# D:\helios-ci\runner\.env (docs/runbooks/win-gpu-runner.md). The runner runs it as the runner account, in a step
# called "Set up runner", after "Set up job" has downloaded the job's actions and before any of the job's steps
# (actions' `pre:` steps included).
#
# 1. Refuse the job unless it is a `schedule`, `push` or `workflow_dispatch` run of refs/heads/main from a workflow
#    file on main. A branch's own workflow file decides its own triggers and guards (a push to an unreviewed
#    branch can request this runner before any review), so this check, which lives on the machine, is what keeps
#    unmerged code off the PC; tools/ci/check_runner_policy.py is the PR-tier half.
#    A refused job is ended, not just failed: a non-zero exit fails only this step, and the runner then still runs
#    every later step whose `if:` holds after a failure (`always()`, `failure()`, `!cancelled()`) and every
#    action's `pre:` step (`pre-if` defaults to `always()`), all in the same pass (actions/runner: JobExtension,
#    StepsRunner). So the hook stops its parent process, the runner's Runner.Worker.exe under
#    D:\helios-ci\runner, which runs the job's steps; the runner's listener then reports the job failed and stays
#    online. If WMI cannot show the parent, or the parent is anything else, it stops every Runner.Worker.exe whose
#    file is under D:\helios-ci\runner instead (Get-Process, no WMI): the runner runs one job at a time, so that is
#    this job's worker. If there is none, it stops nothing and says so. It never stops the listener
#    (Runner.Listener.exe): a worker outlives its listener, polling the listener's closed pipe (actions/runner:
#    Worker, StreamString), so that would take the runner offline without stopping the job's steps.
# 2. Empty D:\helios-ci\work so that the job starts clean. Kept, because the runner made them for this job or for
#    itself: `_actions` (the actions "Set up job" downloaded for this job), `_temp` (RUNNER_TEMP, which the runner
#    empties itself and which holds the event payload), `_PipelineMapping` (the runner's workspace bookkeeping),
#    `_diag` (never touched), `_update` (where the listener unpacks its own new version while it still takes jobs; it
#    waits for the running job only afterwards, and empties the directory itself before each download: actions/runner
#    SelfUpdater), and the job's workspace directory itself (GITHUB_WORKSPACE, emptied, not removed). Everything else
#    goes: earlier checkouts and builds, `_tool`, other repositories' directories, failed wipes.
#
# Deletion never follows a link: a junction or symbolic link is removed as a link, and a real directory is first
# renamed to `D:\helios-ci\work\.helios-wipe-<guid>` (so cmd.exe only ever sees a name this script made) and then
# removed with `rd /s /q` on the \\?\ form of that path (long paths; read-only files; links inside are removed,
# not followed). A path that is not where the runner's layout puts it, or a root, pipeline or workspace directory
# that is itself a link, stops the hook with an error instead.
#
# The runner dot-sources the file (`powershell -command ". '<path>'"`, profiles included), so the last block always
# runs; there is no switch to skip it. The tests (test_runner_scripts.ps1) load the functions and constants from the
# parsed file instead. Windows PowerShell 5.1 and PowerShell 7 both run it.

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$HeliosCiWorkRoot = 'D:\helios-ci\work'
$HeliosCiRunnerRoot = 'D:\helios-ci\runner'
$HeliosCiKeep = @('_actions', '_temp', '_PipelineMapping', '_diag', '_update')

# $null when the job may run here, else why not. $Environment: the job's GITHUB_* variables. Functions that return
# a value write their messages with Write-Host, so that only the value reaches the pipeline.
function Test-HeliosCiJobAllowed {
    param([Parameter(Mandatory = $true)] [System.Collections.IDictionary]$Environment)
    $missing = @('GITHUB_EVENT_NAME', 'GITHUB_REF', 'GITHUB_WORKFLOW_REF' | Where-Object { -not $Environment[$_] })
    if ($missing.Count -gt 0) {
        return "the job's $($missing -join ', ') is not set, so it cannot be shown to be merged code"
    }
    $eventName = [string]$Environment['GITHUB_EVENT_NAME']
    $ref = [string]$Environment['GITHUB_REF']
    $workflowRef = [string]$Environment['GITHUB_WORKFLOW_REF']
    if (@('schedule', 'push', 'workflow_dispatch') -cnotcontains $eventName) {
        return "event '$eventName': this runner runs only schedule, push and workflow_dispatch jobs"
    }
    if ($ref -cne 'refs/heads/main') {
        return "ref '$ref': this runner runs only refs/heads/main"
    }
    if (-not $workflowRef.EndsWith('@refs/heads/main', [StringComparison]::Ordinal)) {
        return "workflow '$workflowRef' is not read from refs/heads/main"
    }
    return $null
}

function Get-HeliosCiFullPath {
    param([Parameter(Mandatory = $true)] [string]$Path)
    return [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}

function Test-HeliosCiLink {
    param([Parameter(Mandatory = $true)] [IO.FileSystemInfo]$Item)
    return ($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
}

# The full paths to delete under $Root before the job whose workspace is $Workspace. Throws when the layout is not
# the runner's (<root>\<repository>\<repository>) or one of its directories is a link.
function Get-HeliosCiWipeTargets {
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Workspace
    )
    $rootFull = Get-HeliosCiFullPath $Root
    $rootItem = Get-Item -LiteralPath $rootFull -Force
    if (-not $rootItem.PSIsContainer -or (Test-HeliosCiLink $rootItem)) {
        throw "$rootFull is not a plain directory (a link would point the wipe elsewhere)"
    }
    if (-not $Workspace) {
        throw 'GITHUB_WORKSPACE is not set, so the job''s workspace cannot be told apart'
    }
    $workspaceFull = Get-HeliosCiFullPath $Workspace
    $pipeline = [IO.Path]::GetDirectoryName($workspaceFull)
    if (-not $pipeline -or -not ([IO.Path]::GetDirectoryName($pipeline) -ieq $rootFull)) {
        throw "GITHUB_WORKSPACE '$workspaceFull' is not <work>\<repository>\<repository> under $rootFull"
    }
    # The runner creates both in "Set up job"; one that is missing has nothing to keep.
    foreach ($dir in $pipeline, $workspaceFull) {
        $item = Get-Item -LiteralPath $dir -Force -ErrorAction SilentlyContinue
        if ($item -and (-not $item.PSIsContainer -or (Test-HeliosCiLink $item))) {
            throw "$dir is not a plain directory (a link would point the wipe elsewhere)"
        }
    }
    $targets = New-Object System.Collections.Generic.List[string]
    foreach ($item in @(Get-ChildItem -LiteralPath $rootFull -Force)) {
        if ($HeliosCiKeep -contains $item.Name) { continue }
        if ($item.FullName -ieq $pipeline) {
            foreach ($child in @(Get-ChildItem -LiteralPath $pipeline -Force)) {
                if ($child.FullName -ieq $workspaceFull) {
                    foreach ($entry in @(Get-ChildItem -LiteralPath $workspaceFull -Force)) { $targets.Add($entry.FullName) }
                } else {
                    $targets.Add($child.FullName)
                }
            }
            continue
        }
        $targets.Add($item.FullName)
    }
    return , $targets.ToArray()
}

# Deletes one target under $Root without following links (Windows only: cmd.exe's rd). Writes no output.
function Remove-HeliosCiWipeTarget {
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [Parameter(Mandatory = $true)] [string]$Path
    )
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if (-not $item) { return }
    if (Test-HeliosCiLink $item) {
        # The link itself: RemoveDirectory / DeleteFile on a reparse point never touch its target.
        if ($item.PSIsContainer) { [IO.Directory]::Delete($item.FullName, $false) } else { [IO.File]::Delete($item.FullName) }
        return
    }
    if (-not $item.PSIsContainer) {
        $item.Attributes = [IO.FileAttributes]::Normal
        $item.Delete()
        return
    }
    $parking = Join-Path (Get-HeliosCiFullPath $Root) ('.helios-wipe-' + [guid]::NewGuid().ToString('N'))
    if ($parking -notmatch '^[A-Za-z]:\\[A-Za-z0-9_.\\-]+$') {
        throw "refusing to pass '$parking' to cmd.exe"
    }
    if ($item.FullName -ine $parking) {
        [IO.Directory]::Move($item.FullName, $parking)
    }
    $null = & "$env:SystemRoot\System32\cmd.exe" /d /c rd /s /q "\\?\$parking"
}

# The ids of the running processes named $Name (without .exe) whose file is under $Prefix (a folder path ending in
# '\'). Get-Process needs no WMI. Never throws: a failure is reported and gives no ids.
function Get-HeliosCiRunnerProcessId {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$Prefix
    )
    try {
        return @(Get-Process -Name $Name -ErrorAction SilentlyContinue | Where-Object {
                $_.Path -and ([string]$_.Path).StartsWith($Prefix, [StringComparison]::OrdinalIgnoreCase)
            } | ForEach-Object { [int]$_.Id })
    } catch {
        Write-Host "job-started hook: could not list the $Name processes: $($_.Exception.Message)"
        return @()
    }
}

# Ends the job this hook runs in: stops process $ProcessId's parent when that is Runner.Worker.exe under
# $RunnerRoot (the runner starts the hook's shell directly). When WMI fails or the parent is anything else, stops
# every Runner.Worker.exe under $RunnerRoot instead: the runner runs one job at a time, so that is this job's worker.
# Returns whether it stopped a process. Waits $DelayMilliseconds first, to give the worker time to send the refusal
# to the job's log. Nothing else runs meanwhile: the worker waits for this step, and cancelling the run does not
# stop a running step whose `if:` is `always()` (StepsRunner re-evaluates it), which the hook's is.
function Stop-HeliosCiJob {
    param(
        [Parameter(Mandatory = $true)] [string]$RunnerRoot,
        [int]$ProcessId = $PID,
        [int]$DelayMilliseconds = 2000
    )
    $prefix = $RunnerRoot.TrimEnd('\') + '\'
    $ids = @()
    $what = 'its parent, Runner.Worker.exe'
    try {
        $self = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $ProcessId"
        $worker = $null
        if ($self) {
            $worker = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($self.ParentProcessId)"
        }
        if ($worker -and $worker.Name -ieq 'Runner.Worker.exe' -and $worker.ExecutablePath -and
            ([string]$worker.ExecutablePath).StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            $ids = @([int]$worker.ProcessId)
        } else {
            $found = 'the hook''s own process not found'
            if ($worker) { $found = "$($worker.Name) ($($worker.ExecutablePath))" } elseif ($self) { $found = 'not found' }
            Write-Host ("job-started hook: the parent process is not the runner's Runner.Worker.exe under " +
                "${RunnerRoot}: $found")
        }
    } catch {
        Write-Host "job-started hook: the parent process lookup failed: $($_.Exception.Message)"
    }
    if ($ids.Count -eq 0) {
        $ids = @(Get-HeliosCiRunnerProcessId -Name 'Runner.Worker' -Prefix $prefix)
        $what = "every Runner.Worker.exe under $RunnerRoot"
    }
    if ($ids.Count -eq 0) {
        Write-Host ("::error::job-started hook: could not end the job: no Runner.Worker.exe under $RunnerRoot was " +
            "found, so this job's if: always() and pre: steps may still run.")
        return $false
    }
    Write-Host "job-started hook: ending the job: stopping $what (process $($ids -join ', '))"
    if ($DelayMilliseconds -gt 0) { Start-Sleep -Milliseconds $DelayMilliseconds }
    $stopped = 0
    foreach ($id in $ids) {
        try {
            Stop-Process -Id $id -Force
            $stopped++
        } catch {
            Write-Host ("::error::job-started hook: could not end the job: stopping process $id failed " +
                "($($_.Exception.Message)), so this job's if: always() and pre: steps may still run.")
        }
    }
    return $stopped -gt 0
}

function Invoke-HeliosCiJobStarted {
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [Parameter(Mandatory = $true)] [string]$RunnerRoot,
        [Parameter(Mandatory = $true)] [System.Collections.IDictionary]$Environment,
        [int]$DelayMilliseconds = 2000
    )
    $refused = Test-HeliosCiJobAllowed -Environment $Environment
    if ($refused) {
        Write-Host "::error::job-started hook: refused: $refused (09 section 5.4a; docs/runbooks/win-gpu-runner.md)"
        $null = Stop-HeliosCiJob -RunnerRoot $RunnerRoot -DelayMilliseconds $DelayMilliseconds
        return 1
    }
    $workspace = [string]$Environment['GITHUB_WORKSPACE']
    $targets = Get-HeliosCiWipeTargets -Root $Root -Workspace $workspace
    foreach ($target in $targets) {
        try {
            Remove-HeliosCiWipeTarget -Root $Root -Path $target
        } catch {
            Write-Host "job-started hook: could not remove ${target}: $($_.Exception.Message)"
        }
    }
    $left = Get-HeliosCiWipeTargets -Root $Root -Workspace $workspace
    if ($left.Count -gt 0) {
        $shown = ($left | Select-Object -First 20) -join ', '
        Write-Host "::error::job-started hook: $($left.Count) entries survived the wipe (in use?): $shown"
        return 1
    }
    $kept = $HeliosCiKeep -join ', '
    Write-Host "job-started hook: $($Environment['GITHUB_EVENT_NAME']) job on $($Environment['GITHUB_REF']); removed $($targets.Count) entries from $Root (kept $kept and the workspace directory)"
    return 0
}

$jobEnvironment = @{}
foreach ($name in 'GITHUB_EVENT_NAME', 'GITHUB_REF', 'GITHUB_WORKFLOW_REF', 'GITHUB_WORKSPACE') {
    $jobEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}
exit (Invoke-HeliosCiJobStarted -Root $HeliosCiWorkRoot -RunnerRoot $HeliosCiRunnerRoot -Environment $jobEnvironment)
