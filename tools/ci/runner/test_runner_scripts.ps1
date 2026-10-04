# Tests for the win-gpu runner scripts (WP-0.4): CTest `lint_runner_scripts` (label lint) runs them with
# PowerShell 7 wherever pwsh is installed (every GitHub-hosted image), Windows PowerShell 5.1 can run them too.
#
#   pwsh -NoProfile -File tools/ci/runner/test_runner_scripts.ps1 [-WorkDir DIR]
#
# Everywhere: every .ps1 here parses and is ASCII; firewall.ps1's block lists (CIDR splitting around lab
# addresses, IPv4 and IPv6, stray and non-canonical addresses) and the rules it adds and removes (the firewall
# cmdlets mocked); job-started.ps1's refusal rules, how it ends a refused job (the process cmdlets mocked) and the
# entries it would delete; every PowerShell block of docs/runbooks/win-gpu-runner.md parses, and its step 4b audit
# (what helios-ci can open at the root of each local drive), step 9's check before the service starts (the hook is
# set in .env and readable by helios-ci) and the rotate blocks (what helios-ci owns; the hook's ACL for a new
# account) run against stand-ins for WMI, the account, file, ACL and service cmdlets. The
# scripts have no test switch: their functions and constants are loaded from the parsed files, so their last block
# (the run) never runs here. On Windows also the wipe itself on a scratch tree under -WorkDir: a junction to a
# directory outside, a directory symbolic link where creating one is allowed, read-only files and a deep tree; the
# links go, their targets stay. The last line says whether the wipe ran, and CTest requires "(wipe: ran)" on Windows
# (tools/ci/CMakeLists.txt).
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

# -- Every script parses and is ASCII ------------------------------------------------------------------------
# Windows PowerShell 5.1 reads a file without a BOM as ANSI: a non-ASCII character becomes mojibake, and some
# (the UTF-8 bytes of a dash end in 0x94, an ANSI curly quote) change how a string parses.
foreach ($file in Get-ChildItem -LiteralPath $here -Filter *.ps1) {
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors)
    Assert-Equal '' (($errors | ForEach-Object { "$($_.Extent.StartLineNumber): $($_.Message)" }) -join '; ') "$($file.Name) parses"
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    Assert-Equal 0 @($bytes | Where-Object { $_ -gt 127 }).Count "$($file.Name) is ASCII"
}

# A script's functions and top-level assignments (its constants), from its syntax tree, as a script block to
# dot-source; everything else at its top level (its param block, Set-StrictMode, the run itself) is left out.
function Get-HeliosCiScriptDefinitions([string]$Path) {
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    $parts = @($ast.EndBlock.Statements | Where-Object {
            $_ -is [System.Management.Automation.Language.FunctionDefinitionAst] -or
            $_ -is [System.Management.Automation.Language.AssignmentStatementAst]
        } | ForEach-Object { $_.Extent.Text })
    return [scriptblock]::Create($parts -join [Environment]::NewLine)
}
Set-StrictMode -Version 2.0
Add-Type -AssemblyName System.Numerics

# -- firewall.ps1 --------------------------------------------------------------------------------------------
. (Get-HeliosCiScriptDefinitions (Join-Path $here 'firewall.ps1'))
$ipv4 = @('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', '169.254.0.0/16', '100.64.0.0/10', '224.0.0.0/4',
    '255.255.255.255/32')
Assert-Equal $ipv4 (Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv4 -Allow @()) 'IPv4 without lab addresses'
Assert-Equal @('fc00::/7', 'fe80::/10', 'ff00::/8') (Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv6) 'IPv6 without lab addresses'
Assert-Equal @('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0-192.168.1.49', '192.168.1.51-192.168.255.255', '169.254.0.0/16',
    '100.64.0.0/10', '224.0.0.0/4', '255.255.255.255/32') `
    (Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv4 -Allow @('192.168.1.50')) 'one lab host splits its range'
Assert-Equal @('10.0.0.1-10.255.255.254') (Get-HeliosCiBlockList -Block @('10.0.0.0/8') -Allow @('10.255.255.255', '10.0.0.0')) `
    'lab hosts at both ends of a range'
Assert-Equal @('192.168.0.0-192.168.1.49', '192.168.1.52-192.168.255.255') `
    (Get-HeliosCiBlockList -Block @('192.168.0.0/16') -Allow @('192.168.1.51', '192.168.1.50', '192.168.1.50')) `
    'adjacent and repeated lab hosts'
Assert-Equal @('192.168.0.0', '192.168.0.2-192.168.255.255') (Get-HeliosCiBlockList -Block @('192.168.0.0/16') -Allow @('192.168.0.1')) `
    'a one-address remainder is a single address'
Assert-Equal @('fc00::-fd00::4', 'fd00::6-fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff', 'fe80::/10', 'ff00::/8') `
    (Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv6 -Allow @('fd00::5')) 'an IPv6 lab host'
Assert-Equal @('100.64.0.0-100.100.100.99', '100.100.100.101-100.127.255.255') `
    (Get-HeliosCiBlockList -Block @('100.64.0.0/10') -Allow @('100.100.100.100')) 'a lab host on an overlay network'
Assert-Equal @('10.0.0.0/8') (Get-HeliosCiBlockList -Block @('10.0.0.0/8') -Allow @('fd00::5')) 'the other family is untouched'
Assert-Equal @('8.8.8.8', '2001:db8::1') `
    (Get-HeliosCiStrayAddress -Block ($HeliosCiBlockedIPv4 + $HeliosCiBlockedIPv6) -Allow @('8.8.8.8', '10.1.1.1', '2001:db8::1')) `
    'addresses outside every blocked range are reported'
Assert-Throws { Get-HeliosCiBlockList -Block @('10.0.0.0/8') -Allow @('10.0.0.0/24') } 'not an IPv4 or IPv6 address' 'a range is not a lab address'
Assert-Throws { Get-HeliosCiBlockList -Block @('10.0.0.0/8') -Allow @('lab-host') } 'not an IPv4 or IPv6 address' 'a name is not a lab address'
# IPAddress.TryParse also reads shorthand that means another address; each would open a host nobody named.
foreach ($shorthand in '192.168.150', '10.1', '3232235876', '192.168.001.050', '0x0a.0.0.1', 'fd00:0:0:0:0:0:0:5') {
    Assert-Throws { Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv4 -Allow @($shorthand) } 'not written as a full' `
        "a shorthand lab address ($shorthand) fails"
}
Assert-Equal @('fc00::-fd00::4', 'fd00::6-fdff:ffff:ffff:ffff:ffff:ffff:ffff:ffff', 'fe80::/10', 'ff00::/8') `
    (Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv6 -Allow @('FD00::5')) 'an IPv6 lab address in upper case'

# Invoke-HeliosCiFirewall with the firewall cmdlets, the elevation check and the SID lookup mocked (functions win
# over cmdlets): what it removes and adds, and that nothing is removed when the new rules cannot be made.
$script:fwExisting = @('old-rule')
$script:fwLog = New-Object System.Collections.Generic.List[string]
function Test-HeliosCiElevated { return $true }
function Get-HeliosCiAccountSid {
    param([string]$Name)
    if ($Name -ceq 'helios-ci') { return 'S-1-5-21-1-2-3-1001' }
    throw "No mapping between account names and security IDs was done ($Name)"
}
function Get-NetFirewallRule {
    [CmdletBinding()] param([string]$Group)
    $script:fwLog.Add("get $Group")
    return $script:fwExisting
}
function Remove-NetFirewallRule {
    [CmdletBinding()] param([Parameter(ValueFromPipeline = $true)] $InputObject)
    process { $script:fwLog.Add("remove $InputObject") }
}
function New-NetFirewallRule {
    [CmdletBinding()]
    param($Name, $DisplayName, $Group, $Description, $Direction, $Action, $Profile, [string[]]$RemoteAddress, $LocalUser)
    $script:fwLog.Add("new $Name $Direction $Action $Profile $LocalUser [$($RemoteAddress -join ',')]")
}
$fwGroup = 'get Helios CI runner: LAN block for helios-ci'
Invoke-HeliosCiFirewall -Account 'helios-ci' 6>$null
Assert-Equal @($fwGroup, 'remove old-rule',
    "new Helios-CI-LAN-Block-helios-ci-IPv4 Outbound Block Any D:(A;;CC;;;S-1-5-21-1-2-3-1001) [$($ipv4 -join ',')]",
    'new Helios-CI-LAN-Block-helios-ci-IPv6 Outbound Block Any D:(A;;CC;;;S-1-5-21-1-2-3-1001) [fc00::/7,fe80::/10,ff00::/8]',
    'new Helios-CI-LAN-Block-helios-ci-LocalSubnet Outbound Block Any D:(A;;CC;;;S-1-5-21-1-2-3-1001) [LocalSubnet]') `
    $script:fwLog.ToArray() 'the firewall replaces its rules for the account'
$script:fwLog.Clear()
Invoke-HeliosCiFirewall -Account 'helios-ci' -AllowAddress '192.168.1.50' 6>$null 3>$null
Assert-Equal @($fwGroup, 'remove old-rule', 'Helios-CI-LAN-Block-helios-ci-IPv4', 'Helios-CI-LAN-Block-helios-ci-IPv6') `
    @($script:fwLog | ForEach-Object { if ($_ -like 'new *') { $_.Split(' ')[1] } else { $_ } }) `
    'with a lab address, no LocalSubnet rule'
