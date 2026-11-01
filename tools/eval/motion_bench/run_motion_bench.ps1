# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
<#
Unattended motion-bench runner skeleton. Dry run by default: prints the planned commands, starts nothing.
  -Clips   clip files or a directory under D:\testclips
  -Matrix  off | diagnostics | <switch> | all   (one or more)
  -OutDir  output directory (per-run csv files, summary.csv, summary.md)
  -Run     really launch VP (needs the GPU): takes D:\codex\GPU-QUIET (max 15 min per chunk), refuses while
           D:\codex\build-slots\PRIORITY exists, releases the lock in finally.
Each run sets VP_MOTION_BENCH=<OutDir>\<clip>__<config>.csv for the child process only.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Clips,
    [string[]]$Matrix = @('off', 'diagnostics', 'all'),
    [Parameter(Mandatory)][string]$OutDir,
    [string]$VpExe = 'C:\Videoprocessor\vp\VideoProcessor.exe',
    [string]$VpWorkDir = 'C:\Videoprocessor\vp',
    [int]$ClipSeconds = 120,
    [int]$ChunkMinutes = 15,
    [switch]$Run
)
$ErrorActionPreference = 'Stop'
$switches = 'vector_model_global', 'vector_guided_candidate', 'vector_narrow_window', 'vector_e1_skip',
            'vector_static_gate', 'vector_cut_skip', 'vector_fade_normalised', 'vector_flash_bridge',
            'vector_temporal_seed', 'vector_tiny_mover_roi'
$lockDir = 'D:\codex\GPU-QUIET'
$priority = 'D:\codex\build-slots\PRIORITY'

# configs: name -> hashtable of config keys forced to 1 (everything else untouched = the deployed config)
$configs = [ordered]@{}
foreach ($m in $Matrix) {
    switch -Regex ($m) {
        '^off$'         { $configs['off'] = @{ _all_off = $true } }
        '^diagnostics$' { $configs['diagnostics'] = @{} }
        '^all$'         { $configs['all'] = @{ _all_on = $true } }
        default {
            if ($switches -notcontains $m) { throw "unknown matrix entry '$m' (off, diagnostics, all, or one of: $($switches -join ', '))" }
            $configs[$m] = @{ $m = 1 }
        }
    }
}

$clipFiles = foreach ($c in $Clips) {
    if (Test-Path -LiteralPath $c -PathType Container) { Get-ChildItem -LiteralPath $c -File | Where-Object Extension -in '.mkv', '.mp4', '.m2ts', '.ts', '.mov' }
    else { Get-Item -LiteralPath $c }
}

$plan = foreach ($clip in $clipFiles) {
    foreach ($cfg in $configs.Keys) {
        $stem = '{0}__{1}' -f ([IO.Path]::GetFileNameWithoutExtension($clip.Name)), $cfg
        [pscustomobject]@{ Clip = $clip.FullName; Config = $cfg; Overrides = $configs[$cfg]; Csv = (Join-Path $OutDir "$stem.csv") }
    }
}

function Show-Plan {
    foreach ($p in $plan) {
        $ov = ($p.Overrides.GetEnumerator() | ForEach-Object { '{0}={1}' -f $_.Key, $_.Value }) -join ' '
        Write-Host ("[plan] VP_MOTION_BENCH={0}  cwd={1}  {2} `"{3}`"  overrides: {4}" -f $p.Csv, $VpWorkDir, $VpExe, $p.Clip, $(if ($ov) { $ov } else { '(deployed config)' }))
    }
    Write-Host ("[plan] then: python -I `"{0}\summarize_bench.py`" --in `"{1}`" --out `"{1}`"" -f $PSScriptRoot, $OutDir)
}

if (-not $Run) {
    Write-Host "DRY RUN (pass -Run to launch VP). $($plan.Count) run(s) planned."
    Show-Plan
    return
}

# --- real run: GPU lock protocol ---
if (Test-Path $priority) { throw "refusing to run: $priority exists" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$locked = $false
$chunkStart = $null
try {
    foreach ($p in $plan) {
        if ($locked -and ((Get-Date) - $chunkStart).TotalMinutes -ge $ChunkMinutes) {
            Remove-Item -LiteralPath $lockDir -Force -Recurse -ErrorAction SilentlyContinue; $locked = $false
            Start-Sleep -Seconds 5   # let others take the GPU between chunks
        }
        if (Test-Path $priority) { throw "stopping: $priority appeared" }
        if (-not $locked) {
            try { New-Item -ItemType Directory -Path $lockDir -ErrorAction Stop | Out-Null }   # mkdir is the atomic lock
            catch { throw "GPU-QUIET is held by someone else; not running" }
            $locked = $true; $chunkStart = Get-Date
        }
        # config overrides are the operator's job to stage in the deployed config; this skeleton only records them
        # (never edits deployed cfg files here). Mark them next to the csv so the summary can be audited.
        ($p.Overrides.GetEnumerator() | ForEach-Object { '{0}={1}' -f $_.Key, $_.Value }) | Set-Content -Path ($p.Csv + '.overrides.txt')
        $env:VP_MOTION_BENCH = $p.Csv
        $proc = $null
        try {
            $proc = Start-Process -FilePath $VpExe -ArgumentList "`"$($p.Clip)`"" -WorkingDirectory $VpWorkDir -PassThru
            if (-not $proc.WaitForExit($ClipSeconds * 1000)) { Stop-Process -Id $proc.Id }   # only the PID launched here
        }
        finally { Remove-Item Env:\VP_MOTION_BENCH -ErrorAction SilentlyContinue }
    }
}
finally {
    if ($locked) { Remove-Item -LiteralPath $lockDir -Force -Recurse -ErrorAction SilentlyContinue }
}
python -I (Join-Path $PSScriptRoot 'summarize_bench.py') --in $OutDir --out $OutDir
