# TEMPORARY diagnostic for the doctest lambda-merge fix; reverted before the PR is merged.
# Lists external symbols whose names contain a doctest test-function name (DOCTEST_ANON_*) and that
# more than one object file of the same test executable defines. Each one is a COMDAT of which the
# linker keeps a single copy, i.e. code of one test file that another test file may end up running.
param([Parameter(Mandatory = $true)][string]$BuildDir)
$ErrorActionPreference = 'Stop'
$total = 0
foreach ($d in Get-ChildItem -Path $BuildDir -Recurse -Directory -Filter '*_tests.dir') {
    $map = @{}
    foreach ($o in Get-ChildItem -Path $d.FullName -Recurse -Filter '*.obj') {
        foreach ($l in (& dumpbin /nologo /symbols $o.FullName)) {
            if ($l -match '\sSECT[0-9A-F]+\s.*\sExternal\s+\|\s+(\S+)') {
                $s = $Matches[1]
                if ($s -like '*DOCTEST_ANON*') {
                    if (-not $map.ContainsKey($s)) { $map[$s] = [System.Collections.Generic.List[string]]::new() }
                    if (-not $map[$s].Contains($o.Name)) { $map[$s].Add($o.Name) }
                }
            }
        }
    }
    $dups = @($map.GetEnumerator() | Where-Object { $_.Value.Count -gt 1 } | Sort-Object Key)
    Write-Host "== $($d.Name): $($map.Count) external DOCTEST_ANON symbols, $($dups.Count) defined by more than one object"
    foreach ($e in @($map.GetEnumerator() | Sort-Object Key)) {
        $pretty = ((& undname $e.Key) | Select-String -Pattern 'is :- ' | Select-Object -First 1) -replace '^.*is :- ', ''
        Write-Host "  all: [$($e.Value -join ' + ')] $pretty"
    }
    foreach ($e in $dups) {
        $pretty = ((& undname $e.Key) | Select-String -Pattern 'is :- ' | Select-Object -First 1) -replace '^.*is :- ', ''
        Write-Host "  [$($e.Value -join ' + ')] $pretty"
        Write-Host "      $($e.Key)"
    }
    $total += $dups.Count
}
Write-Host "total: $total symbols defined by more than one object of a test executable"
