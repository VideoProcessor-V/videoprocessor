# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
Scores the X5 cut A/B (FIX-X5-CUT-P4; owner-intent fix 2026-11-15: false cuts are EQUALLY unacceptable, not only misses; C5 audit
x5dup MUST 2: no vacuous pass).
  x5ab-score.ps1 [-Out D:\testclips\x5ab] [-Before before] [-After after] [-Truth <Out>\x5ab-truth.txt] [-List <Out>\x5ab-list.txt]
                 [-Fps 24] [-BeforeTolerance 3] [-AfterDeliveryOffset 1] [-AfterTolerance 0]
Reads <Out>\<Before>.csv and <Out>\<After>.csv (written by X5AbCaptureTests with VP_X5AB_SIDE = the side name), the list file the
clips were made from (tools/x5ab-extract.ps1) and the hand-marked truth list. Writes the table to stdout and to <Out>\x5ab-score.md.

Per clip and per side: frames, minutes, truth cuts, detected cuts, missed cuts (frames), false cuts (total, per minute, frames),
recall and precision against the truth list, grain-term snaps (every row with cut=1 is one snap: count and frames, including the
definitional first-frame snap), Unavailable and unasked frames.
- A row with cut_state 'first' (no previous advanced frame: a cut by definition) is a snap but is never scored as a true or false cut.
- After side (C5: the skeleton SCENE PRODUCT, cut_rule u7_scene): the snap is the DELIVERED cut, which goes out with the group of the
  frame after the cut (cut_late = 1). The scored frame is the scene product's own cut frame: the snap frame minus -AfterDeliveryOffset
  (1: the harness seals contiguous frames with no look-ahead, so a late delivery is exactly one frame late) when cut_late = 1, the
  snap frame itself when cut_late = 0. That frame must carry the product's own confirmed verdict (cut_here = 1, confirmed = 1), and
  every confirmed cut_here = 1 frame must have been delivered (a snap at it or at it + the offset): either mismatch fails. It is then
  matched against the truth with -AfterTolerance (default 0: the product's frame is the cut's first frame).
- Before side (the retired private mean-step rule, re-derived from Stats.meanLuma, which is read back a few frames late): its own
  snap frames, matched within -BeforeTolerance. Reported only, never a pass criterion.
