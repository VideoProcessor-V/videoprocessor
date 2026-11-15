# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
Makes the raw clips and the list file for X5AbCaptureTests (FIX-X5-CUT-P4 before/after). ffmpeg only; no film pixels are committed.
  x5ab-extract.ps1 [-Out D:\testclips\x5ab] [-Frames 120] [-Set cuts|all]
Raw files are rgb24, 120 frames each, written to <Out>\raw\; the list file <Out>\x5ab-list.txt feeds VP_X5AB_LIST.
Geometry is reduced to keep disk use small (the disk is 97% full): sources 1260x720 (or 1260x528 for 'scope' clips) -> 840x480
(or 840x352), the 1.5x downscale ratio of the design's 1080p -> 720p and 4K -> 1440p cases. The upscale cases (ratio 1 / 2) are not
covered by this harness. The crop case has its crop baked into the pixels (no crop rect given to the renderer).
Synthetic clips (from tos_24p_3min): 'cut' = a hard splice of two scenes at frame 60; 'flash' = a +64-code brightness flash on frames 60-61;
'pan' = one still frame panned left and right (a smooth 0 -> 630 -> 0 source-pixel crop offset, up to about 16 px a frame, no cut);
'fade' = one still frame faded to black over frames 30-59 and back in over frames 60-89 (no cut).
-Set cuts (default) makes the owner's cut A/B set: cut + flash + pan + fade (owner-intent fix 2026-11-15: false cuts are counted).
-Set all adds the six grain/image cases (hdr4k, hdr2k, sdr1080, sdrlin, cropbaked, sdrmeridian). At 120 frames each clip is about 240 MB (1260x528) or 330 MB (1260x720).

Truth list <Out>\x5ab-truth.txt (read by tools/x5ab-score.ps1), one line per case:  case|kind|truth cut frames|status|note
  kind = cut / flash / pan / fade / other (the pass rule: zero false cuts on pan, flash and fade; no missed truth cut on cut).
  truth cut frames = comma-separated clip frame indices of the FIRST frame of each new shot (empty = no cut).
  status = synthetic (no cut by construction: one still frame) or UNMARKED (live footage: a person watches the clip, confirms the
  construction splice and adds any real cut inside it, then sets status to marked:<who>:<date>). The scorer refuses UNMARKED lines.