Assert-Equal $true ($script:fwLog[2] -like '*,192.168.0.0-192.168.1.49,192.168.1.51-192.168.255.255,*') 'the lab address is left out'
$script:fwLog.Clear()
try { Invoke-HeliosCiFirewall -Account 'deleted-account' -Remove 6>$null } catch { $script:fwLog.Add("error: $($_.Exception.Message)") }
Assert-Equal @('get Helios CI runner: LAN block for deleted-account', 'remove old-rule') $script:fwLog.ToArray() `
    '-Remove works for an account that no longer exists'
$script:fwLog.Clear()
Assert-Throws { Invoke-HeliosCiFirewall -Account 'typo' 6>$null } 'No mapping' 'an unknown account fails'
Assert-Throws { Invoke-HeliosCiFirewall -Account 'helios-ci' -AllowAddress '8.8.8.8' 6>$null } 'Not in a blocked range' `
    'a stray lab address fails'
Assert-Throws { Invoke-HeliosCiFirewall -Account 'helios-ci' -AllowAddress '192.168.150' 6>$null } 'not written as a full' `
    'a shorthand lab address fails before any rule changes'
Assert-Equal 0 $script:fwLog.Count 'a failed run removes no rule'

# -- docs/runbooks/win-gpu-runner.md -------------------------------------------------------------------------
# The owner pastes the runbook's PowerShell into an elevated window, so every block must parse. Step 4b's audit (what
# helios-ci can read or change at the root of each local drive) runs here against stand-ins for WMI, Get-ChildItem
# and Get-Acl (functions win over cmdlets); an ACL is an object with the members the audit reads.
$repoRoot = Split-Path (Split-Path (Split-Path $here))
$runbook = [IO.File]::ReadAllText([IO.Path]::Combine($repoRoot, 'docs', 'runbooks', 'win-gpu-runner.md'))
$blocks = @([regex]::Matches($runbook, '(?ms)^[ \t]*```powershell[ \t]*\r?\n(.*?)^[ \t]*```') |
        ForEach-Object { $_.Groups[1].Value })
$script:checks++
if ($blocks.Count -lt 10) {
    $script:failures++
    Write-Host "FAIL: the runbook has $($blocks.Count) PowerShell blocks; expected at least 10"
}
foreach ($block in $blocks) {
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseInput($block, [ref]$tokens, [ref]$errors)
    Assert-Equal '' (($errors | ForEach-Object { "$($_.Extent.StartLineNumber): $($_.Message)" }) -join '; ') `
        "the runbook block '$(($block.Trim() -split "`n")[0].Trim())' parses"
}
$audit = @($blocks | Where-Object { $_ -match '(?m)^# Step 4b audit:' })
Assert-Equal 1 $audit.Count 'the runbook has one step 4b audit'

$ciSid = 'S-1-5-21-1-2-3-1002'
function New-AuditRule([string]$Sid, [int]$Rights, [string]$Type = 'Allow', [switch]$Inherited) {
    return [pscustomobject]@{ IdentityReference = [pscustomobject]@{ Value = $Sid }; AccessControlType = $Type
        FileSystemRights = $Rights; IsInherited = [bool]$Inherited }
}
# What an IdentityReference of $Type says for $Sid: the SID itself for SecurityIdentifier, an account name otherwise
# (as NTAccount gives), so that code which asks for names matches none of the SIDs it looks for.
$auditNames = @{ 'S-1-1-0' = 'Everyone'; 'S-1-2-0' = 'LOCAL'; 'S-1-5-6' = 'NT AUTHORITY\SERVICE'
    'S-1-5-11' = 'NT AUTHORITY\Authenticated Users'; 'S-1-5-15' = 'NT AUTHORITY\This Organization'
    'S-1-5-18' = 'NT AUTHORITY\SYSTEM'; 'S-1-5-32-544' = 'BUILTIN\Administrators'; 'S-1-5-32-545' = 'BUILTIN\Users'
    'S-1-5-113' = 'NT AUTHORITY\Local account'; $ciSid = 'PC\helios-ci' }
function ConvertTo-AuditIdentity([string]$Sid, $Type) {
    if ($Type -eq [System.Security.Principal.SecurityIdentifier]) { return $Sid }
    if ($auditNames.ContainsKey($Sid)) { return $auditNames[$Sid] }
    return 'PC\user' + $Sid.Split('-')[-1]
}
# An ACL as FileSystemSecurity gives it: GetAccessRules(includeExplicit, includeInherited, targetType) returns only the
# kinds of entries asked for, and GetOwner and GetAccessRules name accounts in the targetType asked for.
function New-AuditAcl([string]$Owner, [object[]]$Rules = @()) {
    $acl = [pscustomobject]@{ OwnerSid = $Owner; Rules = $Rules }
    $acl | Add-Member -MemberType ScriptMethod -Name GetOwner -Value {
        param($Type)
        [pscustomobject]@{ Value = (ConvertTo-AuditIdentity $this.OwnerSid $Type) }
    }
    $acl | Add-Member -MemberType ScriptMethod -Name GetAccessRules -Value {
        param([bool]$Explicit, [bool]$Inherited, $Type)
        foreach ($rule in $this.Rules) {
            if (($rule.IsInherited -and $Inherited) -or (-not $rule.IsInherited -and $Explicit)) {
                [pscustomobject]@{
                    IdentityReference = [pscustomobject]@{ Value = (ConvertTo-AuditIdentity $rule.IdentityReference.Value $Type) }
                    AccessControlType = $rule.AccessControlType; FileSystemRights = $rule.FileSystemRights
                    IsInherited = $rule.IsInherited
                }
            }
        }
    }
    return $acl
}
# Runs the audit on $AuditDisks (drive -> file system), $AuditItems (drive root -> names) and $AuditAcls (path -> ACL,
# or the message Get-Acl fails with), in a child scope so that its variables cannot change this script's. Leaves its
# rows ('path|access|who') in $script:auditRows, its output in $script:auditOutput and its warnings in
# $script:auditWarnings.
function Invoke-HeliosCiAudit([hashtable]$AuditDisks, [hashtable]$AuditItems, [hashtable]$AuditAcls,
    [string]$AuditAccount = $ciSid) {
    function Get-CimInstance {
        [CmdletBinding()]
        param([Parameter(Position = 0)] [string]$ClassName, [string]$Filter)
        if ($ClassName -eq 'Win32_UserAccount' -and $Filter -eq "LocalAccount = TRUE AND Name = 'helios-ci'") {
            if ($AuditAccount) { [pscustomobject]@{ SID = $AuditAccount } }
        } elseif ($ClassName -eq 'Win32_LogicalDisk' -and $Filter -eq 'DriveType = 2 OR DriveType = 3') {
            foreach ($id in @($AuditDisks.Keys | Sort-Object)) { [pscustomobject]@{ DeviceID = $id; FileSystem = $AuditDisks[$id] } }
        } else {
            throw "unexpected Get-CimInstance $ClassName -Filter $Filter"
        }
    }
    function Get-ChildItem {
        [CmdletBinding()]
        param([string]$LiteralPath, [switch]$Force)
        if (-not $Force) { return }   # the audit must list hidden items too
        foreach ($name in @($AuditItems[$LiteralPath])) { [pscustomobject]@{ Name = $name; FullName = $LiteralPath + $name } }
    }
    function Get-Acl {
        [CmdletBinding()]
        param([string]$LiteralPath)
        $entry = $AuditAcls[$LiteralPath]
        if ($entry -is [string]) { throw $entry }
        if ($null -eq $entry) { throw "unexpected Get-Acl $LiteralPath" }
        return $entry
    }
    function Write-Warning([string]$Message) { $script:auditWarnings.Add($Message) }
    $script:auditWarnings = New-Object System.Collections.Generic.List[string]
    $script:auditRows = @()
    $script:auditOutput = @()
    $savedDrive = $env:SystemDrive
    $env:SystemDrive = 'C:'
    try {
        & {
            $script:auditOutput = @(. ([scriptblock]::Create($audit[0])))
            $script:auditRows = @($found | ForEach-Object { "$($_.Path)|$($_.Access)|$($_.Who)" })
        }
    } finally {
        $env:SystemDrive = $savedDrive
    }
}
if ($audit.Count -eq 1) {
    $modify = 0x1301bf
    $readExecute = 0x1200a9
    $full = 0x1f01ff
    $admins = New-AuditRule 'S-1-5-32-544' $full
    # What a folder created at the root of a drive inherits from it.
    $openToAll = @((New-AuditRule 'S-1-5-11' $modify -Inherited), (New-AuditRule 'S-1-5-32-545' $readExecute -Inherited),
        (New-AuditRule 'S-1-5-32-544' $full -Inherited))
    $closed = New-AuditAcl 'S-1-5-32-544' @($admins, (New-AuditRule 'S-1-5-21-1-2-3-1001' $full))
    $disks = @{ 'C:' = 'NTFS'; 'D:' = 'NTFS'; 'E:' = 'exFAT'; 'F:' = $null; 'G:' = 'ReFS' }
    $items = @{
        'C:\' = @('Windows', 'Program Files', 'Users', '$Recycle.Bin', 'pagefile.sys', 'dev', 'Unreadable', 'Closed',
            'VulkanSDK', 'Traverse', 'GenericWrite', 'GenericRead', 'AppendOnly', 'Made', 'Granted', 'Denied')
        'D:\' = @('helios-ci', 'System Volume Information', 'Windows', 'Users', 'taxes.pdf')
        'G:\' = @('Archive')
    }
    $acls = @{
        'C:\dev'          = New-AuditAcl 'S-1-5-21-1-2-3-1001' $openToAll
        'C:\VulkanSDK'    = New-AuditAcl 'S-1-5-32-544' @($admins, (New-AuditRule 'S-1-5-32-545' $readExecute))
        'C:\Closed'       = $closed
        'C:\Traverse'     = New-AuditAcl 'S-1-5-32-544' @($admins, (New-AuditRule 'S-1-1-0' 0x100020))
        'C:\GenericWrite' = New-AuditAcl 'S-1-5-32-544' @((New-AuditRule 'S-1-5-11' 0x40000000))
        'C:\GenericRead'  = New-AuditAcl 'S-1-5-32-544' @((New-AuditRule 'S-1-5-6' 0x80000000))
        'C:\AppendOnly'   = New-AuditAcl 'S-1-5-32-544' @((New-AuditRule 'S-1-5-113' 0x4))
        'C:\Made'         = New-AuditAcl $ciSid
        'C:\Granted'      = New-AuditAcl 'S-1-5-32-544' @($admins, (New-AuditRule $ciSid $full))
        'C:\Denied'       = New-AuditAcl 'S-1-5-32-544' @($admins, (New-AuditRule $ciSid $full 'Deny'))
        'C:\Unreadable'   = 'Attempted to perform an unauthorized operation.'
        'D:\Windows'      = New-AuditAcl 'S-1-5-21-1-2-3-1001' $openToAll
        'D:\Users'        = New-AuditAcl 'S-1-5-21-1-2-3-1001' @((New-AuditRule 'S-1-2-0' $readExecute))
        'D:\taxes.pdf'    = New-AuditAcl 'S-1-5-21-1-2-3-1001' @((New-AuditRule 'S-1-5-15' $modify))
        'G:\Archive'      = New-AuditAcl 'S-1-5-32-544' @((New-AuditRule 'S-1-5-11' $modify 'Deny'), (New-AuditRule 'S-1-5-11' $modify))
    }
    Invoke-HeliosCiAudit -AuditDisks $disks -AuditItems $items -AuditAcls $acls
    Assert-Equal @(
        'C:\dev|write|Authenticated Users: 1245631; Users: 1179817',
        'C:\Unreadable|?|Attempted to perform an unauthorized operation.',
        'C:\VulkanSDK|read|Users: 1179817',
        'C:\GenericWrite|write|Authenticated Users: 1073741824',
        'C:\GenericRead|read|SERVICE: -2147483648',
        'C:\AppendOnly|write|Local account: 4',
        'C:\Made|write|owner: helios-ci',
        'C:\Granted|write|helios-ci: 2032127',
        'D:\Windows|write|Authenticated Users: 1245631; Users: 1179817',
        'D:\Users|read|LOCAL: 1179817',
        'D:\taxes.pdf|write|This Organization: 1245631',
        'E:\|write|every account (exFAT has no permissions)',
        'G:\Archive|write|Authenticated Users: 1245631') $script:auditRows `
        'the audit: what helios-ci can open, outside Windows'' own folders on the Windows drive and D:\helios-ci'
    Assert-Equal @('F:\ has no file system (locked, or no medium): audit it again once it is open') $script:auditWarnings `
        'the audit warns about a drive it cannot read'

    Invoke-HeliosCiAudit -AuditDisks @{ 'C:' = 'NTFS' } -AuditItems @{ 'C:\' = @('Closed', 'Windows') } `
        -AuditAcls @{ 'C:\Closed' = $closed }
    Assert-Equal @('Nothing at the root of a local drive is open to helios-ci') $script:auditOutput 'the audit when nothing is open'
    Assert-Equal 0 $script:auditRows.Count 'and it has no rows'
    Assert-Throws { Invoke-HeliosCiAudit -AuditDisks @{} -AuditItems @{} -AuditAcls @{} -AuditAccount '' } 'do step 2 first' `
        'the audit without the helios-ci account'
}

# Step 9 starts the service only when the runner will run the hook: .env's last hook line names it, helios-ci (by
# its SID now: an account created again has a new one) can read it, it carries no download mark, and Windows
# PowerShell's machine-wide execution policy lets it run. Otherwise "Set up runner" fails without running the hook,
# or the hook fails to start, and nothing ends the job. Runs step 9's block with stand-ins: $EnvLines (the lines of
# .env, $null when it is missing), $HookAcl (the hook's ACL, $null when the hook is missing), $AccountSid (helios-ci's
# SID), $Downloaded (the hook has a Zone.Identifier stream) and $Registry (registry key -> its values, as a
# hashtable; a key that is not there is missing), with the console's default error preference (Continue: a cmdlet's
# error does not stop the block unless it says so); leaves the service changes in $script:serviceLog, and an error
# that ends the block propagates.
$step9 = @($blocks | Where-Object { $_ -match '(?m)^# Step 9:' })
Assert-Equal 1 $step9.Count 'the runbook has one step 9 block'
$hookPath = 'D:\helios-ci\hooks\job-started.ps1'
$machinePolicyKey = 'HKLM:\SOFTWARE\Microsoft\PowerShell\1\ShellIds\Microsoft.PowerShell'
$groupPolicyKey = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\PowerShell'
$remoteSigned = @{ $machinePolicyKey = @{ ExecutionPolicy = 'RemoteSigned' } }
function Invoke-HeliosCiStep9([object[]]$EnvLines, [object]$HookAcl, [string]$AccountSid = $ciSid,
    [bool]$Downloaded = $false, [hashtable]$Registry = $remoteSigned) {
    function Get-LocalUser {
        [CmdletBinding()] param([string]$Name)
        if ($Name -cne 'helios-ci') { throw "unexpected Get-LocalUser $Name" }
        [pscustomobject]@{ SID = [pscustomobject]@{ Value = $AccountSid } }
    }
    function Test-Path {
        [CmdletBinding()] param([string]$LiteralPath, [string]$PathType)
        if ($LiteralPath -cne $hookPath -or $PathType -ne 'Leaf') { throw "unexpected Test-Path $LiteralPath $PathType" }
        return $null -ne $HookAcl
    }
    function Get-Content {
        [CmdletBinding()] param([string]$LiteralPath)
        if ($LiteralPath -cne 'D:\helios-ci\runner\.env') { throw "unexpected Get-Content $LiteralPath" }
        if ($null -eq $EnvLines) { Write-Error "Cannot find path '$LiteralPath' because it does not exist."; return }
        $EnvLines
    }
    function Get-Acl {
        [CmdletBinding()] param([string]$LiteralPath)
        if ($LiteralPath -cne $hookPath -or $null -eq $HookAcl) { throw "unexpected Get-Acl $LiteralPath" }
        $HookAcl
    }
    # A missing stream or key is a non-terminating error, as from the cmdlets (the block asks to ignore it).
    function Get-Item {
        [CmdletBinding()] param([string]$LiteralPath, [string]$Stream)
        if ($LiteralPath -cne $hookPath -or $Stream -cne 'Zone.Identifier') { throw "unexpected Get-Item $LiteralPath $Stream" }
        if ($Downloaded) { [pscustomobject]@{ FileName = $hookPath; Stream = $Stream; Length = 26 } }
        else { Write-Error "Could not open the alternate data stream '$Stream' of the file '$hookPath'." }
    }
    function Get-ItemProperty {
        [CmdletBinding()] param([string]$LiteralPath)
        if ($LiteralPath -cne $machinePolicyKey -and $LiteralPath -cne $groupPolicyKey) { throw "unexpected Get-ItemProperty $LiteralPath" }
        if (-not $Registry.ContainsKey($LiteralPath)) { Write-Error "Cannot find path '$LiteralPath' because it does not exist."; return }
        [pscustomobject]$Registry[$LiteralPath]
    }
    function Get-Service {
        [CmdletBinding()] param([Parameter(Position = 0)] [string]$Name)
        if ($Name -cne 'actions.runner.*') { throw "unexpected Get-Service $Name" }
        [pscustomobject]@{ Name = 'actions.runner.PageMastr-HeliosEngine.helios-win-gpu'; Status = 'Stopped'; StartType = 'Manual' }
    }
    function Set-Service {
        [CmdletBinding()] param([Parameter(ValueFromPipeline = $true)] $InputObject, [string]$StartupType)
        process { $script:serviceLog.Add("$StartupType $($InputObject.Name)") }
    }
    function Start-Service {
        [CmdletBinding()] param([Parameter(ValueFromPipeline = $true)] $InputObject)
        process { $script:serviceLog.Add("start $($InputObject.Name)") }
    }
    $script:serviceLog = New-Object System.Collections.Generic.List[string]
    $ErrorActionPreference = 'Continue'
    & { [void](. ([scriptblock]::Create($step9[0]))) } 2>$null
}
if ($step9.Count -eq 1) {
    $service = 'actions.runner.PageMastr-HeliosEngine.helios-win-gpu'
    $hookLine = "ACTIONS_RUNNER_HOOK_JOB_STARTED=$hookPath"
    # What step 3's icacls line gives the hook: the hooks folder's entries, inherited.
    $hookAcl = New-AuditAcl 'S-1-5-32-544' @((New-AuditRule 'S-1-5-32-544' 0x1f01ff -Inherited),
        (New-AuditRule 'S-1-5-18' 0x1f01ff -Inherited), (New-AuditRule $ciSid 0x1200a9 -Inherited))
    $envLines = @('LANG=en_US.UTF-8', $hookLine)
    try { Invoke-HeliosCiStep9 $envLines $hookAcl } catch { $script:serviceLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal @("Automatic $service", "start $service") $script:serviceLog.ToArray() 'step 9 starts the runner when the hook is in place'
    $refused = @(
        @{ What = 'helios-ci was created again (new SID) and the hooks ACL still names the old one'
            Env = $envLines; Acl = $hookAcl; Sid = 'S-1-5-21-1-2-3-1003'; Error = 'run the icacls line of step 3 again' },
        @{ What = 'helios-ci may only list the hook, not read it'; Env = $envLines; Sid = $ciSid; Error = 'cannot read'
            Acl = New-AuditAcl 'S-1-5-32-544' @((New-AuditRule $ciSid 0x1200a0 -Inherited)) },
        @{ What = 'only Users may read the hook'; Env = $envLines; Sid = $ciSid; Error = 'cannot read'
            Acl = New-AuditAcl 'S-1-5-32-544' @((New-AuditRule 'S-1-5-32-545' 0x1200a9 -Inherited)) },
        @{ What = 'a Deny entry on the hook'; Env = $envLines; Sid = $ciSid; Error = 'Deny entry'
            Acl = New-AuditAcl 'S-1-5-32-544' @((New-AuditRule 'S-1-5-6' 0x1200a9 'Deny'), (New-AuditRule $ciSid 0x1200a9 -Inherited)) },
        @{ What = 'the hook is missing'; Env = $envLines; Acl = $null; Sid = $ciSid; Error = 'is missing: do step 6' },
        @{ What = '.env is missing (the block stops at the cmdlet''s error)'; Env = $null; Acl = $hookAcl; Sid = $ciSid
            Error = 'Cannot find path' },
        @{ What = '.env has no hook line'; Env = @('LANG=en_US.UTF-8'); Acl = $hookAcl; Sid = $ciSid; Error = 'must name' },
        @{ What = '.env has the hook line only as a comment'; Env = @("# $hookLine"); Acl = $hookAcl; Sid = $ciSid; Error = 'must name' },
        @{ What = '.env indents the hook line (the runner would set a variable whose name starts with a space)'
            Env = @(" $hookLine"); Acl = $hookAcl; Sid = $ciSid; Error = 'must name' },
        @{ What = '.env has a space after the path (the runner does not trim it)'; Env = @("$hookLine "); Acl = $hookAcl
            Sid = $ciSid; Error = 'must name' },
        @{ What = 'a later empty hook line in .env switches the hook off'; Env = @($hookLine, 'ACTIONS_RUNNER_HOOK_JOB_STARTED=')
            Acl = $hookAcl; Sid = $ciSid; Error = 'must name' },
        @{ What = 'a later hook line in .env, in lower case, names another script'
            Env = @($hookLine, 'actions_runner_hook_job_started=C:\Users\Public\hook.ps1'); Acl = $hookAcl; Sid = $ciSid
            Error = 'must name' },
        @{ What = 'the hook is marked as downloaded'; Env = $envLines; Acl = $hookAcl; Sid = $ciSid; Downloaded = $true
            Error = 'marked as downloaded' },
        @{ What = 'no execution policy is set (Windows PowerShell then uses Restricted)'; Env = $envLines; Acl = $hookAcl
            Sid = $ciSid; Registry = @{}; Error = 'execution policy is Restricted' },
        @{ What = 'the policy key has no ExecutionPolicy value'; Env = $envLines; Acl = $hookAcl; Sid = $ciSid
            Registry = @{ $machinePolicyKey = @{ Path = 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' } }
            Error = 'execution policy is Restricted' },
        @{ What = 'the machine policy is AllSigned (the hook is not signed)'; Env = $envLines; Acl = $hookAcl; Sid = $ciSid
            Registry = @{ $machinePolicyKey = @{ ExecutionPolicy = 'AllSigned' } }; Error = 'execution policy is AllSigned' },
        @{ What = 'a group policy turns scripts off, over a RemoteSigned machine policy'; Env = $envLines; Acl = $hookAcl
            Sid = $ciSid; Error = 'execution policy is Restricted'
            Registry = @{ $machinePolicyKey = @{ ExecutionPolicy = 'RemoteSigned' }; $groupPolicyKey = @{ EnableScripts = 0 } } },
        @{ What = 'a group policy allows only signed scripts'; Env = $envLines; Acl = $hookAcl; Sid = $ciSid
            Error = 'execution policy is AllSigned'
            Registry = @{ $machinePolicyKey = @{ ExecutionPolicy = 'Unrestricted' }
                $groupPolicyKey = @{ EnableScripts = 1; ExecutionPolicy = 'AllSigned' } } }
    )
    foreach ($case in $refused) {
        $registry = if ($case.ContainsKey('Registry')) { $case.Registry } else { $remoteSigned }
        Assert-Throws { Invoke-HeliosCiStep9 $case.Env $case.Acl $case.Sid ([bool]$case['Downloaded']) $registry } $case.Error `
            "step 9 refuses: $($case.What)"
        Assert-Equal 0 $script:serviceLog.Count "and leaves the service alone: $($case.What)"
    }
    $allowed = @(
        @{ What = 'the machine policy is Unrestricted'; Registry = @{ $machinePolicyKey = @{ ExecutionPolicy = 'Unrestricted' } } },
        @{ What = 'the machine policy is Bypass'; Registry = @{ $machinePolicyKey = @{ ExecutionPolicy = 'Bypass' } } },
        @{ What = 'a group policy allows local scripts over a Restricted machine policy'
            Registry = @{ $machinePolicyKey = @{ ExecutionPolicy = 'Restricted' }
                $groupPolicyKey = @{ EnableScripts = 1; ExecutionPolicy = 'RemoteSigned' } } },
        @{ What = 'the group policy key exists without the setting'
            Registry = @{ $machinePolicyKey = @{ ExecutionPolicy = 'RemoteSigned' }; $groupPolicyKey = @{ ScriptBlockLogging = 1 } } }
    )
    foreach ($case in $allowed) {
        try { Invoke-HeliosCiStep9 $envLines $hookAcl $ciSid $false $case.Registry } catch { $script:serviceLog.Add("error: $($_.Exception.Message)") }
        Assert-Equal @("Automatic $service", "start $service") $script:serviceLog.ToArray() "step 9 starts the runner: $($case.What)"
    }
}

