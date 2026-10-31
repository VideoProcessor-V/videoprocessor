# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
#
# D3D11 debug-layer probe. Runs the NearBlackTimelineVsPlainProbe test with
# VP_D3D11_DEBUG=1 (libplacebo device created with debug=true), then summarises
# D3D11 debug-layer messages (CORRUPTION, ERROR, WARNING, "still bound on input",
# hazard) found in the vstest output, the TRX and any vp.log written during the run.
#
# REQUIRES the Windows optional feature "Graphics Tools" (Settings > System >
# Optional features), otherwise device creation with the debug layer fails or
# produces no messages. Needs a prior x64 Release build of VideoProcessor-Test.dll.
# Not run by CI; the coordinator runs it. Environment variables are restored on exit.
[CmdletBinding()]
param(
    [int]$MaxLines = 40,
    [string]$VsTest = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\TestWindow\vstest.console.exe',
    [string]$Filter = 'NearBlackTimelineVsPlainProbe'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$dll = Join-Path $repo 'x64\Release\VideoProcessor-Test.dll'
if (-not (Test-Path $dll)) { throw "Missing $dll (build x64 Release first)" }
if (-not (Test-Path $VsTest)) { throw "Missing $VsTest" }
$results = Join-Path $repo 'TestResults\d3d11-debug'
New-Item -ItemType Directory -Force $results | Out-Null
$stamp = Get-Date
$pattern = 'CORRUPTION|ERROR|WARNING|still bound on input|hazard'

$names = 'VP_D3D11_DEBUG', 'VP_TEST_GPU', 'VP_TEST_ADAPTER'
$saved = @{}
foreach ($n in $names) { $saved[$n] = [Environment]::GetEnvironmentVariable($n, 'Process') }
try {
    $env:VP_D3D11_DEBUG = '1'
    $env:VP_TEST_GPU = 'hardware'
    $env:VP_TEST_ADAPTER = 'NVIDIA'
    $out = & $VsTest $dll "/Tests:$Filter" '/Logger:trx' "/ResultsDirectory:$results" 2>&1 | ForEach-Object { "$_" }
} finally {
    foreach ($n in $names) { [Environment]::SetEnvironmentVariable($n, $saved[$n], 'Process') }
}
$outFile = Join-Path $results 'vstest-output.txt'
$out | Set-Content -Encoding utf8 $outFile

# Sources: console output, TRX files from this run, vp.log files touched during the run.
$sources = @([pscustomobject]@{ Name = 'vstest output'; Lines = $out })
Get-ChildItem $results -Filter *.trx -Recurse | Where-Object { $_.LastWriteTime -ge $stamp } | ForEach-Object {
    $sources += [pscustomobject]@{ Name = $_.FullName; Lines = (Get-Content $_.FullName) }
}
$logDirs = @((Join-Path $repo 'x64\Release\logs'), (Join-Path $repo 'logs'), (Join-Path $PWD 'logs'), 'C:\logs')
$logDirs += (Get-ChildItem (Join-Path $repo 'x64\Release') -Directory -Recurse -Filter logs -ErrorAction SilentlyContinue | ForEach-Object FullName)
$logDirs | Select-Object -Unique | Where-Object { Test-Path $_ } | ForEach-Object {
    Get-ChildItem $_ -Filter 'vp*.log*' -File | Where-Object { $_.LastWriteTime -ge $stamp } | ForEach-Object {
        $sources += [pscustomobject]@{ Name = $_.FullName; Lines = (Get-Content $_.FullName) }
    }
}

"=== D3D11 debug probe summary ($Filter) ==="
$debugOn = $false
foreach ($s in $sources) { if ($s.Lines -match 'libplacebo D3D11 device:.*debug=1') { $debugOn = $true } }
"debug layer requested in device log line: $debugOn"
foreach ($s in $sources) {
    $hits = @($s.Lines | Where-Object { $_ -match $pattern })
    "--- $($s.Name): $($hits.Count) matching lines"
    foreach ($k in 'CORRUPTION', 'ERROR', 'WARNING', 'still bound on input', 'hazard') {
        $c = @($hits | Where-Object { $_ -match $k }).Count
        if ($c) { "    $k : $c" }
    }
    $hits | Select-Object -First $MaxLines | ForEach-Object { "    $_" }
}
"Results: $results"
