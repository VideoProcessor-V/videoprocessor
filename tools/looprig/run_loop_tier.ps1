# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
The unattended loop test tier (LoopRig category). VP plays a stamped clip, its output is captured by the
DeckLink Mini Recorder 4K through StreamRecorder --stamps, and tools/looprig/analyze.py scores the loop.
Writes <RunRoot>\<stamp>\summary.md and loop-tier.trx (pass/fail/skip per scenario, the same outcomes our
other tiers report). Exit 0 = all pass or SKIP-NO-RIG, 1 = any failure / incomplete, 2 = bad setup.

  run_loop_tier.ps1                              # 1080p60 unattended set
  run_loop_tier.ps1 -Mode 2160p30
  run_loop_tier.ps1 -Only film24_to_60 -Seconds 30
  run_loop_tier.ps1 -OwnerAttended               # also the scenarios that need a person (prompts for each step)
  run_loop_tier.ps1 -DryRunSynthetic             # no rig: synthetic capture folders through the whole scoring pipeline
  run_loop_tier.ps1 -DryRun D:\some\capture-dir  # no rig: score an existing capture folder (one sub-folder per scenario)
  run_loop_tier.ps1 -PrintPlan                   # show the scenarios and stop

Rig detection: StreamRecorder --list must show a device matching -Match (default "Mini Recorder 4K");
otherwise the tier writes SKIP-NO-RIG and exits 0 without touching anything else.

Safety: the test folder is a COPY of the x64\Release outputs under <RunRoot>\<stamp>\vp (never
C:\Videoprocessor, never E:); only the PIDs this script launched are stopped; D:\codex\GPU-QUIET is held
for the run and released in finally; mains power only (-AllowBattery overrides).