# Rotating after suspected misuse (the runbook's Rotate step 3, blocks a to d) lists what helios-ci owns outside its
# profile before the account goes (afterwards the owner is a bare SID); ends the account's processes and deletes its
# profile (folder and registry entry) and the account; sets the old runner and work folders aside and gives the new
# account step 3's folders and hook access (the hooks ACL names the old SID, and config.cmd re-grants only runner and
# work); and installs the hook, .env and the firewall rules BEFORE the runner is registered again: config.cmd starts
# the service at once, and a job queued for the runner since its removal would otherwise run all of its steps, with
# the LAN open. Each block runs here against stand-ins; $script:rotateLog records what it did, in order.
$rotateStart = $runbook.IndexOf('## Rotate or remove')
$rotateEnd = $runbook.IndexOf('## Not covered yet')
$rotateSection = if ($rotateStart -ge 0 -and $rotateEnd -gt $rotateStart) { $runbook.Substring($rotateStart, $rotateEnd - $rotateStart) } else { '' }
$rotateHeaders = @('# Rotate: what helios-ci owns', '# Rotate: end helios-ci''s processes',
    '# Rotate: set the old runner and work folders aside', '# Rotate: before registering')
$rotateList, $rotateRemove, $rotateFolders, $rotateInstall = @($rotateHeaders | ForEach-Object {
        $header = $_
        , @($blocks | Where-Object { $_ -match ('(?m)^[ \t]*' + [regex]::Escape($header)) })
    })
