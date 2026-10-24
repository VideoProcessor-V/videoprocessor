<#
Screenshots of the settings layout for the usability audit (organisation pass, 2026-10-24).

  tools\settings_preview_screens.ps1 [-Config D:\codex\vp-settings-preview\sandbox\VideoProcessor.cfg]
                                      [-Out D:\codex\vp-settings-preview\screens]

Opens a COPY of the preview's sandbox configuration off screen, with the real Windows fonts, at the
default window size (1040x700), and saves a PNG of every VP-V unit tab's Main and Advanced page, the
Pipeline (expert) and Proposed retirement tabs, the Dynamic Drive page (Main, Advanced) and the
Rendering page, in the VP-V and the Original pathway. Each picture comes as the visible window and as
"-full" (the whole scrolled page at that width). Uses VideoProcessorConfigTests.exe --screens from
the Release build (the same Config code the preview runs). Nothing is written to any configuration.
#>
param(
    [string]$Config = "D:\codex\vp-settings-preview\sandbox\VideoProcessor.cfg",
    [string]$Out = "D:\codex\vp-settings-preview\screens",
    [string]$Exe = (Join-Path $PSScriptRoot "..\x64\Release\VideoProcessorConfigTests.exe")
)
$ErrorActionPreference = "Stop"
if ($Out -match '^(?i)C:\\Videoprocessor') { throw "Screens never go into the real VP folder ($Out)" }
$Exe = (Resolve-Path $Exe).Path
$Config = (Resolve-Path $Config).Path
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force
$before = (Get-FileHash $Config).Hash
$previousPlatform = $env:QT_QPA_PLATFORM
$previousFontDir = $env:QT_QPA_FONTDIR
try {
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QT_QPA_FONTDIR = Join-Path $env:WINDIR "Fonts"
    & $Exe --screens $Config $Out
    if ($LASTEXITCODE -ne 0) { throw "VideoProcessorConfigTests --screens failed ($LASTEXITCODE)" }
}
finally {
    if ($null -eq $previousPlatform) { Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue } else { $env:QT_QPA_PLATFORM = $previousPlatform }
    if ($null -eq $previousFontDir) { Remove-Item Env:QT_QPA_FONTDIR -ErrorAction SilentlyContinue } else { $env:QT_QPA_FONTDIR = $previousFontDir }
}
if ((Get-FileHash $Config).Hash -ne $before) { throw "The configuration changed while taking screenshots: $Config" }
$pngs = @(Get-ChildItem $Out -Filter *.png)
"Screens: $($pngs.Count) PNGs in $Out (configuration unchanged)"
