# Hunt 2 (2026-10-17, MEDIUM-1/2): EXE/DLL build identity.
#
# Writes one C++ header with VP_BUILD_ID, a content identity of the sources and
# build inputs. VideoProcessor-Lib generates it before it compiles (it is the one
# writer: VideoProcessor-GUI, both renderer DLLs and the tests reference Lib, so
# MSBuild builds Lib, and this header, before any of them). Lib compiles the ID
# into VpBuildIdentity.cpp, which is linked into VideoProcessor.exe and into each
# renderer DLL; the DLL exports it and the EXE refuses a DLL with another ID.
#
# Why a content hash (not commit + dirty flag + timestamp):
#  - Two different dirty trees on one commit must never share an ID; a commit and
#    a dirty flag cannot tell them apart. The hash covers every source byte.
#  - It needs no Git (shallow, detached or Git-less checkouts give the same ID for
#    the same sources) and no clock, so it is deterministic: an incremental build
#    of unchanged sources keeps the ID and relinks nothing.
#  - A timestamp or random GUID would change on every build and so relink the
#    EXE and both DLLs every time, and two builds of identical sources would
#    falsely mismatch.
# Configuration, platform, toolset and the CI Magewell switch are hashed too, so a
# Debug DLL never pairs with a Release EXE. Generation fails the build on any
# error: there is no "unknown" fallback ID that could match another build.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepositoryRoot,
    [Parameter(Mandatory = $true)][string]$OutputFile,
    [string]$BuildInputs = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepositoryRoot = [IO.Path]::GetFullPath($RepositoryRoot).TrimEnd('\')
$OutputFile = [IO.Path]::GetFullPath($OutputFile)
$scriptVersion = 'vp-build-identity-1'

# Source inventory: every file that can change a VP binary, by extension, under
# src and 3rdparty, plus the build glue. Build output directories and generated
# headers are excluded (version.h is rewritten by GUI pre-build steps while other
# projects build; it carries only commit text, which the content already covers).
$roots = @('src', '3rdparty')
$extensions = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($e in @('.h', '.hpp', '.hh', '.hxx', '.inl', '.c', '.cc', '.cpp', '.cxx', '.def',
        '.rc', '.rc2', '.idl', '.hlsl', '.hlsli', '.fx', '.glsl', '.vcxproj', '.props',
        '.targets', '.manifest', '.qrc', '.ui')) { [void]$extensions.Add($e) }
$excludedDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($d in @('x64', 'x86', 'Win32', 'ARM64', 'Debug', 'Release', 'obj', 'bin', '.vs',
        '.git', 'GeneratedFiles', 'TestResults')) { [void]$excludedDirectories.Add($d) }
$excludedFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($f in @('src/VideoProcessor-GUI/version.h', 'src/VideoProcessor-VPRenderer/version.h')) {
    [void]$excludedFiles.Add($f)
}

$files = [Collections.Generic.List[string]]::new()
function Add-VpSourceFiles([string]$Directory) {
    foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($Directory)) {
        $name = [IO.Path]::GetFileName($entry)
        $attributes = [IO.File]::GetAttributes($entry)
        if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
            # Junctions/symlinks are not followed: their target is not part of this tree.
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
            if ($excludedDirectories.Contains($name)) { continue }
            Add-VpSourceFiles $entry
        } elseif ($extensions.Contains([IO.Path]::GetExtension($name))) {
            $relative = $entry.Substring($RepositoryRoot.Length + 1).Replace('\', '/')
            if (-not $excludedFiles.Contains($relative)) { $files.Add($relative) }
        }
    }
}
foreach ($root in $roots) {
    $path = Join-Path $RepositoryRoot $root
    if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "Build identity: missing source root $path" }
    Add-VpSourceFiles $path
}
foreach ($glue in @('Directory.Build.targets', 'VideoProcessor.sln', 'tools/generate_build_identity.ps1')) {
    if (Test-Path -LiteralPath (Join-Path $RepositoryRoot $glue) -PathType Leaf) { $files.Add($glue) }
}
if ($files.Count -lt 10) { throw "Build identity: implausible source inventory ($($files.Count) files) under $RepositoryRoot" }
$sorted = $files.ToArray()
[Array]::Sort($sorted, [StringComparer]::OrdinalIgnoreCase)

