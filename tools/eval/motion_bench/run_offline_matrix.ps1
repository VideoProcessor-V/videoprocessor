# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
Offline motion matrix: NVOF seed / engine matcher / engine + camera / RAFT on synthetic + real clips, scored with
score_flow.py. Writes summary.md / summary.csv to <DataRoot>\results. See offline_matrix.py for the contract.

  .\run_offline_matrix.ps1 [-DataRoot D:\codex\videoprocessor\r1cam-bench-data] [-Stages synth,prep,gpu,cpu,report]
                           [-Only name,...] [-SkipBuild] [-DryRun]

GPU stages (NVOF dump, RAFT) run only on mains power, with VP_TEST_ADAPTER=NVIDIA, one sequence per chunk while holding
the D:\codex\GPU-QUIET directory lock (retry every 2 min while taken, released in finally), and never while
D:\codex\build-slots\PRIORITY exists. Nothing here needs a build slot (camera_eval / nvof_dump are single-file builds).
Data stays outside the repository; frames, flo files and clips are never committed.
#>
param(
    [string]$DataRoot = 'D:\codex\videoprocessor\r1cam-bench-data',
    [string[]]$Stages = @('synth', 'prep', 'gpu', 'cpu', 'report'),
    [string[]]$Only = @(),
    [string]$Python = 'D:\hfr\venv\Scripts\python.exe',
    [switch]$SkipBuild,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$mb = $PSScriptRoot
$camExe = Join-Path $repo 'tools\camera_eval\bin\camera_eval.exe'
$nvofExe = Join-Path $repo 'tools\td-cli\bin\nvof_dump.exe'
$lock = 'D:\codex\GPU-QUIET'
$priority = 'D:\codex\build-slots\PRIORITY'
$vcvars = 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat'

function Invoke-Py {
    param([string[]]$PyArgs)
    Write-Host ('+ python -I ' + ($PyArgs -join ' '))
    if ($DryRun) { return }
    & $Python -I @PyArgs
    if ($LASTEXITCODE -ne 0) { throw "python failed ($LASTEXITCODE)" }
}

function Test-Mains {
    $b = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
    return (-not $b) -or (@($b | Where-Object { $_.BatteryStatus -notin 2, 3, 6, 7, 8, 9 }).Count -eq 0)
}

function Invoke-GpuChunk {
    param([string]$Name)
    while ($true) {
        if (Test-Path $priority) { Write-Host 'PRIORITY exists; waiting 2 min'; if ($DryRun) { return }; Start-Sleep 120; continue }
        if (-not (Test-Mains)) { Write-Host 'not on mains; waiting 2 min'; if ($DryRun) { return }; Start-Sleep 120; continue }
        if ($DryRun) { Write-Host "(dry run) would take $lock for $Name"; break }
        try { $null = New-Item -ItemType Directory -Path $lock -ErrorAction Stop; break }
        catch { Write-Host "$lock taken; retry in 2 min"; Start-Sleep 120 }
    }
    try {
        $env:VP_TEST_ADAPTER = 'NVIDIA'
        $sw = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Py @((Join-Path $mb 'offline_matrix.py'), 'gpu', '--data', $DataRoot, '--nvof-dump', $nvofExe, '--only', $Name)
        Write-Host ("chunk {0}: {1:n0} s" -f $Name, $sw.Elapsed.TotalSeconds)
    }
    finally {
        Remove-Item Env:\VP_TEST_ADAPTER -ErrorAction SilentlyContinue
        if (-not $DryRun -and (Test-Path $lock)) { Remove-Item $lock -Force -Recurse }
    }
}

$camStale = -not (Test-Path $camExe) -or ((Get-Item $camExe).LastWriteTime -lt (Get-ChildItem (Join-Path $repo 'tools\camera_eval\camera_eval.cpp'), (Join-Path $repo 'src\VP-V\modules\motion\estimation\global_model\*.cpp') | Measure-Object LastWriteTime -Maximum).Maximum)
if (-not $SkipBuild -and ($camStale -or -not (Test-Path $nvofExe)) -and -not $DryRun) {
    cmd /c "`"$repo\tools\camera_eval\build_camera_eval.bat`""
    cmd /c "call `"$vcvars`" >nul && `"$repo\tools\td-cli\build_nvof_dump.bat`""
}
New-Item -ItemType Directory -Force -Path $DataRoot | Out-Null
if ($Stages -contains 'synth') { Invoke-Py @((Join-Path $mb 'synth_gt.py'), (Join-Path $DataRoot 'synth')) }
if ($Stages -contains 'prep') { Invoke-Py @((Join-Path $mb 'offline_matrix.py'), 'prep', '--data', $DataRoot) }
if ($Stages -contains 'gpu') {
    $reg = if (Test-Path (Join-Path $DataRoot 'sequences.json')) { Get-Content (Join-Path $DataRoot 'sequences.json') -Raw | ConvertFrom-Json } else { @() }
    foreach ($s in $reg) { if ($Only.Count -eq 0 -or $Only -contains $s.name) { Invoke-GpuChunk $s.name } }
}
if ($Stages -contains 'cpu') {
    $a = @((Join-Path $mb 'offline_matrix.py'), 'cpu', '--data', $DataRoot, '--camera-eval', $camExe, '--check-determinism')
    if ($Only.Count) { $a += @('--only') + $Only }
    Invoke-Py $a
}
if ($Stages -contains 'report') { Invoke-Py @((Join-Path $mb 'offline_matrix.py'), 'report', '--data', $DataRoot) }
