<#
Merges the CPU test shards' TRX files (from plan_test_shards.ps1's plan) and fails (exit 1) unless
the run is complete and clean:
  - every planned test ran exactly once (none missing, none twice, none unplanned);
  - no test failed or ended in another outcome than Passed;
  - every expected shard wrote a TRX.

Tests that cannot run on this machine pass after logging a SKIP-* marker. SKIP-NO-HARDWARE (a
missing hardware precondition: desktop size, CPU class, hardware D3D11 renderer) is reported on its
own line with the test names, so it is never hidden among the other markers.

  summarize_test_shards.ps1 -Plan shards.json -TrxDir test-results            # all shards
  summarize_test_shards.ps1 -Plan shards.json -TrxDir test-results -Shard 2   # one shard's own check

-Summary appends Markdown (GitHub's job summary); -WeightsOut writes measured seconds per test,
the input format of plan_test_shards.ps1 -Weights.
#>
param(
    [Parameter(Mandatory)][string]$Plan,
    [Parameter(Mandatory)][string]$TrxDir,
    [int]$Shard = -1,
    [string]$Summary = '',
    [string]$WeightsOut = ''
)
$ErrorActionPreference = 'Stop'
$planData = Get-Content $Plan -Raw | ConvertFrom-Json
$shards = @($planData.shards)
if ($Shard -ge 0) { $shards = @($shards | Where-Object { $_.index -eq $Shard }) }
if ($shards.Count -eq 0) { throw "No shard $Shard in $Plan" }

function ClassOf([string]$test) { $test.Substring(0, $test.LastIndexOf('::')) }
$classSet = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($s in $shards) { foreach ($c in $s.classes) { [void]$classSet.Add($c) } }
$expected = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($t in $planData.includedTests) { if ($classSet.Contains((ClassOf $t))) { [void]$expected.Add($t) } }

$problems = New-Object System.Collections.Generic.List[string]
$rows = New-Object System.Collections.Generic.List[object]
$shardRows = New-Object System.Collections.Generic.List[string]
foreach ($s in $shards) {
    $trx = Get-ChildItem $TrxDir -Recurse -Filter "cpu-tests-shard$($s.index).trx" -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $trx) { $problems.Add("shard $($s.index): no TRX (cpu-tests-shard$($s.index).trx)"); continue }
    [xml]$x = Get-Content $trx.FullName -Raw
    $names = @{}
    foreach ($d in $x.TestRun.TestDefinitions.UnitTest) { $names[$d.id] = $d.TestMethod.name }
    $n = 0; $seconds = 0.0
    foreach ($r in $x.TestRun.Results.UnitTestResult) {
        $fqn = $names[$r.testId]
        if (-not $fqn) { $fqn = $r.testName }
        $duration = if ($r.duration) { [TimeSpan]::Parse($r.duration).TotalSeconds } else { 0.0 }
        $out = ''
        if ($r.Output) {
            if ($r.Output.StdOut) { $out += [string]$r.Output.StdOut }
            if ($r.Output.ErrorInfo -and $r.Output.ErrorInfo.Message) { $out += "`n" + [string]$r.Output.ErrorInfo.Message }
        }
        $rows.Add([pscustomobject]@{ Shard = $s.index; Test = $fqn; Outcome = $r.outcome; Seconds = $duration; Output = $out })
        ++$n; $seconds += $duration
    }
    $start = $x.TestRun.Times.start; $finish = $x.TestRun.Times.finish
    $wall = if ($start -and $finish) { ([DateTimeOffset]::Parse($finish) - [DateTimeOffset]::Parse($start)).TotalMinutes } else { 0 }
    $shardRows.Add("| $($s.index) | $(@($s.classes).Count) + $(@($s.exact).Count) | $($s.tests) | $n | $([math]::Round($wall, 1)) min |")
}

# Completeness: every expected test exactly once, nothing else.
$seen = @{}
foreach ($row in $rows) { $seen[$row.Test] = 1 + [int]$seen[$row.Test] }
$missing = @($expected | Where-Object { -not $seen.ContainsKey($_) } | Sort-Object)
$twice = @($seen.Keys | Where-Object { $seen[$_] -gt 1 } | Sort-Object)
$unplanned = @($seen.Keys | Where-Object { -not $expected.Contains($_) } | Sort-Object)
if ($missing.Count) { $problems.Add("$($missing.Count) planned test(s) did not run") }
if ($twice.Count) { $problems.Add("$($twice.Count) test(s) ran more than once") }
if ($unplanned.Count) { $problems.Add("$($unplanned.Count) unplanned test(s) ran") }

