<#
Runs VideoProcessorConfigTests.exe faster (owner 2026-10-15).

  run_config_tests.ps1                 # the whole suite in 4 parallel shards
  run_config_tests.ps1 -Shards 1       # the whole suite in one process (as before)
  run_config_tests.ps1 -Match "LED|Motion"   # only tests whose names contain a word
  run_config_tests.ps1 -OnScreen       # with real windows (takes the screen): when the computer is free

By default (owner 2026-10-16) the tests run off screen (QT_QPA_PLATFORM=offscreen): no window is
shown and focus is never taken, so the computer stays usable. The few tests that need a real
window are skipped and listed (SKIP); run -OnScreen to include them.

Tests that need the foreground window or the screen's z-order always run in
shard 0 (the runner keeps them there), so parallel shards never take focus
from each other. Prints PASS/FAIL counts, every failure, and the ten slowest
tests.

The gate fails (exit 1) when any test failed, when any shard process exited non-zero, when a test
printed START but neither PASS nor FAIL (it crashed or hung the shard), or when the number of
PASS+FAIL lines does not equal the number of STARTs (or no test ran at all).
#>
param(
    [string]$Exe = (Join-Path $PSScriptRoot "..\x64\Release\VideoProcessorConfigTests.exe"),
    [int]$Shards = 4,
    [string]$Match = "",
    [string]$OutDir = (Join-Path ([System.IO.Path]::GetTempPath()) "vp-config-tests"),
    [switch]$OnScreen
)
$ErrorActionPreference = "Stop"
if ($Shards -lt 1) { throw "-Shards must be at least 1" }
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$started = Get-Date
$previousPlatform = $env:QT_QPA_PLATFORM
if ($OnScreen) { Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue } else { $env:QT_QPA_PLATFORM = "offscreen" }
$processes = @()
try {
    for ($i = 0; $i -lt $Shards; ++$i) {
        $arguments = @()
        if ($Shards -gt 1) { $arguments += @("--shard", "$i/$Shards") }
        if ($Match) { $arguments += @("--match", "`"$Match`"") }
        $log = Join-Path $OutDir "shard$i.txt"
        Remove-Item $log, "$log.err" -ErrorAction SilentlyContinue
        # Start-Process rejects an empty -ArgumentList (the -Shards 1 case without -Match), so it is
        # passed only when there is something to pass.
        $startArgs = @{
            FilePath = $Exe
            WorkingDirectory = (Split-Path $Exe)
            RedirectStandardOutput = $log
            RedirectStandardError = "$log.err"
            PassThru = $true
            NoNewWindow = $true
        }
        if ($arguments.Count -gt 0) { $startArgs.ArgumentList = $arguments }
        $p = Start-Process @startArgs
        # Touching Handle right after the start keeps the process handle open, so ExitCode is
        # available after the process ends (otherwise it can read back as $null).
        $null = $p.Handle
        $processes += $p
    }
    $processes | Wait-Process
}
finally {
    if ($null -eq $previousPlatform) { Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue } else { $env:QT_QPA_PLATFORM = $previousPlatform }
}
$lines = @()
$shardProblems = @()
for ($i = 0; $i -lt $Shards; ++$i) {
    $log = Join-Path $OutDir "shard$i.txt"
    $shardLines = @(Get-Content $log, "$log.err" -ErrorAction SilentlyContinue)
    $lines += $shardLines
    $p = $processes[$i]
    $p.WaitForExit()
    $code = $p.ExitCode
    # With -Match a shard can legitimately select nothing: the exe then exits 2 and prints no START.
    $emptyMatchShard = $Match -and $code -eq 2 -and @($shardLines | Where-Object { $_ -like "START *" }).Count -eq 0
    if (($null -eq $code -or $code -ne 0) -and -not $emptyMatchShard) { $shardProblems += "shard $i exited with code $code" }
}
# Off screen Qt has no fonts, so text measures as placeholder boxes; the layout-fit test (the VP-V page
# must not scroll sideways at the default window size) skips its check then. It runs once more here
# with the Windows fonts, so it measures what the owner sees (organisation pass 2, 2026-10-24).
if (-not $OnScreen -and (-not $Match -or "VP-V: masters, derived settings, one control per key, fits the window" -match $Match)) {
    $fontLog = Join-Path $OutDir "fonts.txt"
    $previousFontDir = $env:QT_QPA_FONTDIR
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QT_QPA_FONTDIR = Join-Path $env:WINDIR "Fonts"
    try {
        $fp = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) -ArgumentList @("--match", "`"fits the window`"") `
            -RedirectStandardOutput $fontLog -RedirectStandardError "$fontLog.err" -PassThru -NoNewWindow
        $null = $fp.Handle
        $fp.WaitForExit()
        if ($fp.ExitCode -ne 0) { $shardProblems += "fonts pass exited with code $($fp.ExitCode)" }
        $lines += @(Get-Content $fontLog, "$fontLog.err" -ErrorAction SilentlyContinue)
    }
    finally {
        if ($null -eq $previousFontDir) { Remove-Item Env:QT_QPA_FONTDIR -ErrorAction SilentlyContinue } else { $env:QT_QPA_FONTDIR = $previousFontDir }
        if ($null -eq $previousPlatform) { Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue } else { $env:QT_QPA_PLATFORM = $previousPlatform }
    }
}
$passed = @($lines | Where-Object { $_ -like "PASS *" })
$failed = @($lines | Where-Object { $_ -like "FAIL *" })
$skipped = @($lines | Where-Object { $_ -like "SKIP *" })
$starts = @($lines | Where-Object { $_ -like "START *" } | ForEach-Object { $_.Substring(6) })
$crashed = @()
foreach ($name in $starts) {
    $done = @($lines | Where-Object { $_.StartsWith("PASS $name [") -or $_.StartsWith("FAIL ${name}: ") })
    if ($done.Count -eq 0) { $crashed += "crashed in $name (START without PASS/FAIL)" }
}
$elapsed = [math]::Round(((Get-Date) - $started).TotalSeconds)
$mode = if ($OnScreen) { "on screen" } else { "off screen" }
"Config tests: $($passed.Count) passed, $($failed.Count) failed, $($skipped.Count) skipped, $($starts.Count) started in $elapsed s ($Shards shard(s), $mode)"
$failed | ForEach-Object { "  $_" }
$skipped | ForEach-Object { "  $_" }
$crashed | ForEach-Object { "  $_" }
$shardProblems | ForEach-Object { "  $_" }
"Slowest:"
$lines | Where-Object { $_ -match '^(PASS|FAIL) (.*) \[(\d+) ms\]$' } | ForEach-Object {
    [pscustomobject]@{ Ms = [int]($_ -replace '^.*\[(\d+) ms\]$', '$1'); Line = $_ }
} | Sort-Object Ms -Descending | Select-Object -First 10 | ForEach-Object { "  $($_.Ms) ms  $($_.Line)" }
$gateFailed = $false
if ($failed.Count -gt 0) { $gateFailed = $true }
if ($shardProblems.Count -gt 0) { $gateFailed = $true }
if ($crashed.Count -gt 0) { $gateFailed = $true }
if ($starts.Count -eq 0) { "  GATE: no test started"; $gateFailed = $true }
elseif (($passed.Count + $failed.Count) -ne $starts.Count) {
    "  GATE: passed+failed ($($passed.Count + $failed.Count)) != started ($($starts.Count))"; $gateFailed = $true
}
if ($gateFailed) { "Config tests: GATE FAILED"; exit 1 }
"Config tests: GATE PASSED"
exit 0
