# Tests for the win-gpu runner scripts (WP-0.4): CTest `lint_runner_scripts` (label lint) runs them with
# PowerShell 7 wherever pwsh is installed (every GitHub-hosted image), Windows PowerShell 5.1 can run them too.
#
#   pwsh -NoProfile -File tools/ci/runner/test_runner_scripts.ps1 [-WorkDir DIR]
#
# Everywhere: every .ps1 here parses; firewall.ps1's block lists (CIDR splitting around lab addresses, IPv4 and
# IPv6, stray addresses); job-started.ps1's refusal rules and the entries it would delete. On Windows also the
# wipe itself on a scratch tree under -WorkDir: a junction to a directory outside, a directory symbolic link
# where creating one is allowed, read-only files and a deep tree; the links go, their targets stay.
param([string]$WorkDir = (Join-Path ([IO.Path]::GetTempPath()) ('helios-runner-scripts-' + [guid]::NewGuid().ToString('N'))))

$ErrorActionPreference = 'Stop'
$script:failures = 0
$script:checks = 0
function Assert-Equal($Expected, $Actual, [string]$What) {
    $script:checks++
    $e = @($Expected) -join ' | '
    $a = @($Actual) -join ' | '
    if ($e -cne $a) {
        $script:failures++
        Write-Host "FAIL: ${What}`n  expected: $e`n  actual:   $a"
    }
}
function Assert-Throws([scriptblock]$Block, [string]$Pattern, [string]$What) {
    $script:checks++
    try {
        & $Block | Out-Null
        $script:failures++
        Write-Host "FAIL: $What (no error)"
    } catch {
        if ($_.Exception.Message -notmatch $Pattern) {
            $script:failures++
            Write-Host "FAIL: $What (error '$($_.Exception.Message)' does not match '$Pattern')"
        }
    }
}

$here = $PSScriptRoot

# -- Every script parses -----------------------------------------------------------------------------------
foreach ($file in Get-ChildItem -LiteralPath $here -Filter *.ps1) {
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors)
    Assert-Equal '' (($errors | ForEach-Object { "$($_.Extent.StartLineNumber): $($_.Message)" }) -join '; ') "$($file.Name) parses"
}

$HeliosCiScriptTestMode = $true

# -- firewall.ps1 --------------------------------------------------------------------------------------------
. (Join-Path $here 'firewall.ps1')
Assert-Equal @('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', '169.254.0.0/16') `
    (Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv4 -Allow @()) 'IPv4 without lab addresses'
Assert-Equal @('fc00::/7', 'fe80::/10') (Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv6) 'IPv6 without lab addresses'
Assert-Equal @('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0-192.168.1.49', '192.168.1.51-192.168.255.255', '169.254.0.0/16') `
    (Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv4 -Allow @('192.168.1.50')) 'one lab host splits its range'
Assert-Equal @('10.0.0.1-10.255.255.254') (Get-HeliosCiBlockList -Block @('10.0.0.0/8') -Allow @('10.255.255.255', '10.0.0.0')) `
    'lab hosts at both ends of a range'
Assert-Equal @('192.168.0.0-192.168.1.49', '192.168.1.52-192.168.255.255') `
    (Get-HeliosCiBlockList -Block @('192.168.0.0/16') -Allow @('192.168.1.51', '192.168.1.50', '192.168.1.50')) `
    'adjacent and repeated lab hosts'
Assert-Equal @('192.168.0.0', '192.168.0.2-192.168.255.255') (Get-HeliosCiBlockList -Block @('192.168.0.0/16') -Allow @('192.168.0.1')) `
    'a one-address remainder is a single address'
