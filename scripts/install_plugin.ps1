[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true)]
    [string] $StagingPluginRoot,

    [Parameter(Mandatory = $true)]
    [string] $InstallPluginRoot,

    [string] $BackupRoot,

    [switch] $RemoveBackupOnSuccess,

    [string] $EngineVersion,

    [string] $EngineRoot,

    [switch] $PreflightOnly
)

$ErrorActionPreference = 'Stop'

function Resolve-Directory([string] $Path, [string] $Label) {
    $Resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $Resolved.Path -PathType Container)) {
        throw "$Label is not a directory: $Path"
    }
    return [IO.Path]::GetFullPath($Resolved.Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
}

function Get-TreeHashes([string] $Root) {
    $Result = [ordered]@{}
    $CanonicalRoot = [IO.Path]::GetFullPath($Root).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
    $RootPrefix = $CanonicalRoot + [IO.Path]::DirectorySeparatorChar
    Get-ChildItem -LiteralPath $Root -File -Recurse | Sort-Object FullName |
        ForEach-Object {
            $CanonicalFile = [IO.Path]::GetFullPath($_.FullName)
            if (-not $CanonicalFile.StartsWith(
                    $RootPrefix,
                    [StringComparison]::OrdinalIgnoreCase)) {
                throw "Tree entry escapes its canonical root: $CanonicalFile"
            }
            # Windows PowerShell 5.1 targets .NET Framework and has no
            # System.IO.Path.GetRelativePath. The prefix check above makes the
            # compatible substring form both deterministic and traversal-safe.
            $Relative = $CanonicalFile.Substring($RootPrefix.Length).Replace('\', '/')
            $Result[$Relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        }
    return $Result
}

function Assert-SameTree($Expected, $Actual, [string] $Label) {
    $ExpectedKeys = @($Expected.Keys)
    $ActualKeys = @($Actual.Keys)
    if ($ExpectedKeys.Count -ne $ActualKeys.Count) {
        throw "$Label file count differs: expected=$($ExpectedKeys.Count), actual=$($ActualKeys.Count)."
    }
    foreach ($Key in $ExpectedKeys) {
        if (-not $Actual.Contains($Key) -or $Actual[$Key] -ne $Expected[$Key]) {
            throw "$Label hash mismatch: $Key"
        }
    }
}

function Get-EngineMajorMinor([string] $Value, [string] $Label) {
    $Match = [regex]::Match($Value, '^(?<version>\d+\.\d+)(?:$|[.\-+])')
    if (-not $Match.Success) {
        throw "$Label must begin with an Engine major.minor version: $Value"
    }
    return $Match.Groups['version'].Value
}

function Resolve-TraceEngine(
    [string] $RequestedRoot,
    [string] $InstallRoot,
    [string] $SelectedEngineVersion
) {
    $Candidate = $RequestedRoot
    $Source = 'EngineRoot'
    if (-not $Candidate) {
        $Candidate = [Environment]::GetEnvironmentVariable(
            'UEAI_ENGINE_ROOT', 'Process')
        $Source = 'UEAI_ENGINE_ROOT'
    }
    if (-not $Candidate) {
        $Candidate = [Environment]::GetEnvironmentVariable(
            'UE_ENGINE_ROOT', 'Process')
        $Source = 'UE_ENGINE_ROOT'
    }
    if (-not $Candidate) {
        $CanonicalInstall = [IO.Path]::GetFullPath($InstallRoot)
        $ProjectRoot = Split-Path -Parent (Split-Path -Parent $CanonicalInstall)
        $Projects = @(Get-ChildItem -LiteralPath $ProjectRoot -Filter '*.uproject' `
            -File -ErrorAction SilentlyContinue)
        if ($Projects.Count -gt 1) {
            throw 'Multiple .uproject files were found beside InstallPluginRoot; pass -EngineRoot.'
        }
        if ($Projects.Count -eq 1) {
            $Project = Get-Content -LiteralPath $Projects[0].FullName -Raw |
                ConvertFrom-Json
            $Association = [string]$Project.EngineAssociation
            if ($Association -match '^\d+\.\d+$') {
                $Key = "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association"
                if (Test-Path -LiteralPath $Key) {
                    $Candidate = (Get-ItemProperty -LiteralPath $Key `
                        -Name InstalledDirectory -ErrorAction SilentlyContinue).InstalledDirectory
                }
            }
            elseif ($Association -match '^\{[0-9A-Fa-f-]+\}$') {
                $Key = 'HKCU:\Software\Epic Games\Unreal Engine\Builds'
                if (Test-Path -LiteralPath $Key) {
                    $Candidate = (Get-ItemProperty -LiteralPath $Key `
                        -Name $Association -ErrorAction SilentlyContinue).$Association
                }
            }
            $Source = 'projectAssociation'
        }
    }
    if (-not $Candidate) {
        throw 'A matching Engine root could not be resolved; pass -EngineRoot or set UEAI_ENGINE_ROOT.'
    }
    $Canonical = Resolve-Directory $Candidate "$Source Engine root"
    $EngineDirectory = if ([IO.Path]::GetFileName($Canonical) -ieq 'Engine') {
        $Canonical
    }
    else {
        Resolve-Directory (Join-Path $Canonical 'Engine') "$Source Engine directory"
    }
    $BuildVersionPath = Join-Path $EngineDirectory 'Build\Build.version'
    if (-not (Test-Path -LiteralPath $BuildVersionPath -PathType Leaf)) {
        throw "$Source has no Engine/Build/Build.version."
    }
    $BuildVersion = Get-Content -LiteralPath $BuildVersionPath -Raw |
        ConvertFrom-Json
    $ActualVersion = "$($BuildVersion.MajorVersion).$($BuildVersion.MinorVersion)"
    if ($ActualVersion -ne $SelectedEngineVersion) {
        throw "$Source Engine version $ActualVersion does not match Worker $SelectedEngineVersion."
    }
    $Insights = Join-Path $EngineDirectory 'Binaries\Win64\UnrealInsights.exe'
    if (-not (Test-Path -LiteralPath $Insights -PathType Leaf)) {
        throw "The matching Engine has no UnrealInsights.exe: $Insights"
    }
    return [pscustomobject]@{
        engineRoot = Split-Path -Parent $EngineDirectory
        engineDirectory = $EngineDirectory
        engineVersion = $ActualVersion
        source = $Source
        unrealInsightsPath = [IO.Path]::GetFullPath($Insights)
    }
}

function Assert-NoWorkerSavedTree([string] $PluginRoot) {
    $Saved = Join-Path $PluginRoot 'Tools\UEAITraceWorker\Saved'
    if (Test-Path -LiteralPath $Saved) {
        throw 'The plugin package contains generated Tools/UEAITraceWorker/Saved output.'
    }
}

function Resolve-WorkerEngineVersion([string] $PluginRoot, [string] $RequestedVersion) {
    $WorkerRoot = Join-Path $PluginRoot 'Tools\Trace\Win64'
    if (-not (Test-Path -LiteralPath $WorkerRoot -PathType Container)) {
        throw 'The staged Win64 Trace Worker root is missing.'
    }
    if ($RequestedVersion) {
        $Selected = Get-EngineMajorMinor $RequestedVersion 'EngineVersion'
        $Worker = Join-Path $WorkerRoot "$Selected\UEAITraceWorker.exe"
        if (-not (Test-Path -LiteralPath $Worker -PathType Leaf)) {
            throw "The staged Win64 Trace Worker for Engine $Selected is missing."
        }
        return $Selected
    }
    $Candidates = @(
        Get-ChildItem -LiteralPath $WorkerRoot -Directory |
            Where-Object {
                $_.Name -match '^\d+\.\d+$' -and
                (Test-Path -LiteralPath (
                    Join-Path $_.FullName 'UEAITraceWorker.exe') -PathType Leaf)
            } |
            Sort-Object Name
    )
    if ($Candidates.Count -eq 0) {
        throw 'No engine-versioned staged Win64 Trace Worker was found.'
    }
    if ($Candidates.Count -ne 1) {
        $Versions = ($Candidates | ForEach-Object Name) -join ', '
        throw "Multiple staged Trace Worker Engine versions were found ($Versions); pass -EngineVersion."
    }
    return $Candidates[0].Name
}

function Get-Sha256Digest([string] $Path) {
    return 'sha256:' + (
        Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TraceContractEvidence([string] $PluginRoot, [string] $SelectedEngineVersion) {
    $RelativeFiles = @(
        'Resources/Capabilities/production.json',
        "Resources/Trace/insights-actions.$SelectedEngineVersion.json",
        'Resources/Trace/launch-profiles.json',
        'Resources/Trace/worker-protocol.v1.json'
    ) | Sort-Object
    $Stream = [IO.MemoryStream]::new()
    try {
        foreach ($Relative in $RelativeFiles) {
            $FullPath = Join-Path $PluginRoot $Relative
            if (-not (Test-Path -LiteralPath $FullPath -PathType Leaf)) {
                throw "Required Trace contract resource is missing: $Relative"
            }
            $PathBytes = [Text.Encoding]::UTF8.GetBytes($Relative.Replace('\', '/'))
            $Stream.Write($PathBytes, 0, $PathBytes.Length)
            $Stream.WriteByte(0)
            $Contents = [IO.File]::ReadAllBytes($FullPath)
            $Stream.Write($Contents, 0, $Contents.Length)
            $Stream.WriteByte(0)
        }
        $Hasher = [Security.Cryptography.SHA256]::Create()
        try {
            $DigestBytes = $Hasher.ComputeHash($Stream.ToArray())
        }
        finally {
            $Hasher.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }
    $ContractDigest = 'sha256:' + (
        [BitConverter]::ToString($DigestBytes).Replace('-', '').ToLowerInvariant())
    $MappingRelative = "Resources/Trace/insights-actions.$SelectedEngineVersion.json"
    return [pscustomobject]@{
        contractDigest = $ContractDigest
        providerSchemaDigest = Get-Sha256Digest (
            Join-Path $PluginRoot $MappingRelative)
    }
}

function Assert-WorkerBundle(
    [string] $PluginRoot,
    [string] $VersionName,
    [string] $SelectedEngineVersion,
    $ExpectedEvidence
) {
    $WorkerDirectory = Join-Path $PluginRoot "Tools\Trace\Win64\$SelectedEngineVersion"
    $ManifestPath = Join-Path $WorkerDirectory 'bundle-manifest.json'
    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
        throw "Trace Worker bundle manifest is missing for Engine $SelectedEngineVersion."
    }
    $Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ($Manifest.schema -ne 'ue.trace-worker-bundle.v1' -or
        $Manifest.workerVersion -ne $VersionName -or
        $Manifest.engineVersion -ne $SelectedEngineVersion -or
        $Manifest.protocolVersion -ne 1 -or
        $Manifest.contractDigest -ne $ExpectedEvidence.contractDigest -or
        $Manifest.providerSchemaDigest -ne $ExpectedEvidence.providerSchemaDigest) {
        throw 'Trace Worker bundle manifest does not match the selected plugin/Engine contract.'
    }
    $Names = @{}
    foreach ($File in @($Manifest.files)) {
        $Name = [string]$File.name
        if (-not $Name -or [IO.Path]::GetFileName($Name) -ne $Name -or $Names.ContainsKey($Name)) {
            throw "Trace Worker bundle manifest contains an invalid or duplicate file name: $Name"
        }
        $Names[$Name] = $true
        $FullPath = Join-Path $WorkerDirectory $Name
        if (-not (Test-Path -LiteralPath $FullPath -PathType Leaf)) {
            throw "Trace Worker bundle file is missing: $Name"
        }
        $Info = Get-Item -LiteralPath $FullPath
        $ActualHash = (Get-FileHash -LiteralPath $FullPath -Algorithm SHA256).Hash
        if ([int64]$File.size -ne $Info.Length -or
            [string]$File.sha256 -ne $ActualHash) {
            throw "Trace Worker bundle file evidence does not match: $Name"
        }
    }
    $RequiredBundleFiles = @(
        'UEAITraceWorker.exe',
        'UEAITraceWorker.pdb',
        "insights-actions.$SelectedEngineVersion.json",
        'launch-profiles.json',
        'worker-protocol.v1.json'
    )
    foreach ($Name in $RequiredBundleFiles) {
        if (-not $Names.ContainsKey($Name)) {
            throw "Trace Worker bundle manifest omits required file: $Name"
        }
    }
    $ResourcePairs = @(
        @(
            "Resources\Trace\insights-actions.$SelectedEngineVersion.json",
            "insights-actions.$SelectedEngineVersion.json"),
        @('Resources\Trace\launch-profiles.json', 'launch-profiles.json'),
        @('Resources\Trace\worker-protocol.v1.json', 'worker-protocol.v1.json')
    )
    foreach ($Pair in $ResourcePairs) {
        $RootDigest = Get-Sha256Digest (Join-Path $PluginRoot $Pair[0])
        $WorkerDigest = Get-Sha256Digest (Join-Path $WorkerDirectory $Pair[1])
        if ($RootDigest -ne $WorkerDigest) {
            throw "Trace Worker bundled resource differs from plugin resource: $($Pair[1])"
        }
    }
}

function Assert-WorkerHandshake(
    [string] $PluginRoot,
    [string] $VersionName,
    [string] $SelectedEngineVersion,
    $EngineEvidence
) {
    Assert-NoWorkerSavedTree $PluginRoot
    $WorkerDirectory = Join-Path $PluginRoot "Tools\Trace\Win64\$SelectedEngineVersion"
    $Worker = Join-Path $WorkerDirectory 'UEAITraceWorker.exe'
    if (-not (Test-Path -LiteralPath $Worker -PathType Leaf)) {
        throw "The staged Win64 Trace Worker for Engine $SelectedEngineVersion is missing."
    }
    $WorkerPdb = Join-Path $WorkerDirectory 'UEAITraceWorker.pdb'
    if (-not (Test-Path -LiteralPath $WorkerPdb -PathType Leaf)) {
        throw "The staged Win64 Trace Worker PDB for Engine $SelectedEngineVersion is missing."
    }
    $Evidence = Get-TraceContractEvidence $PluginRoot $SelectedEngineVersion
    Assert-WorkerBundle $PluginRoot $VersionName $SelectedEngineVersion $Evidence
    $RequestId = 'install-' + [guid]::NewGuid().ToString('N')
    $Request = @{
        schema = 'ue.trace-worker-request.v1'
        action = 'handshake'
        requestId = $RequestId
    } | ConvertTo-Json -Compress
    $Raw = $Request | & $Worker --stdio -NoLog -NoDefaultLog -SaveToUserDir `
        "-EngineDir=$($EngineEvidence.engineDirectory)"
    if ($LASTEXITCODE -ne 0) {
        throw "Trace Worker handshake failed with exit code $LASTEXITCODE."
    }
    $Envelope = $Raw | ConvertFrom-Json
    $DigestPattern = '^sha256:[0-9a-f]{64}$'
    $HandshakeEngineVersion = Get-EngineMajorMinor `
        ([string]$Envelope.data.engineVersion) 'Trace Worker engineVersion'
    if ($Envelope.schema -ne 'ue.trace-worker-response.v1' -or
        $Envelope.ok -ne $true -or
        $Envelope.meta.requestId -ne $RequestId -or
        $Envelope.data.schema -ne 'ue.trace-worker-handshake.v1' -or
        $Envelope.data.protocolVersion -ne 1 -or
        $Envelope.data.workerVersion -ne $VersionName -or
        $HandshakeEngineVersion -ne $SelectedEngineVersion -or
        $Envelope.data.contractBound -ne $true -or
        [string]$Envelope.data.contractDigest -notmatch $DigestPattern -or
        [string]$Envelope.data.providerSchemaDigest -notmatch $DigestPattern -or
        $Envelope.data.contractDigest -ne $Evidence.contractDigest -or
        $Envelope.data.providerSchemaDigest -ne $Evidence.providerSchemaDigest) {
        throw 'Trace Worker handshake does not match the staged plugin contract.'
    }
    $ReportedInsights = if ($Envelope.data.unrealInsightsPath) {
        [IO.Path]::GetFullPath([string]$Envelope.data.unrealInsightsPath)
    }
    else { '' }
    $ReportedEngineDirectory = if ($Envelope.data.unrealInsightsEngineDir) {
        [IO.Path]::GetFullPath([string]$Envelope.data.unrealInsightsEngineDir)
    }
    else { '' }
    if ($Envelope.data.unrealInsightsAvailable -ne $true -or
        $Envelope.data.unrealInsightsEngineVersion -ne $SelectedEngineVersion -or
        $ReportedEngineDirectory -ne $EngineEvidence.engineDirectory -or
        $ReportedInsights -ne $EngineEvidence.unrealInsightsPath) {
        throw 'Trace Worker handshake did not bind to the selected Engine Unreal Insights executable.'
    }
    $TargetRequestId = 'install-targets-' + [guid]::NewGuid().ToString('N')
    $TargetRequest = @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = $TargetRequestId
        capability = 'production.trace.target.list'
        params = @{}
    } | ConvertTo-Json -Depth 5 -Compress
    $RawTargets = $TargetRequest | & $Worker --stdio -NoLog -NoDefaultLog -SaveToUserDir `
        "-EngineDir=$($EngineEvidence.engineDirectory)"
    if ($LASTEXITCODE -ne 0) {
        throw "Trace Worker target.list failed with exit code $LASTEXITCODE."
    }
    $Targets = $RawTargets | ConvertFrom-Json
    $DevelopmentTarget = @($Targets.data.targets |
        Where-Object { $_.kind -eq 'development' }) | Select-Object -First 1
    $DevelopmentProfile = @($Targets.data.launchProfiles |
        Where-Object { $_.id -eq 'projectDevelopment' }) | Select-Object -First 1
    if ($Targets.schema -ne 'ue.trace-worker-response.v1' -or
        $Targets.ok -ne $true -or
        $Targets.meta.requestId -ne $TargetRequestId -or
        $Targets.data.schema -ne 'ue.trace-target-list.v1' -or
        $null -eq $DevelopmentTarget -or
        $DevelopmentTarget.available -ne $true -or
        $null -eq $DevelopmentProfile -or
        $DevelopmentProfile.executableKind -ne 'projectDevelopment') {
        throw 'Trace Worker does not expose the installed Development launch profile.'
    }
    return [pscustomobject]@{
        workerPath = $Worker
        engineVersion = $SelectedEngineVersion
        contractDigest = $Evidence.contractDigest
        providerSchemaDigest = $Evidence.providerSchemaDigest
        engineDirectory = $EngineEvidence.engineDirectory
        unrealInsightsPath = $EngineEvidence.unrealInsightsPath
    }
}

$Staging = Resolve-Directory $StagingPluginRoot 'Staging plugin root'
$DescriptorPath = Join-Path $Staging 'UE_AI_integration.uplugin'
$Descriptor = Get-Content -LiteralPath $DescriptorPath -Raw | ConvertFrom-Json
$SelectedEngineVersion = Resolve-WorkerEngineVersion $Staging $EngineVersion
$EngineEvidence = Resolve-TraceEngine `
    $EngineRoot $InstallPluginRoot $SelectedEngineVersion
$Required = @(
    'Binaries\Win64\UnrealEditor-UE_AI_integration.dll',
    'Binaries\Win64\UnrealEditor-UE_AI_integration.pdb',
    'Binaries\Win64\UnrealEditor-UEAITraceAnalysisCore.dll',
    'Binaries\Win64\UnrealEditor-UEAITraceAnalysisCore.pdb',
    'Intermediate\Build\Win64\UnrealGame\Development\UEAITraceRuntime\UEAITraceRuntime.precompiled',
    'Intermediate\Build\Win64\x64\UnrealGame\Development\UEAITraceRuntime\Module.UEAITraceRuntime.cpp.obj',
    'Resources\Trace\worker-protocol.v1.json',
    'Resources\Trace\launch-profiles.json'
)
foreach ($Relative in $Required) {
    if (-not (Test-Path -LiteralPath (Join-Path $Staging $Relative) -PathType Leaf)) {
        throw "Required staged file is missing: $Relative"
    }
}
$ShippingRuntimeArtifacts = @(Get-ChildItem -LiteralPath (
        Join-Path $Staging 'Intermediate\Build\Win64') `
        -Recurse -File -ErrorAction SilentlyContinue | Where-Object {
            $_.FullName -match '[\\/]Shipping[\\/]UEAITraceRuntime[\\/]'
        })
if ($ShippingRuntimeArtifacts.Count -ne 0) {
    throw 'UEAITraceRuntime must not be present in a Shipping build artifact.'
}
$StagingEvidence = Assert-WorkerHandshake `
    $Staging $Descriptor.VersionName $SelectedEngineVersion $EngineEvidence
if ($PreflightOnly) {
    [ordered]@{
        schema = 'ue.plugin-install-preflight.v1'
        version = $Descriptor.VersionName
        engineVersion = $StagingEvidence.engineVersion
        workerPath = $StagingEvidence.workerPath
        contractDigest = $StagingEvidence.contractDigest
        providerSchemaDigest = $StagingEvidence.providerSchemaDigest
        engineDirectory = $StagingEvidence.engineDirectory
        unrealInsightsPath = $StagingEvidence.unrealInsightsPath
    } | ConvertTo-Json -Depth 4
    return
}

if (Get-Process -Name UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue) {
    throw 'An Unreal Editor process is running. Close it normally before installing the plugin.'
}

$Install = [IO.Path]::GetFullPath($InstallPluginRoot).TrimEnd(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar)
$InstallParent = Split-Path -Parent $Install
if (-not (Test-Path -LiteralPath $InstallParent -PathType Container)) {
    throw "Install parent does not exist: $InstallParent"
}
if ([IO.Path]::GetFileName($Install) -ne 'UE_AI_integration') {
    throw 'InstallPluginRoot must end in UE_AI_integration.'
}
if ($Staging -eq $Install -or $Staging.StartsWith(
        $Install + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Staging must be outside the active plugin directory.'
}

$ExpectedHashes = Get-TreeHashes $Staging
$Temporary = Join-Path $InstallParent (
    'UE_AI_integration.installing-' + [guid]::NewGuid().ToString('N'))
$ProjectRoot = Split-Path -Parent $InstallParent
if (-not $BackupRoot) {
    $BackupRoot = Join-Path (Split-Path -Parent $ProjectRoot) 'PluginBackups'
}
$BackupRoot = [IO.Path]::GetFullPath($BackupRoot)
if ($BackupRoot.StartsWith(
        $ProjectRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BackupRoot must be outside the Unreal project directory.'
}
New-Item -ItemType Directory -Path $BackupRoot -Force | Out-Null
$Backup = Join-Path $BackupRoot (
    'UE_AI_integration-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$Activated = $false
$BackedUp = $false
try {
    if (-not $PSCmdlet.ShouldProcess($Install, "Install UE_AI_integration $($Descriptor.VersionName)")) {
        return
    }
    Copy-Item -LiteralPath $Staging -Destination $Temporary -Recurse
    Assert-SameTree $ExpectedHashes (Get-TreeHashes $Temporary) 'Staging copy'
    Assert-WorkerHandshake `
        $Temporary $Descriptor.VersionName $SelectedEngineVersion $EngineEvidence | Out-Null
    if (Test-Path -LiteralPath $Install) {
        Move-Item -LiteralPath $Install -Destination $Backup
        $BackedUp = $true
    }
    Move-Item -LiteralPath $Temporary -Destination $Install
    $Activated = $true
    Assert-SameTree $ExpectedHashes (Get-TreeHashes $Install) 'Activated plugin'
    $InstalledEvidence = Assert-WorkerHandshake `
        $Install $Descriptor.VersionName $SelectedEngineVersion $EngineEvidence

    $Manifest = [ordered]@{
        schema = 'ue.plugin-install.v1'
        installedAtUtc = [DateTime]::UtcNow.ToString('o')
        version = $Descriptor.VersionName
        engineVersion = $InstalledEvidence.engineVersion
        contractDigest = $InstalledEvidence.contractDigest
        providerSchemaDigest = $InstalledEvidence.providerSchemaDigest
        engineDirectory = $InstalledEvidence.engineDirectory
        unrealInsightsPath = $InstalledEvidence.unrealInsightsPath
        installRoot = $Install
        stagingRoot = $Staging
        backupRoot = if ($BackedUp) { $Backup } else { $null }
        fileCount = $ExpectedHashes.Count
        files = $ExpectedHashes
    }
    $ManifestPath = Join-Path $BackupRoot (
        'UE_AI_integration-install-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')
    $Manifest['manifestPath'] = $ManifestPath
    [IO.File]::WriteAllText(
        $ManifestPath,
        ($Manifest | ConvertTo-Json -Depth 5),
        [Text.UTF8Encoding]::new($false))
    if ($RemoveBackupOnSuccess -and $BackedUp) {
        Remove-Item -LiteralPath $Backup -Recurse -Force
        $BackedUp = $false
    }
    $Manifest | ConvertTo-Json -Depth 4
}
catch {
    if ($Activated -and (Test-Path -LiteralPath $Install)) {
        Remove-Item -LiteralPath $Install -Recurse -Force
        $Activated = $false
    }
    if ($BackedUp -and (Test-Path -LiteralPath $Backup)) {
        Move-Item -LiteralPath $Backup -Destination $Install
        $BackedUp = $false
    }
    throw
}
finally {
    if (Test-Path -LiteralPath $Temporary) {
        $ResolvedTemporary = [IO.Path]::GetFullPath($Temporary)
        if ($ResolvedTemporary.StartsWith(
                $InstallParent + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($ResolvedTemporary).StartsWith(
                'UE_AI_integration.installing-',
                [StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $ResolvedTemporary -Recurse -Force
        }
    }
}
