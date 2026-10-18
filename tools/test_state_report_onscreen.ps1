<#
.SYNOPSIS
  GATED GPU / ON-SCREEN TEST: presses the report-a-problem hotkey during live
  playback and validates the bundle VP writes (report-a-problem, 2026-10-17).

.DESCRIPTION
  Not run by CI or the CPU suites. Runs only with -Run or with
  VP_TEST_STATE_REPORT_ONSCREEN=1; otherwise prints SKIP-OPT-IN and exits 0.

  1. Starts VideoProcessor.exe from -VpRoot (working directory = -VpRoot, as
     the deployment rules require) unless a VP from that folder is already
     running (then it uses that one and leaves it running).
  2. Waits until logs\vp.log reports a presented live frame (playback), up to
     -PlaybackTimeoutSeconds. A live source must be connected (the laptop's
     DeckLink + Apple TV screensaver, or the cinema PC's capture card).
  3. Brings VP's window to the foreground and presses the configured chord
     (default Ctrl+Alt+D) with SendInput. With -UseCommand it posts
     WM_COMMAND ID_COMMAND_STATE_REPORT (33207) instead.
  4. Waits for a new logs\reports\vp-report-*.zip and validates it with
     Test-StateReportBundle: store-only zip readable, required entries,
     manifest fields (format_version 1, 32-hex build_id, plugin_api 40 (the current VP_LIBPLACEBO_PLUGIN_API_VERSION),
     SHA-256 of VideoProcessor.exe and the renderer DLL equal to the files on
     disk), renderer history present with presented frames (VP Renderer),
     exe frame rows present, no path outside VP, no user or computer name,
     no thumbnail unless report_thumbnail is on, and vp.log's
     "Problem report saved" line.
  5. Closes VP with WM_CLOSE only when this script started it.

.EXAMPLE
  $env:VP_TEST_STATE_REPORT_ONSCREEN = '1'
  .\tools\test_state_report_onscreen.ps1 -VpRoot C:\Videoprocessor\vp

.EXAMPLE
  # Validate an existing bundle only (no VP, CPU only):
  .\tools\test_state_report_onscreen.ps1 -ValidateOnly C:\Videoprocessor\vp\logs\reports\vp-report-20261017-213045-123.zip -VpRoot C:\Videoprocessor\vp
#>
[CmdletBinding()]
param(
	[string]$VpRoot = 'C:\Videoprocessor\vp',
	[switch]$Run,
	[switch]$UseCommand,
	[string]$Chord = 'Ctrl+Alt+D',
	[int]$PlaybackTimeoutSeconds = 90,
	[int]$ReportTimeoutSeconds = 30,
	[int]$ExpectedPluginApi = 40,
	[string]$ValidateOnly
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$message) { $script:failures.Add($message); Write-Host "FAIL: $message" -ForegroundColor Red }
function Pass([string]$message) { Write-Host "ok:   $message" }

function Get-Sha256([string]$path) {
	if (-not (Test-Path -LiteralPath $path)) { return 'missing' }
	return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-StateReportBundle([string]$zipPath, [string]$root, [bool]$expectRenderer) {
	$archive = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
	try {
		$entries = @{}
		foreach ($entry in $archive.Entries) {
			$reader = New-Object System.IO.StreamReader($entry.Open(), [System.Text.Encoding]::UTF8)
			try { $entries[$entry.FullName] = $reader.ReadToEnd() } finally { $reader.Dispose() }
		}
	} finally { $archive.Dispose() }
	Pass "zip opened: $($entries.Count) entries"

	foreach ($name in 'README.md', 'manifest.json', 'state-history.jsonl', 'log-excerpt.txt',
		'crash-state.txt', 'live-settings.json', 'system.json') {
		if ($entries.ContainsKey($name)) { Pass "entry $name" } else { Fail "missing entry $name" }
	}
	if (-not ($entries.Keys | Where-Object { $_ -like 'config/*' })) { Fail 'no config/* entry' } else { Pass 'config entries' }

	$manifest = $entries['manifest.json'] | ConvertFrom-Json
	if ($manifest.format -ne 'vp-state-report' -or $manifest.format_version -ne 1) { Fail 'manifest format' } else { Pass 'manifest format 1' }
	if ($manifest.build_id -notmatch '^[0-9a-f]{32}$') { Fail "build_id '$($manifest.build_id)'" } else { Pass "build_id $($manifest.build_id)" }
	if ($manifest.plugin_api -ne $ExpectedPluginApi) { Fail "plugin_api $($manifest.plugin_api) != $ExpectedPluginApi" } else { Pass "plugin_api $ExpectedPluginApi" }
	$exe = $manifest.binaries | Where-Object { $_.name -eq 'VideoProcessor.exe' }
	if (-not $exe) { Fail 'no VideoProcessor.exe hash' }
	elseif ($exe.sha256 -ne (Get-Sha256 (Join-Path $root 'VideoProcessor.exe'))) { Fail 'EXE SHA-256 differs from the file' }
	else { Pass 'EXE SHA-256 matches' }
	$dll = $manifest.binaries | Where-Object { $_.name -eq 'vprenderer/VideoProcessorVPRenderer.dll' }
	if (-not $dll) { Fail 'no renderer DLL hash' }
	elseif ($dll.sha256 -ne (Get-Sha256 (Join-Path $root 'vprenderer\VideoProcessorVPRenderer.dll'))) { Fail 'DLL SHA-256 differs from the file' }
	else { Pass 'DLL SHA-256 matches' }

	$rows = $entries['state-history.jsonl'] -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json }
	$exeFrames = @($rows | Where-Object { $_.src -eq 'exe' -and $_.ring -eq 'frame' -and $_.stage -eq 'capture_ingress' })
	if ($exeFrames.Count -lt 30) { Fail "exe capture_ingress rows: $($exeFrames.Count)" } else { Pass "exe capture_ingress rows: $($exeFrames.Count)" }
	$exeStats = @($rows | Where-Object { $_.src -eq 'exe' -and $_.ring -eq 'stats' })
	if ($exeStats.Count -lt 2) { Fail "exe stats rows: $($exeStats.Count)" } else { Pass "exe stats rows: $($exeStats.Count)" }
	$reportDecision = @($rows | Where-Object { $_.ring -eq 'decision' -and $_.name -eq 'state-report' })
	if ($reportDecision.Count -lt 1) { Fail 'no state-report decision row' } else { Pass 'state-report decision row' }
	if ($expectRenderer) {
		if (-not $manifest.renderer_history) { Fail 'renderer_history false' } else { Pass 'renderer contributed history' }
		$presented = @($rows | Where-Object { $_.src -eq 'renderer' -and $_.ring -eq 'frame' -and $_.stage -eq 'presented' })
		if ($presented.Count -lt 30) { Fail "renderer presented rows: $($presented.Count)" } else { Pass "renderer presented rows: $($presented.Count)" }
		$late = @($rows | Where-Object { $_.ring -eq 'frame' -and $_.t_ms -gt 1000 })
		if ($late.Count) { Fail "$($late.Count) rows after the report moment" }
	}

	# Privacy: nothing outside VP, no user or computer name (thumbnail excluded).
	$user = $env:USERNAME; $hostName = $env:COMPUTERNAME
	foreach ($name in $entries.Keys) {
		if ($name -eq 'thumbnail.png') { continue }
		$text = $entries[$name]
		if ($text -match '(?i)\b[A-Z]:\\Users\\') { Fail "$name contains a user-profile path" }
		if ($user.Length -ge 3 -and $text -match ('(?i)(?<![A-Za-z0-9])' + [regex]::Escape($user) + '(?![A-Za-z0-9])')) { Fail "$name contains the user name" }
		if ($hostName.Length -ge 3 -and $text -match ('(?i)(?<![A-Za-z0-9])' + [regex]::Escape($hostName) + '(?![A-Za-z0-9])')) { Fail "$name contains the computer name" }
		if ($text.IndexOf($root, [StringComparison]::OrdinalIgnoreCase) -ge 0) { Fail "$name contains the VP folder path unredacted" }
	}
	Pass 'privacy scan done'

	$thumbnailOn = $false
	$settings = $entries['live-settings.json'] | ConvertFrom-Json
	if ($settings.'logging.report_thumbnail' -eq 'true') { $thumbnailOn = $true }
	if (-not $thumbnailOn -and $entries.ContainsKey('thumbnail.png')) { Fail 'thumbnail.png present with report_thumbnail off' }
	if ($thumbnailOn -and -not ($entries.ContainsKey('thumbnail.png') -or $entries.ContainsKey('thumbnail-missing.txt'))) { Fail 'report_thumbnail on but no thumbnail entry' }
}

if ($ValidateOnly) {
	Test-StateReportBundle -zipPath $ValidateOnly -root $VpRoot -expectRenderer $true
	if ($failures.Count) { Write-Host "STATE REPORT BUNDLE: $($failures.Count) failure(s)"; exit 1 }
	Write-Host 'STATE REPORT BUNDLE: PASS'; exit 0
}

if (-not $Run -and $env:VP_TEST_STATE_REPORT_ONSCREEN -ne '1') {
	Write-Host 'SKIP-OPT-IN test_state_report_onscreen: needs a live source, the GPU and the screen; pass -Run or set VP_TEST_STATE_REPORT_ONSCREEN=1'
	exit 0
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class VpStateReportInput {
	[StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
	[StructLayout(LayoutKind.Explicit)] public struct INPUTUNION { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public long pad0; [FieldOffset(8)] public long pad1; [FieldOffset(16)] public long pad2; }
	[StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
	[DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
	[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr w, IntPtr l);
	public static void Key(ushort vk, bool up) {
		INPUT[] i = new INPUT[1]; i[0].type = 1; i[0].u.ki.wVk = vk; i[0].u.ki.dwFlags = up ? 2u : 0u;
		SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
	}
}
'@

$exePath = Join-Path $VpRoot 'VideoProcessor.exe'
if (-not (Test-Path -LiteralPath $exePath)) { throw "VideoProcessor.exe not found in $VpRoot" }
$logPath = Join-Path $VpRoot 'logs\vp.log'
$reportsDir = Join-Path $VpRoot 'logs\reports'
$before = @{}
if (Test-Path $reportsDir) { Get-ChildItem $reportsDir -Filter 'vp-report-*.zip' | ForEach-Object { $before[$_.Name] = $true } }

$process = Get-Process VideoProcessor -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path -ieq $exePath) } | Select-Object -First 1
$started = $false
if (-not $process) {
	$logOffset = if (Test-Path $logPath) { (Get-Item $logPath).Length } else { 0 }
	$process = Start-Process -FilePath $exePath -WorkingDirectory $VpRoot -PassThru
	$started = $true
	Write-Host "Started VideoProcessor (pid $($process.Id))"
} else {
	$logOffset = 0
	Write-Host "Using the running VideoProcessor (pid $($process.Id))"
}

try {
	# 2. Wait for playback.
	$deadline = (Get-Date).AddSeconds($PlaybackTimeoutSeconds)
	$playing = $false
	while ((Get-Date) -lt $deadline -and -not $playing) {
		Start-Sleep -Milliseconds 500
		# The crash-state file (rewritten every second) names the source and
		# the renderer; playback = a source mode plus the VP Renderer.
		$statePath = Join-Path $VpRoot 'logs\crash-state.txt'
		if (Test-Path $statePath) {
			$state = Get-Content $statePath -ErrorAction SilentlyContinue | Out-String
			if ($state -match '(?m)^source: \d+x\d+' -and $state -match '(?m)^renderer: VP Renderer' -and
				$state -match ('(?m)^pid: ' + $process.Id + '\s*$')) { $playing = $true }
		}
	}
	if (-not $playing) { Fail "no live playback within $PlaybackTimeoutSeconds s (is a source connected?)" }
	Start-Sleep -Seconds 8   # let the frame ring fill with real playback

	# 3. Press the hotkey.
	$process.Refresh()
	$hwnd = $process.MainWindowHandle
	if ($UseCommand) {
		[void][VpStateReportInput]::PostMessage($hwnd, 0x0111, [IntPtr]33207, [IntPtr]::Zero)
		Write-Host 'Posted WM_COMMAND ID_COMMAND_STATE_REPORT'
	} else {
		[void][VpStateReportInput]::SetForegroundWindow($hwnd)
		Start-Sleep -Milliseconds 300
		$mods = @()
		$key = $null
		foreach ($part in $Chord.Split('+')) {
			switch -regex ($part.Trim()) {
				'^(?i)ctrl|control$' { $mods += 0x11; break }
				'^(?i)alt$' { $mods += 0x12; break }
				'^(?i)shift$' { $mods += 0x10; break }
				default { $key = [int][char]($part.Trim().ToUpperInvariant()[0]) }
			}
		}
		foreach ($m in $mods) { [VpStateReportInput]::Key([uint16]$m, $false) }
		[VpStateReportInput]::Key([uint16]$key, $false)
		Start-Sleep -Milliseconds 60
		[VpStateReportInput]::Key([uint16]$key, $true)
		[array]::Reverse($mods)
		foreach ($m in $mods) { [VpStateReportInput]::Key([uint16]$m, $true) }
		Write-Host "Pressed $Chord"
	}

	# 4. Wait for the new bundle.
	$deadline = (Get-Date).AddSeconds($ReportTimeoutSeconds)
	$zip = $null
	while ((Get-Date) -lt $deadline -and -not $zip) {
		Start-Sleep -Milliseconds 500
		if (Test-Path $reportsDir) {
			$zip = Get-ChildItem $reportsDir -Filter 'vp-report-*.zip' | Where-Object { -not $before.ContainsKey($_.Name) } |
				Sort-Object Name | Select-Object -Last 1
		}
	}
	if (-not $zip) { Fail "no new report within $ReportTimeoutSeconds s" }
	else {
		Pass "report written: $($zip.Name) ($($zip.Length) bytes)"
		Test-StateReportBundle -zipPath $zip.FullName -root $VpRoot -expectRenderer $true
		$count = @(Get-ChildItem $reportsDir -Filter 'vp-report-*.zip').Count
		if ($count -gt 20) { Fail "$count reports kept (limit 20)" } else { Pass "$count report(s) kept" }
		$log = Get-Content $logPath -Tail 200 | Out-String
		if ($log -notmatch 'Problem report saved') { Fail 'vp.log has no "Problem report saved" line' } else { Pass 'vp.log confirms the save' }
	}
} finally {
	if ($started -and -not $process.HasExited) {
		[void][VpStateReportInput]::PostMessage($process.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
		if (-not $process.WaitForExit(20000)) { Write-Host "VP (pid $($process.Id)) did not exit within 20 s; left running for inspection" }
	}
}

if ($failures.Count) { Write-Host "test_state_report_onscreen: FAIL ($($failures.Count))"; exit 1 }
Write-Host 'test_state_report_onscreen: PASS'
exit 0
