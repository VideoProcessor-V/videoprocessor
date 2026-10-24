<#
.SYNOPSIS
  Overnight combination runner: builds pairwise / high-risk / random combination
  lists and runs the CombinationRunner test class single-process.
.DESCRIPTION
  -Set pairwise|high-risk|random|all   which lists to include (default all)
  -Seed N        random seed (default yyyyMMdd)
  -Count K       random combos (default 500)
  -TestDll PATH  default ..\x64\Release\VideoProcessor-Test.dll (relative to tools)
  -OutDir DIR    default %TEMP%\vp-combinations\<timestamp>
  -Gpu           allow GPU runs (mains power required; VP_TEST_ADAPTER=NVIDIA)
  -Replay FILE   re-run exactly the lines of a previous failures.txt
  Output: OutDir\combos.txt, failures.txt (usable with -Replay), *.trx.
#>
[CmdletBinding()]
param(
    [ValidateSet('pairwise','high-risk','random','all')][string]$Set = 'all',
    [int]$Seed = [int](Get-Date -Format 'yyyyMMdd'),
    [int]$Count = 500,
    [string]$TestDll = (Join-Path $PSScriptRoot '..\x64\Release\VideoProcessor-Test.dll'),
    [string]$OutDir = '',
    [switch]$Gpu,
    [string]$Replay = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP ("vp-combinations\" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $OutDir | Out-Null

if ($Gpu) {
    $bat = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
    if ($bat -and -not (@($bat) | Where-Object { $_.BatteryStatus -eq 2 })) {
        throw 'Refusing -Gpu: not on mains power (battery discharging).'
    }
    $env:VP_TEST_ADAPTER = 'NVIDIA'
    $env:VP_COMBO_GPU = '1'
} else {
    $env:VP_COMBO_GPU = '0'
}

$combos = Join-Path $OutDir 'combos.txt'
$lines = New-Object System.Collections.Generic.List[string]
function Add-Lines([string]$path) {
    Get-Content $path | Where-Object { $_.Trim() -ne '' -and -not $_.StartsWith('#') } | ForEach-Object { $lines.Add($_) }
}

if ($Replay) {
    Get-Content $Replay | Where-Object { $_.Trim() -ne '' -and -not $_.StartsWith('#') } |
        ForEach-Object { $lines.Add(($_ -replace '\s+#.*$', '')) }
} else {
    $tools = Join-Path $repo 'tools\vpv_tests'
    if ($Set -in 'pairwise','all') {
        $f = Join-Path $OutDir 'pairwise.txt'
        & python -I (Join-Path $tools 'pairwise.py') --out $f | Out-Null
        if ($LASTEXITCODE) { throw 'pairwise generation failed' }
        Add-Lines $f
    }
    if ($Set -in 'high-risk','all') { Add-Lines (Join-Path $repo 'tests\vpv\high-risk.txt') }
    if ($Set -in 'random','all') {
        $f = Join-Path $OutDir 'random.txt'
        & python -I (Join-Path $tools 'random_combos.py') --seed $Seed --count $Count --out $f | Out-Null
        if ($LASTEXITCODE) { throw 'random generation failed' }
        Add-Lines $f
    }
}
[System.IO.File]::WriteAllLines($combos, $lines)
Write-Host ("Projected time: {0} combos x 0.5 s = about {1:N1} min (CPU estimate)" -f $lines.Count, ($lines.Count * 0.5 / 60))

$failures = Join-Path $OutDir 'failures.txt'
if (Test-Path $failures) { Remove-Item $failures }
$env:VP_COMBO_FILE = $combos
$env:VP_COMBO_LOG = $failures

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vstest = & $vswhere -latest -products * -find '**\vstest.console.exe' | Select-Object -First 1
if (-not $vstest) { throw 'vstest.console.exe not found via vswhere' }
$dll = (Resolve-Path $TestDll).Path

& $vstest $dll '/Tests:CombinationRunner' '/Logger:trx' "/ResultsDirectory:$OutDir"
$exit = $LASTEXITCODE

$failed = if (Test-Path $failures) { @(Get-Content $failures | Where-Object { $_.Trim() -ne '' }).Count } else { 0 }
Write-Host ''
Write-Host "Combos run:   $($lines.Count)"
Write-Host "Failures:     $failed"
Write-Host "Combos file:  $combos"
Write-Host "Failures file: $failures   (re-run with -Replay)"
Write-Host "vstest exit:  $exit"
if ($failed -gt 0 -or $exit -ne 0) { exit 1 }
