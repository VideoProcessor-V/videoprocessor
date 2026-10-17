<#
The GPU part of the chain gate (test hardening 2026-10-17): runs the renderer capture harness
(Tests.RendererCaptureHarnessTests, every test there carries TEST_METHOD_ATTRIBUTE Category
GpuHardware except the opt-in on-screen ones) plus the libplacebo GPU tests whose names match
-Names, on the hardware GPU, with VP_TEST_REQUIRE_GPU=1 so a hardware test fails instead of
skipping. vstest's native adapter does not filter on TEST_METHOD_ATTRIBUTE traits, so the
harness is selected by its class.

  run_chain_gate.ps1                                  # harness + ExtendedRange|SigmaDelta|Interpolator|DolbyVision
  run_chain_gate.ps1 -Names "DolbyVision"             # harness + only the DV GPU tests
  run_chain_gate.ps1 -NoHarness -Names "SigmaDelta"   # GPU tests only

Reads the TRX (vstest prints no summary for some hosts here) and fails (exit 1) when any test
failed or did not run, when no test ran, or when a SKIP-NO-GPU marker appears (a hardware test
that skipped). Prints totals, failures and every SKIP-* marker.
#>
param(
    [string]$Dll = (Join-Path $PSScriptRoot "..\x64\Release\VideoProcessor-Test.dll"),
    [string]$Names = "ExtendedRange|SigmaDelta|Interpolator|DolbyVision",
    [switch]$NoHarness,
    [string]$Adapter = "NVIDIA",
    [string]$OutDir = (Join-Path ([System.IO.Path]::GetTempPath()) "vp-chain-gate")
)
$ErrorActionPreference = "Stop"
$Dll = (Resolve-Path $Dll).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$vstest = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter vstest.console.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like "*TestPlatform*" } | Select-Object -First 1
if (-not $vstest) { throw "vstest.console.exe not found" }
$clauses = @()
if (-not $NoHarness) { $clauses += "FullyQualifiedName~RendererCaptureHarnessTests" }
foreach ($name in ($Names -split '\|')) { if ($name) { $clauses += "FullyQualifiedName~$name" } }
if ($clauses.Count -eq 0) { throw "nothing selected" }
$filter = $clauses -join '|'
$trx = Join-Path $OutDir "chain-gate.trx"
Remove-Item $trx -ErrorAction SilentlyContinue
$saved = @{}
foreach ($k in "VP_TEST_GPU", "VP_TEST_ADAPTER", "VP_TEST_REQUIRE_GPU") { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
$env:VP_TEST_GPU = "hardware"; $env:VP_TEST_ADAPTER = $Adapter; $env:VP_TEST_REQUIRE_GPU = "1"
try {
    # vstest writes "Test Run Failed." to stderr; under Stop that would end the script before the TRX
    # is read, so the run itself is not allowed to throw (the TRX decides).
    $ErrorActionPreference = "Continue"
    & $vstest.FullName $Dll /Platform:x64 "/TestCaseFilter:$filter" "/Logger:trx;LogFileName=$trx" 2>&1 | ForEach-Object { "$_" }
    $ErrorActionPreference = "Stop"
}
finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}
if (-not (Test-Path $trx)) { "Chain gate: no TRX written"; exit 1 }
[xml]$x = Get-Content $trx -Raw
$results = @($x.TestRun.Results.UnitTestResult)
$byOutcome = $results | Group-Object outcome | ForEach-Object { "$($_.Name) $($_.Count)" }
"Chain gate: $($results.Count) results: $($byOutcome -join ', ')"
$bad = @($results | Where-Object { $_.outcome -ne "Passed" })
$bad | ForEach-Object { "  $($_.outcome): $($_.testName)" }
$markers = @(Select-String -Path $trx -Pattern 'SKIP-[A-Z-]+ [A-Za-z0-9_]+' -AllMatches | ForEach-Object { $_.Matches } |
    ForEach-Object { $_.Value } | Sort-Object -Unique)
$markers | ForEach-Object { "  marker: $_" }
$noGpu = @($markers | Where-Object { $_ -like "SKIP-NO-GPU *" })
if ($results.Count -eq 0 -or $bad.Count -gt 0 -or $noGpu.Count -gt 0) { "Chain gate: FAILED"; exit 1 }
"Chain gate: PASSED"
exit 0
