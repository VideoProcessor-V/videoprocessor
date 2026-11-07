# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
.SYNOPSIS
  Background runner for the source-identity evaluation (doc 78 section 9): vstest on SourceIdChainEvalTests, then the scorer.

.DESCRIPTION
  CPU only. Keeps the whole run within 6 CPU threads: one vstest process (no /Parallel), the ffmpeg child at
  VP_SOURCE_ID_REELS_FFMPEG_THREADS (default 3), and the process affinity of the vstest tree limited to 6 logical processors.
  Refuses to start while D:\codex\build-slots\PRIORITY or D:\codex\GPU-QUIET exists or another test slot (D:\codex\test-slots\t*)
  is held; takes one test slot for the run and releases it in a finally block. Checks that D: keeps >= 40 GB free before
  starting. Writes <Out>\run-summary.txt and exits. Nothing is built here: the test DLL must already exist for an AUDITED tip.

  Order of use (owner rules): audit CLEAN for the exact committed tip -> build (train/package gate) -> this script.
  The full reel set is a TRAIN-time run; for package work pass -Only with a few reel ids.

.PARAMETER Reels     reels.json written by make_identity_reels.py (default D:\testclips\sourceid\reels.json)
.PARAMETER Out       output folder (evaluations.csv, events.csv, reels_done.csv, summary.md, gates.json, run-summary.txt, trx)
.PARAMETER Only      comma list of reel-id substrings (VP_SOURCE_ID_REELS_ONLY)
.PARAMETER TestDll   VideoProcessor-Test.dll (default <repo>\x64\Release\VideoProcessor-Test.dll)
.PARAMETER FfmpegThreads  threads for the ffmpeg child (default 3)
.PARAMETER AffinityMask   logical-processor mask for the vstest tree (default 0x3F = 6 threads; 0 = leave alone)
.PARAMETER WaitMinutes    wait this long for a free slot / cleared markers (default 0 = fail at once)
.PARAMETER Detach    relaunch hidden in the background, write <Out>\runner.pid and return at once
.EXAMPLE
  powershell -File tools\eval\source_id\run_identity_eval.ps1 -Out D:\testclips\sourceid\eval\run1 -Detach