Assert-Equal @('fc00::-fd00::4', 'fd00::6-fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff', 'fe80::/10') `
    (Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv6 -Allow @('fd00::5')) 'an IPv6 lab host'
Assert-Equal @('10.0.0.0/8') (Get-HeliosCiBlockList -Block @('10.0.0.0/8') -Allow @('fd00::5')) 'the other family is untouched'
Assert-Equal @('8.8.8.8', '2001:db8::1') `
    (Get-HeliosCiStrayAddress -Block ($HeliosCiBlockedIPv4 + $HeliosCiBlockedIPv6) -Allow @('8.8.8.8', '10.1.1.1', '2001:db8::1')) `
    'addresses outside every blocked range are reported'
Assert-Throws { Get-HeliosCiBlockList -Block @('10.0.0.0/8') -Allow @('10.0.0.0/24') } 'not an IPv4 or IPv6 address' 'a range is not a lab address'
Assert-Throws { Get-HeliosCiBlockList -Block @('10.0.0.0/8') -Allow @('lab-host') } 'not an IPv4 or IPv6 address' 'a name is not a lab address'

# -- job-started.ps1: which jobs run -------------------------------------------------------------------------
. (Join-Path $here 'job-started.ps1')
$main = @{
    GITHUB_EVENT_NAME   = 'schedule'
    GITHUB_REF          = 'refs/heads/main'
    GITHUB_WORKFLOW_REF = 'PageMastr/HeliosEngine/.github/workflows/win-gpu.yml@refs/heads/main'
}
function With([hashtable]$Changes) {
    $copy = @{}
    foreach ($k in $main.Keys) { $copy[$k] = $main[$k] }
    foreach ($k in $Changes.Keys) { $copy[$k] = $Changes[$k] }
    return $copy
}
Assert-Equal $null (Test-HeliosCiJobAllowed -Environment $main) 'a scheduled run of main'
Assert-Equal $null (Test-HeliosCiJobAllowed -Environment (With @{ GITHUB_EVENT_NAME = 'push' })) 'a push to main'
Assert-Equal $null (Test-HeliosCiJobAllowed -Environment (With @{ GITHUB_EVENT_NAME = 'workflow_dispatch' })) 'a dispatch on main'
foreach ($case in @(
        @{ GITHUB_EVENT_NAME = 'pull_request'; GITHUB_REF = 'refs/pull/7/merge' },
        @{ GITHUB_EVENT_NAME = 'pull_request_target' },
        @{ GITHUB_EVENT_NAME = 'workflow_run' },
        @{ GITHUB_EVENT_NAME = 'repository_dispatch' },
        @{ GITHUB_EVENT_NAME = 'Push' },
        @{ GITHUB_EVENT_NAME = 'push'; GITHUB_REF = 'refs/heads/agent/claude/x';
           GITHUB_WORKFLOW_REF = 'PageMastr/HeliosEngine/.github/workflows/win-gpu.yml@refs/heads/agent/claude/x' },
        @{ GITHUB_EVENT_NAME = 'workflow_dispatch'; GITHUB_REF = 'refs/heads/feature' },
        @{ GITHUB_REF = 'refs/tags/v1' },
        @{ GITHUB_REF = 'refs/heads/Main' },
        @{ GITHUB_WORKFLOW_REF = 'PageMastr/HeliosEngine/.github/workflows/win-gpu.yml@refs/heads/other' },
        @{ GITHUB_EVENT_NAME = '' },
        @{ GITHUB_REF = $null },
        @{ GITHUB_WORKFLOW_REF = '' })) {
    $what = ($case.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '
    $script:checks++
    if (-not (Test-HeliosCiJobAllowed -Environment (With $case))) {
        $script:failures++
        Write-Host "FAIL: the hook accepts $what"
    }
}

# -- job-started.ps1: what it deletes ------------------------------------------------------------------------
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$root = Join-Path $WorkDir 'work'
$workspace = Join-Path (Join-Path $root 'HeliosEngine') 'HeliosEngine'
$outside = Join-Path $WorkDir 'outside'
function New-Tree {
    if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
    foreach ($dir in '_actions\actions\checkout', '_temp\_github_workflow', '_diag', '_PipelineMapping\PageMastr',
        '_tool\Python', 'scifi-test\scifi-test\build', 'HeliosEngine\old-sibling',
        'HeliosEngine\HeliosEngine\build\deep\a\b\c\d\e\f\g\h') {
        New-Item -ItemType Directory -Force -Path (Join-Path $root $dir.Replace('\', [IO.Path]::DirectorySeparatorChar)) | Out-Null
    }
    foreach ($file in '_actions\actions\checkout\action.yml', '_temp\_github_workflow\event.json', '_diag\Runner.log',
        'stray.txt', 'HeliosEngine\HeliosEngine\README.md', 'HeliosEngine\HeliosEngine\build\deep\a\b\c\d\e\f\g\h\x.obj',
        'HeliosEngine\HeliosEngine\.git-object') {
        Set-Content -LiteralPath (Join-Path $root $file.Replace('\', [IO.Path]::DirectorySeparatorChar)) -Value 'x'
    }
    New-Item -ItemType Directory -Force -Path $outside | Out-Null
    Set-Content -LiteralPath (Join-Path $outside 'precious.txt') -Value 'keep me'
}
function Get-Sorted([string[]]$Names) {
    $array = [string[]]@($Names)
    [Array]::Sort($array, [StringComparer]::Ordinal)
    return , $array
}
function Get-Relative([string[]]$Paths) {
    return Get-Sorted @($Paths | ForEach-Object { $_.Substring($root.Length + 1).Replace([IO.Path]::DirectorySeparatorChar, '/') })
}
New-Tree
Assert-Equal @('HeliosEngine/HeliosEngine/.git-object', 'HeliosEngine/HeliosEngine/README.md', 'HeliosEngine/HeliosEngine/build',
    'HeliosEngine/old-sibling', '_tool', 'scifi-test', 'stray.txt') `
    (Get-Relative (Get-HeliosCiWipeTargets -Root $root -Workspace $workspace)) 'wipe targets keep the runner''s directories and the workspace'
