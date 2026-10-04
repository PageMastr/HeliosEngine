# Tests for the win-gpu runner scripts (WP-0.4): CTest `lint_runner_scripts` (label lint) runs them with
# PowerShell 7 wherever pwsh is installed (every GitHub-hosted image), Windows PowerShell 5.1 can run them too.
#
#   pwsh -NoProfile -File tools/ci/runner/test_runner_scripts.ps1 [-WorkDir DIR]
#
# Everywhere: every .ps1 here parses and is ASCII; firewall.ps1's block lists (CIDR splitting around lab
# addresses, IPv4 and IPv6, stray and non-canonical addresses) and the rules it adds and removes (the firewall
# cmdlets mocked); job-started.ps1's refusal rules, how it ends a refused job (the process cmdlets mocked) and the
# entries it would delete; every PowerShell block of docs/runbooks/win-gpu-runner.md parses, and its step 4b audit
# (what helios-ci can open at the root of each local drive, and the folders on the PATH and PSModulePath that it could
# change, replace or make), step 9's check before the service starts (the hook is set in .env, readable by helios-ci
# and not marked as downloaded, and the execution policy lets it run), "Already done"'s first block (helios-ci
# disabled, its processes ended) and the rotate blocks (the runner's service and config.cmd's group deleted with
# Windows' tools; what helios-ci owns; deleting its processes, profile and account; a new D:\helios-ci with step 3's
# folders for a new account, the old logs copied out; the hook, .env and firewall rules before the runner is
# registered again) run against stand-ins for WMI, the account, group, process, file, ACL, registry, git and service
# cmdlets; no runbook step runs anything from D:\helios-ci (config.cmd remove included) or changes into it, every
# registration is in a new runner folder that helios-ci could not change, and every rd runs in cmd.exe called by its
# full path; the "Already done" path cleans the PATH in a clean elevated window (cmd.exe from Win+R sets the PATH and
# the PSModulePath to Windows' own folders, then starts Windows PowerShell by its full path without the profile), and
# Rotate, step 4b and the checklist run the audit there; it, step 4b and the checklist name the steps that matter for
# a runner that already ran without the hook. The scripts have no test switch: their functions and constants are
# loaded from the parsed files, so their last block (the run) never runs here. On Windows also the wipe itself on a
# scratch tree under -WorkDir: a junction to a directory outside, a directory symbolic link where creating one is
# allowed, read-only files and a deep tree; the links go, their targets stay. And the clean window itself: its lines
# run in cmd.exe with planted Get-CimInstance.cmd, Get-Acl.cmd and Disable-LocalUser.cmd first on the inherited PATH,
# and Windows PowerShell 5.1 finds the cmdlets and runs none of them (without the PATH line it runs the planted one).
# The last line says whether the wipe and the clean window ran, and CTest requires both on Windows
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
$onWindows = [IO.Path]::DirectorySeparatorChar -eq '\'

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
# Runs the audit on $AuditDisks (drive -> file system), $AuditItems (drive root -> names), $AuditAcls (path -> ACL,
# or the message Get-Acl fails with), $AuditEnvironment (registry key -> its values, as a hashtable; a key that is not
# there is missing) and $AuditFolders (the folders that exist, for the PATH's entries), in a child scope so that its
# variables cannot change this script's. Leaves its rows ('path|access|who') in $script:auditRows, its output in
# $script:auditOutput and its warnings in $script:auditWarnings.
function Invoke-HeliosCiAudit([hashtable]$AuditDisks, [hashtable]$AuditItems, [hashtable]$AuditAcls,
    [string]$AuditAccount = $ciSid, [hashtable]$AuditEnvironment = @{}, [string[]]$AuditFolders = @()) {
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
    # The PATH settings come from the registry (what every new window gets), as Get-ItemProperty gives them: values
    # expanded. A missing key is a non-terminating error, as from the cmdlet (the audit asks to ignore it).
    function Get-ItemProperty {
        [CmdletBinding()] param([string]$LiteralPath)
        if ($LiteralPath -cne 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' -and
            $LiteralPath -cne 'HKCU:\Environment') { throw "unexpected Get-ItemProperty $LiteralPath" }
        if (-not $AuditEnvironment.ContainsKey($LiteralPath)) { Write-Error "Cannot find path '$LiteralPath'."; return }
        [pscustomobject]$AuditEnvironment[$LiteralPath]
    }
    function Test-Path {
        [CmdletBinding()] param([string]$LiteralPath, [string]$PathType)
        if ($PathType -ne 'Container') { throw "unexpected Test-Path $LiteralPath $PathType" }
        return $AuditFolders -contains $LiteralPath
    }
    function Write-Warning([string]$Message) { $script:auditWarnings.Add($Message) }
    $script:auditWarnings = New-Object System.Collections.Generic.List[string]
    $script:auditRows = @()
    $script:auditOutput = @()
    $savedDrive, $savedRoot = $env:SystemDrive, $env:SystemRoot
    $env:SystemDrive, $env:SystemRoot = 'C:', 'C:\Windows'
    try {
        & {
            $script:auditOutput = @(. ([scriptblock]::Create($audit[0])))
            $script:auditRows = @($found | ForEach-Object { "$($_.Path)|$($_.Access)|$($_.Who)" })
        }
    } finally {
        $env:SystemDrive, $env:SystemRoot = $savedDrive, $savedRoot
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
    Assert-Equal @('Nothing at the root of a local drive or on the PATH is open to helios-ci') $script:auditOutput 'the audit when nothing is open'
    Assert-Equal 0 $script:auditRows.Count 'and it has no rows'
    Assert-Throws { Invoke-HeliosCiAudit -AuditDisks @{} -AuditItems @{} -AuditAcls @{} -AuditAccount '' } 'do step 2 first' `
        'the audit without the helios-ci account'

    # The PATH and PSModulePath, the machine's and the owner's: every program started by name, elevated or not, and
    # every DLL that a program does not find in its own folder or in System32 is looked for in these folders, in order,
    # and the Vulkan SDK puts its Bin first. A folder on them must not be open to helios-ci; a folder above it must not
    # let helios-ci rename it and put its own in its place (the right to make folders, which Users have in ProgramData
    # and every account at a drive root, does not count there); and a missing one must not be creatable.
    $machineKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'
    $userKey = 'HKCU:\Environment'
    $installer = 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464'   # TrustedInstaller
    $systemOnly = New-AuditAcl $installer @($admins, (New-AuditRule 'S-1-5-18' $full), (New-AuditRule 'S-1-5-32-545' $readExecute))
    $ownerOnly = New-AuditAcl 'S-1-5-21-1-2-3-1001' @($admins, (New-AuditRule 'S-1-5-18' $full), (New-AuditRule 'S-1-5-21-1-2-3-1001' $full))
    $makeOnly = 0x116   # WriteData, AppendData, WriteExtendedAttributes, WriteAttributes: Users' entry on ProgramData
    $pathAcls = @{
        'C:\'                                                 = New-AuditAcl $installer @($admins, (New-AuditRule 'S-1-5-11' 0x4),
            (New-AuditRule 'S-1-5-32-545' $readExecute))
        'D:\'                                                 = New-AuditAcl 'S-1-5-32-544' @($admins, (New-AuditRule 'S-1-5-11' $modify),
            (New-AuditRule 'S-1-5-32-545' $readExecute))
        'C:\Windows\system32'                                 = $systemOnly
        'C:\Windows'                                          = $systemOnly
        'C:\VulkanSDK\1.3.290.0\Bin'                          = New-AuditAcl 'S-1-5-32-544' $openToAll
        'C:\Program Files\Git\cmd'                            = $systemOnly
        'C:\Program Files\Git'                                = $systemOnly
        'C:\Program Files'                                    = $systemOnly
        'C:\Tools\Closed\bin'                                 = $closed
        'C:\Tools\Closed'                                     = $closed
        'C:\Tools'                                            = New-AuditAcl 'S-1-5-32-544' $openToAll
        'C:\ProgramData\Vendor\bin'                           = $systemOnly
        'C:\ProgramData\Vendor'                               = $systemOnly
        'C:\ProgramData'                                      = New-AuditAcl 'S-1-5-18' @($admins, (New-AuditRule 'S-1-5-32-545' $readExecute),
            (New-AuditRule 'S-1-5-32-545' $makeOnly))
        'C:\Unreadable\bin'                                   = 'Attempted to perform an unauthorized operation.'
        'C:\Owned'                                            = New-AuditAcl $ciSid @($admins)
        'D:\Closed'                                           = $closed
        'C:\Program Files\WindowsPowerShell\Modules'          = $systemOnly
        'C:\Program Files\WindowsPowerShell'                  = $systemOnly
        'C:\Modules'                                          = New-AuditAcl 'S-1-5-32-544' $openToAll
        'C:\Users\owner\AppData\Local\Microsoft\WindowsApps'  = $ownerOnly
        'C:\Users\owner\AppData\Local\Microsoft'              = $ownerOnly
        'C:\Users\owner\AppData\Local'                        = $ownerOnly
        'C:\Users\owner\AppData'                              = $ownerOnly
        'C:\Users\owner'                                      = $ownerOnly
        'C:\Users'                                            = New-AuditAcl 'S-1-5-18' @($admins, (New-AuditRule 'S-1-1-0' $readExecute),
            (New-AuditRule 'S-1-5-32-545' $readExecute))
        'D:\tools'                                            = New-AuditAcl 'S-1-5-21-1-2-3-1001' $openToAll
    }
    $pathFolders = @($pathAcls.Keys | Where-Object { $_ -notlike '?:\' }) + @('C:\Unreadable')
    $pathEnvironment = @{
        $machineKey = @{
            Path         = ('%SystemRoot%\system32;C:\Windows;C:\VulkanSDK\1.3.290.0\Bin;"C:\Program Files\Git\cmd";;' +
                'C:\Tools\Closed\bin\;C:\ProgramData\Vendor\bin;C:\Gone\bin;C:\Program Files\Gone;relative\bin;' +
                'C:\Unreadable\bin;C:\Owned;D:\Closed')
            PSModulePath = 'C:\Program Files\WindowsPowerShell\Modules;C:\Modules'
        }
        $userKey    = @{ Path = 'C:\Users\owner\AppData\Local\Microsoft\WindowsApps;D:\tools' }
    }
    Invoke-HeliosCiAudit -AuditDisks @{ 'C:' = 'NTFS' } -AuditItems @{ 'C:\' = @('Windows') } -AuditAcls $pathAcls `
        -AuditEnvironment $pathEnvironment -AuditFolders $pathFolders
    Assert-Equal @(
        'C:\VulkanSDK\1.3.290.0\Bin|PATH|machine Path: C:\VulkanSDK\1.3.290.0\Bin (Authenticated Users: 1245631)',
        'C:\Tools\Closed\bin|PATH|machine Path: C:\Tools (Authenticated Users: 1245631)',
        'C:\Gone\bin|PATH|machine Path, missing: C:\ (Authenticated Users: 4)',
        'relative\bin|PATH|machine Path: not a full path on a local drive',
        'C:\Unreadable\bin|PATH|machine Path: C:\Unreadable\bin ? Attempted to perform an unauthorized operation.',
        'C:\Owned|PATH|machine Path: C:\Owned (owner: helios-ci)',
        'C:\Modules|PATH|machine PSModulePath: C:\Modules (Authenticated Users: 1245631)',
        'D:\tools|PATH|your Path: D:\tools (Authenticated Users: 1245631)') $script:auditRows `
        'the audit: what helios-ci can change on the PATH and the PSModulePath, or put in place of a folder there, or make'
    Invoke-HeliosCiAudit -AuditDisks @{ 'C:' = 'NTFS' } -AuditItems @{ 'C:\' = @('Windows') } -AuditAcls $pathAcls `
        -AuditEnvironment @{ $machineKey = @{ Path = '%SystemRoot%\system32;C:\Program Files\Gone' }
            $userKey = @{ Path = 'C:\Users\owner\AppData\Local\Microsoft\WindowsApps' } } -AuditFolders $pathFolders
    Assert-Equal @('Nothing at the root of a local drive or on the PATH is open to helios-ci') $script:auditOutput `
        'the audit when nothing on the PATH is open (a missing folder that only Administrators could make included)'
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
            Env = $envLines; Acl = $hookAcl; Sid = 'S-1-5-21-1-2-3-1003'; Error = 'run the icacls line of step 3 for D:\\helios-ci\\hooks again' },
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
        @{ What = 'a group policy turns scripts off and leaves an old ExecutionPolicy value'; Env = $envLines; Acl = $hookAcl
            Sid = $ciSid; Error = 'execution policy is Restricted'
            Registry = @{ $machinePolicyKey = @{ ExecutionPolicy = 'RemoteSigned' }
                $groupPolicyKey = @{ EnableScripts = 0; ExecutionPolicy = 'Unrestricted' } } },
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

# Every rotation (the runbook's Rotate steps 2 and 3) first deletes the runner's service and config.cmd's group with
# Windows' own tools: config.cmd remove would run, elevated, files that helios-ci can change (config.cmd itself, a
# batch file, and bin\), and a group left behind would hand the next account the folders set aside. It then lists what
# helios-ci owns outside its profile before the account goes (afterwards the owner is a bare SID); ends the account's
# processes and deletes its profile (folder and registry entry) and the account; sets the old D:\helios-ci aside and
# makes step 3's folders again for the new account (a runner folder that helios-ci cannot change before config.cmd
# runs, and a new hooks folder); and installs the hook, .env and the firewall rules BEFORE the runner is registered
# again: config.cmd starts the service at once, and a job queued for the runner since its removal would otherwise run
# all of its steps, with the LAN open. Each block runs here against stand-ins; $script:rotateLog records what it did,
# in order.
$rotateStart = $runbook.IndexOf('## Rotate or remove')
$rotateEnd = $runbook.IndexOf('## Not covered yet')
$rotateSection = if ($rotateStart -ge 0 -and $rotateEnd -gt $rotateStart) { $runbook.Substring($rotateStart, $rotateEnd - $rotateStart) } else { '' }
$rotateHeaders = @('# Rotate: delete the runner''s service and config.cmd''s group', '# Rotate: what helios-ci owns',
    '# Rotate: end helios-ci''s processes', '# Rotate: set the old D:\helios-ci aside', '# Rotate: before registering')
$rotateService, $rotateList, $rotateRemove, $rotateFolders, $rotateInstall = @($rotateHeaders | ForEach-Object {
        $header = $_
        , @($blocks | Where-Object { $_ -match ('(?m)^[ \t]*' + [regex]::Escape($header)) })
    })
Assert-Equal '1 1 1 1 1' "$($rotateService.Count) $($rotateList.Count) $($rotateRemove.Count) $($rotateFolders.Count) $($rotateInstall.Count)" `
    'the runbook has the five rotate blocks'
# Where each block and the step that registers the runner again stand in the Rotate section, in the order the owner
# meets them: the runner is removed first, and the hook, .env and the firewall rules come before the registration.
$rotateOrder = @(($rotateHeaders + @('Then create the account again (step 2)')) | ForEach-Object { $rotateSection.IndexOf($_) })
$registerAt = ([regex]::Match($rotateSection, '(?m)^\d+\. Register again \(step 5')).Index
Assert-Equal $true ($rotateOrder[0] -ge 0 -and $rotateOrder[0] -lt $rotateOrder[1] -and $rotateOrder[1] -lt $rotateOrder[2] -and
    $rotateOrder[2] -lt $rotateOrder[5] -and $rotateOrder[5] -lt $rotateOrder[3] -and $rotateOrder[3] -lt $rotateOrder[4] -and
    $rotateOrder[4] -lt $registerAt) `
    'Rotate: remove the runner, list, delete the account, create it again, new folders, the hook, .env and firewall, register'

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
foreach ($folder in 'D:\helios-ci', 'D:\helios-ci\runner', 'D:\helios-ci\work', 'D:\helios-ci\hooks') {
    $step3Grant = @($blocks | Where-Object { $_ -notmatch '(?m)^[ \t]*# Rotate:' } | ForEach-Object { Get-HeliosCiFolderGrant $_ $folder })
    Assert-Equal 1 $step3Grant.Count "step 3 has one icacls grant on $folder"
    if ($rotateFolders.Count -eq 1) {
        Assert-Equal $step3Grant (Get-HeliosCiFolderGrant $rotateFolders[0] $folder) "rotating with a new account repeats step 3's grant on $folder"
    }
}
# The owner runs config.cmd elevated in the runner folder, so until config.cmd grants the account its group, only
# Administrators and SYSTEM may change it; D:\helios-ci itself must not inherit D:\'s Modify for Authenticated Users,
# with which the account could rename it and put its own tree (hooks included) in its place.
Assert-Equal @('icacls D:\helios-ci\runner /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F"') `
    @($blocks | Where-Object { $_ -notmatch '(?m)^[ \t]*# Rotate:' } | ForEach-Object { Get-HeliosCiFolderGrant $_ 'D:\helios-ci\runner' }) `
    'step 3 gives the runner folder to Administrators and SYSTEM only'
Assert-Equal @('icacls D:\helios-ci /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-545:(RX)"') `
    @($blocks | Where-Object { $_ -notmatch '(?m)^[ \t]*# Rotate:' } | ForEach-Object { Get-HeliosCiFolderGrant $_ 'D:\helios-ci' }) `
    'step 3 closes D:\helios-ci itself: Administrators and SYSTEM, and Users may only list it'

if ($rotateService.Count -eq 1) {
    # $Services: the runner services there are; $Groups: config.cmd's groups; $FailOn: the logged action (its start)
    # that fails. Get-Service and Get-LocalGroup show what is left, so the block's last listing must come out empty.
    function Invoke-HeliosCiRotateService([string[]]$Services = @(), [string[]]$Groups = @(), [string]$FailOn = '') {
        $script:serviceLeft = New-Object System.Collections.Generic.List[string]
        foreach ($name in $Services) { $script:serviceLeft.Add($name) }
        $script:groupLeft = New-Object System.Collections.Generic.List[string]
        foreach ($name in $Groups) { $script:groupLeft.Add($name) }
        function Get-Service {
            [CmdletBinding()] param([string]$Name)
            if ($Name -cne 'actions.runner.*') { throw "unexpected Get-Service $Name" }
            foreach ($left in @($script:serviceLeft)) { [pscustomobject]@{ Name = $left; Status = 'Running' } }
        }
        function Stop-Service {
            [CmdletBinding()] param([string]$Name, [switch]$Force)
            if ($FailOn -and "stop $Name" -like "$FailOn*") { throw "Service '$Name' cannot be stopped." }
            $script:rotateLog.Add("stop $Name force=$Force")
        }
        function sc.exe {
            $line = "sc.exe $($args -join ' ')"
            $script:rotateLog.Add($line)
            if ($FailOn -and $line -like "$FailOn*") { $global:LASTEXITCODE = 1072; return '[SC] DeleteService FAILED 1072:' }
            if ($args.Count -ne 2 -or $args[0] -cne 'delete') { throw "unexpected $line" }
            [void]$script:serviceLeft.Remove([string]$args[1])
            $global:LASTEXITCODE = 0
            '[SC] DeleteService SUCCESS'
        }
        function Get-LocalGroup {
            [CmdletBinding()] param([string]$Name)
            if ($Name -cne 'GITHUB_ActionsRunner_G*') { throw "unexpected Get-LocalGroup $Name" }
            foreach ($left in @($script:groupLeft)) { [pscustomobject]@{ Name = $left } }
        }
        function Remove-LocalGroup {
            [CmdletBinding()] param([Parameter(ValueFromPipeline = $true)] $InputObject)
            process {
                $script:rotateLog.Add("remove group $($InputObject.Name)")
                [void]$script:groupLeft.Remove([string]$InputObject.Name)
            }
        }
        $script:rotateLog = New-Object System.Collections.Generic.List[string]
        $script:rotateOutput = @()
        $script:rotateOutput = @(& { . ([scriptblock]::Create($rotateService[0])) } | ForEach-Object { "$_" })
    }
    $runnerService = 'actions.runner.PageMastr-HeliosEngine.helios-win-gpu'
    try { Invoke-HeliosCiRotateService @($runnerService) @('GITHUB_ActionsRunner_G1a2b3') } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal @("stop $runnerService force=True", "sc.exe delete $runnerService", 'remove group GITHUB_ActionsRunner_G1a2b3') `
        $script:rotateLog.ToArray() 'rotating stops and deletes the runner''s service, then deletes config.cmd''s group'
    Assert-Equal 'What is left (nothing below this line):' $script:rotateOutput[-1] 'and nothing is left of either'
    $oldName = 'actions.runner.PageMastr-scifi-test.helios-win-gpu'   # registered before the repository's rename
    try { Invoke-HeliosCiRotateService @($runnerService, $oldName) @('GITHUB_ActionsRunner_G1a2b3', 'GITHUB_ActionsRunner_G4c5d6') }
    catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal @("stop $runnerService force=True", "sc.exe delete $runnerService", "stop $oldName force=True", "sc.exe delete $oldName",
        'remove group GITHUB_ActionsRunner_G1a2b3', 'remove group GITHUB_ActionsRunner_G4c5d6') $script:rotateLog.ToArray() `
        'every runner service and group goes (one from before the rename too)'
    try { Invoke-HeliosCiRotateService } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal 0 $script:rotateLog.Count 'with neither left, deleting the runner does nothing and does not fail'
    Assert-Throws { Invoke-HeliosCiRotateService @($runnerService) @('GITHUB_ActionsRunner_G1a2b3') "sc.exe delete $runnerService" } `
        'could not delete the service' 'rotating stops when sc.exe cannot delete the service'
    Assert-Equal @("stop $runnerService force=True", "sc.exe delete $runnerService") $script:rotateLog.ToArray() 'and keeps the group'
    Assert-Throws { Invoke-HeliosCiRotateService @($runnerService) @('GITHUB_ActionsRunner_G1a2b3') "stop $runnerService" } `
        'cannot be stopped' 'rotating stops when the service cannot be stopped'
    Assert-Equal 0 $script:rotateLog.Count 'and deletes nothing'
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
    # $FailOn: the logged action (its start) that fails; $Services and $Groups: the runner services and config.cmd
    # groups still there.
    function Invoke-HeliosCiRotateFolders([string]$Old, [string]$Current, [string]$FailOn = '', [string[]]$Services = @(),
        [string[]]$Groups = @(), [string[]]$Links = @()) {
        function Get-LocalUser {
            [CmdletBinding()] param([string]$Name)
            if ($Name -cne 'helios-ci') { throw "unexpected Get-LocalUser $Name" }
            [pscustomobject]@{ SID = [pscustomobject]@{ Value = $Current } }
        }
        function Get-Service {
            [CmdletBinding()] param([string]$Name)
            if ($Name -cne 'actions.runner.*') { throw "unexpected Get-Service $Name" }
            foreach ($left in $Services) { [pscustomobject]@{ Name = $left } }
        }
        function Get-LocalGroup {
            [CmdletBinding()] param([string]$Name)
            if ($Name -cne 'GITHUB_ActionsRunner_G*') { throw "unexpected Get-LocalGroup $Name" }
            foreach ($left in $Groups) { [pscustomobject]@{ Name = $left } }
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
        # The old runner's folder and its _diag ($Links: those that are links), and _diag's logs, listed one level deep
        # (a recursive listing would follow links below it); one of them is a symbolic link.
        function Get-Item {
            [CmdletBinding()] param([string]$LiteralPath, [switch]$Force)
            $script:rotateLog.Add("stat $LiteralPath")
            $attributes = if ($Links -contains $LiteralPath) { 'Directory, ReparsePoint' } else { 'Directory' }
            [pscustomobject]@{ FullName = $LiteralPath; Attributes = [IO.FileAttributes]$attributes }
        }
        function Get-ChildItem {
            [CmdletBinding()] param([string]$LiteralPath, [string]$Filter, [switch]$File, [switch]$Force, [switch]$Recurse)
            if ($Recurse) { throw "unexpected Get-ChildItem -Recurse $LiteralPath" }
            $script:rotateLog.Add("list $LiteralPath $Filter file=$File")
            foreach ($name in 'Runner_20261003-101500-utc.log', 'Worker_20261003-101600-utc.log', 'Worker_20261003-101700-utc.log') {
                $attributes = if ($name -like '*101700*') { 'Archive, ReparsePoint' } else { 'Archive' }
                [pscustomobject]@{ FullName = "$LiteralPath\$name"; Attributes = [IO.FileAttributes]$attributes }
            }
        }
        function Copy-Item {
            [CmdletBinding()] param([Parameter(ValueFromPipeline = $true)] $InputObject, [string]$Destination, [switch]$Recurse)
            process {
                if ($Recurse) { throw "unexpected Copy-Item -Recurse $($InputObject.FullName)" }
                $script:rotateLog.Add("copy $($InputObject.FullName) -> $Destination")
            }
        }
        $script:rotateLog = New-Object System.Collections.Generic.List[string]
        $old = $Old
        $savedProfile = $env:USERPROFILE
        $env:USERPROFILE = 'C:\Users\owner'
        try { & { . ([scriptblock]::Create($rotateFolders[0])) } | Out-Null } finally { $env:USERPROFILE = $savedProfile }
    }
    $grants = '/inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F'
    # The whole tree goes aside (runner, work and hooks: the old account had full control of hooks in the setup of
    # 2026-10-03), and the new runner folder names no account but Administrators and SYSTEM. The old runner's logs, the
    # owner's evidence, are copied into the owner's profile: the tree of 2026-10-03 inherited D:\'s Modify for
    # Authenticated Users, so the new account's jobs may be able to change it.
    $oldRunner = 'D:\helios-ci.old-20261004-120000\runner'
    $oldDiag = "$oldRunner\_diag"
    $logs = 'C:\Users\owner\helios-ci-diag-20261004-120000'
    $foldersMade = @('rename D:\helios-ci -> helios-ci.old-20261004-120000',
        'mkdir Directory D:\helios-ci\runner D:\helios-ci\work D:\helios-ci\hooks', "icacls D:\helios-ci $grants *S-1-5-32-545:(RX)",
        "icacls D:\helios-ci\runner $grants", "icacls D:\helios-ci\work $grants helios-ci:(OI)(CI)F",
        "icacls D:\helios-ci\hooks $grants helios-ci:(OI)(CI)RX", "mkdir Directory $logs", "stat $oldRunner")
    $foldersDone = $foldersMade + @("stat $oldDiag", "list $oldDiag *.log file=True",
        "copy $oldDiag\Runner_20261003-101500-utc.log -> $logs", "copy $oldDiag\Worker_20261003-101600-utc.log -> $logs")
    try { Invoke-HeliosCiRotateFolders $oldSid $newSid } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal $foldersDone $script:rotateLog.ToArray() `
        'rotating sets the old D:\helios-ci aside, makes step 3''s folders again for the new account and copies the old logs'
    try { Invoke-HeliosCiRotateFolders $oldSid $newSid -Links @($oldRunner) } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal $foldersMade $script:rotateLog.ToArray() 'the old logs are not copied, nor looked for, through a runner folder that is a link'
    try { Invoke-HeliosCiRotateFolders $oldSid $newSid -Links @($oldDiag) } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal ($foldersMade + @("stat $oldDiag")) $script:rotateLog.ToArray() 'nor from a _diag that is a link'
    $foldersRefused = @(
        @{ What = 'without block a''s $old'; Old = ''; Current = $newSid; Error = 'run block a first'; Done = 0 },
        @{ What = 'before the account was created again'; Old = $oldSid; Current = $oldSid; Error = 'old account'; Done = 0 },
        @{ What = 'while the runner''s service is still there (step 2 not done)'; Old = $oldSid; Current = $newSid
            Services = @('actions.runner.PageMastr-HeliosEngine.helios-win-gpu'); Error = 'do Rotate''s step 2 first'; Done = 0 },
        @{ What = 'while config.cmd''s group is still there (the new account would join it)'; Old = $oldSid; Current = $newSid
            Groups = @('GITHUB_ActionsRunner_G1a2b3'); Error = 'do Rotate''s step 2 first'; Done = 0 },
        @{ What = 'when D:\helios-ci cannot be set aside (a file in use): nothing is made or granted'
            Old = $oldSid; Current = $newSid; FailOn = 'rename D:\helios-ci'; Error = 'being used'; Done = 0 },
        @{ What = 'when icacls fails on the new D:\helios-ci'; Old = $oldSid; Current = $newSid
            FailOn = 'icacls D:\helios-ci /inheritance'; Error = 'icacls failed on D:\\helios-ci$'; Done = 3 },
        @{ What = 'when icacls fails on the new runner folder'; Old = $oldSid; Current = $newSid
            FailOn = 'icacls D:\helios-ci\runner'; Error = 'icacls failed on D:\\helios-ci\\runner'; Done = 4 },
        @{ What = 'when icacls fails on the new hooks folder'; Old = $oldSid; Current = $newSid
            FailOn = 'icacls D:\helios-ci\hooks'; Error = 'icacls failed on D:\\helios-ci\\hooks'; Done = 6 }
    )
    foreach ($case in $foldersRefused) {
        $services = if ($case.ContainsKey('Services')) { $case.Services } else { @() }
        $groups = if ($case.ContainsKey('Groups')) { $case.Groups } else { @() }
        Assert-Throws { Invoke-HeliosCiRotateFolders $case.Old $case.Current ([string]$case['FailOn']) $services $groups } $case.Error `
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
        function icacls { $script:rotateLog.Add("icacls $($args -join ' ')"); $global:LASTEXITCODE = 0 }
        $script:rotateLog = New-Object System.Collections.Generic.List[string]
        $global:HeliosCiRotateLog = $script:rotateLog
        $script:rotateEnv = $null
        $savedProfile = $env:USERPROFILE
        $env:USERPROFILE = $rotateProfile
        try { & { . ([scriptblock]::Create($rotateInstall[0])) } | Out-Null } finally { $env:USERPROFILE = $savedProfile }
    }
    $installDone = @("git -C $rotateRepo switch main", "git -C $rotateRepo pull --ff-only",
        "copy $rotateRepo\tools\ci\runner\job-started.ps1 -> $hookPath", 'write D:\helios-ci\runner\.env (Ascii)', 'firewall.ps1',
        "icacls $hookPath")
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
$global:LASTEXITCODE = 0   # the icacls and git stand-ins set it

# Nothing in the runbook runs a program or script from D:\helios-ci, or changes into it, in the owner's elevated
# window: helios-ci can change every file there (config.cmd, a plain batch file, and bin\, which the runner even
# replaces itself when it updates), so the next elevated run would run the account's code with the owner's rights.
# That rules out config.cmd remove, which the runbook names only to forbid it, and registering again in a folder the
# service ran from: every registration is step 5's, in the new runner folder that step 3 or Rotate makes.
$fromRunner = New-Object System.Collections.Generic.List[string]
foreach ($block in $blocks) {
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($block, [ref]$tokens, [ref]$errors)
    foreach ($command in $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] }, $true)) {
        $name = $command.CommandElements[0].Extent.Text.Trim('"', "'")
        $arguments = @($command.CommandElements | Select-Object -Skip 1 | ForEach-Object { $_.Extent.Text.Trim('"', "'") })
        if ($name -match '(?i)^(\.[\\/])?(D:\\helios-ci\b|config\.cmd|run\.cmd|bin[\\/])|Runner\.(Listener|Worker)|RunnerService' -or
            ($name -match '(?i)^(cd|chdir|sl|Set-Location|pushd|Push-Location)$' -and
                @($arguments | Where-Object { $_ -match '(?i)^D:\\helios-ci\b' }).Count)) {
            $fromRunner.Add($command.Extent.Text)
        }
    }
}
Assert-Equal '' ($fromRunner -join '; ') 'no runbook block runs anything from D:\helios-ci or changes into it'
$prose = [regex]::Replace($runbook, '(?ms)^[ \t]*```.*?^[ \t]*```', '')
$spans = @([regex]::Matches($prose, '`([^`]+)`') | ForEach-Object { $_.Groups[1].Value -replace '\s+', ' ' })
Assert-Equal '' (@($spans | Where-Object {
                $_ -match '(?i)^(cd|chdir|Set-Location|pushd) D:\\helios-ci|(^|[\s;&])\.[\\/](config|run)\.cmd|config\.cmd remove --token'
            }) -join '; ') 'no command in the runbook''s text runs config.cmd or run.cmd, or changes into D:\helios-ci'
Assert-Equal 1 ([regex]::Matches($runbook, 'config\.cmd remove')).Count 'the runbook names config.cmd remove once'
Assert-Equal 1 ([regex]::Matches($runbook, '\*\*Never run `config\.cmd remove`\*\*')).Count 'and only to forbid it'
$step5Start = $runbook.IndexOf('### 5. Register the runner')
$step5End = $runbook.IndexOf('### 6. Install the job-started hook')
$step5 = if ($step5Start -ge 0 -and $step5End -gt $step5Start) { $runbook.Substring($step5Start, $step5End - $step5Start) -replace '\s+', ' ' } else { '' }
Assert-Equal $true ($step5.Contains('Register only from a fresh download into the new `runner` folder that step 3, or "Rotate", has just made') -and
    $step5.Contains('including the line that checks the download''s SHA-256')) `
    'step 5 registers only from a fresh, checked download into a new runner folder'
Assert-Equal $true ($step5.Contains('step 4b''s audit must show no `PATH` line: `config.cmd` starts `powershell.exe` by name')) `
    'step 5 needs a clean PATH: config.cmd starts powershell.exe by name'
$rotateText = $rotateSection -replace '\s+', ' '
Assert-Equal $true ($rotateText.Contains('only then registers the runner again, from a fresh download in the new `runner` folder') -and
    $rotateText.Contains('**Force remove this runner**, not the command that the dialog shows')) `
    'every rotation removes the runner on GitHub without config.cmd and registers it in a new folder'
# Every rotation starts with step 4b's audit, in a clean window: Rotate's steps 2 and 3 run sc.exe, icacls and git by
# name, and in any other window the audit's own cmdlets are looked for on the PATH first.
$rotatePath = $rotateText.IndexOf('First run step 4b''s audit in a clean elevated window (Setup): this step and the next run ' +
    '`sc.exe`, `icacls` and `git` by name, and in any other window the audit''s own cmdlets may run what a `PATH` folder ' +
    'holds. If it shows a `PATH` line, do steps 1 to 4 of "Already done" before anything else.')
Assert-Equal $true ($rotatePath -ge 0 -and $rotatePath -lt $rotateText.IndexOf('# Rotate: delete the runner''s service')) `
    'every rotation runs the audit in a clean window and cleans the PATH before it runs sc.exe, icacls or git'
$removeAt = $rotateText.IndexOf('Remove for good:')
$remove = if ($removeAt -ge 0) { $rotateText.Substring($removeAt) } else { '' }
Assert-Equal $true ($remove.Contains('do Rotate''s step 2') -and
    $remove.Contains('`& "$env:USERPROFILE\src\HeliosEngine\tools\ci\runner\firewall.ps1" -Remove`') -and
    $remove.Contains('`& "$env:SystemRoot\System32\cmd.exe" /d /c rd /s /q "\\?\D:\helios-ci"`')) `
    'removing for good uses Rotate''s step 2, runs firewall.ps1 from the clone by its full path and deletes with rd'
# Administrator rights are out of the account's reach only while the owner never runs anything from D:\helios-ci
# elevated; the residual risk says so instead of claiming it outright.
$riskStart = $runbook.IndexOf('What stays possible (K33''s residual risk)')
$riskEnd = $runbook.IndexOf('## Names (binding)')
$risk = if ($riskStart -ge 0 -and $riskEnd -gt $riskStart) { $runbook.Substring($riskStart, $riskEnd - $riskStart) -replace '\s+', ' ' } else { '' }
$conditional = 'It cannot reach administrator rights either, **as long as you never run anything from `D:\helios-ci` in an ' +
    'elevated window once the service has run**'
Assert-Equal $true ($risk.Contains($conditional) -and -not $risk.Contains('administrator rights or the LAN')) `
    'the residual risk makes "no administrator rights" conditional on never running D:\helios-ci elevated'
Assert-Equal $true ($risk.Contains('The same holds **as long as no folder on the `PATH` is open to it**')) `
    'and on no folder on the PATH being open to helios-ci'

# The runner was online without the hook from 2026-10-03: the runbook's "Already done" path shows the jobs that ran
# and sends the owner through Rotate, whatever they show (a job can delete its own log); and since step 4b had not
# closed anything yet, it sends the owner to step 4b's list for the folders that were open to that code (closing one
# keeps what was planted in it).
$alreadyStart = $runbook.IndexOf('**Already done on 2026-10-03?**')
$alreadyEnd = $runbook.IndexOf('### 1. Prerequisites')
$already = if ($alreadyStart -ge 0 -and $alreadyEnd -gt $alreadyStart) { $runbook.Substring($alreadyStart, $alreadyEnd - $alreadyStart) } else { '' }
Assert-Equal $true ($already -match "Get-ChildItem D:\\helios-ci\\runner\\_diag -Filter 'Worker_\*\.log'") `
    '"Already done" lists the jobs that ran without the hook'
$alreadyText = $already -replace '\s+', ' '
Assert-Equal $true ($alreadyText -match ('Then start over, whatever the list of jobs showed: .*follow "Rotate" below from its step 2\. ' +
        'It removes the runner with Windows'' own tools, never with `config\.cmd`')) `
    '"Already done" removes the runner without config.cmd, then replaces the account and D:\helios-ci'
Assert-Equal $false ($already -match 'steps 6 to 10') '"Already done" no longer goes on with the same account and runner folder'
Assert-Equal $true ($alreadyText.Contains('every folder that step 4b''s first audit lists as `write`, including `C:\VulkanSDK`') -and
    $alreadyText.Contains('do step 4b''s "After unreviewed code" list for that first audit before you close anything')) `
    '"Already done" sends the owner to step 4b''s list for the folders that were open to unreviewed code'
$step4bStart = $runbook.IndexOf('### 4b. Close your own folders')
$step4b = if ($step4bStart -ge 0 -and $step5Start -gt $step4bStart) { $runbook.Substring($step4bStart, $step5Start - $step4bStart) -replace '\s+', ' ' } else { '' }
Assert-Equal $true ($step4b.Contains('**After unreviewed code.**') -and
    $step4b.Contains('delete the folder without running anything in it, its uninstaller included') -and
    $step4b.Contains('`& "$env:SystemRoot\System32\cmd.exe" /d /c rd /s /q "\\?\C:\VulkanSDK"`') -and
    $step4b.Contains('clone it again into your profile instead of moving the old one') -and
    $step4b.Contains('**Secrets in any listed folder**, `read` lines included') -and
    $step4b.Contains('restore it from a backup made before the runner came online (2026-10-03)')) `
    'step 4b says what to do with folders that unreviewed code could change: reinstall, clone again, replace secrets, restore'

# A folder on the PATH that helios-ci can change gives it every program the owner starts by name, elevated or not (the
# Vulkan SDK puts its Bin first, ahead of System32), and every cmdlet whose module the window has not loaded yet:
# PowerShell looks on the PATH before it loads the module, so a Get-Acl.exe or Disable-LocalUser.cmd there runs in its
# place. Step 4 says so, step 4b's audit lists such folders as PATH lines and deals with them before anything else, and
# "Already done" cleans the PATH in a clean elevated window (cmd.exe from Win+R by its full path, the PATH and the
# PSModulePath set to Windows' own folders before it starts Windows PowerShell by its full path without the profile):
# it disables helios-ci and ends its processes, runs the audit, deletes C:\VulkanSDK with cmd.exe called by its full
# path, takes the other entries off the PATH with the editor started from that window, and runs the audit again, all
# before an ordinary window, the Go line and Rotate (sc.exe, icacls, git by name).
$step4Start = $runbook.IndexOf('### 4. Tools, machine-wide')
$step4 = if ($step4Start -ge 0 -and $step4bStart -gt $step4Start) { $runbook.Substring($step4Start, $step4bStart - $step4Start) -replace '\s+', ' ' } else { '' }
Assert-Equal $true ($step4.Contains('puts `C:\VulkanSDK\<version>\Bin` first on the machine `PATH`, ahead of `C:\Windows\System32`') -and
    $step4.Contains('A folder on the `PATH` that `helios-ci` can write to gives it every command you run elevated')) `
    'step 4 says that the Vulkan SDK puts its Bin first on the PATH, and what a writable PATH folder gives helios-ci'
Assert-Equal $true ($step4b.Contains('**A `PATH` line**: until it is gone, any program that you start by name may be one that `helios-ci` put there') -and
    $step4b.Contains('when the tool''s folder is on the machine `PATH` (the Vulkan SDK''s `Bin` comes first, ahead of System32), the next time you run any program by name') -and
    $step4b.Contains('**`PATH` lines first**') -and $step4b.Contains('Do steps 1 to 4 of "Already done"') -and
    $step4b.Contains('Run the audit again until it has no `PATH` line')) `
    'step 4b deals with PATH lines first, and says that a tool folder on the PATH affects every program run by name'
$step4bRaw = if ($step4bStart -ge 0 -and $step5Start -gt $step4bStart) { $runbook.Substring($step4bStart, $step5Start - $step4bStart) } else { '' }
$bareIcacls = @([regex]::Matches($step4bRaw, '(?ms)^[ \t]*```powershell[ \t]*\r?\n(.*?)^[ \t]*```') | ForEach-Object {
        $tokens = $null
        $errors = $null
        $ast = [System.Management.Automation.Language.Parser]::ParseInput($_.Groups[1].Value, [ref]$tokens, [ref]$errors)
        $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -match '^icacls(\.exe)?$' }, $true) | ForEach-Object { $_.Extent.Text }
    })
Assert-Equal '' ($bareIcacls -join '; ') 'step 4b''s blocks call icacls by its full path'
$alreadyAt = @(('Open a clean elevated window (above) before anything else',
        '# Already done: disable helios-ci and end its processes before you run any program.',
        'Run step 4b''s audit (in the clean window it reads ACLs and the `PATH` settings and starts no program)',
        '# Already done: delete C:\VulkanSDK', 'Take every other `PATH` line''s entry off the `PATH` in System Properties, started from the clean window',
        '& "$env:SystemRoot\System32\SystemPropertiesAdvanced.exe"',
        'Run the audit again in the clean window: it must show no `PATH` line. Then close the window.',
        'Open an ordinary elevated window: it starts with the cleaned `PATH`', '`GoLang.Go`',
        'follow "Rotate" below from its step 2') | ForEach-Object { $alreadyText.IndexOf($_) })
$inOrder = $alreadyAt[0] -ge 0
for ($i = 1; $i -lt $alreadyAt.Count; $i++) { $inOrder = $inOrder -and $alreadyAt[$i] -gt $alreadyAt[$i - 1] }
Assert-Equal $true $inOrder ('"Already done": a clean window first, then disable helios-ci, audit, delete C:\VulkanSDK, take ' +
    'the PATH entries off with the editor started from that window, no PATH line, and only then an ordinary window, ' +
    'the Go line and Rotate')
$beforeGo = $already.Substring(0, [Math]::Max(0, $already.IndexOf('`GoLang.Go`')))
$alreadyBlocks = @([regex]::Matches($beforeGo, '(?ms)^[ \t]*```[A-Za-z]*[ \t]*\r?\n(.*?)^[ \t]*```') | ForEach-Object { $_.Groups[1].Value })
Assert-Equal 5 $alreadyBlocks.Count '"Already done" has five blocks before the Go line'
# Its first block comes after the instruction to open the clean window, and nothing comes before that instruction.
$alreadyFirstFence = [regex]::Match($already, '(?m)^[ \t]*```')
$alreadyIntro = if ($alreadyFirstFence.Success) { $already.Substring(0, $alreadyFirstFence.Index) -replace '\s+', ' ' } else { '' }
Assert-Equal $true ($alreadyIntro.Contains('Open a clean elevated window (above) before anything else, and run nothing in it but ' +
        'what this page shows')) '"Already done" opens the clean window before its first block'
# The claims that the audit and the blocks are safe in any elevated window are gone: they hold in the clean window only.
foreach ($claim in 'In this elevated window, and with only the commands shown here',
    'Run step 4b''s audit (it reads ACLs and the `PATH` settings and starts no program)',
    '(elevated; it reads ACLs and the `PATH` settings, starts no program and changes nothing)',
    'Edit the system environment variables') {
    Assert-Equal $false (($runbook -replace '\s+', ' ').Contains($claim)) "the runbook no longer says: $claim"
}

# The clean window (Setup, before "Already done"): its first block is cmd.exe's, whose first line sets the PATH to
# Windows' own folders, the second the PSModulePath, and the third starts Windows PowerShell by its full path without
# the profile; no block and no other line comes before them. cmd.exe sets both before PowerShell starts, which loads
# PSReadLine from the PSModulePath as it opens.
$setupStart = $runbook.IndexOf('## Setup')
$setupAlready = $runbook.IndexOf('**Already done on 2026-10-03?**')
$setup = if ($setupStart -ge 0 -and $setupAlready -gt $setupStart) { $runbook.Substring($setupStart, $setupAlready - $setupStart) } else { '' }
$cleanFence = [regex]::Match($setup, '(?ms)^[ \t]*```([A-Za-z]*)[ \t]*\r?\n(.*?)^[ \t]*```')
Assert-Equal 'bat' $cleanFence.Groups[1].Value 'the first block of the Setup is the clean window''s cmd.exe block, before "Already done"'
$cleanLines = @($cleanFence.Groups[2].Value -split '\r?\n' | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() })
# A list of folders is Windows' own when every entry is %SystemRoot% or a folder below it (no '..', no other variable).
function Test-HeliosCiWindowsFolders([string]$List) {
    $entries = @($List.Split(';'))
    return $entries.Count -gt 0 -and @($entries | Where-Object { $_ -cnotmatch '^%SystemRoot%(\\[A-Za-z0-9][A-Za-z0-9.]*)*$' }).Count -eq 0
}
$cleanPath = if ($cleanLines.Count -ge 1 -and $cleanLines[0] -cmatch '^set "PATH=([^"]+)"$') { $Matches[1] } else { '' }
Assert-Equal $true ((Test-HeliosCiWindowsFolders $cleanPath) -and @($cleanPath.Split(';')) -ccontains '%SystemRoot%\System32') `
    'the clean window''s first line sets the PATH to Windows'' own folders only, System32 included'
$cleanModules = if ($cleanLines.Count -ge 2 -and $cleanLines[1] -cmatch '^set "PSModulePath=([^"]+)"$') { $Matches[1] } else { '' }
Assert-Equal $true (Test-HeliosCiWindowsFolders $cleanModules) 'its second line sets the PSModulePath to Windows'' own folders only'
$cleanShell = '"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile'
Assert-Equal @($cleanShell) @($cleanLines | Select-Object -Skip 2) `
    'then it starts Windows PowerShell by its full path, without the profile, and runs nothing else'
$setupText = $setup -replace '\s+', ' '
Assert-Equal $true ($setupText.Contains('Press Win+R, type `%SystemRoot%\System32\cmd.exe /d`, press Ctrl+Shift+Enter (run as ' +
        'administrator)') -and $cleanFence.Index -gt $setup.IndexOf('Press Win+R')) `
    'the clean window is cmd.exe, started elevated from Win+R by its full path, without AutoRun commands'
# Everything that may run while a PATH line exists runs in the clean window: step 4b's audit, its PATH line, its list
# after unreviewed code, and the checklist's audit.
Assert-Equal $true ($step4b.Contains('in a clean elevated window (Setup, above) and only there. In it, the audit reads ACLs and ' +
        'the `PATH` settings, starts no program and changes nothing') -and
    $step4b.Contains('so deal with it before anything else, in the clean window') -and
    $step4b.Contains('Do steps 1 to 4 of "Already done" in a clean elevated window')) `
    'step 4b runs the audit, deals with a PATH line and cleans the PATH after unreviewed code in a clean window'

# On Windows: the clean window's lines as cmd.exe runs them, after a line that puts a folder first on the PATH (as the
# Vulkan SDK's Bin is) with a planted Get-CimInstance.cmd, Get-Acl.cmd and Disable-LocalUser.cmd in it, each of which
# leaves a file when it runs. Windows PowerShell 5.1 then finds the cmdlets and runs none of them. Without the clean
# window's PATH line, it runs the planted Get-CimInstance.cmd: the lookup that the clean window is for.
$cleanWindow = 'skipped, not Windows'
if (-not $onWindows) {
    Write-Host 'The clean window itself (cmd.exe, Windows PowerShell 5.1) runs on Windows only; skipped here.'
} elseif ($cleanLines.Count -eq 3) {
    $cleanWindow = 'ran'
    $cleanDir = [IO.Path]::Combine([IO.Path]::GetFullPath($WorkDir), 'clean-window')
    $planted = Join-Path $cleanDir 'planted'
    New-Item -ItemType Directory -Force -Path $planted | Out-Null
    foreach ($name in 'Get-CimInstance', 'Get-Acl', 'Disable-LocalUser') {
        Set-Content -LiteralPath (Join-Path $planted "$name.cmd") -Encoding Ascii -Value "@echo planted>""%~dp0ran-$name.txt"""
    }
    # Runs $Lines in cmd.exe, after a line that puts the planted folder first on the PATH, with $Probe given to the last
    # line (Windows PowerShell); returns what it printed. Not with the test's own PATH after the planted folder: a '"'
    # in it would end the quoted set.
    function Invoke-HeliosCiWindow([string[]]$Lines, [string]$Probe) {
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($Probe))
        $batch = Join-Path $cleanDir 'window.cmd'
        $all = @('@echo off', "set ""PATH=$planted;%SystemRoot%\System32;%SystemRoot%""") + @($Lines[0..($Lines.Count - 2)]) +
            @("$($Lines[-1]) -NonInteractive -EncodedCommand $encoded")
        Set-Content -LiteralPath $batch -Encoding Ascii -Value $all
        return @(& (Join-Path $env:SystemRoot 'System32\cmd.exe') /d /c $batch | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
    }
    $cleanProbe = "foreach (`$name in 'Get-CimInstance', 'Get-Acl', 'Disable-LocalUser') { " +
        "'{0} {1}' -f `$name, (Get-Command `$name).CommandType }; " +
        "`$null = Get-CimInstance -ClassName Win32_OperatingSystem; `$null = Get-Acl -LiteralPath `$env:SystemRoot; 'probe done'"
    Assert-Equal @('Get-CimInstance Cmdlet', 'Get-Acl Cmdlet', 'Disable-LocalUser Cmdlet', 'probe done') `
        (Invoke-HeliosCiWindow $cleanLines $cleanProbe) 'in the clean window, Windows PowerShell finds the cmdlets, not the planted files'
    Assert-Equal '' (@(Get-ChildItem -LiteralPath $planted -Filter 'ran-*' | ForEach-Object Name) -join ', ') `
        'the clean window runs none of the planted files'
    $dirtyProbe = "`$null = Get-CimInstance -ClassName Win32_OperatingSystem; 'probe done'"
    Assert-Equal @('probe done') (Invoke-HeliosCiWindow @($cleanLines[1], $cleanLines[2]) $dirtyProbe) `
        'without the clean window''s PATH, Windows PowerShell still answers'
    Assert-Equal 'ran-Get-CimInstance.txt' (@(Get-ChildItem -LiteralPath $planted -Filter 'ran-*' | ForEach-Object Name) -join ', ') `
        'without the clean window''s PATH, Windows PowerShell runs the planted Get-CimInstance.cmd (the lookup the window is for)'
    Remove-Item -LiteralPath $cleanDir -Recurse -Force
} else {
    $script:checks++
    $script:failures++
    $cleanWindow = 'not run'
    Write-Host 'FAIL: the clean window cannot run: its block does not have three lines'
}
$alreadyDisable = @($blocks | Where-Object { $_ -match '(?m)^[ \t]*# Already done: disable helios-ci' })
Assert-Equal 1 $alreadyDisable.Count '"Already done" has one block that disables helios-ci'
if ($alreadyDisable.Count -eq 1) {
    function Invoke-HeliosCiAlreadyDisable {
        function Disable-LocalUser {
            [CmdletBinding()] param([string]$Name)
            $script:rotateLog.Add("disable $Name")
        }
        function Get-Process {
            [CmdletBinding()] param([switch]$IncludeUserName)
            if (-not $IncludeUserName) { throw 'without -IncludeUserName, Get-Process shows no owners' }
            [pscustomobject]@{ Id = 11; UserName = 'PC\helios-ci' }
            [pscustomobject]@{ Id = 12; UserName = 'PC\owner' }
            [pscustomobject]@{ Id = 13; UserName = 'PC\HELIOS-CI' }
            [pscustomobject]@{ Id = 15; UserName = $null }
            [pscustomobject]@{ Id = 16; UserName = 'PC\helios-ci2' }
        }
        function Stop-Process {
            [CmdletBinding()] param([Parameter(ValueFromPipeline = $true)] $InputObject, [switch]$Force)
            process { $script:rotateLog.Add("stop $($InputObject.Id) force=$Force") }
        }
        $script:rotateLog = New-Object System.Collections.Generic.List[string]
        $savedComputer = $env:COMPUTERNAME
        $env:COMPUTERNAME = 'PC'
        try { & { . ([scriptblock]::Create($alreadyDisable[0])) } | Out-Null } finally { $env:COMPUTERNAME = $savedComputer }
    }
    try { Invoke-HeliosCiAlreadyDisable } catch { $script:rotateLog.Add("error: $($_.Exception.Message)") }
    Assert-Equal @('disable helios-ci', 'stop 11 force=True', 'stop 13 force=True') $script:rotateLog.ToArray() `
        '"Already done" disables helios-ci first, then ends its processes and no others'
}
# Every rd runs in cmd.exe called by its full path, with /d (no AutoRun commands): a bare cmd is looked for on the
# PATH first. No command in the runbook calls cmd by name.
$runbookText = $runbook -replace '\s+', ' '
$rdAll = [regex]::Matches($runbookText, '\brd /s /q').Count
Assert-Equal $true ($rdAll -ge 4) 'the runbook deletes with rd (C:\VulkanSDK, the old trees, D:\helios-ci)'
$rdBare = @([regex]::Matches($runbook, '(?m)^.*\brd /s /q.*$') | ForEach-Object { $_.Value.Trim() } |
        Where-Object { $_ -notmatch [regex]::Escape('& "$env:SystemRoot\System32\cmd.exe" /d /c rd /s /q "\\?\') })
Assert-Equal '' ($rdBare -join '; ') 'every rd in the runbook runs in cmd.exe called by its full path, with /d'
Assert-Equal '' (@([regex]::Matches($runbook, '(?i)(?<![\\\w.-])cmd(\.exe)?\s+/[a-z]') | ForEach-Object { $_.Value }) -join '; ') `
    'no command in the runbook calls cmd by name'
# The checklist looks for helios-ci's PowerShell profiles, which the runner loads before the hook, from a window that
# does not load them, and asks for the execution policy in effect for helios-ci in both shells (the runner starts the
# hook with pwsh when PowerShell 7 is installed, which has policies of its own that step 9's block does not read).
$checklistStart = $runbook.IndexOf('## Verification checklist')
$checklistEnd = $runbook.IndexOf('## Day to day')
$checklist = if ($checklistStart -ge 0 -and $checklistEnd -gt $checklistStart) { $runbook.Substring($checklistStart, $checklistEnd - $checklistStart) } else { '' }
Assert-Equal $true ($checklist.Contains('runas /user:helios-ci "powershell -NoProfile"') -and
    $checklist.Contains('$PROFILE.CurrentUserAllHosts') -and $checklist.Contains('$PROFILE.CurrentUserCurrentHost')) `
    'the checklist looks for helios-ci''s PowerShell profiles from a window without them'
$checklistText = $checklist -replace '\s+', ' '
Assert-Equal $true ($checklistText.Contains('Step 4b''s audit, in a clean elevated window, lists no `PATH` line, no `write` line but')) `
    'the checklist repeats the audit in a clean window, PATH included'
Assert-Equal $true ($checklistText.Contains('`Get-ExecutionPolicy` (the policy in effect for `helios-ci`) answers `RemoteSigned`, `Unrestricted` or `Bypass`') -and
    $checklistText.Contains('`pwsh -NoProfile -c Get-ExecutionPolicy` and `pwsh -NoProfile -c Get-ExecutionPolicy -Scope CurrentUser`')) `
    'the checklist asks for helios-ci''s execution policy in effect, in Windows PowerShell and in pwsh'

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
    Write-Host "runner scripts: $($script:failures) of $($script:checks) checks failed (wipe: $wipe, clean window: $cleanWindow)"
    exit 1
}
Write-Host "runner scripts: all $($script:checks) checks passed (wipe: $wipe, clean window: $cleanWindow)"
exit 0
