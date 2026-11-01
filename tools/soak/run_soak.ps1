# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
.SYNOPSIS
  Soak-test runner: launches VideoProcessor, samples the launched PID only, collects logs, runs report.py.
.DESCRIPTION
  Windows PowerShell 5.1 compatible. Never copies, deploys, edits or deletes anything in -WorkingDir;
  it only launches the exe and reads. Exit codes: 2 refused/usage, 4 report could not run,
  otherwise the report.py exit code (0 PASS, 1 FAIL, 3 INCOMPLETE).
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$WorkingDir,
    [string]$Arguments,
    [double]$Hours = 1.0,
    [int]$IntervalSeconds = 10,
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'
$inv = [System.Globalization.CultureInfo]::InvariantCulture

function Refuse([string]$msg) {
    [Console]::Error.WriteLine("run_soak: REFUSED: $msg")
    exit 2
}

function Norm([string]$p) {
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}

function IsUnder([string]$child, [string]$parent) {
    $c = (Norm $child) + '\'
    $p = (Norm $parent) + '\'
    return $c.StartsWith($p, [System.StringComparison]::OrdinalIgnoreCase)
}

# ---- validation: nothing is written to the file system before every check has passed ----
if ($IntervalSeconds -lt 1) { Refuse "-IntervalSeconds must be at least 1" }
if ($Hours -le 0) { Refuse "-Hours must be greater than 0" }
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { Refuse "-Exe does not exist: $Exe" }
$exeFull = Norm $Exe
if ([string]::IsNullOrEmpty($WorkingDir)) { $WorkingDir = Split-Path -Parent $exeFull }
if (-not (Test-Path -LiteralPath $WorkingDir -PathType Container)) { Refuse "-WorkingDir does not exist: $WorkingDir" }
$workFull = Norm $WorkingDir
$outFull = Norm $Out
if (IsUnder $outFull $workFull) { Refuse "-Out ($outFull) is inside -WorkingDir ($workFull)" }
if (IsUnder $outFull 'C:\Videoprocessor') { Refuse "-Out ($outFull) is under C:\Videoprocessor" }

$running = @()
foreach ($proc in (Get-Process -ErrorAction SilentlyContinue)) {
    $pp = $null
    try { $pp = $proc.Path } catch { $pp = $null }
    if ($pp -and ([string]::Equals((Norm $pp), $exeFull, [System.StringComparison]::OrdinalIgnoreCase))) {
        $running += $proc.Id
    }
}
if ($running.Count -gt 0) {
    Refuse ("a process from this exe is already running (PID {0}); it was not touched" -f ($running -join ','))
}

$reportPy = Join-Path $PSScriptRoot 'report.py'
if (-not (Test-Path -LiteralPath $reportPy)) { Refuse "report.py not found next to the runner: $reportPy" }

# ---- win32 helpers ----
if (-not ('VpSoakNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class VpSoakNative {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint GetGuiResources(IntPtr h, uint flags);
}
'@
}

function Get-GuiCounts([int]$ProcId) {
    $h = [VpSoakNative]::OpenProcess(0x1000, $false, [uint32]$ProcId)  # PROCESS_QUERY_LIMITED_INFORMATION
    if ($h -eq [IntPtr]::Zero) { return @('', '') }
    try {
        return @([VpSoakNative]::GetGuiResources($h, 0).ToString($inv), [VpSoakNative]::GetGuiResources($h, 1).ToString($inv))
    } finally { [void][VpSoakNative]::CloseHandle($h) }
}

function Get-GpuMem([int]$ProcId) {
    $res = @('', '')
    $names = @('Dedicated Usage', 'Shared Usage')
    for ($i = 0; $i -lt 2; $i++) {
        try {
            $c = Get-Counter -Counter ("\GPU Process Memory(pid_{0}_*)\{1}" -f $ProcId, $names[$i]) -ErrorAction Stop
            $sum = 0.0
            $n = 0
            foreach ($s in $c.CounterSamples) { $sum += $s.CookedValue; $n++ }
            if ($n -gt 0) { $res[$i] = ([int64]$sum).ToString($inv) }
        } catch { $res[$i] = '' }
    }
    return $res
}

$nvsmi = Get-Command 'nvidia-smi' -ErrorAction SilentlyContinue
function Get-Nvidia() {
    $empty = @('', '', '', '')
    if (-not $nvsmi) { return $empty }
    try {
        $line = & $nvsmi.Source '--query-gpu=utilization.gpu,temperature.gpu,clocks.gr,clocks.mem' '--format=csv,noheader,nounits' 2>$null | Select-Object -First 1
        if (-not $line) { return $empty }
        $parts = @(([string]$line).Split(',') | ForEach-Object { $_.Trim() })
        if ($parts.Count -ne 4) { return $empty }
        return $parts
    } catch { return $empty }
}

# ---- start ----
New-Item -ItemType Directory -Force -Path $outFull | Out-Null
$csv = Join-Path $outFull 'soak.csv'
$notes = Join-Path $outFull 'soak-notes.txt'
$exitJson = Join-Path $outFull 'soak-exit.json'
$header = 'utc,elapsed_s,private_bytes,working_set,handles,threads,gdi_objects,user_objects,gpu_dedicated,gpu_shared,nvidia_util,nvidia_temp,nvidia_clk_gr,nvidia_clk_mem,log_offset'
Set-Content -LiteralPath $csv -Value $header -Encoding ASCII

