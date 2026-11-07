<#
The GPU part of the chain gate (test hardening 2026-10-17): runs the renderer capture harness
(Tests.RendererCaptureHarnessTests, every test there carries TEST_METHOD_ATTRIBUTE Category
GpuHardware except the opt-in on-screen ones, selected with the other GpuHardware/GpuOnScreen tests by
tools/gen_test_filters.py --gpu) plus the libplacebo GPU tests whose names match
-Names, on the hardware GPU, with VP_TEST_REQUIRE_GPU=1 so a hardware test fails instead of
skipping. vstest's native adapter does not filter on TEST_METHOD_ATTRIBUTE traits, so the
generator lists the name clauses too. Owner rule: the GpuWarp tests run on WARP (CPU run) and on the
real GPU (-GpuWarp here).

  run_chain_gate.ps1                                  # harness + ExtendedRange|SigmaDelta|Interpolator|DolbyVision
  run_chain_gate.ps1 -Names "DolbyVision"             # harness + only the DV GPU tests
  run_chain_gate.ps1 -NoHarness -Names "SigmaDelta"   # GPU tests only

  run_chain_gate.ps1 -GpuWarp                         # + every GpuWarp test, on the hardware GPU
  run_chain_gate.ps1 -LoopTier                        # + the unattended loop tier when the DeckLink rig is present
  run_chain_gate.ps1 -PrintFilter                     # show the generated /TestCaseFilter and stop

  run_chain_gate.ps1 -OnScreen                        # + the opt-in on-screen tests, windowed
  run_chain_gate.ps1 -OnScreen -Fullscreen both       # windowed, then fullscreen (reported apart)

Reads the TRX (vstest prints no summary for some hosts here) and fails (exit 1) when any test
not on the KNOWN FAILURES allow-list (below, with reasons) failed or did not run, when no test ran, or when a SKIP-NO-GPU marker appears (a hardware test
that skipped). With an on-screen run requested (-OnScreen, or VP_TEST_HARNESS_ONSCREEN=1 in the
environment) a SKIP-NO-DISPLAY-TIMING marker also fails it: an on-screen test whose display gave
no frame statistics tested nothing (render-ahead follow-up 2026-10-17). Prints totals, failures
and every SKIP-* marker.

Chain gate clip and rig labels:
  - VP_TEST_FILTER_CLIP (a path; default D:\testclips\gate\gate_h264.mp4) is the clip for the file-source test.
    Absent or invalid: that test reports SKIP-NO-CLIP, which fails under VP_TEST_REQUIRE_GPU=1.
  - tools/make_gate_clips.ps1 writes the synthetic testsrc2 gate clips (no film frames).
  - A WARNING is printed (console only; the adapter choice is not changed) when the primary display is not on the render adapter.
  - "rig: no display timing" failures are still counted; the label reads "(suspected, ...)" when only the test name matched.
