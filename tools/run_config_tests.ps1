<#
Runs VideoProcessorConfigTests.exe faster (owner 2026-10-15).

  run_config_tests.ps1                 # the whole suite in 4 parallel shards
  run_config_tests.ps1 -Shards 1       # the whole suite in one process (as before)
  run_config_tests.ps1 -Match "LED|Motion"   # only tests whose names contain a word

Tests that need the foreground window or the screen's z-order always run in
shard 0 (the runner keeps them there), so parallel shards never take focus
from each other. Prints PASS/FAIL counts, every failure, and the ten slowest
tests; exits 1 when anything failed.
#>
param(
    [string]$Exe = (Join-Path $PSScriptRoot "..\x64\Release\VideoProcessorConfigTests.exe"),
    [int]$Shards = 4,
    [string]$Match = "",
    [string]$OutDir = (Join-Path ([System.IO.Path]::GetTempPath()) "vp-config-tests")
)
$ErrorActionPreference = "Stop"
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force $OutDir | Out-Null
$started = Get-Date
$processes = @()
for ($i = 0; $i -lt $Shards; ++$i) {
    $arguments = @()
    if ($Shards -gt 1) { $arguments += @("--shard", "$i/$Shards") }
    if ($Match) { $arguments += @("--match", $Match) }
    $log = Join-Path $OutDir "shard$i.txt"
    $processes += Start-Process -FilePath $Exe -ArgumentList $arguments -WorkingDirectory (Split-Path $Exe) `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -NoNewWindow
}
$processes | Wait-Process
$lines = @()
for ($i = 0; $i -lt $Shards; ++$i) {
    $log = Join-Path $OutDir "shard$i.txt"
    $lines += Get-Content $log, "$log.err" -ErrorAction SilentlyContinue
}
$passed = @($lines | Where-Object { $_ -like "PASS *" })
$failed = @($lines | Where-Object { $_ -like "FAIL *" })
$elapsed = [math]::Round(((Get-Date) - $started).TotalSeconds)
"Config tests: $($passed.Count) passed, $($failed.Count) failed in $elapsed s ($Shards shard(s))"
$failed | ForEach-Object { "  $_" }
"Slowest:"
$lines | Where-Object { $_ -match '^(PASS|FAIL) (.*) \[(\d+) ms\]$' } | ForEach-Object {
    [pscustomobject]@{ Ms = [int]($_ -replace '^.*\[(\d+) ms\]$', '$1'); Line = $_ }
} | Sort-Object Ms -Descending | Select-Object -First 10 | ForEach-Object { "  $($_.Ms) ms  $($_.Line)" }
if ($failed.Count -gt 0) { exit 1 }
