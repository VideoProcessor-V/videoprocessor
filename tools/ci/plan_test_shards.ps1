<#
Splits the CPU test suite (VideoProcessor-Test.dll) into N disjoint shards for the GitHub Actions
matrix in .github/workflows/ci-windows.yml.

  plan_test_shards.ps1 -TestList tests.txt -Shards 5 -OutFile shards.json

-TestList is the output of
  vstest.console VideoProcessor-Test.dll /ListFullyQualifiedTests /ListTestsTargetPath:tests.txt
(one "Namespace::Class::Method" per line).

Units of work:
  - a test class, selected by "FullyQualifiedName~Namespace::Class::". A clause is a substring match,
    so a class whose clause also matches tests of another class (Tests::X:: inside UnitTests::X::)
    is merged with that class into one unit;
  - a heavy class (alone in its unit, weight above -SplitAboveFraction of one shard's share) is split
    into one unit per test, each selected exactly by "FullyQualifiedName=Namespace::Class::Method".
Units are assigned largest-first to the least-loaded shard (deterministic: ties by name, then the
lowest shard). A unit's weight is the sum of its tests' seconds from -Weights (JSON object
"Namespace::Class::Method" -> seconds, measured on the hosted runner by summarize_test_shards.ps1
-WeightsOut); a test without a weight counts -DefaultSecondsPerTest.

Classes whose name contains an -Exclude entry are left out (the GPU harness: every test there is
GpuHardware/GpuOnScreen, which vstest's native adapter cannot select by trait).

The plan is verified before it is written, and the script fails (exit 1) unless:
  - every included test is matched by exactly one shard's filter (complete and disjoint);
  - no excluded test is matched by any shard;
  - every shard has at least one unit.
#>
param(
    [Parameter(Mandatory)][string]$TestList,
    [Parameter(Mandatory)][int]$Shards,
    [Parameter(Mandatory)][string]$OutFile,
    [string[]]$Exclude = @('RendererCaptureHarnessTests'),
    [string]$Weights = '',
    [double]$DefaultSecondsPerTest = 0.5,
    [double]$SplitAboveFraction = 0.25
)
$ErrorActionPreference = 'Stop'
if ($Shards -lt 1) { throw '-Shards must be at least 1' }
# $PSScriptRoot is empty in Windows PowerShell 5.1 parameter defaults.
if (-not $Weights) { $Weights = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'test-weights.json' }

function ClassOf([string]$test) {
    $i = $test.LastIndexOf('::')
    if ($i -le 0) { throw "Not a Namespace::Class::Method name: $test" }
    $test.Substring(0, $i)
}

$all = @(Get-Content $TestList | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '::' } | Sort-Object -Unique)
if ($all.Count -eq 0) { throw "No tests in $TestList" }
$excludedSet = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($t in $all) { $c = ClassOf $t; if (@($Exclude | Where-Object { $c -like "*$_*" }).Count -gt 0) { [void]$excludedSet.Add($t) } }
$excludedTests = @($all | Where-Object { $excludedSet.Contains($_) })
$included = @($all | Where-Object { -not $excludedSet.Contains($_) })

$byClass = [ordered]@{}
foreach ($t in $included) {
    $c = ClassOf $t
    if (-not $byClass.Contains($c)) { $byClass[$c] = New-Object System.Collections.Generic.List[string] }
    $byClass[$c].Add($t)
}
$classes = @($byClass.Keys | Sort-Object)

# Union-find over classes whose clause matches another class's tests.
$parent = @{}
foreach ($c in $classes) { $parent[$c] = $c }
function Find([string]$c) { while ($parent[$c] -ne $c) { $c = $parent[$c] }; $c }
# Method names hold no "::", so "A::" occurs in a test of class B exactly when it occurs in "B::".
foreach ($a in $classes) {
    $clause = "${a}::"
    foreach ($b in $classes) {
        if ($a -ne $b -and "${b}::".Contains($clause)) {
            $ra = Find $a; $rb = Find $b
            if ($ra -ne $rb) { if ($ra -lt $rb) { $parent[$rb] = $ra } else { $parent[$ra] = $rb } }
        }
    }
}

$weightTable = @{}
if ($Weights -and (Test-Path $Weights)) {
    (Get-Content $Weights -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $weightTable[$_.Name] = [double]$_.Value }
}
$weighted = 0
# A key "Namespace::Class::*" gives the seconds of each test of that class without its own key.
function TestWeight([string]$t) {
    if ($weightTable.ContainsKey($t)) { return [math]::Max($weightTable[$t], 0.01) }
    $classKey = (ClassOf $t) + '::*'
    if ($weightTable.ContainsKey($classKey)) { return [math]::Max($weightTable[$classKey], 0.01) }
    $DefaultSecondsPerTest
}
foreach ($t in $included) { if ($weightTable.ContainsKey($t) -or $weightTable.ContainsKey((ClassOf $t) + '::*')) { ++$weighted } }
$total = ($included | ForEach-Object { TestWeight $_ } | Measure-Object -Sum).Sum
$splitAbove = $SplitAboveFraction * $total / $Shards

$groups = @{}
foreach ($c in $classes) {
    $r = Find $c
    if (-not $groups.ContainsKey($r)) { $groups[$r] = New-Object System.Collections.Generic.List[string] }
    $groups[$r].Add($c)
}
$units = New-Object System.Collections.Generic.List[object]
$splitClasses = New-Object System.Collections.Generic.List[string]
foreach ($g in ($groups.Keys | Sort-Object)) {
    $members = @($groups[$g] | Sort-Object)
    $tests = @($members | ForEach-Object { $byClass[$_] } | ForEach-Object { $_ })
    $w = ($tests | ForEach-Object { TestWeight $_ } | Measure-Object -Sum).Sum
    if ($Shards -gt 1 -and $members.Count -eq 1 -and $w -gt $splitAbove -and $tests.Count -gt 1) {
        $splitClasses.Add($members[0])
        foreach ($t in $tests) {
            $units.Add([pscustomobject]@{ Name = $t; Classes = @(); Exact = @($t); Tests = 1; Weight = (TestWeight $t) })
        }
    } else {
        $units.Add([pscustomobject]@{ Name = $g; Classes = $members; Exact = @(); Tests = $tests.Count; Weight = $w })
    }
}
$unitList = @($units | Sort-Object @{ Expression = 'Weight'; Descending = $true }, @{ Expression = 'Name'; Descending = $false })
if ($unitList.Count -lt $Shards) { throw "Only $($unitList.Count) units for $Shards shards" }

$load = New-Object double[] $Shards
$assigned = @(for ($i = 0; $i -lt $Shards; ++$i) { , (New-Object System.Collections.Generic.List[object]) })
foreach ($u in $unitList) {
    $best = 0
    for ($i = 1; $i -lt $Shards; ++$i) { if ($load[$i] -lt $load[$best]) { $best = $i } }
    $load[$best] += $u.Weight
    $assigned[$best].Add($u)
}

$plan = @(for ($i = 0; $i -lt $Shards; ++$i) {
    $cls = @($assigned[$i] | ForEach-Object { $_.Classes } | ForEach-Object { $_ } | Sort-Object)
    $exact = @($assigned[$i] | ForEach-Object { $_.Exact } | ForEach-Object { $_ } | Sort-Object)
    $clauses = @($cls | ForEach-Object { "FullyQualifiedName~${_}::" }) + @($exact | ForEach-Object { "FullyQualifiedName=$_" })
    [pscustomobject]@{
        index   = $i
        classes = $cls
        exact   = $exact
        filter  = ($clauses -join '|')
        tests   = ($assigned[$i] | Measure-Object Tests -Sum).Sum
        weight  = [math]::Round($load[$i], 1)
    }
})

# Verification: every included test matched by exactly one shard, no excluded test matched at all.
$problems = New-Object System.Collections.Generic.List[string]
$shardClauses = @($plan | ForEach-Object { , @($_.classes | ForEach-Object { "${_}::" }) })
$shardExact = @($plan | ForEach-Object { $h = New-Object 'System.Collections.Generic.HashSet[string]'; foreach ($e in $_.exact) { [void]$h.Add($e) }; , $h })
foreach ($t in $all) {
    $hits = 0
    for ($i = 0; $i -lt $Shards; ++$i) {
        $hit = $shardExact[$i].Contains($t)
        if (-not $hit) { foreach ($cl in $shardClauses[$i]) { if ($t.Contains($cl)) { $hit = $true; break } } }
        if ($hit) { ++$hits }
    }
    $want = if ($excludedSet.Contains($t)) { 0 } else { 1 }
    if ($hits -ne $want) { $problems.Add("$t matched by $hits shard(s), expected $want") }
}
foreach ($s in $plan) {
    if ($s.classes.Count + $s.exact.Count -eq 0) { $problems.Add("shard $($s.index) is empty") }
    if ($s.filter.Length -gt 30000) { $problems.Add("shard $($s.index) filter is $($s.filter.Length) characters (command-line limit)") }
}
$sumTests = ($plan | Measure-Object tests -Sum).Sum
if ($sumTests -ne $included.Count) { $problems.Add("shards hold $sumTests tests, expected $($included.Count)") }
if ($problems.Count -gt 0) { $problems | Select-Object -First 50 | ForEach-Object { "PLAN ERROR: $_" }; exit 1 }

[pscustomobject]@{
    shards        = $plan
    includedTests = $included
    excludedTests = $excludedTests
} | ConvertTo-Json -Depth 5 | Set-Content -Encoding utf8 $OutFile

"Planned $($included.Count) tests in $($classes.Count) classes ($($unitList.Count) units; split per test: $(if ($splitClasses.Count) { $splitClasses -join ', ' } else { 'none' })) over $Shards shards."
"Weights: $weighted of $($included.Count) tests measured ($Weights); total $([math]::Round($total)) s. Excluded $($excludedTests.Count) ($($Exclude -join ', '))."
$plan | ForEach-Object { "  shard $($_.index): $($_.classes.Count) classes + $($_.exact.Count) single tests, $($_.tests) tests, weight $($_.weight) s, filter $($_.filter.Length) chars" }
"Verified: every included test is matched by exactly one shard; no excluded test is matched."
exit 0