Assert-Equal '1 1 1 1' "$($rotateList.Count) $($rotateRemove.Count) $($rotateFolders.Count) $($rotateInstall.Count)" `
    'the runbook has the four rotate blocks'
# Where each block and the step that registers the runner again stand in the Rotate section, in the order the owner
# meets them: the hook, .env and the firewall rules come before the registration.
$rotateOrder = @(($rotateHeaders + @('Then create the account again (step 2)')) | ForEach-Object { $rotateSection.IndexOf($_) })
$registerAt = ([regex]::Match($rotateSection, '(?m)^\d+\. Register again \(step 5')).Index
Assert-Equal $true ($rotateOrder[0] -ge 0 -and $rotateOrder[0] -lt $rotateOrder[1] -and $rotateOrder[1] -lt $rotateOrder[4] -and
    $rotateOrder[4] -lt $rotateOrder[2] -and $rotateOrder[2] -lt $rotateOrder[3] -and $rotateOrder[3] -lt $registerAt) `
    'Rotate: list, delete the account, create it again, new folders, then the hook, .env and firewall, then register'

# Each icacls grant on $Folder, as its command elements.
function Get-HeliosCiFolderGrant([string]$Block, [string]$Folder) {
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($Block, [ref]$tokens, [ref]$errors)
    $grants = $ast.FindAll({
            param($node)
            $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'icacls' -and
            $node.CommandElements.Count -gt 2 -and $node.CommandElements[1].Extent.Text -eq $Folder -and
            @($node.CommandElements | Where-Object { $_.Extent.Text -eq '/grant:r' }).Count -gt 0
        }, $true)
    return @($grants | ForEach-Object { @($_.CommandElements | ForEach-Object { $_.Extent.Text }) -join ' ' })
}
foreach ($folder in 'D:\helios-ci\runner', 'D:\helios-ci\work', 'D:\helios-ci\hooks') {
    $step3Grant = @($blocks | Where-Object { $_ -notmatch '(?m)^[ \t]*# Rotate:' } | ForEach-Object { Get-HeliosCiFolderGrant $_ $folder })
    Assert-Equal 1 $step3Grant.Count "step 3 has one icacls grant on $folder"
    if ($rotateFolders.Count -eq 1) {
        Assert-Equal $step3Grant (Get-HeliosCiFolderGrant $rotateFolders[0] $folder) "rotating with a new account repeats step 3's grant on $folder"
    }
}

