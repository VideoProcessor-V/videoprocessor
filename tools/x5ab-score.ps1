# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
Scores the X5 cut A/B (FIX-X5-CUT-P4; owner-intent fix 2026-11-15: false cuts are EQUALLY unacceptable, not only misses).
  x5ab-score.ps1 [-Out D:\testclips\x5ab] [-Before before] [-After after] [-Truth <Out>\x5ab-truth.txt] [-Fps 24]
                 [-BeforeTolerance 3] [-AfterTolerance 0]
Reads <Out>\<Before>.csv and <Out>\<After>.csv (written by X5AbCaptureTests with VP_X5AB_SIDE = the side name) and the hand-marked
truth list (tools/x5ab-extract.ps1 writes its template). Writes the table to stdout and to <Out>\x5ab-score.md.

Per clip and per side: frames, minutes, truth cuts, detected cuts, missed cuts (frames), false cuts (total, per minute, frames),
recall and precision against the truth list, grain-term snaps (every row with cut=1 is one snap: count and frames, including the
definitional first-frame snap), Unavailable and unasked frames.
- A row with cut_state 'first' (no previous advanced frame: a cut by definition) is a snap but is never scored as a true or false cut.
- A detected cut matches the nearest unmatched truth frame within the side's tolerance (each truth cut matches once). The before
  side's cut is re-derived from Stats.meanLuma, which is read back a few frames late, hence its tolerance; the after side's verdict
  is the frame's own, so its tolerance is 0. Every match offset is printed.
