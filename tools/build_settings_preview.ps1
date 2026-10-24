<#
Builds the standalone SETTINGS PREVIEW (queue item 23): a folder the owner opens to see the whole
Config organisation and every option, without VP, a capture card or the projector, and without
touching the real configuration.

  tools\build_settings_preview.ps1 [-Out D:\codex\vp-settings-preview] [-BuildDir x64\Release]

Run it after an x64 Release build of VideoProcessor-Config (windeployqt has then already placed the
Qt runtime beside VideoProcessorConfig.exe). It:
- copies VideoProcessorConfig.exe, VideoProcessorConfigDiscovery.dll and the windeployqt Qt files
  into <Out>\app;
- copies the app-local Microsoft C++ runtime from the licensed Visual Studio Release redist folder
  (x64, Microsoft-signed, at or above packaging/release-manifest.json's vcRuntime.minimumVersion;
  never from Windows\System32);
- writes the SANDBOX settings file <Out>\sandbox\VideoProcessor.cfg: the cinema madVR-approximation
  configuration (docs/cinema/VideoProcessor-test.cfg) plus the sections of the current defaults
  sample (VideoProcessor.cfg) that it lacks; every key it does not set shows VP's current built-in
  default in Config; the shaders it names go to <Out>\sandbox\shaders;
- writes the launcher "Open settings preview.bat", which runs Config with --sandbox and
  --config <Out>\sandbox\VideoProcessor.cfg (Config then never contacts a running VP or the LAN),
  and README-PREVIEW.txt.
An existing <Out>\sandbox\VideoProcessor.cfg is kept (it holds the owner's preview edits) unless
-ResetSandbox is given.
#>
param(
    [string]$Out = "D:\codex\vp-settings-preview",
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\x64\Release"),
    [switch]$ResetSandbox
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = (Resolve-Path $BuildDir).Path
$exe = Join-Path $BuildDir "VideoProcessorConfig.exe"
if (-not (Test-Path $exe)) { throw "No Release build of Config at $exe" }
if ($Out -match '^(?i)C:\\Videoprocessor') { throw "The preview must never be built into the real VP folder ($Out)" }

$app = Join-Path $Out "app"
$sandbox = Join-Path $Out "sandbox"
New-Item -ItemType Directory -Force $app, $sandbox | Out-Null

# 1. Config and its Qt runtime (what windeployqt placed beside the exe).
$files = @("VideoProcessorConfig.exe", "VideoProcessorConfigDiscovery.dll") +
    @(Get-ChildItem $BuildDir -Filter "Qt6*.dll" | ForEach-Object Name) +
    @("dxcompiler.dll", "dxil.dll", "opengl32sw.dll")
foreach ($f in $files) {
    $src = Join-Path $BuildDir $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $app $f) -Force }
}
foreach ($dir in "platforms", "styles", "iconengines", "imageformats", "tls", "networkinformation", "generic") {
    $src = Join-Path $BuildDir $dir
    if (Test-Path $src) { Copy-Item $src (Join-Path $app $dir) -Recurse -Force }
}

# 2. App-local C++ runtime from the Visual Studio redist (never System32).
$manifest = Get-Content (Join-Path $repo "packaging\release-manifest.json") -Raw | ConvertFrom-Json
$floor = [version]$manifest.vcRuntime.minimumVersion
$crt = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory -ErrorAction SilentlyContinue |
    Where-Object { (Test-Path (Join-Path $_.FullName "msvcp140.dll")) -and
        [version](Get-Item (Join-Path $_.FullName "msvcp140.dll")).VersionInfo.FileVersion -ge $floor } |
    Sort-Object { [version](Get-Item (Join-Path $_.FullName "msvcp140.dll")).VersionInfo.FileVersion } -Descending |
    Select-Object -First 1
if (-not $crt) { throw "No Visual Studio x64 VC143 CRT redist at or above $floor" }
foreach ($f in "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
    $src = Join-Path $crt.FullName $f
    $sig = Get-AuthenticodeSignature $src
    if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "Microsoft Corporation") {
        throw "$src is not Microsoft-signed ($($sig.Status))"
    }
    Copy-Item $src (Join-Path $app $f) -Force
}