#>
param(
    [string]$Dll = (Join-Path $PSScriptRoot "..\x64\Release\VideoProcessor-Test.dll"),
    [string]$Names = "ExtendedRange|SigmaDelta|Interpolator|DolbyVision|DelayGate|CombinationRunner|PipelineSkeletonTests",
    [switch]$NoSettingsChecks,   # skip the VP-V data checks (settings agent 2026-10-24)
    [switch]$AllowBattery,       # GPU runs need mains power (owner rule); this overrides the check
    [switch]$NoHarness,          # skip the generated hardware set (harness and the other GpuHardware/GpuOnScreen tests)
    [switch]$GpuWarp,            # also run every GpuWarp test (manifest set) on the hardware GPU (VP_TEST_GPU=hardware)
    [switch]$PrintFilter,        # print the /TestCaseFilter and exit (no run, no power or vstest checks)
    [switch]$OnScreen,
    [string]$Fullscreen = "",   # VP_TEST_HARNESS_FULLSCREEN for the on-screen tests: 0, 1 or both
    [string]$Adapter = "NVIDIA",
    [string]$OutDir = (Join-Path ([System.IO.Path]::GetTempPath()) "vp-chain-gate"),
    # The pre-skeleton build's renderer DLL for the byte-identity test (skeleton lead 2026-10-24);
    # empty: the test reports SKIP-NO-BASELINE.
    [string]$BaselineRendererDll = "",
    [switch]$LoopTier,           # after the gate, also run the loop tier (tools/looprig/run_loop_tier.ps1) when a rig is present; no rig = SKIP-NO-RIG, not a failure
    [string]$LoopTierMode = "1080p60",
    [switch]$LenientDelay        # the delay gate reports instead of failing (VP_DELAY_GATE_STRICT off)
)
$ErrorActionPreference = "Stop"
# The hardware set comes from tools/gen_test_filters.py (manifest tools/test-categories.json), never
# from a copy here; a generator failure stops the gate. -Names still narrows by name on top.
. (Join-Path $PSScriptRoot "test_filters.ps1")
$clauses = @()
if (-not $NoHarness) { $clauses += Get-VpTestFilter -Kind gpu -Categories GpuHardware, GpuOnScreen }
if ($GpuWarp) { $clauses += Get-VpTestFilter -Kind gpuwarp }
foreach ($name in ($Names -split '\|')) { if ($name) { $clauses += "FullyQualifiedName~$name" } }
if ($clauses.Count -eq 0) { throw "nothing selected" }
$filter = $clauses -join '|'
if ($PrintFilter) { $filter; exit 0 }
# GPU tests only on mains power (BatteryStatus 2 = on AC; no battery = a desktop).
$battery = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
if ($battery -and $battery.BatteryStatus -ne 2 -and -not $AllowBattery) { throw "on battery: the GPU chain gate runs on mains only (-AllowBattery overrides)" }
# VP-V data checks (docs/design/test-structure.md): the tier table, slot matrix, option model and
# pairwise coverage must be current; CPU only, seconds.
if (-not $NoSettingsChecks) {
    $root = Join-Path $PSScriptRoot ".."
    foreach ($check in @(@("tools/gen_vpv_tiers.py", "--check"), @("tools/vpv_tests/slot_matrix.py", "--check"),
                         @("tools/vpv_tests/dimensions.py", "--check"), @("tools/vpv_tests/pairwise.py", "--verify", "tests/vpv/pairwise.txt"),
                         @("tools/vpv_tests/seed_module_readmes.py", "--check"), @("tools/check_module_manifests.py"))) {
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
$trx = Join-Path $OutDir "chain-gate.trx"
Remove-Item $trx -ErrorAction SilentlyContinue
$saved = @{}
foreach ($k in "VP_TEST_GPU", "VP_TEST_ADAPTER", "VP_TEST_REQUIRE_GPU", "VP_TEST_REQUIRE_HARDWARE", "VP_TEST_HARNESS_ONSCREEN", "VP_TEST_HARNESS_FULLSCREEN", "VP_DELAY_GATE_STRICT", "VP_TEST_BASELINE_RENDERER_DLL", "VP_TEST_FILTER_CLIP") {
    $saved[$k] = [Environment]::GetEnvironmentVariable($k)
}
# WARNING when the primary display is not on the render adapter: timing tests are then invalid (2026-11-01:
# 6 tests failed with "display timing state=unavailable" on Optimus, panel on Intel, VP on NVIDIA).
. (Join-Path $PSScriptRoot "primary_display_adapter.ps1")
try {
    $rigMismatch = $false
    $primary = Get-VpPrimaryDisplayAdapter
    if (-not $primary) { "WARNING: could not determine which adapter drives the primary display; timing tests may be invalid." }
    elseif ($primary -notlike "*$Adapter*") {
        $rigMismatch = $true
        "WARNING: the primary display is driven by '$primary', not by the render adapter '$Adapter'."
        "WARNING: VP gets no DXGI frame statistics for it, so the display-timing tests are INVALID on this rig."
        "WARNING: their failures are reported as 'rig: no display timing' (still counted). Put the primary display on '$Adapter' for a valid timing run."
    }
}
catch { "WARNING: could not determine which adapter drives the primary display ($($_.Exception.Message)); timing tests may be invalid." }
$onScreenRequested = $OnScreen -or $env:VP_TEST_HARNESS_ONSCREEN -eq "1"
$env:VP_TEST_GPU = "hardware"; $env:VP_TEST_ADAPTER = $Adapter; $env:VP_TEST_REQUIRE_GPU = "1"
# The delay gate is strict (owner 2026-10-24: the same total at every rate, render_ahead off);
# the byte-identity test runs against the baseline build when one is given.
if (-not $LenientDelay) { $env:VP_DELAY_GATE_STRICT = "1" }
if ($BaselineRendererDll) { $env:VP_TEST_BASELINE_RENDERER_DLL = (Resolve-Path $BaselineRendererDll).Path }
# Fail closed on the owner's machine: a test whose hardware precondition (desktop size, CPU class,
# hardware D3D11 renderer) is missing fails instead of logging SKIP-NO-HARDWARE.
$env:VP_TEST_REQUIRE_HARDWARE = "1"
if ($OnScreen) { $env:VP_TEST_HARNESS_ONSCREEN = "1" }
if ($Fullscreen) { $env:VP_TEST_HARNESS_FULLSCREEN = $Fullscreen }
try {
    # The file-source graph test needs a clip: the synthetic gate clip (D:\testclips\gate, no film frames; see
    # tools/make_gate_clips.ps1) unless VP_TEST_FILTER_CLIP is already set.
    $gateClip = "D:\testclips\gate\gate_h264.mp4"
    if (-not $env:VP_TEST_FILTER_CLIP) {
        if (Test-Path $gateClip) { $env:VP_TEST_FILTER_CLIP = $gateClip }
        else { "NOTE: $gateClip is absent and VP_TEST_FILTER_CLIP is unset: FileSourceLavVpRendererPlaysSeeksStepsAndStops cannot run (fails under VP_TEST_REQUIRE_GPU=1); run tools/make_gate_clips.ps1." }
    }
    if ($env:VP_TEST_FILTER_CLIP -and -not (Test-Path $env:VP_TEST_FILTER_CLIP)) { "WARNING: VP_TEST_FILTER_CLIP=$($env:VP_TEST_FILTER_CLIP) does not exist; the clip test will fail" }
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
# (CalibrationPatternsAreExactCodesInTheBackbuffer left the list 2026-11-07: passed 10/10 on 2026-11-01.)
$knownFailures = [ordered]@{
}
$failed = @($results | Where-Object { $_.outcome -ne "Passed" })
$known = @($failed | Where-Object { $knownFailures.Contains([string]$_.testName) })
$bad = @($failed | Where-Object { -not $knownFailures.Contains([string]$_.testName) })
# Timing tests that failed only because the display gave no timing (VP logs "display timing state:
# state=unavailable", seen 2026-11-01 on Optimus with the panel on Intel): still counted as failures,
# but labelled so they are not mistaken for a product regression.
# The 2026-11-01 timing set is also labelled by name when the primary display is on another adapter.
$timingTests = "InterpolationOffIsPixelIdenticalAtAWholeRatio", "InterpolationRunsAtAMeasuredRateMismatch", "StreamFaultsKeepTheDejudderSafeAndRecovering", "EntranceOnceMotionPicturesFromTheProcessedFrames", "DelayBudgetFrameLadderHoldsTheTotalAtEveryRate", "OnScreenRenderAheadExtras"
$rigNoTiming = @($bad | Where-Object { ([string]$_.Output.ErrorInfo.Message -match 'display timing state: state=unavailable|stats_hr=0x887A0004') -or ($rigMismatch -and $timingTests -contains [string]$_.testName) })
$rigMsg = @($bad | Where-Object { [string]$_.Output.ErrorInfo.Message -match 'display timing state: state=unavailable|stats_hr=0x887A0004' })
$bad | ForEach-Object {
    $label = if ($rigMsg -contains $_) { "rig: no display timing" }
        elseif ($rigNoTiming -contains $_) { "rig: no display timing (suspected, primary display on another adapter)" }
        else { $_.outcome }
    "  ${label}: $($_.testName)"
}
$known | ForEach-Object { "  KNOWN FAILURE ($($_.outcome)): $($_.testName) - $($knownFailures[[string]$_.testName])" }
foreach ($name in $knownFailures.Keys) {
    if (@($results | Where-Object { $_.testName -eq $name -and $_.outcome -eq "Passed" }).Count -gt 0) {
        "  known failure now PASSES (remove it from the allow-list): $name"
    }
}
"Chain gate: $($bad.Count) new failure(s), $($known.Count) known failure(s) (allow-listed)"
if ($rigNoTiming.Count -gt 0) {
    "Chain gate: $($rigNoTiming.Count) of those failure(s) are 'rig: no display timing' (the display gave VP no frame statistics; timing tests are invalid on this rig, see the WARNING at the start)"
}
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
if ($LoopTier) {
    # The loop tier holds D:\codex\GPU-QUIET itself and runs unattended; SKIP-NO-RIG (exit 0) is reported, not failed.
    & (Join-Path $PSScriptRoot "looprig\run_loop_tier.ps1") -Mode $LoopTierMode -AllowBattery:$AllowBattery -BuildDir (Split-Path -Parent $Dll) | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { "Chain gate: FAILED (loop tier)"; exit 1 }
}
"Chain gate: PASSED"
exit 0
