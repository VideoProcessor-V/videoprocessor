# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
# Synthetic gate clips (ffmpeg testsrc2, no film frames) for the file-source graph test; written outside the repo.
param([string]$Ffmpeg = "D:\tools\ffmpeg\bin\ffmpeg.exe", [string]$OutDir = "D:\testclips\gate")
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force $OutDir | Out-Null
& $Ffmpeg -y -v error -f lavfi -i "testsrc2=size=1280x720:rate=24:duration=6" -f lavfi -i "sine=frequency=440:duration=6" -c:v libx264 -pix_fmt yuv420p -preset veryfast -c:a aac -shortest (Join-Path $OutDir "gate_h264.mp4")
& $Ffmpeg -y -v error -f lavfi -i "testsrc2=size=1280x720:rate=24:duration=6" -c:v libx265 -pix_fmt yuv420p10le -preset veryfast (Join-Path $OutDir "gate_hevc10.mkv")
