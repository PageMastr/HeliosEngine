# job-started.ps1 — the win-gpu runner's job-started hook (docs/plan/09-roadmap-and-process.md §5.4a; WP-0.4; K33).
#
# Installed as D:\helios-ci\hooks\job-started.ps1 and named by ACTIONS_RUNNER_HOOK_JOB_STARTED in
# D:\helios-ci\runner\.env (docs/runbooks/win-gpu-runner.md). The runner runs it as the runner account, in a step
# called "Set up runner", after "Set up job" has downloaded the job's actions and before any of the job's steps.
# A non-zero exit fails the job.
#
# 1. Refuse the job unless it is a `schedule`, `push` or `workflow_dispatch` run of refs/heads/main from a workflow
#    file on main. A branch's own workflow file decides its own triggers and guards (a push to an unreviewed
#    branch can request this runner before any review), so this check, which lives on the machine, is what keeps
#    unmerged code off the PC; tools/ci/check_runner_policy.py is the PR-tier half.
# 2. Empty D:\helios-ci\work so that the job starts clean. Kept, because the runner made them for this job:
#    `_actions` (the actions "Set up job" downloaded for this job), `_temp` (RUNNER_TEMP, which the runner empties
#    itself and which holds the event payload), `_PipelineMapping` (the runner's workspace bookkeeping), `_diag`
#    (never touched), and the job's workspace directory itself (GITHUB_WORKSPACE, emptied, not removed). Everything
#    else goes: earlier checkouts and builds, `_tool`, other repositories' directories, failed wipes.
#
# Deletion never follows a link: a junction or symbolic link is removed as a link, and a real directory is first
# renamed to `D:\helios-ci\work\.helios-wipe-<guid>` (so cmd.exe only ever sees a name this script made) and then
# removed with `rd /s /q` on the \\?\ form of that path (long paths; read-only files; links inside are removed,
# not followed). A path that is not where the runner's layout puts it, or a root, pipeline or workspace directory
# that is itself a link, stops the hook with an error instead.
#
# The runner dot-sources the file (`powershell -command ". '<path>'"`), so the last block always runs. The tests
# (test_runner_scripts.ps1) define $HeliosCiScriptTestMode before dot-sourcing to load the functions alone.
# Windows PowerShell 5.1 and PowerShell 7 both run it.

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$HeliosCiWorkRoot = 'D:\helios-ci\work'
$HeliosCiKeep = @('_actions', '_temp', '_PipelineMapping', '_diag')

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

function Invoke-HeliosCiJobStarted {
    param(
        [Parameter(Mandatory = $true)] [string]$Root,
        [Parameter(Mandatory = $true)] [System.Collections.IDictionary]$Environment
    )
    $refused = Test-HeliosCiJobAllowed -Environment $Environment
    if ($refused) {
        Write-Host "::error::job-started hook: refused: $refused (09 §5.4a; docs/runbooks/win-gpu-runner.md)"
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

if (-not (Get-Variable -Name HeliosCiScriptTestMode -ErrorAction SilentlyContinue)) {
    $jobEnvironment = @{}
    foreach ($name in 'GITHUB_EVENT_NAME', 'GITHUB_REF', 'GITHUB_WORKFLOW_REF', 'GITHUB_WORKSPACE') {
        $jobEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
    }
    exit (Invoke-HeliosCiJobStarted -Root $HeliosCiWorkRoot -Environment $jobEnvironment)
}
