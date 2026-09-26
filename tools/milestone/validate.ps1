<#
.SYNOPSIS
  Validates a Helios milestone on Windows (docs/plan/09-roadmap-and-process.md section 5.9; WP-0.3).

.DESCRIPTION
  From the repository root, in plain PowerShell or a Visual Studio developer prompt:

    powershell -ExecutionPolicy Bypass -File tools\milestone\validate.ps1 -Milestone M0

  1. Enters the developer shell of the newest Visual Studio in [17.14, 19.0) (vswhere) unless cl.exe is
     already on PATH, then configures, builds and tests the windows-msvc-release preset. The build is
     incremental, so an up-to-date tree costs little and a stale or missing one is rebuilt.
  2. Checks the toolset: MSVC 19.44 (VS 2022 17.14, the floor) or any 19.5x (VS 2026, the primary).
  3. Builds the Go services into build\go\ and runs the scripted checks: helios-backend first-run and warm
     start times and idle memory (05 BE-A1), a dev1 login, helios-cell ticking a zone and helios-gateway
     listening, and a connect token from the session service.
  4. Evaluates the scorecard for the milestone's phase from this machine's results (needs Python 3), lists
     the interactive steps of 09 section 5.9's table, and writes build\milestone-<M>.txt to paste back.

  -NoInteractive skips opening the launcher. -SelfTest runs the script's own checks without building.
  Compatible with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [ValidateSet('M0', 'M1', 'M2', 'M3', 'M4')]
    [string]$Milestone = 'M0',
    [switch]$NoInteractive,
    [switch]$SelfTest
)

# Keep this file ASCII: Windows PowerShell 5.1 reads a script without a BOM in the ANSI code page.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Preset = 'windows-msvc-release'
$BuildDir = Join-Path $Root "build\$Preset"
$Work = Join-Path $Root 'build\milestone'
$script:Checks = New-Object System.Collections.Generic.List[object]

# The 09 section 5.9 table: what the user does and should see, per milestone.
$Manual = @{
    'M0' = @('Launcher: log in as dev1 / dev; the manifest is fetched and verified (needs WP-0.16, WP-0.17)',
             'Client: the RC-1 test scenes render (needs WP-0.17)',
             'Editor: edit the sample hull record, undo it; the undo is clean (needs WP-0.18)')
    'M1' = @('Launcher -> Play: the BENCH-2 descent with no loading screen, >= 60 fps at 1080p Medium on REF-class hardware',
             'Editor -> F5: PIE with 2 clients')
    'M2' = @('Trade between two accounts: escrow settles', 'Patch: the download is about the changed bytes',
             'Install the SDK and create a project from starter-blank: New Project -> PIE with no engine checkout')
    'M3' = @('Scripted BENCH tours and free play: the in-game budget overlay matches the nightly report',
             'Follow one starter-template tutorial: it works as written')
    'M4' = @('Scripted BENCH tours and free play: the in-game budget overlay matches the nightly report',
             'Follow one starter-template tutorial: it works as written')
}

function Add-Check([string]$Name, [string]$Status, [string]$Detail) {
    $script:Checks.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail })
    Write-Host ('[{0}] {1}: {2}' -f $Status, $Name, $Detail)
}

function Test-Toolset([string]$Version) {
    # 19.44 is VS 2022 17.14 (the floor, ADR-001a); 19.5x is VS 2026 (the primary).
    if ($Version -notmatch '^(\d+)\.(\d+)') { return $false }
    $major = [int]$Matches[1]
    $minor = [int]$Matches[2]
    return ($major -eq 19) -and (($minor -eq 44) -or ($minor -ge 50 -and $minor -le 59))
}

function Get-CompilerVersion([string]$Dir) {
    # CMake keeps the version in CMakeFiles\<version>\CMakeCXXCompiler.cmake; the cache has it only when
    # a project stores it there, so both are read.
    $cache = Join-Path $Dir 'CMakeCache.txt'
    if (Test-Path $cache) {
        $hit = Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER_VERSION(:\w+)?=(.+)$' | Select-Object -First 1
        if ($hit) { return $hit.Matches[0].Groups[2].Value.Trim() }
    }
    $files = @(Get-ChildItem -Path (Join-Path $Dir 'CMakeFiles') -Filter 'CMakeCXXCompiler.cmake' -Recurse -ErrorAction SilentlyContinue)
    foreach ($f in $files) {
        $hit = Select-String -Path $f.FullName -Pattern 'set\(CMAKE_CXX_COMPILER_VERSION "([^"]+)"\)' | Select-Object -First 1
        if ($hit) { return $hit.Matches[0].Groups[1].Value }
    }
    return $null
}

