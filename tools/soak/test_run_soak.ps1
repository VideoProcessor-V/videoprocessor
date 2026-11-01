# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
# Test for run_soak.ps1 (Windows PowerShell 5.1).
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File tools/soak/test_run_soak.ps1
# Uses only a private copy of ping.exe as the dummy child; kills only PIDs it started itself.
param(
    [string]$ScratchRoot = 'C:\Users\ADMIN\AppData\Local\Temp\claude\C--dev-videoprocessor\cf1d27fa-e9e0-4e7a-b3a8-9416d0edfd34\scratchpad'
)
$ErrorActionPreference = 'Continue'
$runner = Join-Path $PSScriptRoot 'run_soak.ps1'
$fail = 0
function Check([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "PASS  $what" } else { Write-Host "FAIL  $what"; $script:fail++ }
}

if (-not (Test-Path -LiteralPath $ScratchRoot)) { New-Item -ItemType Directory -Force -Path $ScratchRoot | Out-Null }
$base = Join-Path $ScratchRoot ('test-tooling-wave1-w5-soak-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$work = Join-Path $base 'work'
$bin = Join-Path $base 'bin'
New-Item -ItemType Directory -Force -Path (Join-Path $work 'logs') | Out-Null
New-Item -ItemType Directory -Force -Path $bin | Out-Null
$exe = Join-Path $bin 'test-tooling-wave1-w5-ping.exe'
Copy-Item -LiteralPath (Join-Path $env:SystemRoot 'System32\PING.EXE') -Destination $exe
Set-Content -LiteralPath (Join-Path $work 'logs\vp.log') -Value "2026-10-31 10:00:00 | VP build identity: module=x commit=https://example.invalid/c/abc branch=b dirty=0 build=1" -Encoding ASCII
Set-Content -LiteralPath (Join-Path $work 'VideoProcessor.cfg') -Value 'key=value' -Encoding ASCII

function Hash-Listing([string]$dir) {
    $rl = $dir.TrimEnd('\') + '\'
    $lines = foreach ($f in (Get-ChildItem -LiteralPath $dir -Recurse -Force)) {
        if ($f.PSIsContainer) { 'D ' + $f.FullName.Substring($rl.Length) }
        else { 'F ' + $f.FullName.Substring($rl.Length) + ' ' + (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash }
    }
    return (($lines | Sort-Object) -join "`n")
}

function Run-Soak([string[]]$argList) {
    $o = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $runner @argList 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Text = $o }
}

$started = New-Object System.Collections.Generic.List[int]
try {
    $before = Hash-Listing $work

    # ---- refusal cases (no filesystem writes) ----
    $r = Run-Soak @('-Exe', $exe, '-WorkingDir', $work, '-Hours', '0.01', '-Out', (Join-Path $work 'out'))
    Check ($r.Code -eq 2) 'Out inside WorkingDir -> exit 2'
    Check (-not (Test-Path -LiteralPath (Join-Path $work 'out'))) 'Out inside WorkingDir: nothing created'

    $dep = 'C:\Videoprocessor\soak-test-w5-must-not-exist'
    $depExisted = Test-Path -LiteralPath $dep
    $r = Run-Soak @('-Exe', $exe, '-WorkingDir', $work, '-Hours', '0.01', '-Out', $dep)
    Check ($r.Code -eq 2) 'Out under C:\Videoprocessor -> exit 2'
    Check ((Test-Path -LiteralPath $dep) -eq $depExisted) 'Out under C:\Videoprocessor: nothing created'

    $r = Run-Soak @('-Exe', (Join-Path $bin 'missing.exe'), '-WorkingDir', $work, '-Hours', '0.01', '-Out', (Join-Path $base 'out-missing'))
    Check ($r.Code -eq 2) 'missing Exe -> exit 2'
    Check (-not (Test-Path -LiteralPath (Join-Path $base 'out-missing'))) 'missing Exe: nothing created'

    # ---- already-running exe: own child from the private copy ----
    $held = Start-Process -FilePath $exe -ArgumentList '-n 60 127.0.0.1' -WindowStyle Hidden -PassThru
    $started.Add($held.Id)
    Start-Sleep -Milliseconds 800
    $r = Run-Soak @('-Exe', $exe, '-WorkingDir', $work, '-Hours', '0.01', '-Out', (Join-Path $base 'out-running'))
    Check ($r.Code -eq 2) 'already-running exe -> exit 2'
    Check ($r.Text -match [string]$held.Id) 'refusal reports the running PID'
    $held.Refresh()
    Check (-not $held.HasExited) 'already-running process left untouched'
    Check (-not (Test-Path -LiteralPath (Join-Path $base 'out-running'))) 'already-running: nothing created'
    Stop-Process -Id $held.Id -Force -ErrorAction SilentlyContinue
    [void]$held.WaitForExit(5000)

    # ---- main run (about 25 s of a roughly 40 s child) ----
    $pingBefore = @(Get-Process -Name 'ping' -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $out = Join-Path $base 'out'
    $r = Run-Soak @('-Exe', $exe, '-WorkingDir', $work, '-Arguments', '-n 40 127.0.0.1', '-Hours', '0.007', '-IntervalSeconds', '2', '-Out', $out)
    Write-Host ($r.Text.Trim())
    Check ($r.Code -eq 3) "runner returns report exit code 3 (INCOMPLETE) for a short run (got $($r.Code))"
    $csv = @(Import-Csv -LiteralPath (Join-Path $out 'soak.csv'))
    Check ($csv.Count -ge 8) "soak.csv has rows ($($csv.Count))"
    $want = 'utc,elapsed_s,private_bytes,working_set,handles,threads,gdi_objects,user_objects,gpu_dedicated,gpu_shared,nvidia_util,nvidia_temp,nvidia_clk_gr,nvidia_clk_mem,log_offset'
    $hdr = (Get-Content -LiteralPath (Join-Path $out 'soak.csv') -TotalCount 1)
    Check ($hdr -eq $want) 'soak.csv header columns'
    Check ($csv.Count -gt 0 -and [int64]$csv[0].private_bytes -gt 0 -and [int]$csv[0].threads -gt 0) 'process counters populated'
    Check ($csv.Count -gt 1 -and [double]$csv[-1].elapsed_s -gt [double]$csv[0].elapsed_s) 'elapsed_s increases'
    Check ($csv.Count -gt 0 -and $csv[0].log_offset -ne '') 'log_offset recorded'
    $ej = Get-Content -LiteralPath (Join-Path $out 'soak-exit.json') -Raw | ConvertFrom-Json
    Check ($ej.crash -eq $false -and $ej.reason -eq 'hours_elapsed') 'soak-exit.json: no crash, hours_elapsed'
    $gone = $true
    try { [void](Get-Process -Id $ej.pid -ErrorAction Stop) } catch { $gone = $false }
    Check (-not $gone) 'launched child is gone'
    $pingAfter = @(Get-Process -Name 'ping' -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $missing = @($pingBefore | Where-Object { $pingAfter -notcontains $_ })
    $extra = @($pingAfter | Where-Object { $pingBefore -notcontains $_ })
    Check ($missing.Count -eq 0 -and $extra.Count -eq 0) 'other ping.exe processes unchanged'
    Check (Test-Path -LiteralPath (Join-Path $out 'collected\vp.log')) 'vp.log copied to collected'
    Check ((Get-Content -LiteralPath (Join-Path $out 'collected\vp.log') -Raw) -eq (Get-Content -LiteralPath (Join-Path $work 'logs\vp.log') -Raw)) 'collected copy equals source'
    Check (Test-Path -LiteralPath (Join-Path $out 'report.json')) 'report.json written'
    Check (@(Get-ChildItem -LiteralPath $out -Filter 'soak-*.md').Count -eq 1) 'soak-YYYYMMDD.md written'
    Check ((Test-Path -LiteralPath (Join-Path $out 'soak-notes.txt')) -and ((Get-Content -LiteralPath (Join-Path $out 'soak-notes.txt') -Raw) -match 'stale log')) 'stale log noted'

    # ---- unexpected early exit is a crash ----
    $out2 = Join-Path $base 'out-crash'
    $r2 = Run-Soak @('-Exe', $exe, '-WorkingDir', $work, '-Arguments', '-n 3 127.0.0.1', '-Hours', '0.01', '-IntervalSeconds', '1', '-Out', $out2)
    $ej2 = Get-Content -LiteralPath (Join-Path $out2 'soak-exit.json') -Raw | ConvertFrom-Json
    Check ($ej2.crash -eq $true -and $ej2.reason -eq 'process_exited') 'early exit recorded as crash'
    Check ($r2.Code -eq 1) "crash -> report FAIL exit 1 (got $($r2.Code))"
    Check (Test-Path -LiteralPath (Join-Path $out2 'triage\crash.json')) 'crash triage seed written'

    $after = Hash-Listing $work
    Check ($before -eq $after) 'WorkingDir contents unchanged (hash listing)'
}
finally {
    foreach ($id in $started) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    if ($base -and (Split-Path -Leaf $base).StartsWith('test-tooling-wave1-w5-') -and (Test-Path -LiteralPath $base)) {
        Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
    }
}

if ($fail -eq 0) { Write-Host 'ALL PASS'; exit 0 }
Write-Host "$fail check(s) FAILED"
exit 1