Assumptions to confirm on the first rig run (see tools/looprig/README.md, section "Unattended tier"):
 - -PlayerArgs: how VP is told to play the file clip ({clip} is replaced). Default "/file `"{clip}`"".
 - the resync schedule is handed over through the child env VP_RESYNC_SCHEDULE (path of schedule.txt);
   the VP host integration of feat/resync-q0-q3 must read it, otherwise those scenarios have no events and fail.
#>
param(
    [ValidateSet("1080p60", "2160p30")][string]$Mode = "1080p60",
    [string]$Only = "",
    [int]$Seconds = 0,                 # 0 = the plan's duration per scenario
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\..\x64\Release"),
    [string]$RunRoot = "D:\codex\looprig-run",
    [string]$Recorder = "",            # default: <BuildDir>\VideoProcessorStreamRecorder.exe
    [string]$Match = "Mini Recorder 4K",
    [string]$PlayerArgs = '/file "{clip}"',
    [int]$Seed = 1,
    [switch]$OwnerAttended,
    [switch]$AllowBattery,
    [string]$DryRun = "",
    [switch]$DryRunSynthetic,
    [switch]$PrintPlan,
    [int]$GpuQuietWaitMinutes = 30,
    [string]$GpuQuietPath = "D:\codex\GPU-QUIET"
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$tools = Join-Path $here ".."
$helper = Join-Path $here "loop_tier.py"
function Die([string]$m) { [Console]::Error.WriteLine($m); exit 2 }
function Py { & python -I @args 2>&1 | ForEach-Object { "$_" } }

if ($PrintPlan) {
    $a = @($helper, "plan", "--mode", $Mode); if ($Only) { $a += @("--only", $Only) }; if ($OwnerAttended) { $a += "--attended" }
    Py @a; exit $LASTEXITCODE
}

# ---- never inside the production deployment or on the owner's T7 drive -------------------------------
$RunRoot = [System.IO.Path]::GetFullPath($RunRoot)
if ($RunRoot -like "C:\Videoprocessor*" -or $RunRoot -like "E:*") { Die "RunRoot must not be C:\Videoprocessor or on E:" }
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$run = Join-Path $RunRoot $stamp
New-Item -ItemType Directory -Force $run | Out-Null

function Finish([string]$rigState) {
    Py $helper score --run $run --rig-state $rigState | Out-Host
    $rc = $LASTEXITCODE
    "Loop tier: $run\summary.md, $run\loop-tier.trx (exit $rc)"
    exit $rc
}

# ---- offline modes: the whole scoring pipeline without a rig ------------------------------------------
if ($DryRun -or $DryRunSynthetic) {
    if ($DryRun) {
        if (-not (Test-Path $DryRun)) { Die "dry-run folder missing: $DryRun" }
        Copy-Item (Join-Path $DryRun "*") $run -Recurse -Force
    } else {
        $a = @($helper, "synth", "--out", $run, "--mode", $Mode, "--seconds", ($(if ($Seconds -gt 0) { $Seconds } else { 20 })))
        if ($Only) { $a += @("--only", $Only) }
        Py @a | Out-Host
        if ($LASTEXITCODE -ne 0) { exit 2 }
    }
    Finish "ran"
}

# ---- rig detection: no rig = SKIP-NO-RIG, exit 0 -------------------------------------------------------
if (-not $Recorder) { $Recorder = Join-Path $BuildDir "VideoProcessorStreamRecorder.exe" }
$device = 0
if (-not (Test-Path $Recorder)) {
    Finish "SKIP-NO-RIG StreamRecorder not found: $Recorder"
}
$det = Py $helper detect --recorder $Recorder --match $Match
$det | ForEach-Object { $_ }
if ($LASTEXITCODE -ne 0) { Finish ("" + ($det | Select-Object -First 1)) }
if (("" + $det[0]) -match '^RIG (\d+):') { $device = [int]$Matches[1] }

# ---- preconditions --------------------------------------------------------------------------------------
$battery = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
if ($battery -and $battery.BatteryStatus -ne 2 -and -not $AllowBattery) { Die "on battery: the loop tier runs on mains only (-AllowBattery overrides)" }
$drive = (Get-Item $RunRoot).PSDrive
if ($drive.Free -lt 10GB) { Die "less than 10 GB free on $($drive.Name): (tell the coordinator)" }
foreach ($need in @("VideoProcessor.exe")) { if (-not (Test-Path (Join-Path $BuildDir $need))) { Die "$need missing in $BuildDir (build x64 Release first)" } }

$only = @(); if ($Only) { $only = $Only -split "," }
$planArgs = @($helper, "plan", "--mode", $Mode); if ($Only) { $planArgs += @("--only", $Only) }; if ($OwnerAttended) { $planArgs += "--attended" }
$plan = (Py @planArgs) -join "`n" | ConvertFrom-Json
if (-not $plan) { Die "no scenarios selected" }
$modeInfo = (Get-Content (Join-Path $here "loop_tier_plan.json") -Raw | ConvertFrom-Json).modes.$Mode

# ---- the test folder: a copy of the Release outputs, never the deployment --------------------------------
$vpDir = Join-Path $run "vp"
robocopy $BuildDir $vpDir /E /XF *.pdb *.ilk *.iobj *.ipdb *.lib *.exp *.obj /XD "*Test*" /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Die "copy of the build outputs failed (robocopy $LASTEXITCODE)" }
$cfg = Join-Path $here "..\..\VideoProcessor.cfg"
if ((Test-Path $cfg) -and -not (Test-Path (Join-Path $vpDir "VideoProcessor.cfg"))) { Copy-Item $cfg $vpDir }

# ---- GPU-QUIET for the run (wait for the coordinator's marker / PRIORITY first) ---------------------------
$tookQuiet = $false
$deadline = (Get-Date).AddMinutes($GpuQuietWaitMinutes)
while ((Test-Path $GpuQuietPath) -or (Test-Path "D:\codex\build-slots\PRIORITY")) {
    if ((Get-Date) -gt $deadline) { Die "GPU-QUIET/PRIORITY still present after $GpuQuietWaitMinutes min; not running" }
    Start-Sleep -Seconds 60
}
New-Item -ItemType File -Force $GpuQuietPath | Out-Null
$tookQuiet = $true

