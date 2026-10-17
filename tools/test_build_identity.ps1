# Hunt 2 (2026-10-17): EXE/DLL build identity regression checks (CPU only; nothing is
# installed, executed or deployed). Synthetic fixtures always run. With -BuildRoot
# (an x64 Release build tree) the real VideoProcessor-GUI.exe and renderer DLLs are
# checked as well: same embedded build ID, current schema-2 records, and the
# packaging check accepts the real pair. Run by CI after the solution build.
[CmdletBinding()]
param([string]$BuildRoot)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'runtime_packaging.ps1')
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('vp-build-identity-tests-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $fixtureRoot
$script:checks = 0
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL $Message" }
    $script:checks++
    Write-Host "PASS $Message"
}
function Assert-Throws([scriptblock]$Action, [string]$Pattern, [string]$Message) {
    $caught = $false
    try { & $Action | Out-Null } catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw "FAIL $Message (unexpected error: $($_.Exception.Message))" }
        $caught = $true
    }
    Assert-True $caught $Message
}
$latin1 = [Text.Encoding]::GetEncoding(28591)
function New-FakeBinary([string]$Path, [string[]]$Ids) {
    # Random filler around the markers, as in a real PE image.
    $bytes = [Collections.Generic.List[byte]]::new()
    $random = [Random]::new(7)
    $filler = [byte[]]::new(4096)
    $random.NextBytes($filler); $bytes.AddRange($filler)
    foreach ($id in $Ids) {
        $bytes.AddRange($latin1.GetBytes("VP-BUILD-ID:$id"))
        $bytes.Add(0)
        $random.NextBytes($filler); $bytes.AddRange($filler)
    }
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force
    [IO.File]::WriteAllBytes($Path, $bytes.ToArray())
}
function Write-Record([string]$Binary, $BuildId, [int]$Schema = 2) {
    $record = [ordered]@{
        schemaVersion = $Schema; configuration = 'Release'; architecture = 'x64'
        toolsetVersion = '14.29.30133'
        sha256 = (Get-FileHash -LiteralPath $Binary -Algorithm SHA256).Hash
    }
    if ($Schema -ge 2) { $record.buildId = $BuildId }
    $record | ConvertTo-Json | Set-Content -LiteralPath ($Binary + '.runtime.json') -Encoding UTF8
}

