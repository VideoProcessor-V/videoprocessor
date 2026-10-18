<#
.SYNOPSIS
    Validates a Dynamic Drive device profile (*.vpprojector) offline.

.DESCRIPTION
    Runs VideoProcessor.exe /validate_dd_profile <file>, which applies exactly
    the validator VP itself uses when it loads a profile (DeviceProfile.h).
    Nothing is sent to any projector, no serial port is opened, and capture
    and the renderer are not started.

    Prints "OK ..." with a summary and example command bytes per framing, or
    "INVALID <reason>". Exit code: 0 valid, 1 invalid, 2 usage or tool error.

    See docs\dynamic-drive\PROFILE-AUTHORING-FOR-AI-AGENTS.md.

.PARAMETER Path
    The profile file to validate.

.PARAMETER VideoProcessorExe
    VideoProcessor.exe to use. Default: the first that exists of the x64
    Release build output next to this script's repository, then
    C:\Videoprocessor\vp\VideoProcessor.exe.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\validate_dd_profile.ps1 -Path profiles\dynamic-drive\barco-fl35.vpprojector
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Path,
    [string]$VideoProcessorExe
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    Write-Output "INVALID device profile ${Path}: file not found"
    exit 1
}
$profilePath = (Resolve-Path -LiteralPath $Path).Path

if (-not $VideoProcessorExe) {
    $repository = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path $repository 'x64\Release\VideoProcessor.exe'),
        (Join-Path $repository 'x64\Release\VideoProcessor-GUI.exe'),
        'C:\Videoprocessor\vp\VideoProcessor.exe'
    )
    $VideoProcessorExe = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $VideoProcessorExe) {
        Write-Output ("validate_dd_profile: VideoProcessor.exe not found (looked in: " + ($candidates -join ', ') +
            "); pass -VideoProcessorExe")
        exit 2
    }
}
elseif (-not (Test-Path -LiteralPath $VideoProcessorExe -PathType Leaf)) {
    Write-Output "validate_dd_profile: $VideoProcessorExe not found"
    exit 2
}

$output = [System.IO.Path]::GetTempFileName()
$errors = [System.IO.Path]::GetTempFileName()
try {
    $process = Start-Process -FilePath $VideoProcessorExe `
        -ArgumentList @('/validate_dd_profile', ('"' + $profilePath + '"')) `
        -WorkingDirectory (Split-Path -Parent $VideoProcessorExe) `
        -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $output -RedirectStandardError $errors
    $text = Get-Content -LiteralPath $output -Raw -Encoding UTF8
    if ($text) { Write-Output $text.TrimEnd() }
    $code = $process.ExitCode
    if ($null -eq $code) { $code = 2 }
    if (-not $text -and $code -ne 0) {
        Write-Output "validate_dd_profile: $VideoProcessorExe printed nothing (exit $code); is it a build with /validate_dd_profile?"
        if ($code -ne 1) { $code = 2 }
    }
    exit $code
}
finally {
    Remove-Item -LiteralPath $output, $errors -ErrorAction SilentlyContinue
}
