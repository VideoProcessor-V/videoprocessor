# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
# Synthetic gate clips (ffmpeg testsrc2, no film frames) for the file-source graph test; written outside the repo.
param([string]$Ffmpeg = "D:\tools\ffmpeg\bin\ffmpeg.exe", [string]$OutDir = "D:\testclips\gate")
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Ffmpeg)) { throw "ffmpeg not found: $Ffmpeg" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
# gate_hevc10.mkv is not used by the gate; it is kept for the FileSourceLav HEVC 10-bit bug repro.
& $Ffmpeg -y -v error -f lavfi -i "testsrc2=size=1280x720:rate=24:duration=6" -f lavfi -i "sine=frequency=440:duration=6" -c:v libx264 -pix_fmt yuv420p -preset veryfast -c:a aac -shortest (Join-Path $OutDir "gate_h264.mp4")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed ($LASTEXITCODE)" }
& $Ffmpeg -y -v error -f lavfi -i "testsrc2=size=1280x720:rate=24:duration=6" -c:v libx265 -pix_fmt yuv420p10le -preset veryfast (Join-Path $OutDir "gate_hevc10.mkv")
if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed ($LASTEXITCODE)" }