try {
    # --- Binary scan -------------------------------------------------------------
    $idA = '0123456789abcdef0123456789abcdef'
    $idB = 'fedcba9876543210fedcba9876543210'
    $one = Join-Path $fixtureRoot 'one.dll'
    New-FakeBinary $one @($idA, $idA)
    Assert-True ((Get-VpBinaryBuildId $one) -eq $idA) 'embedded build ID is read from the binary (repeated marker allowed)'
    $none = Join-Path $fixtureRoot 'none.dll'
    New-FakeBinary $none @()
    Assert-True ($null -eq (Get-VpBinaryBuildId $none)) 'binary without a marker has no build ID'
    $two = Join-Path $fixtureRoot 'two.dll'
    New-FakeBinary $two @($idA, $idB)
    Assert-Throws { Get-VpBinaryBuildId $two } 'more than one VP build ID' 'two different IDs in one binary are rejected'
    $long = Join-Path $fixtureRoot 'long.dll'
    New-FakeBinary $long @($idA + 'ab')
    Assert-True ($null -eq (Get-VpBinaryBuildId $long)) 'a longer hex run is not taken as an ID'

    # --- Paired packaging check ----------------------------------------------------
    $paired = @('VideoProcessor.exe', 'vprenderer/VideoProcessorVPRenderer.dll', 'vprenderer-beta/VideoProcessorVPRendererBeta.dll')
    function New-Pair([string]$Name, [string]$ExeId, [string]$DllId, [string]$BetaId) {
        $root = Join-Path $fixtureRoot $Name
        $plan = @()
        foreach ($item in @(@('VideoProcessor-GUI.exe', 'VideoProcessor.exe', $ExeId),
                @('vprenderer\VideoProcessorVPRenderer.dll', 'vprenderer\VideoProcessorVPRenderer.dll', $DllId),
                @('vprenderer-beta\VideoProcessorVPRendererBeta.dll', 'vprenderer-beta\VideoProcessorVPRendererBeta.dll', $BetaId))) {
            $source = Join-Path $root $item[0]
            New-FakeBinary $source @($item[2])
            Write-Record $source $item[2]
            $plan += [pscustomobject]@{ Source = $source; RelativeDestination = $item[1] }
        }
        return ,$plan
    }
    $plan = New-Pair 'match' $idA $idA $idA
    Assert-True ((Assert-VpPairedBuildIdentity $plan $paired) -eq $idA) 'matching EXE and DLL build IDs pass packaging'

    $plan = New-Pair 'stale-dll' $idB $idA $idB
    Assert-Throws { Assert-VpPairedBuildIdentity $plan $paired } 'build ID mismatch.*VideoProcessorVPRenderer\.dll=' 'GUI-only build with a stale renderer DLL fails packaging'
    $plan = New-Pair 'stale-beta' $idA $idA $idB
    Assert-Throws { Assert-VpPairedBuildIdentity $plan $paired } 'build ID mismatch' 'stale Beta renderer DLL fails packaging'

    $plan = New-Pair 'record-lies' $idA $idA $idA
    Write-Record $plan[1].Source $idB
    Assert-Throws { Assert-VpPairedBuildIdentity $plan $paired } 'names build ID .* carries' 'record whose build ID differs from the binary is rejected'

    $plan = New-Pair 'schema1' $idA $idA $idA
    Write-Record $plan[0].Source $null 1
    Assert-Throws { Assert-VpPairedBuildIdentity $plan $paired } 'Stale or invalid runtime build record' 'schema-1 record without a build ID requires a rebuild'

    $plan = New-Pair 'stale-hash' $idA $idA $idA
    New-FakeBinary $plan[1].Source @($idA, $idA)
    Assert-Throws { Assert-VpPairedBuildIdentity $plan $paired } 'Stale or invalid runtime build record' 'binary changed after its record was written is rejected'

    $plan = New-Pair 'no-marker' $idA $idA $idA
    New-FakeBinary $plan[2].Source @()
    Write-Record $plan[2].Source $null
    Assert-Throws { Assert-VpPairedBuildIdentity $plan $paired } 'has no VP build ID' 'paired binary without a build ID is rejected'

    $plan = New-Pair 'missing-record' $idA $idA $idA
    Remove-Item -LiteralPath ($plan[1].Source + '.runtime.json')
    Assert-Throws { Assert-VpPairedBuildIdentity $plan $paired } 'Missing build runtime record' 'missing record is rejected'

    $plan = New-Pair 'missing-artifact' $idA $idA $idA
    Assert-Throws { Assert-VpPairedBuildIdentity @($plan[0], $plan[1]) $paired } 'Missing/duplicate paired build artifact' 'missing paired artifact is rejected'
    Assert-Throws { Assert-VpPairedBuildIdentity $plan @('VideoProcessor.exe') } 'at least two' 'a single paired artifact is not a pairing policy'

    $manifest = Get-Content -LiteralPath (Join-Path $repositoryRoot 'packaging\release-manifest.json') -Raw | ConvertFrom-Json
    Assert-True ((@($manifest.buildIdentity.pairedArtifacts) -join '|') -eq ($paired -join '|')) 'release manifest pairs VideoProcessor.exe with both renderer DLLs'

    # --- Generator ----------------------------------------------------------------
    # A synthetic tree without .git: the ID must not need Git, must be deterministic,
    # and must change with any source byte or build input.
    $tree = Join-Path $fixtureRoot 'tree'
    foreach ($i in 1..12) {
        $file = Join-Path $tree ("src\Project\file$i.cpp")
        $null = New-Item -ItemType Directory -Path (Split-Path -Parent $file) -Force
        Set-Content -LiteralPath $file -Value "int f$i() { return $i; }" -Encoding ASCII
    }
    $null = New-Item -ItemType Directory -Path (Join-Path $tree '3rdparty\lib') -Force
    Set-Content -LiteralPath (Join-Path $tree '3rdparty\lib\api.h') -Value '#pragma once' -Encoding ASCII
    $generator = Join-Path $PSScriptRoot 'generate_build_identity.ps1'
    $header = Join-Path $fixtureRoot 'gen\VpBuildIdentity.generated.h'
    function Get-GeneratedId([string]$Inputs = 'Configuration=Release;Platform=x64') {
        $ErrorActionPreference = 'Continue'
        $output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $generator -RepositoryRoot $tree -OutputFile $header -BuildInputs $Inputs 2>&1
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -ne 0) { throw "generator failed: $output" }
        $text = [IO.File]::ReadAllText($header)
        if ($text -notmatch '#define VP_BUILD_ID "([0-9a-f]{32})"') { throw "no ID in generated header: $text" }
        return $Matches[1]
    }
    $first = Get-GeneratedId
    $stamp = (Get-Item -LiteralPath $header).LastWriteTimeUtc
    Start-Sleep -Milliseconds 50
    $again = Get-GeneratedId
    Assert-True ($first -eq $again) 'same sources and inputs give the same ID (no Git, no clock)'
    Assert-True ((Get-Item -LiteralPath $header).LastWriteTimeUtc -eq $stamp) 'unchanged ID leaves the header untouched (no recompile)'
    Assert-True ([IO.File]::ReadAllText($header) -match 'VP_BUILD_ID_COMMIT "unknown"') 'commit is informational and "unknown" without Git'
    Add-Content -LiteralPath (Join-Path $tree 'src\Project\file3.cpp') -Value '// edit' -Encoding ASCII
    $edited = Get-GeneratedId
    Assert-True ($edited -ne $first) 'a source edit changes the ID (dirty trees never share one)'
    Set-Content -LiteralPath (Join-Path $tree '3rdparty\lib\api.h') -Value '#pragma once // changed' -Encoding ASCII
    Assert-True ((Get-GeneratedId) -ne $edited) 'a third-party header edit changes the ID'
    $edited = Get-GeneratedId
    Assert-True ((Get-GeneratedId 'Configuration=Debug;Platform=x64') -ne $edited) 'configuration is part of the ID'
    Set-Content -LiteralPath (Join-Path $tree 'src\Project\notes.txt') -Value 'not a source' -Encoding ASCII
    $null = New-Item -ItemType Directory -Path (Join-Path $tree 'src\Project\x64\Release') -Force
    Set-Content -LiteralPath (Join-Path $tree 'src\Project\x64\Release\generated.h') -Value '// build output' -Encoding ASCII
    Assert-True ((Get-GeneratedId) -eq $edited) 'build outputs and non-source files do not change the ID'
    Rename-Item -LiteralPath (Join-Path $tree 'src\Project\file5.cpp') -NewName 'file5b.cpp'
    Assert-True ((Get-GeneratedId) -ne $edited) 'a renamed source changes the ID'
    $emptyTree = Join-Path $fixtureRoot 'empty'
    $null = New-Item -ItemType Directory -Path (Join-Path $emptyTree 'src') -Force
    $null = New-Item -ItemType Directory -Path (Join-Path $emptyTree '3rdparty') -Force
    # Native stderr must not become a terminating error under ErrorActionPreference=Stop.
    $ErrorActionPreference = 'Continue'
    $null = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $generator -RepositoryRoot $emptyTree -OutputFile (Join-Path $fixtureRoot 'gen\empty.h') 2>&1
    $ErrorActionPreference = 'Stop'
    Assert-True ($LASTEXITCODE -ne 0 -and -not (Test-Path -LiteralPath (Join-Path $fixtureRoot 'gen\empty.h'))) 'an implausible inventory fails the build instead of writing a placeholder ID'

    # --- Real build tree -------------------------------------------------------------
    if ($BuildRoot) {
        $BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
        $realPlan = @(
            [pscustomobject]@{ Source = Join-Path $BuildRoot 'VideoProcessor-GUI.exe'; RelativeDestination = 'VideoProcessor.exe' },
            [pscustomobject]@{ Source = Join-Path $BuildRoot 'vprenderer\VideoProcessorVPRenderer.dll'; RelativeDestination = 'vprenderer\VideoProcessorVPRenderer.dll' },
            [pscustomobject]@{ Source = Join-Path $BuildRoot 'vprenderer-beta\VideoProcessorVPRendererBeta.dll'; RelativeDestination = 'vprenderer-beta\VideoProcessorVPRendererBeta.dll' })
        $realId = Assert-VpPairedBuildIdentity $realPlan $paired
        Assert-True ($realId -match '^[0-9a-f]{32}$') "built EXE and both renderer DLLs carry one build ID ($realId) with current records"
        $config = Join-Path $BuildRoot 'VideoProcessorConfig.exe'
        if (Test-Path -LiteralPath ($config + '.runtime.json')) {
            $configRecord = Get-Content -LiteralPath ($config + '.runtime.json') -Raw | ConvertFrom-Json
            Assert-True ($configRecord.schemaVersion -eq 2 -and $configRecord.PSObject.Properties['buildId']) 'every runtime record is schema 2 with a buildId field'
        }
        # A copy of the real EXE with one ID digit changed models an EXE of another build.
        $staleRoot = Join-Path $fixtureRoot 'real-stale'
        $staleExe = Join-Path $staleRoot 'VideoProcessor-GUI.exe'
        $null = New-Item -ItemType Directory -Path $staleRoot
        $bytes = [IO.File]::ReadAllBytes($realPlan[0].Source)
        $at = $latin1.GetString($bytes).IndexOf("VP-BUILD-ID:$realId")
        Assert-True ($at -ge 0) 'the EXE embeds its ID as one contiguous marker'
        $digit = $at + 12
        $bytes[$digit] = if ($bytes[$digit] -eq [byte][char]'0') { [byte][char]'1' } else { [byte][char]'0' }
        [IO.File]::WriteAllBytes($staleExe, $bytes)
        Write-Record $staleExe (Get-VpBinaryBuildId $staleExe)
        $mixed = @([pscustomobject]@{ Source = $staleExe; RelativeDestination = 'VideoProcessor.exe' }) + $realPlan[1..2]
        Assert-Throws { Assert-VpPairedBuildIdentity $mixed $paired } 'build ID mismatch' 'real renderer DLLs with an EXE of another build fail packaging'
    }
} finally {
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "Passed $script:checks build identity checks."
