<#
.SYNOPSIS
    Blocks the win-gpu runner account's outbound traffic to the home LAN (docs/plan/09-roadmap-and-process.md
    §5.4a "Network"; WP-0.4; K33).

.DESCRIPTION
    Adds Windows Defender Firewall outbound block rules that apply only to processes of one local account (the
    runner's, helios-ci), through New-NetFirewallRule -LocalUser with that account's SID:
      - IPv4: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16;
      - IPv6: fc00::/7 (unique local), fe80::/10 (link-local);
      - the LocalSubnet keyword, which also covers the LAN's on-link prefixes outside those ranges, such as the
        global IPv6 addresses the ISP's prefix gives LAN devices. Only without -AllowAddress: block rules win over
        allow rules and a keyword cannot be split, so with lab addresses the on-link IPv6 prefix stays open (the
        script warns).
    -AllowAddress lists lab machines the runner must reach (09 §4.3.1): the block ranges are split around each
    address, since an allow rule could not override a block rule. Internet traffic is untouched: its remote
    addresses are public, even though it is routed through the LAN's gateway. Name resolution keeps working because
    Windows' DNS Client service sends queries as NETWORK SERVICE, not as the runner account.

    Idempotent: every run first removes the rules it made before (its rule group), then adds the current set.
    -Remove only removes them. Needs an elevated PowerShell (Windows PowerShell 5.1 or PowerShell 7).

.PARAMETER Account
    The runner's local account. Default: helios-ci.

.PARAMETER AllowAddress
    IPv4 or IPv6 addresses (not ranges) of lab machines inside the blocked ranges that the account may reach.

.PARAMETER Remove
    Remove this script's rules for the account and add none.

.EXAMPLE
    .\firewall.ps1                                  # block the LAN for helios-ci
    .\firewall.ps1 -AllowAddress 192.168.1.50       # ... except one lab host
    .\firewall.ps1 -WhatIf                          # show what would change
    .\firewall.ps1 -Remove                          # undo
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$Account = 'helios-ci',
    [string[]]$AllowAddress = @(),
    [switch]$Remove
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Numerics

$HeliosCiBlockedIPv4 = @('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', '169.254.0.0/16')
$HeliosCiBlockedIPv6 = @('fc00::/7', 'fe80::/10')

# An IP address as {Length (4 or 16 bytes); Value (an unsigned BigInteger)}.
function ConvertTo-HeliosCiIPNumber {
    param([Parameter(Mandatory = $true)] [string]$Address)
    $ip = $null
    if ($Address -match '/' -or -not [System.Net.IPAddress]::TryParse($Address, [ref]$ip)) {
        throw "'$Address' is not an IPv4 or IPv6 address"
    }
    $bytes = $ip.GetAddressBytes()
    [Array]::Reverse($bytes)
    $unsigned = [byte[]]($bytes + [byte]0)  # little-endian with a zero sign byte: never negative
    return [pscustomobject]@{ Length = $ip.GetAddressBytes().Length; Value = [System.Numerics.BigInteger]::new($unsigned) }
}

function ConvertFrom-HeliosCiIPNumber {
    param(
        [Parameter(Mandatory = $true)] [System.Numerics.BigInteger]$Value,
        [Parameter(Mandatory = $true)] [int]$Length
    )
    $little = $Value.ToByteArray()
    $bytes = New-Object byte[] $Length
    for ($i = 0; $i -lt $Length -and $i -lt $little.Length; $i++) { $bytes[$i] = $little[$i] }
    [Array]::Reverse($bytes)
    return ([System.Net.IPAddress]::new($bytes)).ToString()
}

# A CIDR block as {Cidr; Length; Start; End}.
function Get-HeliosCiCidrRange {
    param([Parameter(Mandatory = $true)] [string]$Cidr)
    $parts = $Cidr.Split('/')
    if ($parts.Count -ne 2) { throw "'$Cidr' is not a CIDR block" }
    $base = ConvertTo-HeliosCiIPNumber $parts[0]
    $bits = 8 * $base.Length
    $prefix = [int]$parts[1]
    if ($prefix -lt 0 -or $prefix -gt $bits) { throw "'$Cidr' has a bad prefix length" }
    $size = [System.Numerics.BigInteger]::Pow(2, $bits - $prefix)
    $start = [System.Numerics.BigInteger]::Subtract($base.Value, [System.Numerics.BigInteger]::Remainder($base.Value, $size))
    $end = [System.Numerics.BigInteger]::Subtract([System.Numerics.BigInteger]::Add($start, $size), 1)
    return [pscustomobject]@{ Cidr = $Cidr; Length = $base.Length; Start = $start; End = $end }
}

# The -RemoteAddress list that blocks every address of $Block (CIDR blocks of one family) except $Allow: an
# untouched block keeps its CIDR form, a split one becomes `first-last` ranges (or single addresses).
function Get-HeliosCiBlockList {
    param(
        [Parameter(Mandatory = $true)] [string[]]$Block,
        [AllowEmptyCollection()] [string[]]$Allow = @()
    )
    $ranges = @($Block | ForEach-Object { Get-HeliosCiCidrRange $_ })
    $points = @($Allow | ForEach-Object { ConvertTo-HeliosCiIPNumber $_ })
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($range in $ranges) {
        $inside = @($points | Where-Object {
                $_.Length -eq $range.Length -and
                [System.Numerics.BigInteger]::Compare($_.Value, $range.Start) -ge 0 -and
                [System.Numerics.BigInteger]::Compare($_.Value, $range.End) -le 0
            } | Sort-Object -Property Value -Unique)
        if ($inside.Count -eq 0) {
            $out.Add($range.Cidr)
            continue
        }
        $cursor = $range.Start
        foreach ($point in $inside + @([pscustomobject]@{ Value = [System.Numerics.BigInteger]::Add($range.End, 1) })) {
            if ([System.Numerics.BigInteger]::Compare($point.Value, $cursor) -gt 0) {
                $last = [System.Numerics.BigInteger]::Subtract($point.Value, 1)
                $first = ConvertFrom-HeliosCiIPNumber -Value $cursor -Length $range.Length
                if ([System.Numerics.BigInteger]::Compare($last, $cursor) -eq 0) {
                    $out.Add($first)
                } else {
                    $out.Add($first + '-' + (ConvertFrom-HeliosCiIPNumber -Value $last -Length $range.Length))
                }
            }
            $cursor = [System.Numerics.BigInteger]::Add($point.Value, 1)
        }
    }
    return , $out.ToArray()
}

# Allowed addresses that lie in no blocked range (allowing them would change nothing: likely a typo).
function Get-HeliosCiStrayAddress {
    param(
        [Parameter(Mandatory = $true)] [string[]]$Block,
        [AllowEmptyCollection()] [string[]]$Allow = @()
    )
    $ranges = @($Block | ForEach-Object { Get-HeliosCiCidrRange $_ })
    return , @($Allow | Where-Object {
            $point = ConvertTo-HeliosCiIPNumber $_
            -not ($ranges | Where-Object {
                    $_.Length -eq $point.Length -and
                    [System.Numerics.BigInteger]::Compare($point.Value, $_.Start) -ge 0 -and
                    [System.Numerics.BigInteger]::Compare($point.Value, $_.End) -le 0
                })
        })
}

function Invoke-HeliosCiFirewall {
    $principal = New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated PowerShell (Run as administrator).'
    }
    $sid = (New-Object Security.Principal.NTAccount $Account).Translate([Security.Principal.SecurityIdentifier]).Value
    $group = "Helios CI runner: LAN block for $Account"
    $old = @(Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue)
    if ($old.Count -gt 0) {
        Write-Host "Removing $($old.Count) earlier rule(s) of '$group'"
        $old | Remove-NetFirewallRule
    }
    if ($Remove) { return }

    $stray = Get-HeliosCiStrayAddress -Block ($HeliosCiBlockedIPv4 + $HeliosCiBlockedIPv6) -Allow $AllowAddress
    if ($stray.Count -gt 0) {
        throw "Not in a blocked range, so allowing it would change nothing: $($stray -join ', ')"
    }
    $rules = [ordered]@{
        IPv4 = Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv4 -Allow $AllowAddress
        IPv6 = Get-HeliosCiBlockList -Block $HeliosCiBlockedIPv6 -Allow $AllowAddress
    }
    if ($AllowAddress.Count -eq 0) {
        $rules['LocalSubnet'] = @('LocalSubnet')
    } else {
        Write-Warning ('With -AllowAddress the LocalSubnet rule is left out (a block rule cannot be split around a ' +
            'keyword), so LAN devices reachable through the ISP''s global IPv6 prefix are not blocked.')
    }
    # D: DACL; A: allow; CC: the firewall's "match" right; the SID: the account whose processes the rule covers.
    $localUser = "D:(A;;CC;;;$sid)"
    foreach ($name in $rules.Keys) {
        $params = @{
            Name          = "Helios-CI-LAN-Block-$Account-$name"
            DisplayName   = "Helios CI: block the LAN for $Account ($name)"
            Group         = $group
            Description   = 'Keeps the win-gpu runner account off the home LAN (docs/runbooks/win-gpu-runner.md; ' +
                            'tools/ci/runner/firewall.ps1 replaces these rules on every run).'
            Direction     = 'Outbound'
            Action        = 'Block'
            Profile       = 'Any'
            RemoteAddress = $rules[$name]
            LocalUser     = $localUser
        }
        New-NetFirewallRule @params | Out-Null
        $done = if ($WhatIfPreference) { 'Would add' } else { 'Added' }
        Write-Host "$done an outbound block for $Account ($sid), ${name}: $($rules[$name] -join ', ')"
    }
}

if (-not (Get-Variable -Name HeliosCiScriptTestMode -ErrorAction SilentlyContinue)) {
    Invoke-HeliosCiFirewall
}