$saved = @{}
foreach ($k in "VP_DEBUG_OUTPUT_STAMP", "VP_RESYNC_INJECT", "VP_RESYNC_SCHEDULE") { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
$launched = New-Object System.Collections.Generic.List[int]
try {
    foreach ($e in $plan) {
        $id = $e.id
        $dir = Join-Path $run $id
        New-Item -ItemType Directory -Force $dir | Out-Null
        $secs = if ($Seconds -gt 0) { $Seconds } else { [int]$e.seconds }
        "== $id ($secs s)"
        if ($e.attended) { Read-Host "ATTENDED step for $id (Apple TV switch / HDMI re-plug are driven by the schedule prompts). Press Enter when ready" | Out-Null }

        # clip + sent.csv
        $frames = [int][math]::Ceiling($secs * $e.src_fps[0] / $e.src_fps[1]) + 2
        $clipArgs = @((Join-Path $tools "framestamp\make_stamped_clip.py"), "--out", (Join-Path $dir "clip"), "--frames", $frames,
            "--fps", "$($e.src_fps[0])/$($e.src_fps[1])", "--size", "$($modeInfo.size[0])x$($modeInfo.size[1])")
        if ($e.cadence) { $clipArgs += @("--cadence", $e.cadence) }
        Py @clipArgs | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "clip generation failed for $id" }
        $clip = (Get-ChildItem (Join-Path $dir "clip") -Filter *.y4m | Select-Object -First 1).FullName
        Copy-Item (Join-Path $dir "clip\sent.csv") $dir

        # resync schedule + expectations
        Remove-Item Env:VP_RESYNC_SCHEDULE -ErrorAction SilentlyContinue
        $env:VP_DEBUG_OUTPUT_STAMP = "1"
        if ($e.schedule) {
            $sa = @((Join-Path $here "stress_schedule.py"), "--out", (Join-Path $dir "sched"), "--seed", $Seed, "--duration", $secs,
                "--groups", $e.schedule.group, "--inputs", $e.schedule.input)
            if ($e.schedule.manual) { $sa += "--manual-atv" }
            Py @sa | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "stress_schedule failed for $id" }
            $sd = Join-Path $dir ("sched\" + $e.schedule.group + "_" + $e.schedule.input)
            Copy-Item (Join-Path $sd "expected.json") $dir
            $env:VP_RESYNC_INJECT = "1"
            $env:VP_RESYNC_SCHEDULE = (Join-Path $sd "schedule.txt")
        } else { Remove-Item Env:VP_RESYNC_INJECT -ErrorAction SilentlyContinue }

        # VP plays the clip, StreamRecorder captures. Only PIDs started here are stopped.
        Remove-Item (Join-Path $vpDir "logs\vp.log") -ErrorAction SilentlyContinue
        $vp = Start-Process -FilePath (Join-Path $vpDir "VideoProcessor.exe") -WorkingDirectory $vpDir `
            -ArgumentList ($PlayerArgs.Replace("{clip}", $clip)) -PassThru
        $launched.Add($vp.Id)
        try {
            Start-Sleep -Seconds 15   # start-up / first frames before the capture window
            $rec = Start-Process -FilePath $Recorder -WorkingDirectory $dir -PassThru -Wait -NoNewWindow `
                -ArgumentList @("--device", $device, "--stamps", "--seconds", ($secs - 15 + 5), "--out", $dir)
            $recExit = $rec.ExitCode
        } finally {
            if (-not $vp.HasExited) { try { [void]$vp.CloseMainWindow(); Start-Sleep -Seconds 3 } catch {} }
            if (-not $vp.HasExited) { Stop-Process -Id $vp.Id -Force }
            $launched.Remove($vp.Id) | Out-Null
        }
        $log = Join-Path $vpDir "logs\vp.log"
        if (Test-Path $log) { Copy-Item $log (Join-Path $dir "vp.log") }
        $meta = @{ scenario = $(if ($e.scenario) { $e.scenario } else { $id }); width = $modeInfo.size[0]; height = $modeInfo.size[1]; synthetic = $false; recorder_exit = $recExit }
        if ($recExit -ne 0) { $meta.recorder_error = "StreamRecorder exit $recExit" }
        $meta | ConvertTo-Json | Set-Content (Join-Path $dir "meta.json") -Encoding utf8
    }
}
finally {
    foreach ($id in @($launched)) { try { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue } catch {} }   # only PIDs we launched
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    if ($tookQuiet) { Remove-Item $GpuQuietPath -Force -ErrorAction SilentlyContinue }
}
Finish "ran"