- A detected cut matches the nearest unmatched truth frame within tolerance (each truth cut matches once). Every match offset is printed.
Run fails (no vacuous pass): an empty CSV; a case of the list, the truth or either CSV missing from any of the others; a kind other
than cut / flash / pan / fade / other; any of the kinds cut, flash, pan and fade absent from the truth list (and so from the run);
a case whose rows on either side are not exactly frames 0 .. N-1 of its list line; a truth frame outside 0 .. N-1; a 'cut' line
with no truth frame; an UNMARKED truth line; an after row whose cut_rule is not u7_scene or whose cut_state is not first / known
(unavailable and unasked are counted and fail); a before row whose cut_rule is not meanstep20_derived or cut_state not derived.
Pass rule (after side, EVERY clip, every kind): zero false cuts, zero missed truth cuts, recall and precision >= 99.5 % where defined
(OD 2026-11-12 :771), no Unavailable or unasked frame, the delivery consistency above.
A false cut on the after side is a defect of the SHARED U7 law (CutFadeJudge::CutTracker, the skeleton's scene product): report it to
U7-ACCURACY. It is NEVER fixed with an x5dup-local threshold or veto (one copy, OD 11-12 17:21; no private detector).
These 5-second clips are a per-clip A/B, not the U7-ACCURACY recall/precision >= 99.5 % corpus gate (that needs that lane's corpus).
Exit code: 0 pass, 1 fail, 2 bad input.
#>
param(
    [string]$Out = 'D:\testclips\x5ab',
    [string]$Before = 'before',
    [string]$After = 'after',
    [string]$Truth = '',
    [string]$List = '',
    [double]$Fps = 24.0,
    [int]$BeforeTolerance = 3,
    [int]$AfterDeliveryOffset = 1,
    [int]$AfterTolerance = 0
)
$ErrorActionPreference = 'Stop'
if (-not $Truth) { $Truth = Join-Path $Out 'x5ab-truth.txt' }
if (-not $List) { $List = Join-Path $Out 'x5ab-list.txt' }
$kinds = @('cut', 'flash', 'pan', 'fade', 'other')
$requiredKinds = @('cut', 'flash', 'pan', 'fade')
$minRate = 0.995

function Fail-Input([string]$why) { Write-Error $why -ErrorAction Continue; exit 2 }

function Read-Side([string]$side) {
  $path = Join-Path $Out "$side.csv"
  if (-not (Test-Path $path)) { Fail-Input "missing $path" }
  $cases = @{}
  $header = $null
  $n = 0
  foreach ($line in Get-Content $path) {
    if (-not $line) { continue }
    if ($line.StartsWith('case,side,')) { $header = $line -split ','; continue }
    if ($null -eq $header) { Fail-Input "$path has rows before its header" }
    $f = $line -split ','
    if ($f.Count -ne $header.Count) { Fail-Input "$path has a row with $($f.Count) fields, header $($header.Count): $line" }
    $row = @{}
    for ($k = 0; $k -lt $header.Count; ++$k) { $row[$header[$k]] = $f[$k] }
    foreach ($col in @('cut_state', 'cut_here', 'confirmed', 'cut_late', 'cut_rule')) {
      if (-not $row.ContainsKey($col)) { Fail-Input "$path has no $col column: re-run with the current X5AbCaptureTests.inl on this side" }
    }
    if ($row['side'] -ne $side) { Fail-Input "$path holds a row of side '$($row['side'])'" }
    $c = $row['case']
    if (-not $cases.ContainsKey($c)) { $cases[$c] = @{} }
    $frame = [int]$row['frame']
    if ($cases[$c].ContainsKey($frame)) { Fail-Input "$path has frame $frame of '$c' twice (the harness appends: clear the CSV before a run)" }
    $cases[$c][$frame] = $row
    ++$n
  }
  if ($n -eq 0) { Fail-Input "$path has no data rows (an empty run is never a pass)" }
  return $cases
}

function Read-Truth {
  if (-not (Test-Path $Truth)) { Fail-Input "missing truth list $Truth (tools/x5ab-extract.ps1 writes a template)" }
  $t = @{}
  foreach ($line in Get-Content $Truth) {
    if (-not $line -or $line[0] -eq '#') { continue }
    $f = $line -split '\|'
    if ($f.Count -lt 4) { Fail-Input "truth line needs case|kind|frames|status: $line" }
    $name = $f[0].Trim()
    if ($t.ContainsKey($name)) { Fail-Input "truth list has '$name' twice" }
    $frames = @()
    if ($f[2].Trim()) { $frames = @($f[2] -split ',' | ForEach-Object { [int]$_.Trim() } | Sort-Object -Unique) }
    $t[$name] = @{ kind = $f[1].Trim(); frames = $frames; status = $f[3].Trim() }
  }
  if ($t.Count -eq 0) { Fail-Input "truth list $Truth has no case lines" }
  return $t
}

function Read-List {
  if (-not (Test-Path $List)) { Fail-Input "missing list file $List (the frame counts come from it)" }
  $l = @{}
  foreach ($line in Get-Content $List) {
    if (-not $line -or $line[0] -eq '#') { continue }
    $f = $line -split '\|'
    if ($f.Count -lt 9) { Fail-Input "list line needs 9 fields: $line" }
    $name = $f[0].Trim()
    if ($l.ContainsKey($name)) { Fail-Input "list file has '$name' twice" }
    $l[$name] = [int]$f[8].Trim()
  }
  if ($l.Count -eq 0) { Fail-Input "list file $List has no case lines" }
  return $l
}

# Detected cuts of one side: (scored frame, snap frame) pairs, plus structural faults of the side's rows.
function Detect([hashtable]$rows, [bool]$isAfter) {
  $frames = @($rows.Keys | Sort-Object)
  $snaps = @(); $detected = @(); $faults = @(); $unavail = 0; $unasked = 0
  foreach ($fr in $frames) {
    $r = $rows[$fr]
    if ($isAfter) {
      if ($r['cut_rule'] -ne 'u7_scene') { $faults += "frame $fr cut_rule '$($r['cut_rule'])' is not the scene-product rule u7_scene" }
      switch ($r['cut_state']) {
        'first' { }
        'known' { }
        'unavailable' { ++$unavail }
        'unasked' { ++$unasked }
        default { $faults += "frame $fr cut_state '$($r['cut_state'])' is not first / known" }
      }
      foreach ($col in @('cut_here', 'confirmed', 'cut_late')) {
        if (@('0', '1') -notcontains $r[$col]) { $faults += "frame $fr $col '$($r[$col])' is not 0 / 1" }
      }
    } else {
      if ($r['cut_rule'] -ne 'meanstep20_derived') { $faults += "frame $fr cut_rule '$($r['cut_rule'])' is not meanstep20_derived" }
      if ($r['cut_state'] -ne 'derived') { $faults += "frame $fr cut_state '$($r['cut_state'])' is not derived" }
    }
    if ($r['cut'] -eq '1') {
      $snaps += $fr
      if ($r['cut_state'] -ne 'first') {
        $at = $fr
        if ($isAfter -and $r['cut_late'] -eq '1') { $at = $fr - $AfterDeliveryOffset }
        $detected += $at
        if ($isAfter) {
          $own = $rows[$at]
          if ($null -eq $own -or $own['cut_here'] -ne '1' -or $own['confirmed'] -ne '1') {
            $faults += "snap at frame $fr delivers a cut at frame $at that has no confirmed cut_here there"
          }
        }
      }
    }
  }
  if ($isAfter) {
    foreach ($fr in $frames) {
      $r = $rows[$fr]
      if ($r['cut_here'] -eq '1' -and $r['confirmed'] -eq '1' -and $r['cut_state'] -ne 'first') {
        $deliveredNow = $r['cut'] -eq '1' -and $r['cut_late'] -eq '0'
        $next = $rows[$fr + $AfterDeliveryOffset]
        $deliveredLate = $null -ne $next -and $next['cut'] -eq '1' -and $next['cut_late'] -eq '1'
        if (-not ($deliveredNow -or $deliveredLate)) { $faults += "confirmed cut at frame $fr was never delivered" }
      }
    }
  }
  return [pscustomobject]@{ frames = $frames; snaps = $snaps; detected = @($detected | Sort-Object); faults = $faults; unavailable = $unavail; unasked = $unasked }
}

function Score($det, [int[]]$truthFrames, [int]$tol) {
  $matchedTruth = @{}; $hits = @(); $falseCuts = @()
  foreach ($d in $det.detected) {
    $best = $null
    foreach ($t in $truthFrames) {
      if ($matchedTruth.ContainsKey($t)) { continue }
      $off = $d - $t
      if ([math]::Abs($off) -le $tol -and ($null -eq $best -or [math]::Abs($off) -lt [math]::Abs($best - $d))) { $best = $t }
    }
    if ($null -eq $best) { $falseCuts += $d } else { $matchedTruth[$best] = $true; $hits += ("{0}{1:+0;-0;+0}" -f $best, ($d - $best)) }
  }
  $missed = @($truthFrames | Where-Object { -not $matchedTruth.ContainsKey($_) })
  $minutes = $det.frames.Count / $Fps / 60.0
  $tp = $matchedTruth.Count
  return [pscustomobject]@{
    frames = $det.frames.Count; minutes = $minutes; truth = $truthFrames.Count; detected = $det.detected.Count; matched = $tp
    hits = $hits; missed = $missed; falseCuts = $falseCuts
    falsePerMin = if ($minutes -gt 0) { $falseCuts.Count / $minutes } else { [double]::NaN }
    recall = if ($truthFrames.Count -gt 0) { $tp / $truthFrames.Count } else { $null }
    precision = if ($det.detected.Count -gt 0) { $tp / $det.detected.Count } else { $null }
    snaps = $det.snaps; unavailable = $det.unavailable; unasked = $det.unasked; faults = $det.faults
  }
}

function Pct($v) { if ($null -eq $v) { 'n/a' } else { '{0:0.0} %' -f (100.0 * $v) } }
function Lst($a) { if ($a.Count -eq 0) { '-' } else { $a -join ' ' } }

$truthMap = Read-Truth
$listMap = Read-List
$sides = [ordered]@{ $Before = (Read-Side $Before); $After = (Read-Side $After) }
$tols = @{ $Before = $BeforeTolerance; $After = $AfterTolerance }
$fail = @(); $u7 = @()

# Coverage: every case of the truth list, the list file and both CSVs is in all four; the four required kinds are present.
$allCases = @(@($truthMap.Keys) + @($listMap.Keys) + @($sides.Values | ForEach-Object { $_.Keys }) | Sort-Object -Unique)
foreach ($c in $allCases) {
  if (-not $truthMap.ContainsKey($c)) { $fail += "$c has no truth line" }
  if (-not $listMap.ContainsKey($c)) { $fail += "$c is not in the list file $List" }
  foreach ($side in $sides.Keys) { if (-not $sides[$side].ContainsKey($c)) { $fail += "$c has no rows on side $side" } }
}
foreach ($c in $truthMap.Keys) {
  $t = $truthMap[$c]
  if ($kinds -notcontains $t.kind) { $fail += "$c has an unknown kind '$($t.kind)' (cut / flash / pan / fade / other)" }
  if ($t.status -eq 'UNMARKED') { $fail += "$c truth is UNMARKED (hand-mark it first)" }
  if ($t.kind -eq 'cut' -and $t.frames.Count -eq 0) { $fail += "$c is kind cut but lists no truth frame (a cut clip must have one)" }
  if ($listMap.ContainsKey($c)) {
    $bad = @($t.frames | Where-Object { $_ -lt 0 -or $_ -ge $listMap[$c] })
    if ($bad.Count -gt 0) { $fail += "$c truth frame(s) $(Lst $bad) outside 0 .. $($listMap[$c] - 1)" }
  }
}
foreach ($k in $requiredKinds) {
  if (-not @($truthMap.Values | Where-Object { $_.kind -eq $k }).Count) { $fail += "no case of kind $k in the truth list (the cut A/B needs cut, flash, pan and fade)" }
}

$md = @()
$md += "# X5 cut A/B score ($(Get-Date -Format 'yyyy-MM-dd HH:mm'))"
$md += ''
$md += "CSVs: $(Join-Path $Out "$Before.csv"), $(Join-Path $Out "$After.csv"). Truth: $Truth. List: $List. Fps $Fps."
$md += "Scored frame: $Before = its snap frame, tolerance $BeforeTolerance; $After = the scene product's cut frame (snap - $AfterDeliveryOffset when delivered late), tolerance $AfterTolerance."
$md += ''
$md += '| clip | kind | side | frames | truth | detected | missed (frames) | false (frames) | false/min | recall | precision | snaps (frames) | matches | unavail | unasked | verdict |'
$md += '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
foreach ($c in $allCases) {
  if (-not $truthMap.ContainsKey($c) -or -not $listMap.ContainsKey($c)) { continue }
  $t = $truthMap[$c]
  $n = $listMap[$c]
  foreach ($side in $sides.Keys) {
    if (-not $sides[$side].ContainsKey($c)) { continue }
    $rows = $sides[$side][$c]
    $keys = @($rows.Keys | Sort-Object)
    $contiguous = $keys.Count -eq $n
    for ($i = 0; $contiguous -and $i -lt $n; ++$i) { if ($keys[$i] -ne $i) { $contiguous = $false } }
    if (-not $contiguous) { $fail += "$c ($side): $($keys.Count) rows, the list says $n frames (rows must be exactly frames 0 .. $($n - 1))" }
    $isAfter = $side -eq $After
    $det = Detect $rows $isAfter
    $s = Score $det $t.frames $tols[$side]
    foreach ($why in $s.faults) { $fail += "$c ($side): $why" }
    $verdict = 'report'
    if ($isAfter) {
      if ($s.falseCuts.Count -gt 0) { $u7 += "$c ($side): false cut(s) at frame(s) $(Lst $s.falseCuts)" }
      $why = @()
      if ($s.falseCuts.Count -gt 0) { $why += 'false cut' }
      if ($s.missed.Count -gt 0) { $why += 'missed truth cut' }
      if ($null -ne $s.precision -and $s.precision -lt $minRate) { $why += 'precision < 99.5 %' }
      if ($null -ne $s.recall -and $s.recall -lt $minRate) { $why += 'recall < 99.5 %' }
      if ($s.unavailable -gt 0 -or $s.unasked -gt 0) { $why += 'Unavailable/unasked frames' }
      if ($s.faults.Count -gt 0 -or -not $contiguous) { $why += 'row faults' }
      if ($why.Count -gt 0) { $verdict = 'FAIL: ' + ($why -join ', '); $fail += "$c (after): $($why -join ', ')" }
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
