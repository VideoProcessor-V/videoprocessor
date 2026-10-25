<#
Decides what a CI run builds and tests (tier 1 "affected" vs tier 2 "full"), for
.github/workflows/ci-windows.yml. Conservative: whenever a change cannot be mapped with confidence,
the result is the full suite.

  select_ci_scope.ps1 -Branch fix/foo -OutFile scope.json [-Full]

Tier 2 (full: every project, the whole sharded CPU suite and the config tests):
  - -Full (workflow_dispatch input full: true), or a branch under ci/;
  - any branch that is not a feature branch (fix/, feat/, test/, research/);
  - a feature branch whose base cannot be found, or whose changes touch anything not mapped below.

Tier 1 (affected) for feature branches. The base is the most recent merge-base with any
non-feature remote branch (the integration branch the work started from). Every changed file is
classified:
  - documentation (docs/, *.md, root *.txt notes, .agents/, images/): no build, no tests;
  - a source file listed in a solution project (ClCompile/ClInclude/ResourceCompile/None):
    that project and every project that references it (transitively) are rebuilt;
  - a header under src/: every source file that includes it (transitively, matched by file name,
    so over-approximated) makes its projects affected;
  - anything else (project/solution/props/targets files, 3rdparty, shaders, fixtures, tools, the
    workflow): full suite.
Tests:
  - VideoProcessor-Test classes: the TEST_CLASSes of test .cpp files that changed or include a
    changed header; for a changed non-test .cpp, the test files including its same-named header.
    A changed non-test .cpp without such a header, or one no test includes, means full suite.
  - Config editor tests whenever VideoProcessor-ConfigTests is affected.
Nothing is excluded by name: the affected tier only narrows by dependency.

Output JSON: mode (full | affected | none), base, reasons[], projects[] (MSBuild solution targets;
empty for full = whole solution), testClasses[] (empty for full = all), configTests (bool),
cpuTests (bool).
#>
param(
    [Parameter(Mandatory)][string]$Branch,
    [Parameter(Mandatory)][string]$OutFile,
    [switch]$Full,
    [string]$Remote = 'origin',
    [string]$Head = 'HEAD'
)
$ErrorActionPreference = 'Stop'
$root = (git rev-parse --show-toplevel).Trim()
$featurePrefixes = @('fix/', 'feat/', 'test/', 'research/')
$reasons = New-Object System.Collections.Generic.List[string]

function Write-Scope([string]$mode, [string]$base, $projects, $classes, [bool]$config, [bool]$cpu) {
    $scope = [ordered]@{
        mode        = $mode
        branch      = $Branch
        base        = $base
        reasons     = @($reasons)
        projects    = @($projects)
        testClasses = @($classes)
        configTests = $config
        cpuTests    = $cpu
    }
    $scope | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 $OutFile
    "Scope: $mode$(if ($base) { " (base $base)" })"
    $reasons | ForEach-Object { "  $_" }
    if ($mode -eq 'affected') {
        "  projects: $(@($projects) -join ', ')"
        "  CPU test classes ($(@($classes).Count)): $(@($classes) -join ', ')"
        "  config tests: $config"
    }
    exit 0
}
function Write-Full([string]$why, [string]$base = '') {
    $reasons.Add($why)
    Write-Scope 'full' $base @() @() $true $true
}

if ($Full) { Write-Full 'full run requested (workflow_dispatch full: true)' }
if ($Branch -like 'ci/*') { Write-Full "branch $Branch is under ci/: full suite" }
if (-not ($featurePrefixes | Where-Object { $Branch.StartsWith($_) })) { Write-Full "branch $Branch is not a feature branch ($($featurePrefixes -join ', ')): full suite" }

# Base: the closest merge-base with a non-feature remote branch.
$candidates = @(git for-each-ref --format='%(refname:short)' "refs/remotes/$Remote" | ForEach-Object { $_.Trim() } |
    Where-Object { $_ -and $_ -ne "$Remote/HEAD" -and $_ -ne $Remote } |
    Where-Object { $name = $_.Substring($Remote.Length + 1); -not ($featurePrefixes | Where-Object { $name.StartsWith($_) }) })
