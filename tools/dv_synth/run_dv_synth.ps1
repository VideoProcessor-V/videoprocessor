<#
.SYNOPSIS
  Synthetic Dolby Vision tunnel validation (test/dv-synthetic, 2026-10-24): generates the corpus,
  runs the CPU tests, the GPU tests (mains power, NVIDIA) and the analysis.

.EXAMPLE
  powershell -File tools\dv_synth\run_dv_synth.ps1 -Corpus D:\codex\dv-synth-corpus -Out D:\codex\dv-synth-out
  -SkipGpu      CPU tests only (no GPU, any power state)
  -Regenerate   rewrite the corpus even when it exists (about 5.5 GB, about 4 minutes)
#>
param(
    [Parameter(Mandatory = $true)][string]$Corpus,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Build = (Join-Path $PSScriptRoot "..\..\x64\Release"),
    [switch]$SkipGpu,
    [switch]$Regenerate
)
$ErrorActionPreference = "Stop"
$python = "python"
$here = $PSScriptRoot
New-Item -ItemType Directory -Force $Out | Out-Null
if ($Regenerate -or -not (Test-Path (Join-Path $Corpus "robust-1080p24\truth.txt"))) {
    & $python -I (Join-Path $here "make_corpus.py") $Corpus
    if ($LASTEXITCODE -ne 0) { throw "corpus generation failed" }
}
$jobs = Join-Path $Out "jobs.txt"
& $python -I (Join-Path $here "run_dv_synth.py") jobs $Corpus $jobs
$vstest = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\Common7\IDE\Extensions\TestPlatform\vstest.console.exe" | Select-Object -First 1
$dll = Join-Path $Build "VideoProcessor-Test.dll"
$env:VP_TEST_DV_SYNTH_CORPUS = $Corpus
$cpu = "FullyQualifiedName~DvSyntheticCorpusTests"
& $vstest.FullName $dll "/TestCaseFilter:$cpu" "/Logger:trx;LogFileName=dv-synth-cpu.trx" "/ResultsDirectory:$Out"
if (-not $SkipGpu) {
    if ((Get-CimInstance Win32_Battery).BatteryStatus -ne 2) { throw "GPU tests need mains power" }
    $env:VP_TEST_GPU = "hardware"
    $env:VP_TEST_ADAPTER = "NVIDIA"
    $env:VP_TEST_DV_SYNTH_JOBS = $jobs
    $env:VP_TEST_DV_SYNTH_OUT = Join-Path $Out "renders"
    $gpu = "FullyQualifiedName~DvSynthGpu|FullyQualifiedName~GpuDolbyVisionTunnelUnpackMatchesTheSyntheticCorpus|FullyQualifiedName~DvAutotestGpu|FullyQualifiedName~DolbyVisionGpuUnpackMatchesTheCpuUnpackThroughTheRenderer"
    $env:VP_TEST_DV_RENDER_CORPUS = Join-Path $Corpus "acc-1080p24-p5-r210-low8"
    $env:VP_TEST_DV_RENDER_OUT = Join-Path $Out "corpus-render"
    $env:VP_TEST_DV_RENDER_EVERY = "2"
    & $vstest.FullName $dll "/TestCaseFilter:$gpu" "/Logger:trx;LogFileName=dv-synth-gpu.trx" "/ResultsDirectory:$Out"
    & $python -I (Join-Path $here "run_dv_synth.py") analyze $Corpus $jobs (Join-Path $Out "renders") (Join-Path $Out "report.json")
}
