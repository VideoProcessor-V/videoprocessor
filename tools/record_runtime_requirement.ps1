[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BinaryPath,
    [Parameter(Mandatory = $true)][version]$ToolsetVersion
)
$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Utility
. (Join-Path $PSScriptRoot 'runtime_packaging.ps1')
# Build-only sidecar, deliberately excluded from the release package.
# Schema 2 (hunt 2, 2026-10-17): buildId is the VP build ID embedded in the binary
# (tools/generate_build_identity.ps1), read from the binary itself, or null for a
# binary that does not link VideoProcessor-Lib. Packaging requires the EXE and the
# renderer DLLs to carry the same one.
[ordered]@{
    schemaVersion = 2
    configuration = 'Release'
    architecture = 'x64'
    toolsetVersion = $ToolsetVersion.ToString()
    sha256 = (Get-FileHash -LiteralPath $BinaryPath -Algorithm SHA256).Hash
    buildId = (Get-VpBinaryBuildId -FilePath $BinaryPath)
} | ConvertTo-Json | Set-Content -LiteralPath ($BinaryPath + '.runtime.json') -Encoding UTF8
