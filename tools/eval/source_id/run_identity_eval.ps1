# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
.SYNOPSIS
  Background runner for the source-identity evaluation (doc 78 section 9): vstest on SourceIdChainEvalTests, then the scorer.

.DESCRIPTION
  CPU only. Keeps the whole run within 4 CPU threads: one vstest process (no /Parallel), the ffmpeg child at
  VP_SOURCE_ID_REELS_FFMPEG_THREADS (default 2 per input), BelowNormal priority and the affinity of the vstest tree limited to
  4 logical processors (the testhost and its ffmpeg children are spawned after the limit is set and inherit it; the runner also
  reasserts it on its own descendants every 30 s).
  Refuses to start while D:\codex\build-slots\PRIORITY exists or all of the test slots t1-t3 (D:\codex\test-slots) are held; takes the
  first free slot and releases it in a finally block; a GPU-QUIET marker is reported but does not block (this run is CPU only). The
  harness itself checks, between reels, <Out>\STOP (clean end), PRIORITY (pauses while it exists: integration builds go first) and the
  40 GB floor of the output drive. Re-running with the same -Out RESUMES: completed reels are skipped.

  Only the PIDs this runner started are ever stopped (the vstest tree, found through ParentProcessId), never by name. To end a long
  run early: create <Out>\STOP (the run ends after the current reel), or use -TimeoutHours. A hard-killed runner leaves its slot
  directory behind: run-summary.txt names it, remove it by hand.

  Nothing is built here: the test DLL must already exist for an AUDITED tip. The full reel set is a TRAIN-time run; for package work
  pass -Only with a few reel ids.

.PARAMETER Reels     reels.json written by make_identity_reels.py (default D:\testclips\sourceid\reels.json)
.PARAMETER Out       output folder (reels\<reel>\ file sets, reels_done.csv, summary.md, gates.json, run-summary.txt, trx)
.PARAMETER Only      comma list of reel-id substrings (VP_SOURCE_ID_REELS_ONLY)
.PARAMETER TestDll   VideoProcessor-Test.dll (default <repo>\x64\Release\VideoProcessor-Test.dll)
.PARAMETER FfmpegThreads  decoder threads per input for the ffmpeg child (default 2)
.PARAMETER AffinityMask   logical-processor mask for the vstest tree (default 0xF = 4 threads; 0 = leave alone)
.PARAMETER WaitMinutes    wait this long for a free slot / cleared PRIORITY (default 0 = fail at once)
.PARAMETER TimeoutHours   0 = none; otherwise create <Out>\STOP after this long and stop the run's own processes after 15 more minutes
.PARAMETER Detach    relaunch hidden in the background, write <Out>\runner.pid and return at once
.EXAMPLE
  powershell -File tools\eval\source_id\run_identity_eval.ps1 -Out D:\testclips\sourceid\eval\run1 -Detach
#>
param(
    [string]$Reels = 'D:\testclips\sourceid\reels.json',
    [string]$Out = '',
    [string]$Only = '',
    [string]$TestDll = '',
    [int]$FfmpegThreads = 2,
    [int]$AffinityMask = 0xF,
    [int]$WaitMinutes = 0,
    [double]$TimeoutHours = 0,
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
                 '-FfmpegThreads', $FfmpegThreads, '-AffinityMask', $AffinityMask, '-WaitMinutes', $WaitMinutes, '-TimeoutHours', $TimeoutHours)
    if ($Only) { $argList += @('-Only', $Only) }
    if ($TestDll) { $argList += @('-TestDll', $TestDll) }
    if ($NoScore) { $argList += '-NoScore' }
    $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -WindowStyle Hidden -PassThru
    Set-Content -Path (Join-Path $Out 'runner.pid') -Value $p.Id
    "started background runner PID $($p.Id); summary will be $summary"
    return
}

function Log([string]$m) { $line = ('{0} {1}' -f (Get-Date -Format 'HH:mm:ss'), $m); Add-Content -Path $summary -Value $line; $line }
Add-Content -Path $summary -Value ("source identity evaluation run, started " + (Get-Date -Format 's'))

function Fail([string]$m, [int]$code) { Log ('FAILED: ' + $m); Log 'state: failed'; exit $code }

# the processes below a PID, found through ParentProcessId (this run's own children only)
function Get-Descendants([int]$rootPid) {
    $all = @(Get-CimInstance Win32_Process)
    $found = New-Object System.Collections.ArrayList
    $queue = New-Object System.Collections.Queue
    $queue.Enqueue($rootPid)
    while ($queue.Count -gt 0) {
        $parent = $queue.Dequeue()
        foreach ($c in ($all | Where-Object { $_.ParentProcessId -eq $parent })) {
            [void]$found.Add($c)
            $queue.Enqueue([int]$c.ProcessId)
        }
    }
    return $found
}

if (-not (Test-Path $Reels)) { Fail "reels.json not found: $Reels (run make_identity_reels.py first)" 2 }
if (-not (Test-Path $TestDll)) { Fail "test DLL not found: $TestDll (build the audited tip first; this script never builds)" 2 }
$vstest = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\TestWindow\vstest.console.exe'
if (-not (Test-Path $vstest)) { Fail "vstest.console.exe not found: $vstest" 2 }

$drive = (Split-Path -Qualifier $Out)
$free = (Get-PSDrive -Name $drive.TrimEnd(':')).Free / 1GB
if ($free -lt 40) { Fail ("{0} has only {1:N1} GB free (< 40 GB floor)" -f $drive, $free) 3 }

