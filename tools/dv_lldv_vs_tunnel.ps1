<#
LLDV-vs-tunnel check, one command (DV autotest, 2026-10-17).

  powershell -File tools\dv_lldv_vs_tunnel.ps1 -Lldv <LLDV recording> -Tunnel <tunnel recording> `
      -TargetNits <LLDV EDID max> -BlackNits <LLDV EDID min> [-Gamma 2.4] [-Primaries bt2020] [-Every 6] [-SkipRender]

Steps:
  1. CPU: replay the tunnel recording through VP's tunnel path (vp-decoded.csv: per-frame
     presentation and DM L1 values) - test RecordedCorpusReplaysThroughTheTunnelPath.
  2. GPU (skipped with -SkipRender): render every -Every-th tunnel frame through VP to the
     LLDV target - test DvAutotestGpuRendersTunnelCorpusForLldvCompare (VP_TEST_GPU=hardware).
  3. Compare: tools\dv_compare\dv_lldv_vs_tunnel.py (alignment by content, Delta E ITP per
     frame and scene, LLDV brightness vs L1). Report in <Out>\report.md.
#>
param(
    [Parameter(Mandatory = $true)][string]$Lldv,
    [Parameter(Mandatory = $true)][string]$Tunnel,
    [double]$TargetNits = 100,
    [double]$BlackNits = 0.005,
    [double]$Gamma = 2.4,
    [ValidateSet("bt2020", "rec709")][string]$Primaries = "bt2020",
    [int]$Every = 6,
    [string]$Out = "",
    [string]$TestDll = "",
    # Extra renderer configuration for the render step (e.g. a [vprenderer.color.*]
    # profile whose output_gamma / target_primaries match -Gamma / -Primaries).
    [string]$RendererConfig = "",
    [switch]$SkipRender,
    # Print the two /TestCaseFilter texts and exit (give any -Lldv and -Tunnel).
    [switch]$PrintFilter
)
$ErrorActionPreference = "Stop"
# The test sets come from tools/gen_test_filters.py (manifest tools/test-categories.json) through
# tools/test_filters.ps1; the named test narrows the set. A generator failure stops the run.
. (Join-Path $PSScriptRoot "test_filters.ps1")
$replayFilter = "($(Get-VpTestFilter -Kind cpu))&(FullyQualifiedName~RecordedCorpusReplaysThroughTheTunnelPath)"
$renderFilter = "($(Get-VpTestFilter -Kind gpuall))&(FullyQualifiedName~DvAutotestGpuRendersTunnelCorpusForLldvCompare)"
if ($PrintFilter) { "replay: $replayFilter"; "render: $renderFilter"; exit 0 }
$repo = Split-Path -Parent $PSScriptRoot
if (-not $TestDll) { $TestDll = Join-Path $repo "x64\Release\VideoProcessor-Test.dll" }
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $Tunnel) ("lldv-vs-tunnel-" + (Get-Date -Format "yyyyMMdd-HHmmss")) }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$vstest = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\TestWindow\vstest.console.exe" | Select-Object -First 1
if (-not $vstest) { throw "vstest.console.exe not found" }

function Run-Test([string]$filter, [string]$trx) {
    & $vstest.FullName $TestDll "/TestCaseFilter:$filter" "/Logger:trx;LogFileName=$trx" "/ResultsDirectory:$Out" | Out-Null
    $xml = Get-Content (Join-Path $Out $trx) -Raw
    if ($xml -match '<Counters total="(\d+)" executed="(\d+)" passed="(\d+)" failed="(\d+)"') {
        Write-Host "  $filter : passed $($Matches[3]) of $($Matches[1]), failed $($Matches[4])"
        if ([int]$Matches[4] -gt 0) { throw "test failed: $filter (see $Out\$trx)" }
    }
}

Write-Host "1/3 Replaying the tunnel recording through VP's tunnel path (CPU)"
$env:VP_TEST_RAW_CORPUS_DIR = $Tunnel
Run-Test $replayFilter "replay.trx"
Remove-Item Env:VP_TEST_RAW_CORPUS_DIR

# The tunnel segment with the most frames (the title, not a menu).
$segment = Get-ChildItem -Path $Tunnel -Recurse -Filter capture.txt | ForEach-Object { $_.Directory } |
    Sort-Object { (Get-ChildItem $_.FullName -Filter "frame_*.bin").Count } -Descending | Select-Object -First 1
if (-not $segment) { throw "no recorded segment under $Tunnel" }

$render = Join-Path $Out "render"
if (-not $SkipRender) {
    Write-Host "2/3 Rendering tunnel frames through VP to $TargetNits / $BlackNits nits (GPU)"
    $env:VP_TEST_GPU = "hardware"
    $env:VP_TEST_DV_RENDER_CORPUS = $segment.FullName
    $env:VP_TEST_DV_RENDER_OUT = $render
    $env:VP_TEST_DV_RENDER_EVERY = "$Every"
    $extra = "[vprenderer.Default]\ndithering: off\ndeband: off\nsdr_target_nits: $TargetNits\nsdr_black_nits: $BlackNits\n"
    if ($RendererConfig) { $extra += ((Get-Content $RendererConfig -Raw) -replace "`r?`n", "\n") }
    $env:VP_TEST_DV_RENDER_EXTRA = $extra
    Write-Host "  The comparison assumes output gamma $Gamma and primaries $Primaries for the renders: check the capture JSON."
    try { Run-Test $renderFilter "render.trx" }
    finally {
        foreach ($name in "VP_TEST_GPU", "VP_TEST_DV_RENDER_CORPUS", "VP_TEST_DV_RENDER_OUT", "VP_TEST_DV_RENDER_EVERY", "VP_TEST_DV_RENDER_EXTRA") {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        }
    }
}

Write-Host "3/3 Comparing"
$pyArgs = @((Join-Path $repo "tools\dv_compare\dv_lldv_vs_tunnel.py"), "--lldv", $Lldv, "--tunnel", $segment.FullName, "--out", $Out,
    "--render-peak", "$TargetNits", "--render-black", "$BlackNits", "--render-gamma", "$Gamma", "--render-primaries", $Primaries)
if (-not $SkipRender) { $pyArgs += @("--tunnel-render", $render) }
& python -I @pyArgs
Write-Host "Report: $(Join-Path $Out 'report.md')"