Pass rule (after side only): zero false cuts on the pan, flash and fade clips, no missed truth cut on the cut clip, and no
Unavailable or unasked frame on any clip (otherwise "no false cut" could be vacuous). An UNMARKED truth line fails the run.
A false cut on ANY clip of the after side is a defect of the SHARED U7 law (CutFadeJudge::ClassifyEvents): report it to U7-ACCURACY. It is NEVER fixed
with an x5dup-local threshold or veto (one copy, OD 11-12 17:21; no private detector).
These 5-second clips are a per-clip A/B, not the U7-ACCURACY recall/precision >= 99.5 % gate (that needs that lane's corpus).
Exit code: 0 pass, 1 fail, 2 bad input.
#>
param(
    [string]$Out = 'D:\testclips\x5ab',
    [string]$Before = 'before',
    [string]$After = 'after',
    [string]$Truth = '',
    [double]$Fps = 24.0,
    [int]$BeforeTolerance = 3,
    [int]$AfterTolerance = 0
)
$ErrorActionPreference = 'Stop'
if (-not $Truth) { $Truth = Join-Path $Out 'x5ab-truth.txt' }

function Fail-Input([string]$why) { Write-Error $why -ErrorAction Continue; exit 2 }

function Read-Side([string]$side) {
  $path = Join-Path $Out "$side.csv"
  if (-not (Test-Path $path)) { Fail-Input "missing $path" }
  $cases = @{}
  $header = $null
  foreach ($line in Get-Content $path) {
    if (-not $line) { continue }
    if ($line.StartsWith('case,side,')) { $header = $line -split ','; continue }
    if ($null -eq $header) { Fail-Input "$path has rows before its header" }
    $f = $line -split ','
    $row = @{}
    for ($k = 0; $k -lt $header.Count -and $k -lt $f.Count; ++$k) { $row[$header[$k]] = $f[$k] }
    if (-not $row.ContainsKey('cut_state')) { Fail-Input "$path has no cut_state column: re-run with the current X5AbCaptureTests.inl on this side" }
    if ($row['side'] -ne $side) { Fail-Input "$path holds a row of side '$($row['side'])'" }
    $c = $row['case']
    if (-not $cases.ContainsKey($c)) { $cases[$c] = @{} }
    $frame = [int]$row['frame']
    if ($cases[$c].ContainsKey($frame)) { Fail-Input "$path has frame $frame of '$c' twice (the harness appends: clear the CSV before a run)" }
    $cases[$c][$frame] = $row
  }
  return $cases
}

function Read-Truth {
  if (-not (Test-Path $Truth)) { Fail-Input "missing truth list $Truth (tools/x5ab-extract.ps1 writes a template)" }
  $t = @{}
  foreach ($line in Get-Content $Truth) {
    if (-not $line -or $line[0] -eq '#') { continue }
    $f = $line -split '\|'
    if ($f.Count -lt 4) { Fail-Input "truth line needs case|kind|frames|status: $line" }
    $frames = @()
    if ($f[2].Trim()) { $frames = @($f[2] -split ',' | ForEach-Object { [int]$_.Trim() } | Sort-Object) }
    $t[$f[0].Trim()] = @{ kind = $f[1].Trim(); frames = $frames; status = $f[3].Trim() }
  }
  return $t
}

function Score([hashtable]$rows, [int[]]$truthFrames, [int]$tol) {
  $frames = @($rows.Keys | Sort-Object)
  $snaps = @(); $detected = @(); $unavail = 0; $unasked = 0
  foreach ($fr in $frames) {
    $r = $rows[$fr]
    if ($r['cut_state'] -eq 'unavailable') { ++$unavail }
    if ($r['cut_state'] -eq 'unasked') { ++$unasked }
    if ($r['cut'] -eq '1') {
      $snaps += $fr
      if ($r['cut_state'] -ne 'first') { $detected += $fr }
    }
  }
  $matchedTruth = @{}; $hits = @(); $falseCuts = @()
  foreach ($d in $detected) {
    $best = $null
    foreach ($t in $truthFrames) {
      if ($matchedTruth.ContainsKey($t)) { continue }
      $off = $d - $t
      if ([math]::Abs($off) -le $tol -and ($null -eq $best -or [math]::Abs($off) -lt [math]::Abs($best - $d))) { $best = $t }
    }
    if ($null -eq $best) { $falseCuts += $d } else { $matchedTruth[$best] = $true; $hits += ("{0}{1:+0;-0;+0}" -f $best, ($d - $best)) }
  }
  $missed = @($truthFrames | Where-Object { -not $matchedTruth.ContainsKey($_) })
  $minutes = $frames.Count / $Fps / 60.0
  $tp = $matchedTruth.Count
  return [pscustomobject]@{
    frames = $frames.Count; minutes = $minutes; truth = $truthFrames.Count; detected = $detected.Count; matched = $tp
    hits = $hits; missed = $missed; falseCuts = $falseCuts
    falsePerMin = if ($minutes -gt 0) { $falseCuts.Count / $minutes } else { [double]::NaN }
    recall = if ($truthFrames.Count -gt 0) { $tp / $truthFrames.Count } else { $null }
    precision = if ($detected.Count -gt 0) { $tp / $detected.Count } else { $null }
    snaps = $snaps; unavailable = $unavail; unasked = $unasked
  }
}

function Pct($v) { if ($null -eq $v) { 'n/a' } else { '{0:0.0} %' -f (100.0 * $v) } }
function Lst($a) { if ($a.Count -eq 0) { '-' } else { $a -join ' ' } }

$truthMap = Read-Truth
$sides = [ordered]@{ $Before = (Read-Side $Before); $After = (Read-Side $After) }
$tols = @{ $Before = $BeforeTolerance; $After = $AfterTolerance }
$allCases = @($sides.Values | ForEach-Object { $_.Keys } | Sort-Object -Unique)
$md = @()
$md += "# X5 cut A/B score ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
$md += ''
$md += "CSVs: $(Join-Path $Out "$Before.csv"), $(Join-Path $Out "$After.csv"). Truth: $Truth. Fps $Fps. Tolerance: $Before $BeforeTolerance, $After $AfterTolerance frames."
$md += ''
$md += '| clip | kind | side | frames | truth | detected | missed (frames) | false (frames) | false/min | recall | precision | snaps (frames) | matches | unavail | unasked | verdict |'
$md += '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
$fail = @(); $u7 = @()
foreach ($c in $allCases) {
  if (-not $truthMap.ContainsKey($c)) { $fail += "$c has no truth line"; continue }
  $t = $truthMap[$c]
  if ($t.status -eq 'UNMARKED') { $fail += "$c truth is UNMARKED (hand-mark it first)" }
  foreach ($side in $sides.Keys) {
    if (-not $sides[$side].ContainsKey($c)) { $fail += "$c has no rows on side $side"; continue }
    $s = Score $sides[$side][$c] $t.frames $tols[$side]
    $verdict = 'report'
    if ($side -eq $After) {
      # Only the after side runs the shared U7 law (the before side is the retired private mean-step rule).
      if ($s.falseCuts.Count -gt 0) { $u7 += "$c ($side): false cut(s) at frame(s) $(Lst $s.falseCuts)" }
      $why = @()
      if (@('pan', 'flash', 'fade') -contains $t.kind -and $s.falseCuts.Count -gt 0) { $why += 'false cut' }
      if ($t.kind -eq 'cut' -and $s.missed.Count -gt 0) { $why += 'missed truth cut' }
      if ($s.unavailable -gt 0 -or $s.unasked -gt 0) { $why += 'Unavailable/unasked frames' }
      if ($t.kind -eq 'other') { $verdict = 'n/a (not in the pass set)' }
      elseif ($why.Count -gt 0) { $verdict = 'FAIL: ' + ($why -join ', '); $fail += "$c (after): $($why -join ', ')" }
      else { $verdict = 'pass' }
    }
    $md += ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} ({7}) | {8} ({9}) | {10:0.00} | {11} | {12} | {13} ({14}) | {15} | {16} | {17} | {18} |' -f `
      $c, $t.kind, $side, $s.frames, $s.truth, $s.detected, $s.missed.Count, (Lst $s.missed), $s.falseCuts.Count, (Lst $s.falseCuts), $s.falsePerMin,
      (Pct $s.recall), (Pct $s.precision), $s.snaps.Count, (Lst $s.snaps), (Lst $s.hits), $s.unavailable, $s.unasked, $verdict)
  }
}
$md += ''
if ($u7.Count -gt 0) {
  $md += 'False cuts to report to U7-ACCURACY (shared law; never an x5dup-local threshold or veto):'
  $md += @($u7 | ForEach-Object { "- $_" })
  $md += ''
}
$md += if ($fail.Count -eq 0) { 'RESULT: PASS' } else { 'RESULT: FAIL' }
$md += @($fail | ForEach-Object { "- $_" })
$md | ForEach-Object { Write-Host $_ }
Set-Content -Path (Join-Path $Out 'x5ab-score.md') -Value $md -Encoding ascii
if ($fail.Count -eq 0) { exit 0 } else { exit 1 }
