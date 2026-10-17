function Get-VpPeLinkerVersion {
    param([Parameter(Mandatory = $true)][string]$FilePath)
    $stream = [IO.File]::OpenRead($FilePath)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Not a PE file: $FilePath" }
        $stream.Position = 0x3c
        $offset = $reader.ReadInt32()
        if ($offset -lt 0 -or $offset + 28 -gt $stream.Length) { throw "Invalid PE header: $FilePath" }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $FilePath" }
        if ($reader.ReadUInt16() -ne 0x8664) { throw "Release binary is not x64: $FilePath" }
        $stream.Position = $offset + 26
        return [version]::new($reader.ReadByte(), $reader.ReadByte(), 0, 0)
    } finally { $reader.Dispose() }
}

# Hunt 2 (2026-10-17, MEDIUM-2): EXE/DLL build identity. Every binary that links
# VideoProcessor-Lib embeds "VP-BUILD-ID:" plus 32 lower-case hex digits
# (src/VideoProcessor-Lib/VpBuildIdentity.cpp). Returns that ID, or $null when the
# binary has none; two different IDs in one binary are an error.
function Get-VpBinaryBuildId {
    param([Parameter(Mandatory = $true)][string]$FilePath)
    $bytes = [IO.File]::ReadAllBytes($FilePath)
    # Latin-1 maps every byte to one char, so offsets and ASCII text survive.
    $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $ids = @([regex]::Matches($text, 'VP-BUILD-ID:([0-9a-f]{32})(?![0-9a-f])') |
        ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
    if ($ids.Count -gt 1) { throw "Binary carries more than one VP build ID ($($ids -join ', ')): $FilePath" }
    if ($ids.Count -eq 1) { return $ids[0] }
    return $null
}

# The paired binaries (VideoProcessor.exe and the renderer DLLs) must come from one
# build: each record must be current (schema 2, hash), name a build ID equal to the
# one embedded in the binary, and all IDs must be equal. A GUI-only build or a
# manual copy that leaves a stale DLL fails here. Returns the common ID.
function Assert-VpPairedBuildIdentity {
    param($CopyPlan, [string[]]$PairedArtifacts)
    if (@($PairedArtifacts).Count -lt 2) { throw 'The release manifest must name at least two paired build artifacts.' }
    $seen = [ordered]@{}
    foreach ($destination in $PairedArtifacts) {
        $entry = @($CopyPlan | Where-Object { $_.RelativeDestination -eq $destination.Replace('/', '\') })
        if ($entry.Count -ne 1) { throw "Missing/duplicate paired build artifact: $destination" }
        $recordPath = $entry[0].Source + '.runtime.json'
        if (-not (Test-Path -LiteralPath $recordPath -PathType Leaf)) {
            throw "Missing build runtime record: $recordPath. Rebuild x64 Release with Directory.Build.targets before packaging."
        }
        $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
        if ($record.schemaVersion -ne 2 -or -not $record.PSObject.Properties['buildId'] -or
            $record.sha256 -ne (Get-FileHash -LiteralPath $entry[0].Source -Algorithm SHA256).Hash) {
            throw "Stale or invalid runtime build record (build ID): $recordPath. Rebuild the x64 Release solution."
        }
        $embedded = Get-VpBinaryBuildId -FilePath $entry[0].Source
        if (-not $embedded) { throw "Paired build artifact has no VP build ID: $destination" }
        if ($record.buildId -ne $embedded) {
            throw "Runtime build record names build ID $($record.buildId) but $destination carries $embedded."
        }
        $seen[$destination] = $embedded
    }
    $distinct = @($seen.Values | Sort-Object -Unique)
    if ($distinct.Count -ne 1) {
        $detail = ($seen.Keys | ForEach-Object { "$_=$($seen[$_])" }) -join ', '
        throw "EXE/DLL build ID mismatch: $detail. VideoProcessor.exe and the renderer DLLs must come from one x64 Release solution build; rebuild the solution (not one project) and package again."
    }
    return $distinct[0]
}

function Get-VpPackageRuntimeRequirement {
    param($CopyPlan, $Policy, [string]$InstallerPath)
    $minimum = [version]$Policy.minimumVersion
    $buildRecords = @()
    foreach ($destination in $Policy.buildArtifacts) {
        $entry = @($CopyPlan | Where-Object { $_.RelativeDestination -eq $destination.Replace('/', '\') })
        if ($entry.Count -ne 1) { throw "Missing/duplicate runtime build artifact: $destination" }
        $recordPath = $entry[0].Source + '.runtime.json'
        if (-not (Test-Path -LiteralPath $recordPath -PathType Leaf)) {
            throw "Missing build runtime record: $recordPath. Rebuild x64 Release with Directory.Build.targets before packaging."
        }
        $record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
        if ($record.schemaVersion -ne 2 -or $record.configuration -ne 'Release' -or
            $record.architecture -ne 'x64' -or
            $record.sha256 -ne (Get-FileHash -LiteralPath $entry[0].Source -Algorithm SHA256).Hash) {
            throw "Stale or invalid runtime build record: $recordPath"
        }
        $toolset = [version]$record.toolsetVersion
        if ($toolset -gt $minimum) { $minimum = $toolset }
        $buildRecords += [ordered]@{ file = $destination; toolsetVersion = $toolset.ToString(); sha256 = $record.sha256; buildId = $record.buildId }
    }
    # Include shipped third-party DLLs (especially Qt), not just VP's host toolset.
    # The policy floor records the tested full runtime version for binary dependencies;
    # PE headers additionally prevent silently accepting a newer linker family.
    foreach ($entry in $CopyPlan) {
        if ($entry.RelativeDestination -eq 'prerequisites\vc_redist.x64.exe' -or
            [IO.Path]::GetExtension($entry.Source) -notin @('.exe', '.dll')) { continue }
        $linker = Get-VpPeLinkerVersion $entry.Source
        if ($linker.Major -ge 14 -and $linker -gt $minimum) { $minimum = $linker }
    }
    $installerVersion = Assert-VpRedistributable -FilePath $InstallerPath -MinimumVersion $minimum
    return [ordered]@{
        schemaVersion = 1
        architecture = 'x64'
        minimumVersion = $minimum.ToString()
        installerVersion = $installerVersion.ToString()
        installerSha256 = (Get-FileHash -LiteralPath $InstallerPath -Algorithm SHA256).Hash
        runtimeFiles = $Policy.runtimeFiles
        buildArtifacts = $buildRecords
    }
}