if ($candidates.Count -eq 0) { Write-Full 'no integration branch on the remote: full suite' }
# The branch's own commits are those no integration branch contains; their boundary commits are the
# base. With several boundaries (merges) the changes against each are united (over-approximates).
$walk = @(git rev-list --boundary $Head --not @candidates | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$own = @($walk | Where-Object { -not $_.StartsWith('-') })
$boundaries = @($walk | Where-Object { $_.StartsWith('-') } | ForEach-Object { $_.Substring(1) })
if ($own.Count -eq 0) { $reasons.Add('every commit is already on an integration branch: nothing new to build or test'); Write-Scope 'none' '' @() @() $false $false }
if ($boundaries.Count -eq 0) { Write-Full 'no common history with any integration branch: full suite' }
$baseRef = @(git for-each-ref --contains $boundaries[0] --format='%(refname:short)' "refs/remotes/$Remote" | ForEach-Object { $_.Trim() } | Where-Object { $candidates -contains $_ } | Select-Object -First 1)
$base = "$(if ($baseRef) { $baseRef[0] } else { 'integration' }) @ $(($boundaries | ForEach-Object { $_.Substring(0, 8) }) -join ', ') ($($own.Count) commit(s) of its own)"
$changed = @($boundaries | ForEach-Object { git diff --name-only --no-renames $_ $Head } | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
$reasons.Add("$($changed.Count) changed file(s) against $base")
if ($changed.Count -eq 0) { $reasons.Add('no changes: nothing to build or test'); Write-Scope 'none' $base @() @() $false $false }

# Solution projects and the files they list.
$sln = Join-Path $root 'VideoProcessor.sln'
$projects = @{}   # name -> @{ path; dir; files(HashSet); refs(list of names) }
foreach ($line in (Get-Content $sln | Where-Object { $_ -like 'Project(*' })) {
    $parts = $line -split '"'
    $name = $parts[3]; $rel = $parts[5]
    if ($rel -notlike '*.vcxproj') { continue }
    $path = Join-Path $root $rel
    if (-not (Test-Path $path)) { continue }
    $projects[$name] = @{ path = [IO.Path]::GetFullPath($path); dir = Split-Path ([IO.Path]::GetFullPath($path)); files = (New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)); refs = New-Object System.Collections.Generic.List[string] }
}
$byPath = @{}
foreach ($n in $projects.Keys) { $byPath[$projects[$n].path.ToLowerInvariant()] = $n }
foreach ($n in $projects.Keys) {
    $p = $projects[$n]
    [xml]$x = Get-Content $p.path -Raw
    foreach ($item in $x.SelectNodes("//*[local-name()='ClCompile' or local-name()='ClInclude' or local-name()='ResourceCompile' or local-name()='None' or local-name()='CustomBuild' or local-name()='Image' or local-name()='Text']")) {
        $inc = $item.GetAttribute('Include')
        if (-not $inc -or $inc.Contains('$(') -or $inc.Contains('*')) { continue }
        [void]$p.files.Add([IO.Path]::GetFullPath((Join-Path $p.dir $inc)))
    }
    foreach ($r in $x.SelectNodes("//*[local-name()='ProjectReference']")) {
        $rp = [IO.Path]::GetFullPath((Join-Path $p.dir $r.GetAttribute('Include'))).ToLowerInvariant()
        if ($byPath.ContainsKey($rp)) { $p.refs.Add($byPath[$rp]) }
    }
}
function OwnersOf([string]$fullPath) { @($projects.Keys | Where-Object { $projects[$_].files.Contains($fullPath) } | Sort-Object) }

# Include graph over src/ (by file name; over-approximates).
$srcFiles = @(git ls-files src | Where-Object { $_ -match '\.(h|hpp|hh|inl|cpp|cc|c)$' } | ForEach-Object { [IO.Path]::GetFullPath((Join-Path $root $_)) })
$byName = @{}
foreach ($f in $srcFiles) { $k = [IO.Path]::GetFileName($f).ToLowerInvariant(); if (-not $byName.ContainsKey($k)) { $byName[$k] = New-Object System.Collections.Generic.List[string] }; $byName[$k].Add($f) }
$includers = @{}  # header file name -> files including it
foreach ($f in $srcFiles) {
    foreach ($m in [regex]::Matches([IO.File]::ReadAllText($f), '(?m)^\s*#\s*include\s*["<]([^">]+)[">]')) {
        $k = [IO.Path]::GetFileName($m.Groups[1].Value).ToLowerInvariant()
        if (-not $byName.ContainsKey($k)) { continue }
        if (-not $includers.ContainsKey($k)) { $includers[$k] = New-Object System.Collections.Generic.List[string] }
        $includers[$k].Add($f)
    }
}
function IncludeClosure([string[]]$headers) {
    # Every src file that includes any of the headers, directly or through other headers.
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $queue = New-Object System.Collections.Generic.Queue[string]
    foreach ($h in $headers) { $queue.Enqueue([IO.Path]::GetFileName($h).ToLowerInvariant()) }
    $doneNames = New-Object 'System.Collections.Generic.HashSet[string]'
    while ($queue.Count) {
        $k = $queue.Dequeue()
        if (-not $doneNames.Add($k)) { continue }
        if (-not $includers.ContainsKey($k)) { continue }
        foreach ($f in $includers[$k]) { if ($seen.Add($f)) { $queue.Enqueue([IO.Path]::GetFileName($f).ToLowerInvariant()) } }
    }
    @($seen)
}

$docPattern = '^(docs/|anevard-docs/|images/|\.agents/)|\.md$|^[^/]+\.txt$|^LICENSE'
$testDir = (Join-Path $root 'src\VideoProcessor-Test').ToLowerInvariant()
$affectedProjects = New-Object 'System.Collections.Generic.HashSet[string]'
$testFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$runAllCpu = $false
foreach ($rel in $changed) {
    if ($rel -match $docPattern) { $reasons.Add("doc: $rel"); continue }
    if ($rel -notmatch '^src/' -or $rel -match '\.(vcxproj|filters|props|targets|sln|user)$') { Write-Full "not mappable to a project: $rel (full suite)" $base }
    $fullPath = [IO.Path]::GetFullPath((Join-Path $root $rel))
    $isHeader = $rel -match '\.(h|hpp|hh|inl)$'
    $isSource = $rel -match '\.(cpp|cc|c)$'
    $owners = @(OwnersOf $fullPath)
    $users = @()
    if ($isHeader) { $users = @(IncludeClosure @($fullPath)) }
    if ($isSource -and -not (Test-Path $fullPath)) { $owners = @() }  # deleted: ownership comes from the project files, which changed too
    if ($owners.Count -eq 0 -and $users.Count -eq 0) { Write-Full "no project lists or includes $rel (full suite)" $base }
    foreach ($o in $owners) { [void]$affectedProjects.Add($o) }
    foreach ($u in $users) { foreach ($o in (OwnersOf $u)) { [void]$affectedProjects.Add($o) } }
    # CPU test selection.
    $inTest = $fullPath.ToLowerInvariant().StartsWith($testDir + '\') -or ($isSource -and $fullPath -match 'Tests\.cpp$' -and $owners -contains 'VideoProcessor-Test')
    if ($inTest -and $isSource) { [void]$testFiles.Add($fullPath); $reasons.Add("test source: $rel"); continue }
    $headers = @()
    if ($isHeader) { $headers = @($fullPath) }
    elseif ($isSource) {
        $stem = [IO.Path]::GetFileNameWithoutExtension($fullPath).ToLowerInvariant()
        $headers = @($srcFiles | Where-Object { [IO.Path]::GetFileNameWithoutExtension($_).ToLowerInvariant() -eq $stem -and $_ -match '\.(h|hpp|hh|inl)$' })
        if ($headers.Count -eq 0 -and $owners -contains 'VideoProcessor-Test') { Write-Full "$rel (built into the test project) has no same-named header (full suite)" $base }
        if ($headers.Count -eq 0 -and ($owners | Where-Object { $_ -eq 'VideoProcessor-Lib' })) { Write-Full "$rel has no same-named header to trace its tests (full suite)" $base }
    } else {
        if ($owners -contains 'VideoProcessor-Lib' -or $owners -contains 'VideoProcessor-Test') { Write-Full "$rel is a non-code item of a tested project (full suite)" $base }
    }
    $tests = @(IncludeClosure $headers | Where-Object { $_ -match '\.cpp$' -and ($_.ToLowerInvariant().StartsWith($testDir + '\') -or ($_ -match 'Tests\.cpp$' -and (OwnersOf $_) -contains 'VideoProcessor-Test')) })
    if ($inTest) { $tests += @($fullPath) }
    foreach ($t in $tests) { [void]$testFiles.Add($t) }
    $reasons.Add("$rel -> projects [$((@($owners) + @($users | ForEach-Object { OwnersOf $_ }) | Sort-Object -Unique) -join ', ')]; $($tests.Count) test file(s)")
    $linked = @($owners | Where-Object { $_ -eq 'VideoProcessor-Lib' })
    if ($tests.Count -eq 0 -and $linked.Count) { Write-Full "$rel is linked into the tests but no test file includes its header (full suite)" $base }
}

# Close over project references: a project that references an affected one is affected too.
do {
    $added = $false
    foreach ($n in $projects.Keys) {
        if ($affectedProjects.Contains($n)) { continue }
        if ($projects[$n].refs | Where-Object { $affectedProjects.Contains($_) }) { [void]$affectedProjects.Add($n); $added = $true }
    }
} while ($added)

$classes = New-Object 'System.Collections.Generic.SortedSet[string]'
if ($affectedProjects.Contains('VideoProcessor-VPRenderer')) {
    # The renderer DLL is exercised by loading it, not by including its sources.
    [void]$classes.Add('LibplaceboPluginProxyTests'); $reasons.Add('VideoProcessor-VPRenderer affected: LibplaceboPluginProxyTests (loads the renderer DLL)')
}
foreach ($t in $testFiles) {
    if (-not (Test-Path $t)) { continue }
    $text = [IO.File]::ReadAllText($t)
    $ns = [regex]::Matches($text, '(?m)^\s*namespace\s+([A-Za-z_][A-Za-z0-9_:]*)')
    foreach ($m in [regex]::Matches($text, 'TEST_CLASS\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)')) {
        # The class name alone; plan_test_shards.ps1 matches it against the listed Namespace::Class.
        [void]$classes.Add($m.Groups[1].Value)
    }
}
$cpu = $classes.Count -gt 0
$config = $affectedProjects.Contains('VideoProcessor-ConfigTests')
if ($cpu) { [void]$affectedProjects.Add('VideoProcessor-Test') }
if ($classes.Contains('LibplaceboPluginProxyTests')) { [void]$affectedProjects.Add('VideoProcessor-VPRenderer'); $reasons.Add('LibplaceboPluginProxyTests loads vprenderer\VideoProcessorVPRenderer.dll: renderer built') }
if ($affectedProjects.Count -eq 0) { $reasons.Add('only documentation changed: nothing to build or test'); Write-Scope 'none' $base @() @() $false $false }
if ($affectedProjects.Count -ge $projects.Count - 1) { Write-Full "$($affectedProjects.Count) of $($projects.Count) projects affected (full suite)" $base }
Write-Scope 'affected' $base (@($affectedProjects) | Sort-Object) @($classes) $config $cpu
