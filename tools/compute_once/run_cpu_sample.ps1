# Copyright(C) 2026 VP-V contributors
# SPDX-License-Identifier: GPL-3.0-only
#
# COMPUTE-ONCE acceptance + CPU sampling procedure (iface-L-DET section 3).
# Runs RendererCaptureHarnessTests::ComputeOnceRealClipAcceptance alone (its own vstest process: the renderer DLL reads
# VP_DEBUG_COMPUTE_ONCE once) on the hardware GPU, with VP_TEST_CPU_SAMPLE=1 unless -NoCpuSample, and extracts every
# "Compute-once" line (unit counters, DUPLICATE / HIDDEN-COPY / gap rows, GPU owners and UNATTRIBUTED passes, the CPU
# owners and UNATTRIBUTED hot functions, the summaries) from the TRX into -Summary.
# The GPU is taken through D:\codex\GPU-QUIET (mkdir + owner.txt) only when no train waits for it, and released after the
# vstest process has exited.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\compute_once\run_cpu_sample.ps1 -Summary D:\...\co.txt
param(
	[string]$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
	[string]$ResultsDir = '',
	[Parameter(Mandatory = $true)][string]$Summary,
	[string]$AgentId = 'compute-once',
	[switch]$NoCpuSample,
	[int]$MaxMinutes = 15
)
$ErrorActionPreference = 'Stop'
function Out-Line([string]$text) {
	for ($i = 0; $i -lt 20; $i++) {
		try {
			$fs = [System.IO.File]::Open($Summary, 'Append', 'Write', 'ReadWrite')
			$w = New-Object System.IO.StreamWriter($fs)
			$w.WriteLine($text); $w.Close(); return
		} catch { Start-Sleep -Milliseconds 100 }
	}
}
Set-Content -Path $Summary -Value $PID -Encoding ascii
$lock = 'D:\codex\GPU-QUIET'
$held = $false
$status = 'ABORT'
try {
	if (-not $ResultsDir) { $ResultsDir = Join-Path $env:TEMP 'vp-compute-once-trx' }
	New-Item -ItemType Directory -Force $ResultsDir | Out-Null
	$dll = Join-Path $Repo 'x64\Release\VideoProcessor-Test.dll'
	if (-not (Test-Path $dll)) { Out-Line "no test DLL at $dll"; $status = 'FAIL'; return }
	$vstest = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\TestWindow\vstest.console.exe'
	if (-not (Test-Path $vstest)) { Out-Line 'vstest.console.exe not found'; $status = 'FAIL'; return }
	$deadline = (Get-Date).AddMinutes(60)
	while (-not $held) {
		if ((Get-Date) -gt $deadline) { Out-Line 'GPU-QUIET not obtained in 60 min'; return }
		if (-not (Test-Path 'D:\codex\TRAIN-GPU-WAITING')) {
			try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null; $held = $true } catch { }
		}
		if (-not $held) { Start-Sleep -Seconds 20 }
	}
	$start = Get-Date
	Set-Content -Path (Join-Path $lock 'owner.txt') -Encoding ascii -Value @(
		"agent $AgentId", "pid $PID", "start $($start.ToString('s'))", "planned-end $($start.AddMinutes($MaxMinutes).ToString('s'))",
		'job ComputeOnceRealClipAcceptance')
	$env:VP_TEST_GPU = 'hardware'; $env:VP_TEST_REQUIRE_GPU = '1'; $env:VP_TEST_ADAPTER = 'NVIDIA'
	if ($NoCpuSample) { Remove-Item Env:VP_TEST_CPU_SAMPLE -ErrorAction SilentlyContinue } else { $env:VP_TEST_CPU_SAMPLE = '1' }
	$vsArgs = @($dll, '/Tests:ComputeOnceRealClipAcceptance', '/Logger:trx', "/ResultsDirectory:$ResultsDir")
	$p = Start-Process -FilePath $vstest -ArgumentList $vsArgs -NoNewWindow -PassThru
	Out-Line "vstest pid $($p.Id)"
	if (-not $p.WaitForExit($MaxMinutes * 60 * 1000)) {
		Out-Line "vstest exceeded $MaxMinutes min; stopping pid $($p.Id) (launched here)"
		Stop-Process -Id $p.Id -Force
		$p.WaitForExit()
	}
	$trx = Get-ChildItem $ResultsDir -Filter *.trx | Sort-Object LastWriteTime -Descending | Select-Object -First 1
	if (-not $trx) { Out-Line 'no TRX'; $status = 'FAIL'; return }
	Out-Line "trx $($trx.FullName)"
	[xml]$x = Get-Content $trx.FullName -Raw
	$c = $x.TestRun.ResultSummary.Counters
	Out-Line "counters total $($c.total) passed $($c.passed) failed $($c.failed) notExecuted $($c.notExecuted)"
	foreach ($r in $x.TestRun.Results.UnitTestResult) {
		Out-Line "result $($r.testName) $($r.outcome)"
		$text = [string]$r.Output.StdOut
		foreach ($l in ($text -split "`r?`n")) {
			if ($l -match 'Compute-once|SKIP-NO-FOOTAGE|== compute-once') { Out-Line $l.Trim() }
		}
		$err = [string]$r.Output.ErrorInfo.Message
		if ($err) { Out-Line "error $err" }
	}
	$status = if ($c.failed -eq '0' -and $c.passed -ne '0') { 'DONE' } else { 'FAIL' }
} catch {
	Out-Line "exception $($_.Exception.Message)"
	$status = 'FAIL'
} finally {
	if ($held) { Remove-Item -Recurse -Force $lock -ErrorAction SilentlyContinue }
	Out-Line $status
}