function Get-JUnitSummary([string]$Path) {
    [xml]$doc = Get-Content -Raw -Path $Path
    $cases = @($doc.SelectNodes('//testcase'))
    $failed = @($cases | Where-Object { $_.SelectSingleNode('failure') -or $_.GetAttribute('status') -eq 'fail' })
    $skipped = @($cases | Where-Object { $_.SelectSingleNode('skipped') -or $_.GetAttribute('status') -eq 'notrun' })
    return [pscustomobject]@{ Total = $cases.Count; Failed = $failed.Count; Skipped = $skipped.Count
                              FailedNames = @($failed | ForEach-Object { $_.GetAttribute('name') }) }
}

function Format-Report([string]$M, [string]$Toolset, [object[]]$Checks, [string[]]$Steps, [string]$Scorecard) {
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("Helios milestone $M validation (tools/milestone/validate.ps1)")
    $lines.Add('Date: ' + (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss') + ' UTC')
    $lines.Add("Toolset: $Toolset")
    foreach ($c in $Checks) { $lines.Add(('[{0}] {1}: {2}' -f $c.Status, $c.Name, $c.Detail)) }
    $lines.Add('')
    $lines.Add("Interactive steps for $M (reply with what you saw for each):")
    foreach ($s in $Steps) { $lines.Add("- $s") }
    if ($Scorecard) { $lines.Add(''); $lines.Add($Scorecard) }
    return ($lines -join "`r`n")
}

function Join-Arguments([string[]]$Arguments) {
    return (($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' ')
}

function Find-Python {
    # The Windows Store alias `python` exits 9009 and prints to stderr; stderr must not stop the script.
    $ErrorActionPreference = 'Continue'
    foreach ($candidate in @(@('py', '-3'), @('python3'), @('python'))) {
        if (-not (Get-Command $candidate[0] -ErrorAction SilentlyContinue)) { continue }
        $rest = @($candidate | Select-Object -Skip 1)
        try { $version = & $candidate[0] @rest --version 2>&1 | Out-String } catch { continue }
        if ($LASTEXITCODE -eq 0 -and $version -match 'Python 3\.') { return , $candidate }
    }
    return $null
}

function Invoke-Native([string]$Name, [scriptblock]$Command) {
    Write-Host "== $Name"
    & $Command | Out-Host
    $code = $LASTEXITCODE
    if ($code -ne 0) { Add-Check $Name 'FAIL' "exit code $code"; return $false }
    Add-Check $Name 'PASS' ''
    return $true
}

function Enter-DevShell {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return 'cl.exe already on PATH' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found: install Visual Studio 2026, or 2022 17.14 or later, with "Desktop development with C++"' }
    $vs = & $vswhere -version '[17.14,19.0)' -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio 17.14 or later with the C++ tools: install "Desktop development with C++" (09 section 5.9)' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    return "entered the developer shell of $vs"
}

function Start-Logged([string]$Exe, [string[]]$Arguments, [string]$Log) {
    return Start-Process -FilePath $Exe -ArgumentList (Join-Arguments $Arguments) -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $Log -RedirectStandardError "$Log.err"
}

function Wait-Ready([string]$Url, [int]$TimeoutSeconds, [System.Diagnostics.Stopwatch]$Clock) {
    while ($Clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        try {
            if ((Invoke-WebRequest -Uri $Url -UseBasicParsing -TimeoutSec 2).StatusCode -eq 200) { return $Clock.Elapsed.TotalSeconds }
        } catch { Start-Sleep -Milliseconds 100 }
    }
    return -1
}

function Get-PostgresProcesses([string]$DataDir) {
    # The postmaster's command line names the data directory (in either separator and any case); its
    # workers' command lines do not, so they are found by parent.
    $needle = $DataDir.Replace('/', '\').ToLowerInvariant()
    $all = @(Get-CimInstance Win32_Process -Filter "Name = 'postgres.exe'")
    $main = @($all | Where-Object { $_.CommandLine -and $_.CommandLine.Replace('/', '\').ToLowerInvariant().Contains($needle) })
    $ids = @($main | ForEach-Object { $_.ProcessId })
    return @($main) + @($all | Where-Object { $ids -contains $_.ParentProcessId })
}

function Stop-Tree([object[]]$Processes, [string]$DataDir) {
    foreach ($p in $Processes) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
    foreach ($p in (Get-PostgresProcesses $DataDir)) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Wait-LogLine([string[]]$Logs, [string]$Pattern, [int]$TimeoutSeconds) {
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
        foreach ($log in $Logs) {
            if (Test-Path $log) {
                try { $hit = Select-String -Path $log -Pattern $Pattern | Select-Object -Last 1 } catch { $hit = $null }
                if ($hit) { return $hit.Line.Trim() }
            }
        }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

function Invoke-Services {
    $backendExe = Join-Path $Root 'build\go\helios-backend.exe'
    $bin = Join-Path $BuildDir 'bin'
    $data = Join-Path $Work 'backend'
    if (Test-Path $data) { Remove-Item -Recurse -Force $data }
    $logs = Join-Path $Work 'logs'
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    $procs = @()
    try {
        $runArgs = @('run', '--seed', 'dev', '--data', $data)
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        $backend = Start-Logged $backendExe $runArgs (Join-Path $logs 'backend-first.log')
        $procs += $backend
        $first = Wait-Ready 'http://127.0.0.1:7701/readyz' 120 $clock
        if ($first -lt 0) { Add-Check 'backend first run' 'FAIL' 'not ready within 120 s (see build\milestone\logs)'; return }
        Add-Check 'backend first run' $(if ($first -le 30) { 'PASS' } else { 'FAIL' }) ('{0:N1} s (BE-A1: <= 30 s with PostgreSQL binaries cached)' -f $first)
        Stop-Tree @($backend) $data
        Start-Sleep -Seconds 2
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        $backend = Start-Logged $backendExe $runArgs (Join-Path $logs 'backend.log')
        $procs += $backend
        $warm = Wait-Ready 'http://127.0.0.1:7701/readyz' 60 $clock
        if ($warm -lt 0) { Add-Check 'backend warm start' 'FAIL' 'not ready within 60 s'; return }
        Add-Check 'backend warm start' $(if ($warm -le 5) { 'PASS' } else { 'FAIL' }) ('{0:N1} s after a hard stop (BE-A1: <= 5 s)' -f $warm)
        Start-Sleep -Seconds 10
        $bytes = (Get-Process -Id $backend.Id).WorkingSet64
        foreach ($p in (Get-PostgresProcesses $data)) {
            $proc = Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue
            if ($proc) { $bytes += $proc.WorkingSet64 }
        }
        $mb = $bytes / 1MB
        Add-Check 'backend idle memory' $(if ($mb -le 500) { 'PASS' } else { 'FAIL' }) ('{0:N0} MB working set incl. PostgreSQL (BE-A1: <= 500 MB)' -f $mb)

        $login = Invoke-RestMethod -Method Post -ContentType 'application/json' -Body '{"login":"dev1#0001","password":"dev"}' `
            -Uri 'http://127.0.0.1:7700/helios.identity.v1.Identity/Login'
        Add-Check 'login as dev1' $(if ($login.accessToken) { 'PASS' } else { 'FAIL' }) "account $($login.accountId)"

        $cellLog = Join-Path $logs 'cell.log'
        $gwLog = Join-Path $logs 'gateway.log'
        $nats = @('--nats', 'nats://127.0.0.1:4222', '--backend-data', $data)
        $procs += Start-Logged (Join-Path $bin 'helios-cell.exe') ($nats + @('--name', 'cell-m', '--zone', 'dev-sandbox')) $cellLog
        $procs += Start-Logged (Join-Path $bin 'helios-gateway.exe') ($nats + @('--name', 'gw-m', '--listen', '127.0.0.1:7777')) $gwLog
        $tick = Wait-LogLine @($cellLog, "$cellLog.err") "zone \d+ 'dev-sandbox': tick \d+" 60
        Add-Check 'cell ticks a zone' $(if ($tick) { 'PASS' } else { 'FAIL' }) $(if ($tick) { $tick } else { 'no tick line in 60 s (see build\milestone\logs\cell.log)' })
        $listen = Wait-LogLine @($gwLog, "$gwLog.err") 'listening on' 30
        Add-Check 'gateway listens' $(if ($listen) { 'PASS' } else { 'FAIL' }) $(if ($listen) { $listen } else { 'no listening line in 30 s' })
        Start-Sleep -Seconds 3
        $headers = @{ Authorization = "Bearer $($login.accessToken)" }
        $session = Invoke-RestMethod -Method Post -ContentType 'application/json' -Headers $headers -Body '{"zoneId":"1001"}' `
            -Uri 'http://127.0.0.1:7700/helios.session.v1.Session/CreateSession'
        Add-Check 'connect token' $(if ($session.connectToken) { 'PASS' } else { 'FAIL' }) ("gateways: " + ($session.gateways -join ', '))
    } catch {
        Add-Check 'services' 'FAIL' $_.Exception.Message
    } finally {
        Stop-Tree $procs $data
    }
}

function Invoke-Scorecard([int]$Phase, [string]$Results) {
    $py = Find-Python
    if (-not $py) { Add-Check 'scorecard' 'SKIP' 'Python 3 not found; install it to add the scorecard evaluation'; return $null }
    $exe = $py[0]
    $pre = @($py | Select-Object -Skip 1)
    $out = Join-Path $Work 'scorecard'
    & $exe @pre (Join-Path $Root 'tools\scorecard\runners.py') doctest --build-dir $BuildDir --out (Join-Path $Results 'doctest') | Out-Host
    & $exe @pre (Join-Path $Root 'tools\scorecard\report.py') --results (Split-Path $Results) --phase $Phase `
        --out "$out.json" --markdown "$out.md" | Out-Null
    if ($LASTEXITCODE -ne 0) { Add-Check 'scorecard' 'FAIL' "report.py exit code $LASTEXITCODE"; return $null }
    Add-Check 'scorecard' 'PASS' "phase $Phase evaluated from this machine's results (build\milestone\scorecard.md)"
    return (Get-Content -Raw "$out.md")
}

function Invoke-SelfTest {
    $expect = { param($ok, $what) if (-not $ok) { Write-Host "FAIL: $what"; $script:failures++ } }
    $script:failures = 0
    foreach ($v in @('19.44.35207.1', '19.50.1', '19.59.0')) { & $expect (Test-Toolset $v) "accepts $v" }
    foreach ($v in @('19.43.34808', '19.45.0', '19.60.0', '18.0', 'garbage')) { & $expect (-not (Test-Toolset $v)) "rejects $v" }
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('helios-validate-' + [guid]::NewGuid())
    New-Item -ItemType Directory -Force -Path (Join-Path $tmp 'CMakeFiles\3.31.0') | Out-Null
    try {
        & $expect ($null -eq (Get-CompilerVersion $tmp)) 'no version without CMake files'
        Set-Content -Path (Join-Path $tmp 'CMakeFiles\3.31.0\CMakeCXXCompiler.cmake') -Value 'set(CMAKE_CXX_COMPILER_VERSION "19.44.35211.0")'
        & $expect ((Get-CompilerVersion $tmp) -eq '19.44.35211.0') 'reads CMakeCXXCompiler.cmake'
        Set-Content -Path (Join-Path $tmp 'CMakeCache.txt') -Value 'CMAKE_CXX_COMPILER_VERSION:STRING=19.50.35700.0'
        & $expect ((Get-CompilerVersion $tmp) -eq '19.50.35700.0') 'prefers the cache entry'
        $junit = Join-Path $tmp 'ctest.xml'
        Set-Content -Path $junit -Value ('<testsuite><testcase name="a" status="run"/><testcase name="b" status="fail"><failure/></testcase>' +
                                         '<testcase name="c" status="notrun"><skipped/></testcase></testsuite>')
        $sum = Get-JUnitSummary $junit
        & $expect ($sum.Total -eq 3 -and $sum.Failed -eq 1 -and $sum.Skipped -eq 1 -and $sum.FailedNames[0] -eq 'b') 'summarizes JUnit'
        $text = Format-Report 'M0' 'MSVC 19.44' @([pscustomobject]@{ Name = 'x'; Status = 'PASS'; Detail = 'd' }) @('step') ''
        & $expect ($text -match '\[PASS\] x: d' -and $text -match '- step' -and $text -match 'milestone M0') 'formats the report'
        & $expect ((Join-Arguments @('a', 'b c', 'd"e')) -eq 'a "b c" "d\"e"') 'quotes arguments'
        & $expect ($Manual.ContainsKey('M0') -and $Manual.ContainsKey('M4')) 'lists every milestone'
        $wide = @([System.IO.File]::ReadAllBytes($PSCommandPath) | Where-Object { $_ -gt 127 })
        & $expect ($wide.Count -eq 0) 'the script is ASCII (Windows PowerShell 5.1 reads it as ANSI)'
    } finally {
        Remove-Item -Recurse -Force $tmp
    }
    if ($script:failures -gt 0) { Write-Host "validate.ps1 self-test: $($script:failures) failure(s)"; exit 1 }
    Write-Host 'validate.ps1 self-test: OK'
    exit 0
}

if ($SelfTest) { Invoke-SelfTest }

Set-Location $Root
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$results = Join-Path $Work 'results\windows-local'
if (Test-Path $results) { Remove-Item -Recurse -Force $results }
New-Item -ItemType Directory -Force -Path $results | Out-Null
Set-Content -Encoding ascii -Path (Join-Path $results 'run.json') -Value '{"run": "windows-local"}'

$toolset = 'unknown'
try {
    Add-Check 'developer shell' 'PASS' (Enter-DevShell)
    if (Invoke-Native 'configure' { cmake --preset $Preset }) {
        $version = Get-CompilerVersion $BuildDir
        if (-not $version) { $version = 'unknown' }
        if (-not (Test-Toolset $version)) {
            Add-Check 'toolset' 'FAIL' "MSVC $version is not 19.44 (VS 2022 17.14) or 19.5x (VS 2026): install VS 2026, or update VS 2022 to 17.14 or later"
            throw 'unsupported toolset'
        }
        $toolset = 'MSVC ' + $version + $(if ($version -like '19.44*') { ' (VS 2022 17.14, the floor)' } else { ' (VS 2026, the primary)' })
        Add-Check 'toolset' 'PASS' $toolset
        if (Invoke-Native 'build' { cmake --build --preset $Preset }) {
            $junit = Join-Path $results 'ctest.xml'
            & ctest --preset $Preset --output-junit $junit | Out-Host
            $sum = Get-JUnitSummary $junit
            $detail = '{0} tests, {1} failed, {2} skipped' -f $sum.Total, $sum.Failed, $sum.Skipped
            if ($sum.Failed -gt 0) { $detail += ': ' + ($sum.FailedNames -join ', ') }
            Add-Check 'ctest' $(if ($sum.Failed -eq 0 -and $sum.Total -gt 0) { 'PASS' } else { 'FAIL' }) $detail
        }
    }
    if (Get-Command go -ErrorAction SilentlyContinue) {
        Push-Location (Join-Path $Root 'services')
        try { $built = Invoke-Native 'go build' { go build -o ..\build\go\ ./cmd/... } } finally { Pop-Location }
        if ($built -and (Test-Path (Join-Path $BuildDir 'bin\helios-cell.exe'))) { Invoke-Services }
    } else {
        Add-Check 'go build' 'FAIL' 'Go not found: install any Go >= 1.21 (services/go.mod pins go1.27.1)'
    }
} catch {
    if ($_.Exception.Message -ne 'unsupported toolset') { Add-Check 'validation' 'FAIL' $_.Exception.Message }
}

$launcher = Join-Path $BuildDir 'bin\helios-launcher.exe'
if (Test-Path $launcher) {
    if (-not $NoInteractive) { Start-Process -FilePath $launcher -ArgumentList '--channel dev' | Out-Null }
    Add-Check 'launcher' 'PASS' $(if ($NoInteractive) { 'built (not opened: -NoInteractive)' } else { 'opened for the interactive steps' })
} else {
    Add-Check 'launcher' 'SKIP' 'helios-launcher.exe is not built yet (WP-0.17)'
}

$scorecard = $null
if (Test-Path (Join-Path $results 'ctest.xml')) { $scorecard = Invoke-Scorecard ([int]$Milestone.Substring(1)) $results }
$report = Format-Report $Milestone $toolset $script:Checks.ToArray() $Manual[$Milestone] $scorecard
$reportPath = Join-Path $Root "build\milestone-$Milestone.txt"
Set-Content -Encoding utf8 -Path $reportPath -Value $report
Write-Host ''
Write-Host "Wrote ${reportPath}: paste it back, with what you saw for each interactive step."
if (@($script:Checks | Where-Object { $_.Status -eq 'FAIL' }).Count -gt 0) { exit 1 }
exit 0