# 3. The sandbox settings file.
$cfg = Join-Path $sandbox "VideoProcessor.cfg"
function Read-Sections([string]$path) {
    $sections = [ordered]@{}
    $name = ""
    $sections[$name] = New-Object System.Collections.Generic.List[string]
    foreach ($line in [System.IO.File]::ReadAllLines($path)) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            $name = $Matches[1].Trim().ToLowerInvariant()
            if (-not $sections.Contains($name)) { $sections[$name] = New-Object System.Collections.Generic.List[string] }
        }
        $sections[$name].Add($line)
    }
    return $sections
}
if ($ResetSandbox -or -not (Test-Path $cfg)) {
    $cinemaPath = Join-Path $repo "docs\cinema\VideoProcessor-test.cfg"
    $samplePath = Join-Path $repo "VideoProcessor.cfg"
    $cinema = Read-Sections $cinemaPath
    $sample = Read-Sections $samplePath
    $text = New-Object System.Collections.Generic.List[string]
    $text.Add("# SETTINGS PREVIEW SANDBOX - not a real VideoProcessor configuration.")
    $text.Add("# Built $(Get-Date -Format 'yyyy-MM-dd HH:mm') by tools/build_settings_preview.ps1 from:")
    $text.Add("#   docs/cinema/VideoProcessor-test.cfg (the cinema madVR-approximation set-up), then")
    $text.Add("#   the sections of the current defaults sample (VideoProcessor.cfg) that it lacks.")
    $text.Add("# A key that is not set here uses VP's current built-in default; Config shows it as")
    $text.Add("# 'Not set (default: ...)' or 'Default: ...'. Editing this file changes nothing real.")
    $text.Add("")
    foreach ($line in [System.IO.File]::ReadAllLines($cinemaPath)) { $text.Add($line) }
    $added = @()
    foreach ($name in $sample.Keys) {
        if ($name -eq "" -or $cinema.Contains($name)) { continue }
        # Colour, viewport and zoom profiles are chosen by order and rules; the cinema file's own set stays alone.
        if ($name -match '^vprenderer\.(color|viewport|zoom)(\.|$)' -or $name -match '^shader\.standard\.') { continue }
        if ($added.Count -eq 0) {
            $text.Add("")
            $text.Add("# ---- Added from the current defaults sample (VideoProcessor.cfg) ----")
        }
        $text.Add("")
        foreach ($line in $sample[$name]) { $text.Add($line) }
        $added += $name
    }
    [System.IO.File]::WriteAllLines($cfg, $text, (New-Object System.Text.UTF8Encoding($false)))
    # Let Config apply its own load-time migrations once (the cinema file predates some), so the
    # preview opens without "Unsaved changes". Its backup of our just-written file is not kept.
    $tests = Join-Path $BuildDir "VideoProcessorConfigTests.exe"
    if (Test-Path $tests) {
        $before = @(Get-ChildItem $sandbox -Filter "*.bak" -File | ForEach-Object FullName)
        $previous = $env:QT_QPA_PLATFORM; $env:QT_QPA_PLATFORM = "offscreen"
        try { & $tests --resave $cfg | Write-Host; if ($LASTEXITCODE -ne 0) { throw "Config could not re-save the sandbox ($LASTEXITCODE)" } }
        finally { if ($null -eq $previous) { Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue } else { $env:QT_QPA_PLATFORM = $previous } }
        Get-ChildItem $sandbox -Filter "*.bak" -File | Where-Object { $before -notcontains $_.FullName } | Remove-Item
    }
    Copy-Item $cfg (Join-Path $sandbox "VideoProcessor.original.cfg") -Force
    Write-Host "Sandbox written: $cfg (added sections: $($added -join ', '))"
} else {
    Write-Host "Sandbox kept: $cfg (use -ResetSandbox to rebuild it)"
}
$shaders = Join-Path $sandbox "shaders"
New-Item -ItemType Directory -Force $shaders | Out-Null
Copy-Item (Join-Path $repo "shaders\*") $shaders -Recurse -Force

# 4. Launcher and README.
$bat = @"
@echo off
rem Settings preview: opens VideoProcessor Configuration on the SANDBOX file in this folder only.
rem --sandbox: never contacts a running VP or other computers; nothing real is changed.
start "" "%~dp0app\VideoProcessorConfig.exe" --sandbox --config "%~dp0sandbox\VideoProcessor.cfg"
"@
[System.IO.File]::WriteAllText((Join-Path $Out "Open settings preview.bat"), $bat.Replace("`n", "`r`n"), (New-Object System.Text.ASCIIEncoding))
Copy-Item (Join-Path $repo "tools\settings-preview\README-PREVIEW.txt") (Join-Path $Out "README-PREVIEW.txt") -Force
$commit = (git -C $repo rev-parse --short HEAD) 2>$null
[System.IO.File]::WriteAllText((Join-Path $Out "app\BUILD.txt"),
    "Built from $repo at commit $commit (plus uncommitted changes if any) on $(Get-Date -Format s).`r`nC++ runtime: $($crt.FullName)`r`n")
Write-Host "Settings preview ready: $Out"
