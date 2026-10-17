[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidatePattern('^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$')][string]$CoreVersion,
    [Parameter(Mandatory=$true)][string]$VcRedistPath,
    [Parameter(Mandatory=$true)][string]$IsccPath,
    [string]$MSBuildPath, [string[]]$RuntimeDirectories, [switch]$SkipBuild, [switch]$PortableZip,
    [ValidateSet('full','config')][string]$Flavor = 'full',
    [ValidateSet('stable','beta')][string]$UpdateChannel = 'beta',
    [ValidateRange(0,9007199254740991)][long]$ReleaseSequence = 0,
    # audit 2026-10-16: -NoUpdater leaves VideoProcessorUpdate.exe out of setup, the
    # portable ZIP and the manifest's managed list (owner/FL35 deploys). Public
    # releases keep the default and ship the helper.
    [switch]$NoUpdater,
    # audit 2026-10-16: optional official MWCapture SDK root, passed to MSBuild as
    # /p:MagewellSdkRoot=... for isolated worktrees (AGENTS.md "Local x64 Release builds").
    [string]$MagewellSdkRoot
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'installer_build_identity.ps1')
Push-Location $root
try {
    $identity = Get-VpSourceIdentity $root
    $commit = $identity.commit
    $buildLabel = $identity.label
    $installerBaseName = Get-VpInstallerBaseName $CoreVersion $identity
    if ($Flavor -eq 'config') { $installerBaseName = $installerBaseName.Replace('VideoProcessorSetup-', 'VideoProcessorConfigSetup-') }
    $numbers = ($CoreVersion -split '-', 2)[0].Split('.')
    if (@($numbers | Where-Object { [int]$_ -gt 65535 }).Count) { throw 'Version components must fit Windows version metadata (0-65535).' }
    $fileVersion = (($numbers | ForEach-Object { [int]$_ }) -join '.') + '.0'
    $IsccPath = (Resolve-Path -LiteralPath $IsccPath).Path
    $artifactRoot = Join-Path $root 'artifacts'
    $null = New-Item -ItemType Directory -Path $artifactRoot -Force
    $receiptPath = Join-Path $artifactRoot 'installer-build.json'
    if (-not $SkipBuild) {
        if (-not $MSBuildPath) {
            $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
            $MSBuildPath = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        }
        if (-not $MSBuildPath) { throw 'Install Visual Studio C++ build tools and Qt as documented.' }
        # audit 2026-10-16: forward an explicit Magewell SDK root; confirm the header first.
        $sdkArguments = @()
        if ($MagewellSdkRoot) {
            $sdkRoot = (Resolve-Path -LiteralPath $MagewellSdkRoot).Path.TrimEnd('\')
            if (-not (Test-Path -LiteralPath (Join-Path $sdkRoot 'Include\LibMWCapture\MWCapture.h') -PathType Leaf)) {
                throw "MagewellSdkRoot has no Include\LibMWCapture\MWCapture.h: $sdkRoot"
            }
            $sdkArguments += "/p:MagewellSdkRoot=$sdkRoot"
        }
        & $MSBuildPath VideoProcessor.sln /t:Rebuild /m /p:Configuration=Release /p:Platform=x64 @sdkArguments /nologo /v:minimal /fl "/flp:logfile=$artifactRoot\installer-build.log;verbosity=normal"
        if ($LASTEXITCODE -ne 0) { throw 'x64 Release solution rebuild failed. No installer was generated.' }
        $binaries = @(foreach ($relative in @('VideoProcessor-GUI.exe','VideoProcessorConfig.exe','VideoProcessorConfigDiscovery.dll','vprenderer/VideoProcessorVPRenderer.dll','vprenderer-beta/VideoProcessorVPRendererBeta.dll')) {
            $file = Join-Path $root "x64\Release\$relative"
            [ordered]@{ path=$relative; sha256=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
        })
        [ordered]@{ commit=$commit; sourceFingerprint=$identity.fingerprint; dirty=$identity.dirty; configuration='Release'; architecture='x64'; binaries=$binaries } |
            ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    }
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if (-not $receipt.PSObject.Properties['sourceFingerprint'] -or $receipt.sourceFingerprint -ne $identity.fingerprint -or
        $receipt.dirty -ne $identity.dirty -or $receipt.commit -ne $commit -or $receipt.configuration -ne 'Release' -or $receipt.architecture -ne 'x64' -or $receipt.binaries.Count -ne 5) {
        throw 'No successful x64 Release solution build for these exact sources. Rerun without -SkipBuild.'
    }
    foreach ($binary in $receipt.binaries) {
        if ((Get-FileHash -LiteralPath (Join-Path $root "x64\Release\$($binary.path)") -Algorithm SHA256).Hash -ne $binary.sha256) {
            throw "Build output changed since successful rebuild: $($binary.path)"
        }
    }
    $afterBuild = Get-VpSourceIdentity $root
    if ($afterBuild.build -ne $identity.build -or $afterBuild.fingerprint -ne $identity.fingerprint) {
        throw 'Sources changed during the build.'
    }
    & (Join-Path $PSScriptRoot 'package_release.ps1') -VcRedistPath $VcRedistPath
    $payload = Join-Path $artifactRoot 'release\VideoProcessor'
    $localRuntime = & (Join-Path $PSScriptRoot 'stage_app_local_runtime.ps1') -PayloadRoot $payload -RuntimeDirectories $RuntimeDirectories
    # The updater is managed .NET Framework code using Windows' existing runtime.
    # Stage it after native dependency closure, so it is not treated as a VC binary.
    $updaterPath = Join-Path $payload 'VideoProcessorUpdate.exe'
    if ($NoUpdater) {
        # audit 2026-10-16: leave the helper out entirely; drop any stale copy so it
        # cannot enter the managed file list, setup or the portable ZIP.
        if (Test-Path -LiteralPath $updaterPath) { Remove-Item -LiteralPath $updaterPath -Force }
    } else {
        & (Join-Path $PSScriptRoot 'build_updater.ps1') -OutputPath $updaterPath
    }
    $null = New-Item -ItemType Directory -Path (Join-Path $payload 'setup') -Force
    Copy-Item -LiteralPath (Join-Path $root 'packaging\installer\install-support.ps1') -Destination (Join-Path $payload 'setup')
    Copy-Item -LiteralPath (Join-Path $root 'docs\VP-0192_INSTALLER.md') -Destination (Join-Path $payload 'setup\RECOVERY.md')
    # This is only the distributable default; Inno seeds it only when absent.
    Copy-Item -LiteralPath (Join-Path $payload 'VideoProcessor.cfg.example') -Destination (Join-Path $payload 'VideoProcessor.cfg')
    # The portable release remains the validated build input. Only application files
    # enter Inno's installed payload; setup tools are embedded with dontcopy below.
    $setupOnly = @(
        'START-HERE.txt', 'SETUP-RUNTIME.cmd', 'RELEASE-MANIFEST.json', 'RELEASE-LAYOUT.md',
        'VideoProcessor.cfg.example', 'docs/REQ-006-sdr-transfer-implementation.md',
        'docs/VP-0174-config-ui.md'
    )
    $cleanupFiles = @()
    $files = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($payload.Length + 1)
        $portablePath = $relative.Replace('\','/')
        if ($portablePath -in $setupOnly -or $portablePath -match '^(prerequisites|setup)/') {
            $cleanupFiles += [ordered]@{ path=$portablePath; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
            return
        }
        if ($NoUpdater -and $portablePath -eq 'VideoProcessorUpdate.exe') { return }  # audit 2026-10-16
        if ($Flavor -eq 'config' -and $portablePath -notmatch '^(config/|licenses/|LICENSE\.txt$|CONFIGURATION\.html$|VideoProcessorUpdate\.exe$)') { return }
        $policy = if ($relative -match '^shaders\\|^VideoProcessor\.cfg$') { 'seed' } else { 'managed' }
        [ordered]@{ path=$relative.Replace('\','/'); policy=$policy; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
    if ($Flavor -eq 'config') { $localRuntime.files = @($localRuntime.files | Where-Object { $_.path -like 'config/*' }) }
    # audit 2026-10-16: the managed list must agree with -NoUpdater either way.
    $updaterManaged = @($files | Where-Object { $_.path -eq 'VideoProcessorUpdate.exe' }).Count -eq 1
    if ($updaterManaged -eq [bool]$NoUpdater) { throw "Updater staging mismatch (NoUpdater=$([bool]$NoUpdater), managed=$updaterManaged)." }
    $protocol = Get-Content -LiteralPath (Join-Path $root 'src\VideoProcessor-ConfigCore\ConfigurationRpcProtocol.h') -Raw
    if ($protocol -notmatch 'constexpr\s+\w+\s+Version\s*=\s*(\d+)') { throw 'RPC protocol version not found.' }
    $rpcVersion = [int]$Matches[1]
    if ($protocol -notmatch 'constexpr\s+\w+\s+ConfigurationCompatibilityVersion\s*=\s*(\d+)') { throw 'Configuration compatibility version not found.' }
    $configurationVersion = [int]$Matches[1]
    [ordered]@{
        schemaVersion=1; applicationId=$(if ($Flavor -eq 'full') { 'VideoProcessor-42D852F1-70E9-43ED-8739-D61752106D59' } else { 'VideoProcessorConfig-BA15DBE8-210F-42AA-AE86-B4628395E77F' }); flavor=$Flavor; updateSequence=$ReleaseSequence; updateChannel=$UpdateChannel; rpcVersion=$rpcVersion; configurationVersion=$configurationVersion
        coreVersion=$CoreVersion; build=$identity.build; sourceCommit=$commit; sourceFingerprint=$identity.fingerprint; dirty=$identity.dirty; uninstallEntryPoint=$(if ($Flavor -eq 'full') { 'Uninstall VideoProcessor.lnk' } else { 'Uninstall VideoProcessor Config.lnk' }); runtimeMode='app-local'; appLocalRuntime=$localRuntime; compiler='Inno Setup 6.7.3'; files=$files; cleanupFiles=$cleanupFiles
        updaterIncluded=(-not $NoUpdater)  # audit 2026-10-16
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $payload 'INSTALL-MANIFEST.json') -Encoding UTF8
    $include = Join-Path $artifactRoot ('installer-payload-' + $Flavor + '.iss')
    $lines = @(foreach ($entry in $files) {
        $relative = $entry.path.Replace('/','\')
        $directory = Split-Path -Parent $relative
        $dest = if ($directory) { "{app}\$directory" } else { '{app}' }
        $flags = if ($entry.policy -eq 'seed') { 'onlyifdoesntexist uninsneveruninstall' } else { 'ignoreversion' }
        'Source: "' + (Join-Path $payload $relative) + '"; DestDir: "' + $dest + '"; Flags: ' + $flags
    })
    $lines | Set-Content -LiteralPath $include -Encoding UTF8
    $output = Join-Path $artifactRoot 'installers'
    $flavorArguments = @(); if ($Flavor -eq 'config') { $flavorArguments += '/DConfigOnly' }
    & $IsccPath @flavorArguments "/DPayloadRoot=$payload" "/DPayloadInclude=$include" "/DOutputRoot=$output" "/DCoreVersion=$CoreVersion" "/DBuildCommit=$buildLabel" "/DInstallerBaseName=$installerBaseName" "/DFileVersion=$fileVersion" "/DSetupIcon=$root\images\VideoProcessor.ico" (Join-Path $root 'packaging\installer\VideoProcessor.iss')
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
    $installer = Join-Path $output "$installerBaseName.exe"
    # audit 2026-10-16: hashes and manifest copies are internal evidence (AGENTS.md
    # "Published release assets"). Keep them in installers\evidence so a glob upload of
    # artifacts\installers can only pick up setup EXEs, portable ZIPs and vp-update.json.
    # new_update_release.ps1 reads the sidecars from this folder.
    $evidence = Join-Path $output 'evidence'
    $null = New-Item -ItemType Directory -Path $evidence -Force
    # Rejected packages go outside the distributable directory as well.
    $invalidRoot = Join-Path $artifactRoot 'installers-invalid'
    function Move-VpInvalidArtifact([string]$File) {
        $null = New-Item -ItemType Directory -Path $invalidRoot -Force
        Move-Item -LiteralPath $File -Destination (Join-Path $invalidRoot ([IO.Path]::GetFileName($File) + '.invalid-' + [guid]::NewGuid().ToString('N')))
    }
    $installerName = [IO.Path]::GetFileName($installer)
    $installerHash = Join-Path $evidence ($installerName + '.sha256')
    "$((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash)  $installerName" |
        Set-Content -LiteralPath $installerHash -Encoding ASCII
    $manifestCopy = Join-Path $evidence ($installerName + '.manifest.json')
    Copy-Item -LiteralPath (Join-Path $payload 'INSTALL-MANIFEST.json') -Destination $manifestCopy
    $distributionFiles = @($installer, $installerHash, $manifestCopy)
    if ($PortableZip) {
        & (Join-Path $PSScriptRoot 'test_portable_config_contract.ps1')
        # Export the same self-contained payload, without installing anything.
        $portable = [IO.Path]::GetFullPath((Join-Path $artifactRoot ('portable\' + $(if ($Flavor -eq 'full') { 'VideoProcessor' } else { 'VideoProcessorConfig' }))))
        if (-not $portable.StartsWith($artifactRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe portable stage.' }
        if (Test-Path -LiteralPath $portable) { Remove-Item -LiteralPath $portable -Recurse -Force }
        $portableManifest = Get-Content -LiteralPath (Join-Path $payload 'INSTALL-MANIFEST.json') -Raw | ConvertFrom-Json
        foreach ($entry in $portableManifest.files) {
            $source = Join-Path $payload $entry.path
            if ($entry.path -eq 'VideoProcessor.cfg') { $entry.path = 'VideoProcessor.cfg.example' }
            $destination = Join-Path $portable $entry.path
            $null = New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force
            Copy-Item -LiteralPath $source -Destination $destination
        }
        if ($Flavor -eq 'config') {
            $sample = Join-Path $root 'VideoProcessor.cfg'
            Copy-Item -LiteralPath $sample -Destination (Join-Path $portable 'VideoProcessor.cfg.example')
            $portableManifest.files += [pscustomobject]@{path='VideoProcessor.cfg.example';policy='seed';sha256=(Get-FileHash -LiteralPath $sample -Algorithm SHA256).Hash}
        }
        $portableManifest.cleanupFiles = @($portableManifest.cleanupFiles | Where-Object { $_.path -notin $portableManifest.files.path })
        $portableManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $portable 'INSTALL-MANIFEST.json') -Encoding UTF8
        $portableName = if ($Flavor -eq 'full') { 'VideoProcessor' } else { 'VideoProcessorConfig' }
        $zip = Join-Path $output "$portableName-$CoreVersion-$buildLabel-x64-Portable.zip"
        # audit 2026-10-16: Compress-Archive on Windows PowerShell 5.1 writes entry
        # names with backslashes. Build the archive with ZipArchive and explicit
        # forward-slash entry names (ZIP APPNOTE 4.4.17), in a stable order.
        Add-Type -AssemblyName System.IO.Compression
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
        $archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($item in @(Get-ChildItem -LiteralPath $portable -Recurse -File | Sort-Object FullName)) {
                $entryName = $item.FullName.Substring($portable.Length + 1).Replace('\','/')
                $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $item.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally { $archive.Dispose() }
        try {
            & (Join-Path $PSScriptRoot 'test_portable_config.ps1') -ZipPath $zip
        } catch {
            Move-VpInvalidArtifact $zip
            throw
        }
        $zipName = [IO.Path]::GetFileName($zip)
        $zipHash = Join-Path $evidence ($zipName + '.sha256')
        "$((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)  $zipName" |
            Set-Content -LiteralPath $zipHash -Encoding ASCII
        $distributionFiles += @($zip, $zipHash)
    }
    $after = Get-VpSourceIdentity $root
    if ($after.build -ne $identity.build -or $after.fingerprint -ne $identity.fingerprint) {
        # Keep an interrupted package out of the distributable output directory.
        foreach ($file in $distributionFiles) { Move-VpInvalidArtifact $file }
        throw 'Sources changed during packaging; the installer is invalid and must not be distributed.'
    }
    # audit 2026-10-16: flag anything in the upload folder that is not publishable
    # (for example sidecars left by an older builder). Nothing is deleted here.
    $unpublishable = @(Get-ChildItem -LiteralPath $output -File | Where-Object { $_.Extension -notin @('.exe','.zip') -and $_.Name -ne 'vp-update.json' })
    foreach ($file in $unpublishable) { Write-Warning "Not a publishable asset; move it out of artifacts\installers before uploading: $($file.Name)" }
    Write-Host "Installer: $installer"
    Write-Host "Source: $($identity.build)"
    Write-Host 'Publish only qualified builds with a signed vp-update.json descriptor. Windows publisher signing is separate. See docs/VP_UPDATES.md.'
} finally { Pop-Location }
