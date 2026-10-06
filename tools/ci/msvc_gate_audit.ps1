<#
.SYNOPSIS
  MSVC / clang-cl variant of the ISA audit's object checks (02 §1.1 audit item 2, RT-09).

.DESCRIPTION
  tools/lint/isa_audit.cmake checks the flags of every translation unit on all toolchains, but it
  disassembles the CPU-gate objects with GNU binutils. On Windows CI jobs this script does the same
  with dumpbin (run it from a Visual Studio developer prompt, after the build):
    * dumpbin /disasm:nobytes on cpu_gate.c.obj and cpu_gate_hook.c.obj: no VEX/EVEX (mnemonics
      starting with "v", ymm/zmm operands), BMI, LZCNT/TZCNT, POPCNT, MOVBE, CMPXCHG16B or SSE3+
      instruction;
    * dumpbin /symbols: External symbols are only the gate exports (defined) and the allowlisted
      imports (UNDEF), mirroring cmake/isa_allowlist.cmake, plus the compiler's literal pools (below).
  Exits 1 with one line per finding.
  No CI job runs it yet (WP-0.2r part 2 wires it into ci.yml), so its lists have not been checked against
  real dumpbin output.

.EXAMPLE
  pwsh tools/ci/msvc_gate_audit.ps1 -BuildDir build/windows-msvc-release
#>
param(
    [Parameter(Mandatory = $true)][string]$BuildDir
)
$ErrorActionPreference = 'Stop'

# Mirrors HELIOS_ISA_GATE_EXPORTS and HELIOS_ISA_GATE_ALLOWED_IMPORTS in cmake/isa_allowlist.cmake (02 §1.1).
$exports = @('helios_cpu_gate_run', 'helios_cpu_gate_verdict', 'helios_cpu_gate_tls_entry')
$imports = @(
    'helios_cpu_gate_run',
    '__imp_GetStdHandle', '__imp_WriteFile', '__imp_GetModuleHandleW', '__imp_GetEnvironmentVariableW',
    '__imp_LoadLibraryExW', '__imp_GetProcAddress', '__imp_GetCurrentProcess', '__imp_TerminateProcess',
    '__imp_AddVectoredExceptionHandler',
    'GetStdHandle', 'WriteFile', 'GetModuleHandleW', 'GetEnvironmentVariableW', 'LoadLibraryExW', 'GetProcAddress',
    'GetCurrentProcess', 'TerminateProcess', 'AddVectoredExceptionHandler',
    '__chkstk')  # no __security_cookie / __security_check_cookie (/GS-), no _RTC_* (no /RTC), no ExitProcess
$forbidden = '^(v[a-z0-9]+|andn|bextr|blsi|blsmsk|blsr|bzhi|pdep|pext|rorx|sarx|shlx|shrx|mulx|lzcnt|tzcnt|popcnt|movbe|cmpxchg16b|crc32|pshufb|palignr|phadd[wd]|phaddsw|phsub[wd]|phsubsw|pmaddubsw|pmulhrsw|psign[bwd]|pabs[bwd]|ptest|pblendw|pblendvb|blendps|blendpd|blendvps|blendvpd|round[sp][sd]|pminsb|pminsd|pminuw|pminud|pmaxsb|pmaxsd|pmaxuw|pmaxud|pmulld|pmuldq|pinsr[bdq]|pextr[bdq]|pcmpeqq|pcmpgtq|packusdw|pmov[sz]x[a-z]+|dpp[sd]|insertps|extractps|mpsadbw|phminposuw|pcmp[ei]str[im]|movntdqa|hadd[sp][sd]|hsub[sp][sd]|addsub[sp][sd]|movddup|movshdup|movsldup|lddqu|fisttp|k(mov|and|andn|or|xor|xnor|not|ortest|test|shiftl|shiftr|unpck|add)[bwdq]+|aes[a-z0-9]*|pclmul[a-z]*|sha1[a-z0-9]*|sha256[a-z0-9]*|adcx|adox|rdrand|rdseed|prefetchw|xsave[a-z0-9]*|xrstor[a-z0-9]*)$'
# Literal pools that cl and clang-cl emit as External "pick any" COMDATs so that equal literals merge across
# objects: string literals (??_C@..., /GF, which /O1 and /O2 imply) and floating-point and SSE constants
# (__real@..., __xmm@...). Their names encode their bytes, so whichever copy the linker keeps is identical:
# read-only data, never code, unlike a COMDAT function, which another object could replace.
$literalPools = '^(\?\?_C@|__real@|__xmm@)'
# Prefixes dumpbin prints as separate words ("lock cmpxchg16b ...").
$prefixes = @('lock', 'rep', 'repe', 'repne', 'repz', 'repnz', 'xacquire', 'xrelease', 'notrack', 'bnd')

$objects = Get-ChildItem -Path $BuildDir -Recurse -File |
    Where-Object { $_.Name -in @('cpu_gate.c.obj', 'cpu_gate_hook.c.obj') }
if (-not $objects) {
    Write-Error "no CPU-gate objects (cpu_gate.c.obj, cpu_gate_hook.c.obj) under $BuildDir"
    exit 1
}

$findings = New-Object System.Collections.Generic.List[string]
foreach ($obj in $objects) {
    $instructions = 0
    foreach ($line in (& dumpbin /nologo /disasm:nobytes $obj.FullName)) {
        if ($line -match '^\s+[0-9A-Fa-f]+:\s+([a-z0-9]+)\s*(.*)$') {
            $instructions++
            $mnemonic = $Matches[1]
            $operands = $Matches[2]
            while ($prefixes -contains $mnemonic -and $operands -match '^([a-z0-9]+)\s*(.*)$') {
                $mnemonic = $Matches[1]
                $operands = $Matches[2]
            }
            if ($mnemonic -match $forbidden -or $operands -match '(^|[^a-z0-9_])([yz]mm\d|k[1-7]([^a-z0-9_]|$))') {
                $findings.Add("$($obj.FullName): not allowed at the x86-64-v1 baseline: $($line.Trim())")
            }
        }
    }
    if ($instructions -eq 0) { $findings.Add("$($obj.FullName): dumpbin /disasm produced no instructions") }
    foreach ($line in (& dumpbin /nologo /symbols $obj.FullName)) {
        # 00A 00000000 SECT4  notype ()    External     | helios_cpu_gate_run
        if ($line -match 'External\s+\|\s+(\S+)') {
            $name = $Matches[1]
            if ($line -match '\bUNDEF\b') {
                if ($imports -notcontains $name) { $findings.Add("$($obj.FullName): references '$name' (not on the import allowlist)") }
            } elseif ($exports -notcontains $name -and $name -notmatch $literalPools) {
                $findings.Add("$($obj.FullName): exports '$name' (gate code must be static)")
            }
        }
    }
}

if ($findings.Count -gt 0) {
    $findings | ForEach-Object { Write-Host $_ }
    Write-Host "MSVC gate audit failed ($($findings.Count) finding(s))"
    exit 1
}
Write-Host "MSVC gate audit passed: $($objects.Count) objects"
