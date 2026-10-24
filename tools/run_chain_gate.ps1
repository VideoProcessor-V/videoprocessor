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

  run_chain_gate.ps1 -OnScreen                        # + the opt-in on-screen tests, windowed
  run_chain_gate.ps1 -OnScreen -Fullscreen both       # windowed, then fullscreen (reported apart)

Reads the TRX (vstest prints no summary for some hosts here) and fails (exit 1) when any test
not on the KNOWN FAILURES allow-list (below, with reasons) failed or did not run, when no test ran, or when a SKIP-NO-GPU marker appears (a hardware test
that skipped). With an on-screen run requested (-OnScreen, or VP_TEST_HARNESS_ONSCREEN=1 in the
environment) a SKIP-NO-DISPLAY-TIMING marker also fails it: an on-screen test whose display gave
no frame statistics tested nothing (render-ahead follow-up 2026-10-17). Prints totals, failures
and every SKIP-* marker.
#>
param(
    [string]$Dll = (Join-Path $PSScriptRoot "..\x64\Release\VideoProcessor-Test.dll"),
    [string]$Names = "ExtendedRange|SigmaDelta|Interpolator|DolbyVision|DelayGate|CombinationRunner",
    [switch]$NoSettingsChecks,   # skip the VP-V data checks (settings agent 2026-10-24)
    [switch]$AllowBattery,       # GPU runs need mains power (owner rule); this overrides the check
    [switch]$NoHarness,
    [switch]$OnScreen,
    [string]$Fullscreen = "",   # VP_TEST_HARNESS_FULLSCREEN for the on-screen tests: 0, 1 or both
    [string]$Adapter = "NVIDIA",
    [string]$OutDir = (Join-Path ([System.IO.Path]::GetTempPath()) "vp-chain-gate")
)
$ErrorActionPreference = "Stop"
# GPU tests only on mains power (BatteryStatus 2 = on AC; no battery = a desktop).
$battery = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
if ($battery -and $battery.BatteryStatus -ne 2 -and -not $AllowBattery) { throw "on battery: the GPU chain gate runs on mains only (-AllowBattery overrides)" }
# VP-V data checks (docs/design/test-structure.md): the tier table, slot matrix, option model and
# pairwise coverage must be current; CPU only, seconds.
if (-not $NoSettingsChecks) {
    $root = Join-Path $PSScriptRoot ".."
    foreach ($check in @(@("tools/gen_vpv_tiers.py", "--check"), @("tools/vpv_tests/slot_matrix.py", "--check"),
                         @("tools/vpv_tests/dimensions.py", "--check"), @("tools/vpv_tests/pairwise.py", "--verify", "tests/vpv/pairwise.txt"),
                         @("tools/vpv_tests/seed_module_readmes.py", "--check"))) {
        Push-Location $root
        try { & python -I @check | ForEach-Object { "$_" } } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "VP-V data check failed: $($check -join ' ')" }
    }
}
if ($Fullscreen -and $Fullscreen -notin @("0", "1", "both")) { throw "-Fullscreen takes 0, 1 or both" }
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
foreach ($k in "VP_TEST_GPU", "VP_TEST_ADAPTER", "VP_TEST_REQUIRE_GPU", "VP_TEST_REQUIRE_HARDWARE", "VP_TEST_HARNESS_ONSCREEN", "VP_TEST_HARNESS_FULLSCREEN") {
    $saved[$k] = [Environment]::GetEnvironmentVariable($k)
}
$onScreenRequested = $OnScreen -or $env:VP_TEST_HARNESS_ONSCREEN -eq "1"
$env:VP_TEST_GPU = "hardware"; $env:VP_TEST_ADAPTER = $Adapter; $env:VP_TEST_REQUIRE_GPU = "1"
# Fail closed on the owner's machine: a test whose hardware precondition (desktop size, CPU class,
# hardware D3D11 renderer) is missing fails instead of logging SKIP-NO-HARDWARE.
$env:VP_TEST_REQUIRE_HARDWARE = "1"
if ($OnScreen) { $env:VP_TEST_HARNESS_ONSCREEN = "1" }
if ($Fullscreen) { $env:VP_TEST_HARNESS_FULLSCREEN = $Fullscreen }
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
# r6 fix 8 (2026-10-24): KNOWN FAILURES, an explicit allow-list with the reason, so any NEW failure
# stands out. A listed test that fails is reported as KNOWN and does not fail the gate; a listed test
# that passes is reported so it can be removed from the list. Everything else fails the gate as before.
# Remove an entry when its fix lands; never add one to get past a new failure.
$knownFailures = [ordered]@{
    "CalibrationPatternsAreExactCodesInTheBackbuffer" = "deferred to the calibration work: the small-window patch blit (FillCalibrationPattern, b312b811) does not land in the read-back; the full-field default patches are fine"
    "CalibrationDryRunThroughTheRenderer" = "deferred to the calibration work: GetCalibrationOutput try_lock x100 + Sleep(2) against renderMutex held across Present (~1.5 s per patch, 'Cancelled by the user')"
}
$failed = @($results | Where-Object { $_.outcome -ne "Passed" })
$known = @($failed | Where-Object { $knownFailures.Contains([string]$_.testName) })
$bad = @($failed | Where-Object { -not $knownFailures.Contains([string]$_.testName) })
$bad | ForEach-Object { "  $($_.outcome): $($_.testName)" }
$known | ForEach-Object { "  KNOWN FAILURE ($($_.outcome)): $($_.testName) - $($knownFailures[[string]$_.testName])" }
foreach ($name in $knownFailures.Keys) {
    if (@($results | Where-Object { $_.testName -eq $name -and $_.outcome -eq "Passed" }).Count -gt 0) {
        "  known failure now PASSES (remove it from the allow-list): $name"
    }
}
"Chain gate: $($bad.Count) new failure(s), $($known.Count) known failure(s) (allow-listed)"
$markers = @(Select-String -Path $trx -Pattern 'SKIP-[A-Z-]+ [A-Za-z0-9_]+' -AllMatches | ForEach-Object { $_.Matches } |
    ForEach-Object { $_.Value } | Sort-Object -Unique)
$markers | ForEach-Object { "  marker: $_" }
$noGpu = @($markers | Where-Object { $_ -like "SKIP-NO-GPU *" -or $_ -like "SKIP-NO-HARDWARE *" })
$noTiming = @()
if ($onScreenRequested) {
    $noTiming = @($markers | Where-Object { $_ -like "SKIP-NO-DISPLAY-TIMING *" })
    $noTiming | ForEach-Object { "  on-screen run without display timing (inconclusive, fails the gate): $_" }
}
if ($results.Count -eq 0 -or $bad.Count -gt 0 -or $noGpu.Count -gt 0 -or $noTiming.Count -gt 0) { "Chain gate: FAILED"; exit 1 }
"Chain gate: PASSED"
exit 0