$launch = Get-Date
$launchUtc = [DateTime]::UtcNow
$startArgs = @{ FilePath = $exeFull; WorkingDirectory = $workFull; PassThru = $true }
if (-not [string]::IsNullOrEmpty($Arguments)) { $startArgs['ArgumentList'] = $Arguments }
$p = Start-Process @startArgs
$procId = $p.Id
Write-Host ("run_soak: launched PID {0}, {1} h, every {2} s, out {3}" -f $procId, $Hours, $IntervalSeconds, $outFull)

$logPath = Join-Path $workFull 'logs\vp.log'
$staleWarned = $false
$logFresh = $false
$reason = 'interrupted'
$crash = $false
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$limitS = $Hours * 3600.0
$i = 0

function Note([string]$msg) {
    Write-Host "run_soak: $msg"
    Add-Content -LiteralPath $notes -Value ("{0} {1}" -f [DateTime]::UtcNow.ToString('o'), $msg)
}

try {
    while ($true) {
        $p.Refresh()
        if ($p.HasExited) { $reason = 'process_exited'; $crash = $true; break }
        $el = $sw.Elapsed.TotalSeconds
        $gui = Get-GuiCounts $procId
        $gpu = Get-GpuMem $procId
        $nv = Get-Nvidia
        $logOff = ''
        if (Test-Path -LiteralPath $logPath) {
            $li = Get-Item -LiteralPath $logPath
            $logOff = $li.Length.ToString($inv)
            if ($li.LastWriteTime -gt $launch) { $logFresh = $true }
        }
        if (-not $logFresh -and -not $staleWarned -and $i -ge 2) {
            Note "stale log: $logPath is missing or not modified since launch"
            $staleWarned = $true
        }
        $row = @([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'), $el.ToString('0.0', $inv),
            $p.PrivateMemorySize64.ToString($inv), $p.WorkingSet64.ToString($inv),
            $p.HandleCount.ToString($inv), $p.Threads.Count.ToString($inv),
            $gui[0], $gui[1], $gpu[0], $gpu[1], $nv[0], $nv[1], $nv[2], $nv[3], $logOff)
        Add-Content -LiteralPath $csv -Value ($row -join ',') -Encoding ASCII
        $i++
        $next = $i * $IntervalSeconds
        if ($next -ge $limitS) {
            $left = $limitS - $sw.Elapsed.TotalSeconds
            if ($left -gt 0) { Start-Sleep -Milliseconds ([int]($left * 1000)) }
            $reason = 'hours_elapsed'
            break
        }
        $wait = $next - $sw.Elapsed.TotalSeconds
        if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
    }
} finally {
    # PID-only stop. Never by name.
    $p.Refresh()
    if (-not $p.HasExited) {
        [void]$p.CloseMainWindow()
        if (-not $p.WaitForExit(15000)) {
            Stop-Process -Id $procId -Force -ErrorAction SilentlyContinue
            [void]$p.WaitForExit(5000)
        }
    }
    $code = $null
    try { $p.Refresh(); if ($p.HasExited) { $code = $p.ExitCode } } catch { $code = $null }
    if ($reason -eq 'process_exited' -and $code -eq 0) { $crash = $true }
    $info = [ordered]@{
        pid = $procId; exit_code = $code; crash = $crash; reason = $reason
        launch_utc = $launchUtc.ToString('o'); end_utc = [DateTime]::UtcNow.ToString('o')
    }
    ($info | ConvertTo-Json) | Set-Content -LiteralPath $exitJson -Encoding UTF8
    if (-not $logFresh -and -not $staleWarned) {
        Note "stale log: $logPath is missing or not modified since launch"
    }

    # copy (never move) logs and diagnostics produced since launch
    $col = Join-Path $outFull 'collected'
    New-Item -ItemType Directory -Force -Path $col | Out-Null
    foreach ($n in @('vp.log', 'vp.log.0')) {
        $src = Join-Path $workFull ('logs\' + $n)
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $col $n) -Force }
    }
    $repDir = Join-Path $workFull 'logs\reports'
    if (Test-Path -LiteralPath $repDir) {
        foreach ($f in (Get-ChildItem -LiteralPath $repDir -Filter '*.zip' -File | Where-Object { $_.LastWriteTime -gt $launch })) {
            Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $col $f.Name) -Force
        }
    }
    foreach ($d in @($workFull, (Join-Path $workFull 'logs'))) {
        if (Test-Path -LiteralPath $d) {
            foreach ($f in (Get-ChildItem -LiteralPath $d -Filter 'crash state (*).txt' -File | Where-Object { $_.LastWriteTime -gt $launch })) {
                Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $col $f.Name) -Force
            }
        }
    }
}

# ---- report ----
$py = Get-Command 'python' -ErrorAction SilentlyContinue
if (-not $py) {
    [Console]::Error.WriteLine('run_soak: python not found; run report.py manually')
    exit 4
}
& $py.Source -I $reportPy --out $outFull
exit $LASTEXITCODE