Assert-Throws { Get-HeliosCiWipeTargets -Root $root -Workspace '' } 'GITHUB_WORKSPACE is not set' 'no workspace'
Assert-Throws { Get-HeliosCiWipeTargets -Root $root -Workspace (Join-Path $root 'HeliosEngine') } 'is not <work>' 'workspace one level up'
Assert-Throws { Get-HeliosCiWipeTargets -Root $root -Workspace (Join-Path $outside 'a\b'.Replace('\', [IO.Path]::DirectorySeparatorChar)) } `
    'is not <work>' 'workspace outside the work directory'
Assert-Throws { Get-HeliosCiWipeTargets -Root (Join-Path $WorkDir 'missing') -Workspace $workspace } '' 'a missing work directory'

$onWindows = [IO.Path]::DirectorySeparatorChar -eq '\'
if (-not $onWindows) {
    Write-Host 'The wipe itself (cmd.exe rd, junctions) runs on Windows only; skipped here.'
} elseif ($root -notmatch '^[A-Za-z]:\\[A-Za-z0-9_.\\-]+$') {
    Write-Host "The wipe test needs a -WorkDir of letters, digits, '_', '.' and '-' (the hook refuses others); skipped: $root"
} else {
    New-Tree
    # A junction and (where this account may create one) a directory symbolic link inside the workspace, both
    # pointing outside the work directory, plus a read-only file.
    New-Item -ItemType Junction -Path (Join-Path $workspace 'junction') -Target $outside | Out-Null
    New-Item -ItemType Junction -Path (Join-Path $root 'top-junction') -Target $outside | Out-Null
    try { New-Item -ItemType SymbolicLink -Path (Join-Path $workspace 'build\symlink') -Target $outside | Out-Null } catch { }
    $readOnly = Join-Path $workspace 'build\read-only.pack'
    Set-Content -LiteralPath $readOnly -Value 'x'
    (Get-Item -LiteralPath $readOnly).Attributes = 'ReadOnly'
    $environment = With @{ GITHUB_WORKSPACE = $workspace }
    Assert-Equal 0 (Invoke-HeliosCiJobStarted -Root $root -Environment $environment) 'the wipe succeeds'
    Assert-Equal @('HeliosEngine', '_PipelineMapping', '_actions', '_diag', '_temp') `
        (Get-Sorted @(Get-ChildItem -LiteralPath $root -Force | ForEach-Object Name)) 'the work directory after the wipe'
    Assert-Equal @('HeliosEngine') @(Get-ChildItem -LiteralPath (Join-Path $root 'HeliosEngine') -Force | ForEach-Object Name) 'the pipeline directory'
    Assert-Equal 0 @(Get-ChildItem -LiteralPath $workspace -Force).Count 'the workspace is empty'
    Assert-Equal 'keep me' (Get-Content -LiteralPath (Join-Path $outside 'precious.txt')) 'link targets survive'
    Assert-Equal $true (Test-Path -LiteralPath (Join-Path $root '_actions\actions\checkout\action.yml')) 'this job''s actions survive'

    New-Tree
    $refused = With @{ GITHUB_WORKSPACE = $workspace; GITHUB_EVENT_NAME = 'pull_request' }
    Assert-Equal 1 (Invoke-HeliosCiJobStarted -Root $root -Environment $refused) 'a refused job fails'
    Assert-Equal $true (Test-Path -LiteralPath (Join-Path $root 'stray.txt')) 'a refused job deletes nothing'

    New-Tree
    Remove-Item -LiteralPath $workspace -Recurse -Force
    New-Item -ItemType Junction -Path $workspace -Target $outside | Out-Null
    Assert-Throws { Get-HeliosCiWipeTargets -Root $root -Workspace $workspace } 'not a plain directory' 'a workspace that is a link'
    Assert-Equal 'keep me' (Get-Content -LiteralPath (Join-Path $outside 'precious.txt')) 'a linked workspace is not followed'
    [IO.Directory]::Delete($workspace, $false)
}
if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }

if ($script:failures -gt 0) {
    Write-Host "runner scripts: $($script:failures) of $($script:checks) checks failed"
    exit 1
}
Write-Host "runner scripts: all $($script:checks) checks passed"
exit 0
