# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
Row-27 Render-Ahead Proof & Rate/Mode Grid Runner wrapper for post-core owner rig sessions.

Modes:
  -Mode hardware   (default) Probes real loop rig; missing hardware logs SKIP-NO-HARDWARE
                   on every cell and exits 3 (never reported as PASS).
  -Mode synthetic  Runs the full resolved grid through the deterministic synthetic loop
                   simulator + end-to-end stamp verifier (+ optional negative controls).
  -Mode plan       Validates and emits the resolved grid plan (plan.json + plan.md).

Safety:
  Refuses to write inside C:\Videoprocessor* or E:* (owner deployment / T7 drive).

Examples:
  tools\looprig\run_row27_loop.ps1 -Mode plan -Profile row27
  tools\looprig\run_row27_loop.ps1 -Mode synthetic -Profile row27 -IncludeNegativeControls
  tools\looprig\run_row27_loop.ps1 -Mode hardware -Profile row27 -BuildDir x64\Release -ClipsDir D:\testclips
#>
param(
    [ValidateSet("hardware", "synthetic", "plan")][string]$Mode = "hardware",
    [ValidateSet("row27", "smoke", "full", "custom")][string]$Profile = "row27",
    [string]$Filter = "",
    [string]$SourceRates = "",
    [string]$DisplayRates = "",
    [string]$RunRoot = "D:\codex\looprig-row27",
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\..\x64\Release"),
    [string]$Recorder = "",
    [string]$DeviceMatch = "DeckLink|Mini Recorder 4K|Quad HDMI",
    [string]$ClipsDir = "",
    [string]$ReplayDir = "",
    [switch]$IncludeNegativeControls,
    [switch]$WriteStamps,
    [switch]$EmitTrx,
    [switch]$EmitJUnit
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$runner = Join-Path $here "grid_runner.py"

$RunRoot = [System.IO.Path]::GetFullPath($RunRoot)
if ($RunRoot -like "C:\Videoprocessor*" -or $RunRoot -like "E:*") {
    [Console]::Error.WriteLine("RunRoot must not be C:\Videoprocessor or on E:")
    exit 2
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outDir = Join-Path $RunRoot ("row27-" + $Mode + "-" + $Profile + "-" + $stamp)
New-Item -ItemType Directory -Force $outDir | Out-Null

$pyArgs = @($runner, "--mode", $Mode, "--profile", $Profile, "--out", $outDir)
if ($Filter) { $pyArgs += @("--filter", $Filter) }
if ($SourceRates) { $pyArgs += @("--source-rates", $SourceRates) }
if ($DisplayRates) { $pyArgs += @("--display-rates", $DisplayRates) }
if ($Mode -eq "hardware") {
    $pyArgs += @("--build-dir", $BuildDir, "--device-match", $DeviceMatch)
    if ($Recorder) { $pyArgs += @("--recorder", $Recorder) }
    if ($ClipsDir) { $pyArgs += @("--clips-dir", $ClipsDir) }
    if ($ReplayDir) { $pyArgs += @("--replay-dir", $ReplayDir) }
}
if ($IncludeNegativeControls) { $pyArgs += "--include-negative-controls" }
if ($WriteStamps) { $pyArgs += "--write-stamps" }
if ($EmitTrx -and $Mode -ne "plan") { $pyArgs += @("--trx", (Join-Path $outDir "row27-loop.trx")) }
if ($EmitJUnit -and $Mode -ne "plan") { $pyArgs += @("--junit", (Join-Path $outDir "row27-loop.junit.xml")) }

& python -I @pyArgs
$rc = $LASTEXITCODE
"Row-27 loop runner output: $outDir (exit $rc)"
exit $rc
