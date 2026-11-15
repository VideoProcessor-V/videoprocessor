# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
Makes the raw clips and the list file for X5AbCaptureTests (FIX-X5-CUT-P4 before/after). ffmpeg only; no film pixels are committed.
  x5ab-extract.ps1 [-Out D:\testclips\x5ab] [-Frames 120]
Raw files are rgb24, 120 frames each, written to <Out>\raw\; the list file <Out>\x5ab-list.txt feeds VP_X5AB_LIST.
Geometry is reduced to keep disk use small (the disk is 97% full): sources 1260x720 (or 1260x528 for 'scope' clips) -> 840x480
(or 840x352), the 1.5x downscale ratio of the design's 1080p -> 720p and 4K -> 1440p cases. The upscale cases (ratio 1 / 2) are not
covered by this harness. The crop case has its crop baked into the pixels (no crop rect given to the renderer).
Synthetic clips (from tos_24p_3min): 'cut' = a hard splice of two scenes at frame 60; 'flash' = a +64-code brightness flash on frames 60-61.
#>
param(
    [string]$Out = 'D:\testclips\x5ab',
    [int]$Frames = 120,
    [string]$Ffmpeg = 'D:\tools\ffmpeg\bin\ffmpeg.exe'
)
$ErrorActionPreference = 'Stop'
$raw = Join-Path $Out 'raw'
New-Item -ItemType Directory -Force $raw | Out-Null
$tos = 'D:\testclips\variants\tos_24p_3min.mp4'
$items = @(
  @{n='hdr4k';     w=1260; h=720; ow=840; oh=480; tr='pq';      li='gamma';  args=@('-ss','60','-i','D:\testclips\meridian_src\Meridian_UHD4k5994_HDR_P3PQ.mp4','-vf','scale=1260:720')},
  @{n='hdr2k';     w=1260; h=528; ow=840; oh=352; tr='pq';      li='gamma';  args=@('-ss','60','-i','D:\testclips\cosmos_laundromat_src\CosmosLaundromat_2k24p_HDR_P3PQ.mp4','-vf','scale=1260:528')},
  @{n='sdr1080';   w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  args=@('-ss','20','-i',$tos,'-vf','scale=1260:528')},
  @{n='sdrlin';    w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='linear'; args=@('-ss','20','-i',$tos,'-vf','scale=1260:528')},
  @{n='cropbaked'; w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  args=@('-i','D:\testclips\film\cosmos_1920x800_24p.y4m','-vf','crop=1600:672:160:64,scale=1260:528')},
  @{n='sdrmeridian'; w=1260; h=720; ow=840; oh=480; tr='gamma22'; li='gamma'; args=@('-ss','60','-i','D:\testclips\meridian_src\Meridian_UHD4k5994_HDR_P3PQ.mp4','-vf','scale=1260:720')},
  @{n='flash';     w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  args=@('-ss','20','-i',$tos,'-vf',"scale=1260:528,eq=brightness=0.25:enable='between(n,60,61)'")},
  @{n='cut';       w=1260; h=528; ow=840; oh=352; tr='gamma22'; li='gamma';  args=@('-ss','20','-t','2.5','-i',$tos,'-ss','100','-t','2.5','-i',$tos,'-filter_complex','[0:v]trim=end_frame=60,setpts=PTS-STARTPTS,scale=1260:528[a];[1:v]trim=end_frame=60,setpts=PTS-STARTPTS,scale=1260:528[b];[a][b]concat=n=2:v=1[v]','-map','[v]')}
)
$list = @()
foreach ($i in $items) {
  $file = Join-Path $raw "$($i.n).rgb24"
  & $Ffmpeg -y -v error @($i.args) -frames:v $Frames -pix_fmt rgb24 -f rawvideo $file
  if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed for $($i.n)" }
  $bytes = (Get-Item $file).Length
  $got = [int]($bytes / ($i.w * $i.h * 3))
  Write-Host "$($i.n): $got frames"
  $list += "$($i.n)|$file|$($i.w)|$($i.h)|$($i.ow)|$($i.oh)|$($i.tr)|$($i.li)|$Frames"
}
Set-Content -Path (Join-Path $Out 'x5ab-list.txt') -Value $list -Encoding ascii