$utf8 = [Text.UTF8Encoding]::new($false)
$total = [Security.Cryptography.SHA256]::Create()
$perFile = [Security.Cryptography.SHA256]::Create()
try {
    function Add-VpBytes([byte[]]$Bytes) { [void]$total.TransformBlock($Bytes, 0, $Bytes.Length, $null, 0) }
    Add-VpBytes $utf8.GetBytes("$scriptVersion`0$BuildInputs`0")
    foreach ($relative in $sorted) {
        $stream = [IO.File]::Open((Join-Path $RepositoryRoot $relative), [IO.FileMode]::Open,
            [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try { $digest = $perFile.ComputeHash($stream) } finally { $stream.Dispose() }
        Add-VpBytes $utf8.GetBytes($relative.ToLowerInvariant() + "`0")
        Add-VpBytes $digest
    }
    [void]$total.TransformFinalBlock([byte[]]::new(0), 0, 0)
    $id = ([BitConverter]::ToString($total.Hash)).Replace('-', '').ToLowerInvariant().Substring(0, 32)
} finally { $total.Dispose(); $perFile.Dispose() }

# Informational only (never compared): the commit, when Git can tell.
$commit = ''
try {
    $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($git) {
        $out = & $git.Source -C $RepositoryRoot rev-parse --short=12 HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and "$out" -match '^[0-9a-f]{7,40}$') { $commit = "$out" }
    }
} catch { $commit = '' }
if (-not $commit -and $env:GITHUB_SHA -match '^[0-9a-f]{12}') { $commit = $env:GITHUB_SHA.Substring(0, 12) }
if (-not $commit) { $commit = 'unknown' }

$escapedInputs = $BuildInputs.Replace('\', '\\').Replace('"', '\"')
$content = @"
// Generated by tools/generate_build_identity.ps1. Do not edit or commit.
#pragma once

// Content identity of the sources and build inputs ($($sorted.Count) files).
#define VP_BUILD_ID "$id"
// Informational only; never compared.
#define VP_BUILD_ID_COMMIT "$commit"
#define VP_BUILD_ID_INPUTS "$escapedInputs"

"@

# Write only when changed (no recompile otherwise), via a unique temporary file
# and an atomic replace, so a concurrent reader never sees a truncated header.
$directory = Split-Path -Parent $OutputFile
if (-not (Test-Path -LiteralPath $directory)) { $null = New-Item -ItemType Directory -Path $directory -Force }
$existing = if (Test-Path -LiteralPath $OutputFile -PathType Leaf) { [IO.File]::ReadAllText($OutputFile) } else { $null }
if ($existing -cne $content) {
    $temporary = "$OutputFile.$([guid]::NewGuid().ToString('N')).tmp"
    [IO.File]::WriteAllText($temporary, $content, $utf8)
    for ($attempt = 1; ; $attempt++) {
        try {
            if (Test-Path -LiteralPath $OutputFile) { [IO.File]::Replace($temporary, $OutputFile, [NullString]::Value) }
            else { [IO.File]::Move($temporary, $OutputFile) }
            break
        } catch {
            if ($attempt -ge 20) { Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue; throw }
            Start-Sleep -Milliseconds 100
            # Another writer of the same build may have produced identical content.
            if ((Test-Path -LiteralPath $OutputFile) -and [IO.File]::ReadAllText($OutputFile) -ceq $content) {
                Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue; break
            }
        }
    }
}
Write-Host "VP build identity $id (commit $commit, $($sorted.Count) files, inputs: $BuildInputs) -> $OutputFile"