#>
param(
    [string]$Reels = 'D:\testclips\sourceid\reels.json',
    [string]$Out = '',
    [string]$Only = '',
    [string]$TestDll = '',
    [int]$FfmpegThreads = 3,
    [int]$AffinityMask = 0x3F,
    [int]$WaitMinutes = 0,
    [switch]$Detach,
    [switch]$NoScore
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Out) { $Out = Join-Path (Split-Path $Reels -Parent) ('eval\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
if (-not $TestDll) { $TestDll = Join-Path $repo 'x64\Release\VideoProcessor-Test.dll' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$summary = Join-Path $Out 'run-summary.txt'

if ($Detach) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath, '-Reels', $Reels, '-Out', $Out,
                 '-FfmpegThreads', $FfmpegThreads, '-AffinityMask', $AffinityMask, '-WaitMinutes', $WaitMinutes)
    if ($Only) { $argList += @('-Only', $Only) }
    if ($TestDll) { $argList += @('-TestDll', $TestDll) }
    if ($NoScore) { $argList += '-NoScore' }
    $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -WindowStyle Hidden -PassThru
    Set-Content -Path (Join-Path $Out 'runner.pid') -Value $p.Id
    "started background runner PID $($p.Id); summary will be $summary"
    return
}

function Log([string]$m) { $line = ('{0} {1}' -f (Get-Date -Format 'HH:mm:ss'), $m); Add-Content -Path $summary -Value $line; $line }
Set-Content -Path $summary -Value ("source identity evaluation run, started " + (Get-Date -Format 's'))

function Fail([string]$m, [int]$code) { Log ('FAILED: ' + $m); Log 'state: failed'; exit $code }

if (-not (Test-Path $Reels)) { Fail "reels.json not found: $Reels (run make_identity_reels.py first)" 2 }
if (-not (Test-Path $TestDll)) { Fail "test DLL not found: $TestDll (build the audited tip first; this script never builds)" 2 }
$vstest = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\TestWindow\vstest.console.exe'
if (-not (Test-Path $vstest)) { Fail "vstest.console.exe not found: $vstest" 2 }

$drive = (Split-Path -Qualifier $Out)
$free = (Get-PSDrive -Name $drive.TrimEnd(':')).Free / 1GB
if ($free -lt 40) { Fail ("{0} has only {1:N1} GB free (< 40 GB floor)" -f $drive, $free) 3 }

# --- markers and slot (checked before the heavy step) ---
$slotRoot = 'D:\codex\test-slots'
New-Item -ItemType Directory -Force -Path $slotRoot | Out-Null
$deadline = (Get-Date).AddMinutes($WaitMinutes)
$slot = $null
while ($true) {
    $blockers = @()
    if (Test-Path 'D:\codex\build-slots\PRIORITY') { $blockers += 'PRIORITY' }
    if (Test-Path 'D:\codex\GPU-QUIET') { $blockers += 'GPU-QUIET' }
    $held = @(Get-ChildItem -Path $slotRoot -Directory -Filter 't*' -ErrorAction SilentlyContinue)
    if ($held.Count -gt 0) { $blockers += ('test slot held: ' + (($held | ForEach-Object { $_.Name }) -join ',')) }
    if ($blockers.Count -eq 0) {
        try { $slot = Join-Path $slotRoot 't1'; New-Item -ItemType Directory -Path $slot -ErrorAction Stop | Out-Null; break }
        catch { $slot = $null }
    }
    if ((Get-Date) -ge $deadline) { Fail ('not started: ' + ($blockers -join '; ')) 4 }
    Start-Sleep -Seconds 60
}
Log "took test slot $slot"

$proc = $null
try {
    $trx = Join-Path $Out 'trx'
    New-Item -ItemType Directory -Force -Path $trx | Out-Null
    $env:VP_SOURCE_ID_REELS = $Reels
    $env:VP_SOURCE_ID_EVAL_OUT = $Out
    $env:VP_SOURCE_ID_REELS_FFMPEG_THREADS = [string]$FfmpegThreads
    $env:OMP_NUM_THREADS = '1'
    if ($Only) { $env:VP_SOURCE_ID_REELS_ONLY = $Only } else { Remove-Item Env:\VP_SOURCE_ID_REELS_ONLY -ErrorAction SilentlyContinue }
    Log "reels=$Reels only='$Only' out=$Out ffmpegThreads=$FfmpegThreads affinity=0x$('{0:X}' -f $AffinityMask)"
    $argv = @($TestDll, '/Tests:SourceIdChainEvalTests', '/Logger:trx', "/ResultsDirectory:$trx")
    $proc = Start-Process -FilePath $vstest -ArgumentList $argv -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $Out 'vstest.out.txt') -RedirectStandardError (Join-Path $Out 'vstest.err.txt')
    try {
        if ($AffinityMask -ne 0) { $proc.ProcessorAffinity = [IntPtr]$AffinityMask }
        $proc.PriorityClass = 'BelowNormal'
    } catch { Log ('could not set affinity / priority: ' + $_.Exception.Message) }
    Log "vstest PID $($proc.Id)"
    $proc.WaitForExit()
    Log "vstest exit code $($proc.ExitCode)"
} finally {
    if ($proc -and -not $proc.HasExited) { try { Stop-Process -Id $proc.Id -Force } catch { } }
    if ($slot -and (Test-Path $slot)) { Remove-Item -Path $slot -Recurse -Force -ErrorAction SilentlyContinue; Log "released $slot" }
}

# --- the TRX counters (vstest prints no summary for the host DLL here) ---
$trxFile = Get-ChildItem -Path $trx -Filter '*.trx' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$counters = ''
if ($trxFile) {
    $text = Get-Content -Path $trxFile.FullName -Raw
    $m = [regex]::Match($text, '<Counters [^>]*/>')
    if ($m.Success) { $counters = $m.Value }
    Log ('trx: ' + $trxFile.FullName)
    Log ('trx counters: ' + $counters)
} else { Log 'no trx file written' }

if (-not $NoScore) {
    $py = (Get-Command python.exe -ErrorAction SilentlyContinue)
    if (-not $py) { Fail 'python.exe not found for the scorer' 5 }
    $score = Join-Path $PSScriptRoot 'score_identity.py'
    $so = & $py.Source -I $score --reels $Reels --eval-dir $Out --out $Out 2>&1
    Log ('scorer: ' + ($so -join ' '))
    $gj = Join-Path $Out 'gates.json'
    if (Test-Path $gj) {
        $g = Get-Content -Path $gj -Raw | ConvertFrom-Json
        Log ('GO table lane ' + $g.gate_lane + ': ' + ($g.counts | ConvertTo-Json -Compress))
        foreach ($row in $g.gates) { Log ('  [{0}] {1}: {2}' -f $row.status, $row.field, $row.criterion) }
        Log ('summary.md: ' + (Join-Path $Out 'summary.md'))
    }
}
$ok = ($counters -match 'failed="0"') -and ($counters -match 'executed="[1-9]')
if ($ok) { Log 'state: done (test passed)' } else { Log 'state: done (check the trx counters / vstest.err.txt)' }