$passed = @($rows | Where-Object Outcome -eq 'Passed')
$failed = @($rows | Where-Object Outcome -eq 'Failed')
$other = @($rows | Where-Object { $_.Outcome -notin 'Passed', 'Failed' })
if ($rows.Count -eq 0) { $problems.Add('no test ran') }
if ($failed.Count) { $problems.Add("$($failed.Count) test(s) failed") }
if ($other.Count) { $problems.Add("$($other.Count) test(s) ended neither Passed nor Failed") }

# SKIP markers: every marker counted; SKIP-NO-HARDWARE also listed by test with its reason.
$markerCounts = @{}
$skipMarked = 0
$noHardware = New-Object System.Collections.Generic.List[string]
foreach ($row in $rows) {
    $found = [regex]::Matches($row.Output, 'SKIP-[A-Z-]+')
    if ($found.Count) { ++$skipMarked }
    foreach ($m in $found) { $markerCounts[$m.Value] = 1 + [int]$markerCounts[$m.Value] }
    $hw = [regex]::Match($row.Output, 'SKIP-NO-HARDWARE [^\r\n]+')
    if ($hw.Success) { $noHardware.Add($hw.Value.Trim()) }
}
$noHardwareNames = @($noHardware | ForEach-Object { ($_ -split ' ')[1].TrimEnd(':') })

$scope = if ($Shard -ge 0) { "CPU tests, shard $Shard" } else { "CPU tests ($($shards.Count) shards)" }
$md = New-Object System.Collections.Generic.List[string]
$md.Add("### $scope")
$md.Add('| Planned | Ran | Passed | Failed | Not run / other | Passed with SKIP marker | Skipped for missing hardware |')
$md.Add('|---|---|---|---|---|---|---|')
$md.Add("| $($expected.Count) | $($rows.Count) | $($passed.Count) | $($failed.Count) | $($other.Count) | $skipMarked | $($noHardware.Count) |")
$md.Add('')
$md.Add("**Skipped for missing hardware: $($noHardware.Count)$(if ($noHardware.Count) { ', ' + ($noHardwareNames -join ', ') })**")
foreach ($line in $noHardware) { $md.Add("- ``$line``") }
$md.Add('')
if ($shardRows.Count) {
    $md.Add('| Shard | Classes + single tests | Planned | Ran | vstest wall-clock |'); $md.Add('|---|---|---|---|---|')
    foreach ($r in $shardRows) { $md.Add($r) }
    $md.Add('')
}
if ($markerCounts.Count) {
    $md.Add('| SKIP marker | Count |'); $md.Add('|---|---|')
    foreach ($k in ($markerCounts.Keys | Sort-Object { -$markerCounts[$_] }, { $_ })) { $md.Add("| $k | $($markerCounts[$k]) |") }
    $md.Add('')
}
foreach ($p in $problems) { $md.Add("- **Problem:** $p") }
foreach ($t in ($missing | Select-Object -First 50)) { $md.Add("- Did not run: $t") }
foreach ($t in ($twice | Select-Object -First 50)) { $md.Add("- Ran twice: $t") }
foreach ($t in ($unplanned | Select-Object -First 50)) { $md.Add("- Unplanned: $t") }
foreach ($r in ($rows | Where-Object Outcome -ne 'Passed' | Select-Object -First 100)) { $md.Add("- $($r.Outcome): $($r.Test) (shard $($r.Shard))") }

$md | ForEach-Object { Write-Host $_ }
if ($Summary) { $md | Out-File -Append -Encoding utf8 $Summary }

if ($WeightsOut) {
    $weights = [ordered]@{}
    foreach ($r in ($rows | Sort-Object Test)) { $weights[$r.Test] = [math]::Round($r.Seconds, 2) }
    $weights | ConvertTo-Json | Set-Content -Encoding utf8 $WeightsOut
}

if ($problems.Count) { Write-Host "FAILED: $($problems -join '; ')"; exit 1 }
Write-Host "PASSED: $($rows.Count) tests, skipped for missing hardware: $($noHardware.Count)$(if ($noHardware.Count) { ' (' + ($noHardwareNames -join ', ') + ')' })"
exit 0
