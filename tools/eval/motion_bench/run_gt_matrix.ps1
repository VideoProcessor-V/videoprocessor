# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
Ground-truth matrix (MPI-Sintel training clean + final, KITTI 2015 / 2012 flow): NVOF seed / engine / engine + camera / RAFT.
INTERNAL non-commercial evaluation only: the datasets stay in -GtRoot, derived data in -DataRoot, results in -ResultsRoot;
nothing from them is committed.

  .\run_gt_matrix.ps1 [-GtRoot D:\testclips\ground-truth] [-DataRoot D:\codex\videoprocessor\r1cam-gtbench-data]
                      [-ResultsRoot D:\codex\videoprocessor\r1cam-gtbench-results] [-Stages prep,gpu,cpu,report]
                      [-OnlySet sintel_clean,...] [-ChunkSeqs 8] [-ChunkMinutes 12] [-DryRun]

GPU stage (NVOF dump + RAFT) runs only on mains power, with VP_TEST_ADAPTER=NVIDIA, holding the D:\codex\GPU-QUIET directory lock
for one chunk of at most -ChunkMinutes (then released and retaken), never while D:\codex\build-slots\PRIORITY exists; retries every
2 min while the lock is taken; released in finally. Sequences already holding seq.nvf and raft output are skipped quickly.
#>
param(
    [string]$GtRoot = 'D:\testclips\ground-truth',
    [string]$DataRoot = 'D:\codex\videoprocessor\r1cam-gtbench-data',
    [string]$ResultsRoot = 'D:\codex\videoprocessor\r1cam-gtbench-results',
    [string[]]$Stages = @('prep', 'gpu', 'cpu', 'report'),
    [string[]]$OnlySet = @(),
    [int]$SintelFrames = 12,
    [int]$KittiPairs = 0,
    [int]$ChunkSeqs = 8,
    [int]$ChunkMinutes = 12,
    [string]$Python = 'D:\hfr\venv\Scripts\python.exe',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$mb = $PSScriptRoot
$camExe = Join-Path $repo 'tools\camera_eval\bin\camera_eval.exe'
$nvofExe = Join-Path $repo 'tools\td-cli\bin\nvof_dump.exe'
$lock = 'D:\codex\GPU-QUIET'
$priority = 'D:\codex\build-slots\PRIORITY'

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
    param([string[]]$Names)
    while ($true) {
        if (Test-Path $priority) { Write-Host 'PRIORITY exists; waiting 2 min'; if ($DryRun) { return }; Start-Sleep 120; continue }
        if (-not (Test-Mains)) { Write-Host 'not on mains; waiting 2 min'; if ($DryRun) { return }; Start-Sleep 120; continue }
        if ($DryRun) { Write-Host "(dry run) would take $lock for $($Names.Count) sequences"; return }
        try { $null = New-Item -ItemType Directory -Path $lock -ErrorAction Stop; break }
        catch { Write-Host "$lock taken; retry in 2 min"; Start-Sleep 120 }
    }
    try {
        $env:VP_TEST_ADAPTER = 'NVIDIA'
        Invoke-Py (@((Join-Path $mb 'offline_matrix.py'), 'gpu', '--data', $DataRoot, '--nvof-dump', $nvofExe, '--only') + $Names)
    }
    finally {
        Remove-Item Env:\VP_TEST_ADAPTER -ErrorAction SilentlyContinue
        if (Test-Path $lock) { Remove-Item $lock -Force -Recurse }
    }
}

New-Item -ItemType Directory -Force -Path $DataRoot, $ResultsRoot | Out-Null
if ($Stages -contains 'prep') {
    $a = @((Join-Path $mb 'gt_datasets.py'), 'prep', '--data', $DataRoot, '--gt-root', $GtRoot, '--sintel-frames', $SintelFrames, '--kitti-pairs', $KittiPairs)
    if ($OnlySet.Count) { $a += @('--only-set') + $OnlySet }
    Invoke-Py $a
}
$reg = if (Test-Path (Join-Path $DataRoot 'sequences.json')) { @(Get-Content (Join-Path $DataRoot 'sequences.json') -Raw | ConvertFrom-Json) } else { @() }
$names = @($reg | ForEach-Object { $_.name })
if ($Stages -contains 'gpu') {
    $i = 0
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($i -lt $names.Count) {
        # skip sequences that are already complete (no GPU lock needed for them)
        $todo = @()
        while ($i -lt $names.Count -and $todo.Count -lt $ChunkSeqs) {
            $d = Join-Path $DataRoot ('seqs\' + $names[$i])
            if (-not ((Test-Path (Join-Path $d 'seq.nvf')) -and (Test-Path (Join-Path $d 'raft_fwd')))) { $todo += $names[$i] }
            $i++
        }
        if ($todo.Count) {
            $t0 = $sw.Elapsed.TotalSeconds
            Invoke-GpuChunk $todo
            Write-Host ('chunk of {0}: {1:n0} s (limit {2} min)' -f $todo.Count, ($sw.Elapsed.TotalSeconds - $t0), $ChunkMinutes)
        }
    }
}
if ($Stages -contains 'cpu') {
    $a = @((Join-Path $mb 'offline_matrix.py'), 'cpu', '--data', $DataRoot, '--camera-eval', $camExe)
    Invoke-Py $a
}
if ($Stages -contains 'report') {
    Invoke-Py @((Join-Path $mb 'gt_datasets.py'), 'report', '--data', $DataRoot, '--out', $ResultsRoot)
}