if ($rotateList.Count -eq 1) {
    $owned = @{ 'C:\planted' = $ciSid; 'C:\dev' = 'S-1-5-21-1-2-3-1001'; 'C:\ProgramData\Vendor' = 'S-1-5-32-544'
        'C:\ProgramData\Vendor\update.exe' = $ciSid; 'C:\Users\Public\Documents\run.ps1' = $ciSid
        'C:\Windows\Temp\setup.log' = 'S-1-5-18'; 'D:\helios-ci' = 'S-1-5-32-544'; 'E:\notes.txt' = $ciSid }
    $listed = @{ 'C:\' = @('C:\planted', 'C:\dev', 'C:\Unreadable'); 'D:\' = @('D:\helios-ci'); 'E:\' = @('E:\notes.txt')
        'C:\ProgramData' = @('C:\ProgramData\Vendor', 'C:\ProgramData\Vendor\update.exe')
        'C:\Users\Public' = @('C:\Users\Public\Documents\run.ps1'); 'C:\Windows\Temp' = @('C:\Windows\Temp\setup.log') }
    function Invoke-HeliosCiRotateList {
        function Get-LocalUser {
            [CmdletBinding()] param([string]$Name)
            if ($Name -cne 'helios-ci') { throw "unexpected Get-LocalUser $Name" }
            [pscustomobject]@{ SID = [pscustomobject]@{ Value = $ciSid } }
        }
        function Get-CimInstance {
            [CmdletBinding()] param([Parameter(Position = 0)] [string]$ClassName, [string]$Filter)
            if ($ClassName -ne 'Win32_LogicalDisk' -or $Filter -ne 'DriveType = 2 OR DriveType = 3') { throw "unexpected Get-CimInstance $ClassName" }
            foreach ($id in 'C:', 'D:', 'E:') { [pscustomobject]@{ DeviceID = $id } }
        }
        # The drive roots one level deep, the shared folders all the way down (and only with -Force, hidden items too).
        function Get-ChildItem {
            [CmdletBinding()] param([string[]]$LiteralPath, [switch]$Force, [switch]$Recurse)
            foreach ($path in $LiteralPath) {
                if (-not $Force -or ($path -like '?:\' -eq [bool]$Recurse)) { throw "unexpected Get-ChildItem $path" }
                foreach ($full in @($listed[$path])) { [pscustomobject]@{ FullName = $full; LastWriteTime = '2026-10-03' } }
            }
        }
        # An item whose ACL the old account closed to Administrators: Get-Acl fails as it does then.
        function Get-Acl {
            [CmdletBinding()] param([string]$LiteralPath)
            if (-not $owned.ContainsKey($LiteralPath)) { throw 'Attempted to perform an unauthorized operation.' }
            New-AuditAcl $owned[$LiteralPath]
        }
        function Format-Table {
            [CmdletBinding()] param([Parameter(ValueFromPipeline = $true)] $InputObject, [switch]$AutoSize, [switch]$Wrap)
            process { $InputObject }
        }
        $saved = $env:ProgramData, $env:PUBLIC, $env:SystemRoot
        $env:ProgramData, $env:PUBLIC, $env:SystemRoot = 'C:\ProgramData', 'C:\Users\Public', 'C:\Windows'
        try {
            & { @(. ([scriptblock]::Create($rotateList[0]))) | ForEach-Object { if ($_ -is [string]) { $_ } else { "$($_.Path)|$($_.Owner)" } } }
        } finally {
            $env:ProgramData, $env:PUBLIC, $env:SystemRoot = $saved
        }
    }
    $rotateOutput = try { Invoke-HeliosCiRotateList } catch { "error: $($_.Exception.Message)" }
    Assert-Equal @("helios-ci's SID: $ciSid", 'C:\planted|helios-ci', 'C:\Unreadable|? Attempted to perform an unauthorized operation.',
        'E:\notes.txt|helios-ci', 'C:\ProgramData\Vendor\update.exe|helios-ci', 'C:\Users\Public\Documents\run.ps1|helios-ci') $rotateOutput `
        'rotating lists what helios-ci owns at the drive roots, in ProgramData, Public and Windows\Temp, and what it cannot read'
}

$oldSid = $ciSid
$newSid = 'S-1-5-21-1-2-3-1003'
if ($rotateRemove.Count -eq 1) {
    function Invoke-HeliosCiRotateRemove([string]$Old, [switch]$ProfileInUse) {
        function Get-Process {
            [CmdletBinding()] param([switch]$IncludeUserName)
            if (-not $IncludeUserName) { throw 'without -IncludeUserName, Get-Process shows no owners' }
            [pscustomobject]@{ Id = 11; ProcessName = 'Runner.Worker'; UserName = 'PC\helios-ci' }
            [pscustomobject]@{ Id = 12; ProcessName = 'explorer'; UserName = 'PC\owner' }
            [pscustomobject]@{ Id = 13; ProcessName = 'pwsh'; UserName = 'PC\HELIOS-CI' }
            [pscustomobject]@{ Id = 14; ProcessName = 'svchost'; UserName = 'NT AUTHORITY\SYSTEM' }
            [pscustomobject]@{ Id = 15; ProcessName = 'csrss'; UserName = $null }
            [pscustomobject]@{ Id = 16; ProcessName = 'cmd'; UserName = 'PC\helios-ci2' }
        }
        function Stop-Process {
            [CmdletBinding()] param([Parameter(ValueFromPipeline = $true)] $InputObject, [switch]$Force)
            process { $script:rotateLog.Add("stop $($InputObject.Id)") }
        }
        function Get-CimInstance {
            [CmdletBinding()] param([Parameter(Position = 0)] [string]$ClassName, [string]$Filter)
            if ($ClassName -cne 'Win32_UserProfile' -or $Filter -cne "SID = '$Old'") { throw "unexpected Get-CimInstance $ClassName -Filter $Filter" }
            [pscustomobject]@{ SID = $Old; LocalPath = 'C:\Users\helios-ci' }
        }
        function Remove-CimInstance {
            [CmdletBinding()] param([Parameter(ValueFromPipeline = $true)] $InputObject)
            process {
                if ($ProfileInUse) { throw 'The process cannot access the file because it is being used by another process.' }
                $script:rotateLog.Add("remove profile $($InputObject.SID) $($InputObject.LocalPath)")
            }
        }
        function Remove-LocalUser {
            [CmdletBinding()] param([string]$SID)
            $script:rotateLog.Add("remove account $SID")
        }
        $script:rotateLog = New-Object System.Collections.Generic.List[string]
        $old = $Old
        $savedComputer = $env:COMPUTERNAME
        $env:COMPUTERNAME = 'PC'
        try { & { . ([scriptblock]::Create($rotateRemove[0])) } | Out-Null } finally { $env:COMPUTERNAME = $savedComputer }
    }
    try { Invoke-HeliosCiRotateRemove $oldSid } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal @('stop 11', 'stop 13', "remove profile $oldSid C:\Users\helios-ci", "remove account $oldSid") $script:rotateLog.ToArray() `
        'rotating ends helios-ci''s processes, then deletes its profile (not just the folder) and the account'
    Assert-Throws { Invoke-HeliosCiRotateRemove $oldSid -ProfileInUse } 'being used by another process' 'a profile in use stops the block'
    Assert-Equal @('stop 11', 'stop 13') $script:rotateLog.ToArray() 'and keeps the account, so that block a can be run again'
    Assert-Throws { Invoke-HeliosCiRotateRemove '' } 'run block a first' 'deleting the account needs block a''s $old'
    Assert-Equal 0 $script:rotateLog.Count 'and does nothing without it'
}

if ($rotateFolders.Count -eq 1) {
    # $FailOn: the logged action (its start) that fails.
    function Invoke-HeliosCiRotateFolders([string]$Old, [string]$Current, [string]$FailOn = '') {
        function Get-LocalUser {
            [CmdletBinding()] param([string]$Name)
            if ($Name -cne 'helios-ci') { throw "unexpected Get-LocalUser $Name" }
            [pscustomobject]@{ SID = [pscustomobject]@{ Value = $Current } }
        }
        function Get-Date {
            [CmdletBinding()] param([string]$Format)
            if ($Format -cne 'yyyyMMdd-HHmmss') { throw "unexpected Get-Date -Format $Format" }
            '20261004-120000'
        }
        function Rename-Item {
            [CmdletBinding()] param([string]$LiteralPath, [string]$NewName)
            if ($FailOn -and "rename $LiteralPath" -like "$FailOn*") { throw 'The process cannot access the file because it is being used by another process.' }
            $script:rotateLog.Add("rename $LiteralPath -> $NewName")
        }
        function New-Item {
            [CmdletBinding()] param([string]$ItemType, [string[]]$Path)
            $script:rotateLog.Add("mkdir $ItemType $($Path -join ' ')")
            foreach ($p in $Path) { [pscustomobject]@{ FullName = $p } }
        }
        function icacls {
            $line = "icacls $($args -join ' ')"
            $script:rotateLog.Add($line)
            $global:LASTEXITCODE = if ($FailOn -and $line -like "$FailOn*") { 1332 } else { 0 }
        }
        $script:rotateLog = New-Object System.Collections.Generic.List[string]
        $old = $Old
        & { . ([scriptblock]::Create($rotateFolders[0])) } | Out-Null
    }
    $grants = '/inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F'
    $foldersDone = @('rename D:\helios-ci\runner -> runner.old-20261004-120000', 'rename D:\helios-ci\work -> work.old-20261004-120000',
        'mkdir Directory D:\helios-ci\runner D:\helios-ci\work', "icacls D:\helios-ci\runner $grants helios-ci:(OI)(CI)F",
        "icacls D:\helios-ci\work $grants helios-ci:(OI)(CI)F", "icacls D:\helios-ci\hooks $grants helios-ci:(OI)(CI)RX",
        "icacls D:\helios-ci\hooks /remove:g *$oldSid", 'icacls D:\helios-ci\hooks\job-started.ps1')
    try { Invoke-HeliosCiRotateFolders $oldSid $newSid } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal $foldersDone $script:rotateLog.ToArray() `
        'rotating sets the old folders aside, makes new ones for the new account and moves the hook''s entry to it'
    $foldersRefused = @(
        @{ What = 'without block a''s $old'; Old = ''; Current = $newSid; Error = 'run block a first'; Done = 0 },
        @{ What = 'before the account was created again'; Old = $oldSid; Current = $oldSid; Error = 'old account'; Done = 0 },
        @{ What = 'when the runner folder cannot be set aside (a file in use): nothing is granted on the old one'
            Old = $oldSid; Current = $newSid; FailOn = 'rename D:\helios-ci\runner'; Error = 'being used'; Done = 0 },
        @{ What = 'when icacls fails on the new runner folder'; Old = $oldSid; Current = $newSid
            FailOn = 'icacls D:\helios-ci\runner'; Error = 'icacls failed on D:\\helios-ci\\runner'; Done = 4 },
        @{ What = 'when icacls cannot drop the old SID from the hooks folder'; Old = $oldSid; Current = $newSid
            FailOn = 'icacls D:\helios-ci\hooks /remove'; Error = 'could not drop'; Done = 7 }
    )
    foreach ($case in $foldersRefused) {
        Assert-Throws { Invoke-HeliosCiRotateFolders $case.Old $case.Current ([string]$case['FailOn']) } $case.Error `
            "the rotate folders block stops $($case.What)"
        Assert-Equal @($foldersDone | Select-Object -First $case.Done) $script:rotateLog.ToArray() "and does nothing after that: $($case.What)"
    }
}

if ($rotateInstall.Count -eq 1) {
    # The clone lives in the owner's profile; firewall.ps1 is the one script the block runs by path, so a stand-in file
    # takes its place there and records its call.
    $rotateProfile = [IO.Path]::Combine([IO.Path]::GetFullPath($WorkDir), 'rotate-profile')
    $rotateRepo = "$rotateProfile\src\HeliosEngine"
    $stubDir = [IO.Path]::Combine($rotateProfile, 'src', 'HeliosEngine', 'tools', 'ci', 'runner')
    New-Item -ItemType Directory -Force -Path $stubDir | Out-Null
    [IO.File]::WriteAllText([IO.Path]::Combine($stubDir, 'firewall.ps1'),
        '[void]$global:HeliosCiRotateLog.Add("firewall.ps1 $($args -join '' '')".TrimEnd())')
    # $FailOn: the logged action (its start) that fails; $Cloned: whether the clone exists.
    function Invoke-HeliosCiRotateInstall([bool]$Cloned = $true, [string]$FailOn = '') {
        function Test-Path {
            [CmdletBinding()] param([string]$LiteralPath)
            if ($LiteralPath -cne $rotateRepo) { throw "unexpected Test-Path $LiteralPath" }
            return $Cloned
        }
        function git {
            $line = "git $($args -join ' ')"
            $script:rotateLog.Add($line)
            $global:LASTEXITCODE = if ($FailOn -and $line -like "$FailOn*") { 128 } else { 0 }
        }
        function Copy-Item {
            [CmdletBinding()] param([string]$LiteralPath, [string]$Destination)
            $script:rotateLog.Add("copy $LiteralPath -> $Destination")
        }
        function Set-Content {
            [CmdletBinding()] param([string]$LiteralPath, [string]$Encoding, [object[]]$Value)
            $script:rotateLog.Add("write $LiteralPath ($Encoding)")
            $script:rotateEnv = @($Value)
        }
        $script:rotateLog = New-Object System.Collections.Generic.List[string]
        $global:HeliosCiRotateLog = $script:rotateLog
        $script:rotateEnv = $null
        $savedProfile = $env:USERPROFILE
        $env:USERPROFILE = $rotateProfile
        try { & { . ([scriptblock]::Create($rotateInstall[0])) } | Out-Null } finally { $env:USERPROFILE = $savedProfile }
    }
    $installDone = @("git -C $rotateRepo switch main", "git -C $rotateRepo pull --ff-only",
        "copy $rotateRepo\tools\ci\runner\job-started.ps1 -> $hookPath", 'write D:\helios-ci\runner\.env (Ascii)', 'firewall.ps1')
    try { Invoke-HeliosCiRotateInstall } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal $installDone $script:rotateLog.ToArray() 'rotating installs the hook from main, writes .env and adds the firewall rules'
    Assert-Equal @("ACTIONS_RUNNER_HOOK_JOB_STARTED=$hookPath") $script:rotateEnv 'the .env it writes holds the hook line'
    if ($step9.Count -eq 1) {
        # What the registration's service will read must also satisfy step 9's check (with the hook's ACL as the
        # folders block leaves it: the new account's entry).
        $newHookAcl = New-AuditAcl 'S-1-5-32-544' @((New-AuditRule 'S-1-5-32-544' 0x1f01ff -Inherited),
            (New-AuditRule 'S-1-5-18' 0x1f01ff -Inherited), (New-AuditRule $newSid 0x1200a9 -Inherited))
        try { Invoke-HeliosCiStep9 $script:rotateEnv $newHookAcl $newSid } catch { $script:serviceLog.Add("error: $($_.Exception.Message)") }
        Assert-Equal @("Automatic $service", "start $service") $script:serviceLog.ToArray() 'step 9 accepts the .env that rotating writes'
    }
    try { Invoke-HeliosCiRotateInstall -Cloned $false } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal (@("git clone https://github.com/PageMastr/HeliosEngine.git $rotateRepo") + $installDone) $script:rotateLog.ToArray() `
        'rotating clones the repository first when there is no clone'
    foreach ($case in @(@{ FailOn = 'git clone'; Cloned = $false; Done = 1 }, @{ FailOn = 'git -C'; Cloned = $true; Done = 1 },
            @{ FailOn = "git -C $rotateRepo pull"; Cloned = $true; Done = 2 })) {
        Assert-Throws { Invoke-HeliosCiRotateInstall -Cloned $case.Cloned -FailOn $case.FailOn } 'failed' "rotating stops when $($case.FailOn) fails"
        Assert-Equal $case.Done $script:rotateLog.Count "and installs nothing after $($case.FailOn) failed"
    }
}

# The runner was online without the hook from 2026-10-03: the runbook's "Already done" path shows the jobs that ran
# and sends the owner through Rotate's suspected-misuse path, whatever they show (a job can delete its own log).
$alreadyStart = $runbook.IndexOf('**Already done on 2026-10-03?**')
$alreadyEnd = $runbook.IndexOf('### 1. Prerequisites')
$already = if ($alreadyStart -ge 0 -and $alreadyEnd -gt $alreadyStart) { $runbook.Substring($alreadyStart, $alreadyEnd - $alreadyStart) } else { '' }
Assert-Equal $true ($already -match "Get-ChildItem D:\\helios-ci\\runner\\_diag -Filter 'Worker_\*\.log'") `
    '"Already done" lists the jobs that ran without the hook'
Assert-Equal $true ($already -match '(?s)Start over either way.*"Rotate".*suspected-misuse path') `
    '"Already done" replaces the account and the runner folders before the hook goes in'
Assert-Equal $false ($already -match 'steps 6 to 10') '"Already done" no longer goes on with the same account and runner folder'
# The checklist looks for helios-ci's PowerShell profiles, which the runner loads before the hook, from a window that
# does not load them.
$checklistStart = $runbook.IndexOf('## Verification checklist')
$checklistEnd = $runbook.IndexOf('## Day to day')
$checklist = if ($checklistStart -ge 0 -and $checklistEnd -gt $checklistStart) { $runbook.Substring($checklistStart, $checklistEnd - $checklistStart) } else { '' }
Assert-Equal $true ($checklist.Contains('runas /user:helios-ci "powershell -NoProfile"') -and
    $checklist.Contains('$PROFILE.CurrentUserAllHosts') -and $checklist.Contains('$PROFILE.CurrentUserCurrentHost')) `
    'the checklist looks for helios-ci''s PowerShell profiles from a window without them'

# -- job-started.ps1: which jobs run -------------------------------------------------------------------------
. (Get-HeliosCiScriptDefinitions (Join-Path $here 'job-started.ps1'))
Assert-Equal @('D:\helios-ci\work', 'D:\helios-ci\runner') @($HeliosCiWorkRoot, $HeliosCiRunnerRoot) 'the hook''s directories'
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

# -- job-started.ps1: how it ends a refused job ---------------------------------------------------------------
# A failed hook fails only its own step: the runner would still run the job's if: always() steps and actions'
# pre: steps. So the hook stops its parent, the runner's worker, or (when WMI fails or the parent is something else)
# every worker under the runner's folder, found with Get-Process. The process cmdlets are mocked (functions win over
# cmdlets); $script:processes is the process table ('throw' makes WMI fail, 'throw-list' Get-Process), and the mocks
# and Write-Host record into $script:jobLog.
$script:jobLog = New-Object System.Collections.Generic.List[string]
$script:processes = @{}
$script:stopFails = @()
function Get-CimInstance {
    [CmdletBinding()] param([string]$ClassName, [string]$Filter)
    if ($ClassName -cne 'Win32_Process' -or $Filter -cnotmatch '^ProcessId = (\d+)$') { throw "unexpected query: $ClassName $Filter" }
    if ($script:processes.ContainsKey('throw')) { throw 'WMI is not available' }
    return $script:processes[[int]$Matches[1]]
}
function Get-Process {
    [CmdletBinding()] param([string]$Name)
    if ($script:processes.ContainsKey('throw-list')) { throw 'the process list is not available' }
    foreach ($key in @($script:processes.Keys | Where-Object { $_ -is [int] } | Sort-Object)) {
        $process = $script:processes[$key]
        $processName = [IO.Path]::GetFileNameWithoutExtension([string]$process.Name)
        if ($processName -and $processName -ieq $Name) {
            [pscustomobject]@{ Id = $process.ProcessId; ProcessName = $processName; Path = $process.ExecutablePath }
        }
    }
}
function Stop-Process {
    [CmdletBinding()] param([int]$Id, [switch]$Force)
    if ($script:stopFails -contains $Id) { throw "Access is denied ($Id)" }
    $script:jobLog.Add("stop $Id force=$Force")
}
function Start-Sleep {
    [CmdletBinding()] param([int]$Milliseconds)
    $script:jobLog.Add("sleep $Milliseconds")
}
function Invoke-Recorded([scriptblock]$Block) {
    function Write-Host { param([Parameter(Position = 0)] [object]$Object) $script:jobLog.Add("host: $Object") }
    return & $Block
}
function New-Process([int]$Id, [int]$Parent, [string]$Name, [AllowNull()] [string]$Path) {
    $script:processes[$Id] = [pscustomobject]@{ ProcessId = $Id; ParentProcessId = $Parent; Name = $Name; ExecutablePath = $Path }
}
function Set-Parent([string]$Name, [AllowNull()] [string]$Path) {
    $script:processes = @{}
    $script:jobLog.Clear()
    New-Process $PID 4000 'powershell.exe' 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe'
    if ($Name) { New-Process 4000 3000 $Name $Path }
}
$runnerRoot = 'D:\helios-ci\runner'
function Stop-TestJob { return Stop-HeliosCiJob -RunnerRoot $runnerRoot -DelayMilliseconds 0 6>$null }

foreach ($worker in @(
        @('Runner.Worker.exe', 'D:\helios-ci\runner\bin\Runner.Worker.exe'),
        @('runner.worker.EXE', 'd:\HELIOS-CI\Runner\bin.2.330.0\Runner.Worker.exe'))) {
    Set-Parent $worker[0] $worker[1]
    Assert-Equal $true (Stop-TestJob) "the job ends when the parent is $($worker[1])"
    Assert-Equal @('stop 4000 force=True') $script:jobLog.ToArray() "exactly the worker is stopped ($($worker[1]))"
}
foreach ($other in @(
        @('Runner.Worker.exe', 'C:\actions-runner\bin\Runner.Worker.exe'),
        @('Runner.Worker.exe', 'D:\helios-ci\runner-old\bin\Runner.Worker.exe'),
        @('Runner.Worker.exe', 'D:\helios-ci\work\HeliosEngine\HeliosEngine\Runner.Worker.exe'),
        @('Runner.Worker.exe', $null),
        @('Runner.Listener.exe', 'D:\helios-ci\runner\bin\Runner.Listener.exe'),
        @('pwsh.exe', 'D:\helios-ci\runner\bin\pwsh.exe'),
        @('ctest.exe', 'C:\Program Files\CMake\bin\ctest.exe'),
        @($null, $null))) {
    Set-Parent $other[0] $other[1]
    Assert-Equal $false (Stop-TestJob) "nothing is stopped when the parent is $($other[0]) ($($other[1]))"
    Assert-Equal 0 $script:jobLog.Count "no process is stopped when the parent is $($other[0]) ($($other[1]))"
}
Set-Parent $null $null
$script:processes.Remove($PID)
Assert-Equal $false (Stop-TestJob) 'nothing is stopped when the hook''s own process is not found'

# The fallback: when WMI fails or does not show the worker as the parent, every Runner.Worker.exe under the runner's
# folder is stopped (Get-Process); never the listener, and never a worker elsewhere.
Set-Parent 'Runner.Worker.exe' 'D:\helios-ci\runner\bin\Runner.Worker.exe'
New-Process 3000 600 'Runner.Listener.exe' 'D:\helios-ci\runner\bin\Runner.Listener.exe'
$script:processes['throw'] = $true
Assert-Equal $true (Stop-TestJob) 'a WMI failure falls back to Get-Process'
Assert-Equal @('stop 4000 force=True') $script:jobLog.ToArray() 'a WMI failure stops the runner''s worker, not its listener'
Set-Parent 'Runner.Worker.exe' 'D:\helios-ci\runner\bin\Runner.Worker.exe'
$script:processes.Remove($PID)
Assert-Equal $true (Stop-TestJob) 'the hook''s own process missing from WMI falls back to Get-Process'
Assert-Equal @('stop 4000 force=True') $script:jobLog.ToArray() 'and stops the worker'
Set-Parent 'cmd.exe' 'C:\Windows\System32\cmd.exe'
New-Process 3000 600 'Runner.Worker.exe' 'D:\helios-ci\runner\bin.2.330.0\Runner.Worker.exe'
New-Process 600 500 'Runner.Listener.exe' 'D:\helios-ci\runner\bin\Runner.Listener.exe'
New-Process 700 1 'Runner.Worker.exe' 'C:\actions-runner\bin\Runner.Worker.exe'
Assert-Equal $true (Stop-TestJob) 'a parent that is not the worker falls back to Get-Process'
Assert-Equal @('stop 3000 force=True') $script:jobLog.ToArray() 'only the worker under the runner''s folder is stopped'
Set-Parent 'cmd.exe' 'C:\Windows\System32\cmd.exe'
New-Process 3000 600 'Runner.Worker.exe' 'D:\helios-ci\runner\bin\Runner.Worker.exe'
New-Process 3100 600 'Runner.Worker.exe' 'D:\helios-ci\runner\bin\Runner.Worker.exe'
$script:stopFails = @(3000)
Assert-Equal $true (Stop-TestJob) 'every worker under the runner''s folder is stopped, even when one fails'
Assert-Equal @('stop 3100 force=True') $script:jobLog.ToArray() 'the other worker is stopped'
Set-Parent 'cmd.exe' 'C:\Windows\System32\cmd.exe'
New-Process 3000 600 'Runner.Worker.exe' 'D:\helios-ci\runner\bin\Runner.Worker.exe'
Assert-Equal $false (Stop-TestJob) 'a worker that cannot be stopped is reported'
$script:stopFails = @()
Set-Parent 'Runner.Worker.exe' 'D:\helios-ci\runner\bin\Runner.Worker.exe'
$script:processes['throw'] = $true
$script:processes['throw-list'] = $true
Assert-Equal $false (Stop-TestJob) 'WMI and Get-Process both failing stop nothing and do not throw'
Assert-Equal 0 $script:jobLog.Count 'and stop no process'
Set-Parent 'Runner.Worker.exe' 'C:\actions-runner\bin\Runner.Worker.exe'
New-Process 600 500 'Runner.Listener.exe' 'D:\helios-ci\runner\bin\Runner.Listener.exe'
$script:processes['throw'] = $true
Assert-Equal $false (Stop-TestJob) 'with no worker under the runner''s folder nothing is stopped'
Assert-Equal 0 $script:jobLog.Count 'not even the listener'

# Through the hook's entry point: a refused job returns 1 after it says why, waits, then stops the worker; an
# allowed job (with nothing to wipe) stops nothing.
$WorkDir = [IO.Path]::GetFullPath($WorkDir)
$jobRoot = Join-Path $WorkDir 'job'
$jobWorkspace = Join-Path (Join-Path $jobRoot 'HeliosEngine') 'HeliosEngine'
New-Item -ItemType Directory -Force -Path $jobWorkspace, (Join-Path $jobRoot '_actions') | Out-Null
Set-Parent 'Runner.Worker.exe' 'D:\helios-ci\runner\bin\Runner.Worker.exe'
$pushToBranch = @{
    GITHUB_EVENT_NAME   = 'push'
    GITHUB_REF          = 'refs/heads/agent/claude/x'
    GITHUB_WORKFLOW_REF = 'PageMastr/HeliosEngine/.github/workflows/hook-test.yml@refs/heads/agent/claude/x'
    GITHUB_WORKSPACE    = $jobWorkspace
}
$code = Invoke-Recorded { Invoke-HeliosCiJobStarted -Root $jobRoot -RunnerRoot $runnerRoot -Environment $pushToBranch }
Assert-Equal 1 $code 'a refused job fails the hook'
Assert-Equal @('host: ::error::job-started hook: refused', 'host: job-started hook: ending the job', 'sleep 2000', 'stop 4000 force=True') `
    @($script:jobLog | ForEach-Object { if ($_ -like 'host: *') { ($_ -split ': ')[0..2] -join ': ' } else { $_ } }) `
    'a refused job: the reason, then the worker is stopped after a pause'
Assert-Equal $true ($script:jobLog[0] -clike "*refused: ref 'refs/heads/agent/claude/x'*") 'the refusal names the ref'
Set-Parent 'Runner.Worker.exe' 'D:\helios-ci\runner\bin\Runner.Worker.exe'
$mainJob = @{
    GITHUB_EVENT_NAME   = 'schedule'
    GITHUB_REF          = 'refs/heads/main'
    GITHUB_WORKFLOW_REF = 'PageMastr/HeliosEngine/.github/workflows/win-gpu.yml@refs/heads/main'
    GITHUB_WORKSPACE    = $jobWorkspace
}
Assert-Equal 0 (Invoke-HeliosCiJobStarted -Root $jobRoot -RunnerRoot $runnerRoot -Environment $mainJob 6>$null) 'an allowed job passes'
Assert-Equal 0 $script:jobLog.Count 'an allowed job stops nothing'
Set-Parent 'Runner.Worker.exe' 'D:\helios-ci\runner\bin\Runner.Worker.exe'
$script:processes['throw'] = $true
$code = Invoke-Recorded { Invoke-HeliosCiJobStarted -Root $jobRoot -RunnerRoot $runnerRoot -Environment $pushToBranch }
Assert-Equal 1 $code 'a refused job fails the hook when WMI fails'
Assert-Equal @('host: ::error::job-started hook: refused', 'host: job-started hook: the parent process lookup failed',
    'host: job-started hook: ending the job', 'sleep 2000', 'stop 4000 force=True') `
    @($script:jobLog | ForEach-Object { if ($_ -like 'host: *') { ($_ -split ': ')[0..2] -join ': ' } else { $_ } }) `
    'a refused job when WMI fails: the reason, the fallback, then the worker is stopped after a pause'
Set-Parent 'pwsh.exe' 'C:\Program Files\PowerShell\7\pwsh.exe'
Assert-Equal 1 (Invoke-HeliosCiJobStarted -Root $jobRoot -RunnerRoot $runnerRoot -Environment $pushToBranch 6>$null) `
    'a refused job fails the hook even when the job cannot be ended'
Assert-Equal 0 $script:jobLog.Count 'and then stops nothing'
Remove-Item -LiteralPath $jobRoot -Recurse -Force
$script:processes = @{}

# -- job-started.ps1: what it deletes ------------------------------------------------------------------------
$root = Join-Path $WorkDir 'work'
$workspace = Join-Path (Join-Path $root 'HeliosEngine') 'HeliosEngine'
$outside = Join-Path $WorkDir 'outside'
function New-Tree {
    if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
    foreach ($dir in '_actions\actions\checkout', '_temp\_github_workflow', '_diag', '_PipelineMapping\PageMastr',
        '_update\bin.2.330.0', '_tool\Python', 'scifi-test\scifi-test\build', 'HeliosEngine\old-sibling',
        'HeliosEngine\HeliosEngine\build\deep\a\b\c\d\e\f\g\h') {
        New-Item -ItemType Directory -Force -Path (Join-Path $root $dir.Replace('\', [IO.Path]::DirectorySeparatorChar)) | Out-Null
    }
    foreach ($file in '_actions\actions\checkout\action.yml', '_temp\_github_workflow\event.json', '_diag\Runner.log',
        '_update\bin.2.330.0\Runner.Listener.exe', 'stray.txt', 'HeliosEngine\HeliosEngine\README.md', 'HeliosEngine\HeliosEngine\build\deep\a\b\c\d\e\f\g\h\x.obj',
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
Assert-Equal @('_PipelineMapping', '_actions', '_diag', '_temp', '_update') (Get-Sorted $HeliosCiKeep) `
    'the kept directories, including the runner''s self-update'
Assert-Throws { Get-HeliosCiWipeTargets -Root $root -Workspace '' } 'GITHUB_WORKSPACE is not set' 'no workspace'
Assert-Throws { Get-HeliosCiWipeTargets -Root $root -Workspace (Join-Path $root 'HeliosEngine') } 'is not <work>' 'workspace one level up'
Assert-Throws { Get-HeliosCiWipeTargets -Root $root -Workspace (Join-Path $outside 'a\b'.Replace('\', [IO.Path]::DirectorySeparatorChar)) } `
    'is not <work>' 'workspace outside the work directory'
Assert-Throws { Get-HeliosCiWipeTargets -Root (Join-Path $WorkDir 'missing') -Workspace $workspace } '' 'a missing work directory'

$onWindows = [IO.Path]::DirectorySeparatorChar -eq '\'
$wipe = 'skipped, not Windows'
if (-not $onWindows) {
    Write-Host 'The wipe itself (cmd.exe rd, junctions) runs on Windows only; skipped here.'
} elseif ($root -notmatch '^[A-Za-z]:\\[A-Za-z0-9_.\\-]+$') {
    $script:checks++
    $script:failures++
    $wipe = 'not run'
    Write-Host ("FAIL: the wipe test needs a -WorkDir of letters, digits, '_', '.' and '-' (the hook refuses " +
        "others), for example -WorkDir C:\helios-runner-test; got: $root")
} else {
    $wipe = 'ran'
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
    Assert-Equal 0 (Invoke-HeliosCiJobStarted -Root $root -RunnerRoot $runnerRoot -Environment $environment) 'the wipe succeeds'
    Assert-Equal @('HeliosEngine', '_PipelineMapping', '_actions', '_diag', '_temp', '_update') `
        (Get-Sorted @(Get-ChildItem -LiteralPath $root -Force | ForEach-Object Name)) 'the work directory after the wipe'
    Assert-Equal @('HeliosEngine') @(Get-ChildItem -LiteralPath (Join-Path $root 'HeliosEngine') -Force | ForEach-Object Name) 'the pipeline directory'
    Assert-Equal 0 @(Get-ChildItem -LiteralPath $workspace -Force).Count 'the workspace is empty'
    Assert-Equal 'keep me' (Get-Content -LiteralPath (Join-Path $outside 'precious.txt')) 'link targets survive'
    Assert-Equal $true (Test-Path -LiteralPath (Join-Path $root '_actions\actions\checkout\action.yml')) 'this job''s actions survive'
    Assert-Equal $true (Test-Path -LiteralPath (Join-Path $root '_update\bin.2.330.0\Runner.Listener.exe')) 'the runner''s update survives'

    New-Tree
    $refused = With @{ GITHUB_WORKSPACE = $workspace; GITHUB_EVENT_NAME = 'pull_request' }
    Assert-Equal 1 (Invoke-HeliosCiJobStarted -Root $root -RunnerRoot $runnerRoot -Environment $refused -DelayMilliseconds 0 6>$null) `
        'a refused job fails'
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
    Write-Host "runner scripts: $($script:failures) of $($script:checks) checks failed (wipe: $wipe)"
    exit 1
}
Write-Host "runner scripts: all $($script:checks) checks passed (wipe: $wipe)"
exit 0