# --- marker and slot (checked before the heavy step; the harness re-checks PRIORITY between reels) ---
$slotRoot = 'D:\codex\test-slots'
New-Item -ItemType Directory -Force -Path $slotRoot | Out-Null
$deadline = (Get-Date).AddMinutes($WaitMinutes)
$slot = $null
while ($true) {
    $blockers = @()
    if (Test-Path 'D:\codex\build-slots\PRIORITY') { $blockers += 'PRIORITY' }
    if ($blockers.Count -eq 0) {
        foreach ($name in @('t1', 't2', 't3')) {
            try { $cand = Join-Path $slotRoot $name; New-Item -ItemType Directory -Path $cand -ErrorAction Stop | Out-Null; $slot = $cand; break }
            catch { }
        }
        if ($slot) { break }
        $blockers += 'all test slots t1-t3 held'
    }
    if ((Get-Date) -ge $deadline) { Fail ('not started: ' + ($blockers -join '; ')) 4 }
    Start-Sleep -Seconds 60
}
Log "took test slot $slot (release by hand if this runner is killed hard)"
if (Test-Path 'D:\codex\GPU-QUIET') { Log 'note: GPU-QUIET marker present (CPU-only run, continuing)' }

$proc = $null
$timedOut = $false
$trx = Join-Path $Out 'trx'
try {
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
    $started = Get-Date
    while (-not $proc.WaitForExit(30000)) {
        # children spawned after launch inherit the limits; reassert them on the descendants (own tree only)
        foreach ($d in (Get-Descendants $proc.Id)) {
            try {
                $dp = Get-Process -Id ([int]$d.ProcessId) -ErrorAction Stop
                if ($AffinityMask -ne 0 -and [int64]$dp.ProcessorAffinity -ne [int64]$AffinityMask) { $dp.ProcessorAffinity = [IntPtr]$AffinityMask }
                if ($dp.PriorityClass -ne 'BelowNormal') { $dp.PriorityClass = 'BelowNormal' }
            } catch { }
        }
        if ($TimeoutHours -gt 0 -and ((Get-Date) - $started).TotalHours -ge $TimeoutHours) {
            if (-not $timedOut) { $timedOut = $true; Set-Content -Path (Join-Path $Out 'STOP') -Value 'runner timeout'; Log 'timeout: STOP file written, the run ends after the current reel' }
            elseif (((Get-Date) - $started).TotalHours -ge ($TimeoutHours + 0.25)) { Log 'timeout + 15 min: stopping the own processes of this run'; break }
        }
    }
    if ($proc.HasExited) { Log "vstest exit code $($proc.ExitCode)" }
} finally {
    if ($proc) {
        # only the PIDs of this run: descendants first, then the vstest process itself
        foreach ($d in (Get-Descendants $proc.Id)) { try { Stop-Process -Id ([int]$d.ProcessId) -Force -ErrorAction Stop; Log ("stopped own child PID {0} ({1})" -f $d.ProcessId, $d.Name) } catch { } }
        if (-not $proc.HasExited) { try { Stop-Process -Id $proc.Id -Force } catch { } }
    }
    # a STOP left by this run (timeout, or the run was cut before a reel ended) is consumed so a rerun of the same -Out runs
    $stopFile = Join-Path $Out 'STOP'
    if (Test-Path $stopFile) { try { Move-Item -Path $stopFile -Destination ($stopFile + '.done.' + (Get-Date -Format 'yyyyMMdd-HHmmss')) -Force; Log 'STOP consumed' } catch { } }
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

$idx = Join-Path $Out 'reels_done.csv'
if (Test-Path $idx) {
    $rows = @(Import-Csv $idx)
    $bad = @($rows | Where-Object { $_.state -ne 'ok' })
    Log ("reels finished: {0} (ok {1}, not ok {2})" -f $rows.Count, ($rows.Count - $bad.Count), $bad.Count)
    foreach ($b in ($bad | Select-Object -First 10)) { Log ("  not ok: {0} {1} ({2}/{3} frames)" -f $b.reel, $b.state, $b.frames_decoded, $b.frames_expected) }
}

if (-not $NoScore) {
    $py = (Get-Command python.exe -ErrorAction SilentlyContinue)
    if (-not $py) { Fail 'python.exe not found for the scorer' 5 }
    $score = Join-Path $PSScriptRoot 'score_identity.py'
    $scoreLog = Join-Path $Out 'score.log'
    & $py.Source -I $score --reels $Reels --eval-dir $Out --out $Out *> $scoreLog
    Log ('scorer: ' + ((Get-Content $scoreLog -ErrorAction SilentlyContinue) -join ' '))
    $gj = Join-Path $Out 'gates.json'
    if (Test-Path $gj) {
        $g = Get-Content -Path $gj -Raw | ConvertFrom-Json
        Log ('GO table lane ' + $g.gate_lane + ': ' + ($g.counts | ConvertTo-Json -Compress))
        foreach ($row in $g.gates) { Log ('  [{0}] {1}: {2}' -f $row.status, $row.field, $row.criterion) }
        Log ('summary.md: ' + (Join-Path $Out 'summary.md'))
    }
}
$ok = ($counters -match 'failed="0"') -and ($counters -match 'executed="[1-9]')
if ($ok) { Log 'state: done (test passed)' } else { Log 'state: done (check the trx counters / vstest.err.txt / score.log)' }