An existing truth file is NEVER overwritten (it is hand-marked data); new cases are reported so they can be added by hand.
#>
param(
    [string]$Out = 'D:\testclips\x5ab',
    [int]$Frames = 120,
    [ValidateSet('cuts', 'all')][string]$Set = 'cuts',
    [string]$Ffmpeg = 'D:\tools\ffmpeg\bin\ffmpeg.exe'
)
$ErrorActionPreference = 'Stop'
$raw = Join-Path $Out 'raw'
New-Item -ItemType Directory -Force $raw | Out-Null
$tos = 'D:\testclips\variants\tos_24p_3min.mp4'
# One still frame (tos at 30 s) repeated: pan and fade cannot hold a real cut.
$still = 'trim=end_frame=1,loop=loop=-1:size=1:start=0,setpts=N/24/TB'
$items = @(
  @{set='all';  n='hdr4k';     w=1260; h=720; ow=840; oh=480; tr='pq';      li='gamma';  args=@('-ss','60','-i','D:\testclips\meridian_src\Meridian_UHD4k5994_HDR_P3PQ.mp4','-vf','scale=1260:720')},
  @{set='all';  n='hdr2k';     w=1260; h=528; ow=840; oh=352; tr='pq';      li='gamma';  args=@('-ss','60','-i','D:\testclips\cosmos_laundromat_src\CosmosLaundromat_2k24p_HDR_P3PQ.mp4','-vf','scale=1260:528')},
  @{set='all';  n='sdr1080';   w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  args=@('-ss','20','-i',$tos,'-vf','scale=1260:528')},
  @{set='all';  n='sdrlin';    w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='linear'; args=@('-ss','20','-i',$tos,'-vf','scale=1260:528')},
  @{set='all';  n='cropbaked'; w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  args=@('-i','D:\testclips\film\cosmos_1920x800_24p.y4m','-vf','crop=1600:672:160:64,scale=1260:528')},
  @{set='all';  n='sdrmeridian'; w=1260; h=720; ow=840; oh=480; tr='gamma22'; li='gamma'; args=@('-ss','60','-i','D:\testclips\meridian_src\Meridian_UHD4k5994_HDR_P3PQ.mp4','-vf','scale=1260:720')},
  @{set='cuts'; n='flash';     w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  kind='flash'; truth='';   status='UNMARKED'; note='live footage tos 20 s: mark any real cut; the flash itself (60-61) is not a cut';
    args=@('-ss','20','-i',$tos,'-vf',"scale=1260:528,eq=brightness=0.25:enable='between(n,60,61)'")},
  @{set='cuts'; n='cut';       w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  kind='cut';   truth='60'; status='UNMARKED'; note='splice at 60 (tos 20 s | tos 100 s): confirm it and mark any real cut in either half';
    args=@('-ss','20','-t','2.5','-i',$tos,'-ss','100','-t','2.5','-i',$tos,'-filter_complex','[0:v]trim=end_frame=60,setpts=PTS-STARTPTS,scale=1260:528[a];[1:v]trim=end_frame=60,setpts=PTS-STARTPTS,scale=1260:528[b];[a][b]concat=n=2:v=1[v]','-map','[v]')},
  @{set='cuts'; n='pan';       w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  kind='pan';   truth='';   status='synthetic'; note='one still (tos 30 s) panned 0 -> 630 -> 0 px';
    args=@('-ss','30','-i',$tos,'-vf',"$still,scale=1890:792,crop=1260:528:x='315*(1-cos(n*PI/60))':y=132")},
  @{set='cuts'; n='fade';      w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  kind='fade';  truth='';   status='synthetic'; note='one still (tos 30 s): fade out 30-59, fade in 60-89';
    args=@('-ss','30','-i',$tos,'-filter_complex',"[0:v]$still,scale=1260:528,split[s1][s2];[s1]trim=end_frame=60,fade=t=out:start_frame=30:nb_frames=30[a];[s2]trim=end_frame=60,setpts=PTS-STARTPTS,fade=t=in:start_frame=0:nb_frames=30[b];[a][b]concat=n=2:v=1[v]",'-map','[v]')}
)
if ($Set -eq 'cuts') { $items = @($items | Where-Object { $_.set -eq 'cuts' }) }
$list = @()
$truth = @()
foreach ($i in $items) {
  $file = Join-Path $raw "$($i.n).rgb24"
  & $Ffmpeg -y -v error @($i.args) -frames:v $Frames -pix_fmt rgb24 -f rawvideo $file
  if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed for $($i.n)" }
  $bytes = (Get-Item $file).Length
  $got = [int]($bytes / ($i.w * $i.h * 3))
  Write-Host "$($i.n): $got frames"
  if ($got -ne $Frames) { Write-Warning "$($i.n): $got frames, $Frames asked (the truth frames and the per-minute rates use the frames captured)" }
  $list += "$($i.n)|$file|$($i.w)|$($i.h)|$($i.ow)|$($i.oh)|$($i.tr)|$($i.li)|$Frames"
  $kind = if ($i.kind) { $i.kind } else { 'other' }
  $status = if ($i.status) { $i.status } else { 'UNMARKED' }
  $truth += "$($i.n)|$kind|$($i.truth)|$status|$($i.note)"
}
Set-Content -Path (Join-Path $Out 'x5ab-list.txt') -Value $list -Encoding ascii
$truthPath = Join-Path $Out 'x5ab-truth.txt'
if (Test-Path $truthPath) {
  $have = @(Get-Content $truthPath | Where-Object { $_ -and $_[0] -ne '#' } | ForEach-Object { ($_ -split '\|')[0] })
  $missing = @($items | Where-Object { $have -notcontains $_.n } | ForEach-Object { $_.n })
  Write-Host "truth list kept (hand-marked data, never overwritten): $truthPath"
  if ($missing.Count -gt 0) { Write-Warning "truth list has no line for: $($missing -join ', ') (add them by hand)" }
} else {
  $head = @('# case|kind|truth cut frames|status|note   (tools/x5ab-extract.ps1 template; UNMARKED lines must be hand-marked before scoring)')
  Set-Content -Path $truthPath -Value ($head + $truth) -Encoding ascii
  Write-Host "truth template written: $truthPath (hand-mark every UNMARKED line)"
}
