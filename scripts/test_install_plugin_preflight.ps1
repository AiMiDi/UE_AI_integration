[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Installer,

    [Parameter(Mandatory = $true)]
    [string] $WorkerFixture,

    [Parameter(Mandatory = $true)]
    [string] $SourceRoot
)

$ErrorActionPreference = 'Stop'

function Write-Bytes([string] $Path, [byte[]] $Bytes) {
    $Parent = Split-Path -Parent $Path
    if ($Parent) {
        New-Item -ItemType Directory -Path $Parent -Force | Out-Null
    }
    [IO.File]::WriteAllBytes($Path, $Bytes)
}

function Get-Digest([string] $Path) {
    return 'sha256:' + (
        Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-ContractEvidence([string] $PluginRoot, [string] $EngineVersion) {
    $RelativeFiles = @(
        'Resources/Capabilities/production.json',
        "Resources/Trace/insights-actions.$EngineVersion.json",
        'Resources/Trace/launch-profiles.json',
        'Resources/Trace/worker-protocol.v1.json'
    ) | Sort-Object
    $Stream = [IO.MemoryStream]::new()
    try {
        foreach ($Relative in $RelativeFiles) {
            $PathBytes = [Text.Encoding]::UTF8.GetBytes($Relative.Replace('\', '/'))
            $Stream.Write($PathBytes, 0, $PathBytes.Length)
            $Stream.WriteByte(0)
            $Contents = [IO.File]::ReadAllBytes((Join-Path $PluginRoot $Relative))
            $Stream.Write($Contents, 0, $Contents.Length)
            $Stream.WriteByte(0)
        }
        $Hasher = [Security.Cryptography.SHA256]::Create()
        try {
            $Digest = $Hasher.ComputeHash($Stream.ToArray())
        }
        finally {
            $Hasher.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }
    return [pscustomobject]@{
        contractDigest = 'sha256:' + (
            [BitConverter]::ToString($Digest).Replace('-', '').ToLowerInvariant())
        providerSchemaDigest = Get-Digest (
            Join-Path $PluginRoot "Resources/Trace/insights-actions.$EngineVersion.json")
    }
}

function Write-BundleManifest(
    [string] $WorkerDirectory,
    [string] $EngineVersion,
    $Evidence
) {
    $Files = @(
        'UEAITraceWorker.exe',
        'UEAITraceWorker.pdb',
        "insights-actions.$EngineVersion.json",
        'launch-profiles.json',
        'worker-protocol.v1.json'
    ) | ForEach-Object {
        $File = Get-Item -LiteralPath (Join-Path $WorkerDirectory $_)
        [ordered]@{
            name = $File.Name
            size = $File.Length
            sha256 = (Get-FileHash -LiteralPath $File.FullName -Algorithm SHA256).Hash
        }
    }
    $Manifest = [ordered]@{
        schema = 'ue.trace-worker-bundle.v1'
        workerVersion = '0.9.0'
        engineVersion = $EngineVersion
        protocolVersion = 1
        contractDigest = $Evidence.contractDigest
        providerSchemaDigest = $Evidence.providerSchemaDigest
        files = @($Files)
    }
    [IO.File]::WriteAllText(
        (Join-Path $WorkerDirectory 'bundle-manifest.json'),
        ($Manifest | ConvertTo-Json -Depth 5),
        [Text.UTF8Encoding]::new($false))
}

function Invoke-Preflight(
    [string] $Staging,
    [string] $Install,
    [string] $EngineVersion
) {
    $Arguments = @{
        StagingPluginRoot = $Staging
        InstallPluginRoot = $Install
        PreflightOnly = $true
        EngineRoot = $FakeEngineRoot
    }
    if ($EngineVersion) {
        $Arguments['EngineVersion'] = $EngineVersion
    }
    return (& $Installer @Arguments | Out-String) | ConvertFrom-Json
}

function Assert-Throws([scriptblock] $Action, [string] $ExpectedText) {
    try {
        & $Action | Out-Null
    }
    catch {
        if ($_.Exception.Message -notlike "*$ExpectedText*") {
            throw "Expected error containing '$ExpectedText', got: $($_.Exception.Message)"
        }
        return
    }
    throw "Expected an error containing '$ExpectedText'."
}

$Installer = (Resolve-Path -LiteralPath $Installer).Path
$WorkerFixture = (Resolve-Path -LiteralPath $WorkerFixture).Path
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$TemporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'ue-plugin-install-preflight-' + [guid]::NewGuid().ToString('N'))
$Staging = Join-Path $TemporaryRoot 'Staging\UE_AI_integration'
$Install = Join-Path $TemporaryRoot 'Project\Plugins\UE_AI_integration'
$PreviousContract = [Environment]::GetEnvironmentVariable(
    'FAKE_TRACE_CONTRACT_DIGEST', 'Process')
$PreviousProvider = [Environment]::GetEnvironmentVariable(
    'FAKE_TRACE_PROVIDER_DIGEST', 'Process')
$PreviousInsightsAvailable = [Environment]::GetEnvironmentVariable(
    'FAKE_TRACE_INSIGHTS_AVAILABLE', 'Process')
$PreviousInsightsPath = [Environment]::GetEnvironmentVariable(
    'FAKE_TRACE_INSIGHTS_PATH', 'Process')
$FakeEngineRoot = Join-Path $TemporaryRoot 'EngineRoot'

try {
    $FakeEngineDirectory = Join-Path $FakeEngineRoot 'Engine'
    New-Item -ItemType Directory -Path (
        Join-Path $FakeEngineDirectory 'Build') -Force | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $FakeEngineDirectory 'Build\Build.version'),
        (@{ MajorVersion = 5; MinorVersion = 3; PatchVersion = 0 } |
            ConvertTo-Json -Compress),
        [Text.UTF8Encoding]::new($false))
    Write-Bytes (
        Join-Path $FakeEngineDirectory 'Binaries\Win64\UnrealInsights.exe') `
        ([byte[]]@(10, 11, 12))
    New-Item -ItemType Directory -Path $Staging -Force | Out-Null
    $Descriptor = [ordered]@{
        FileVersion = 3
        Version = 900
        VersionName = '0.9.0'
        FriendlyName = 'UE AI Integration Fixture'
    }
    [IO.File]::WriteAllText(
        (Join-Path $Staging 'UE_AI_integration.uplugin'),
        ($Descriptor | ConvertTo-Json),
        [Text.UTF8Encoding]::new($false))

    $Copies = @(
        @('Resources\Capabilities\production.json',
            'Resources\Capabilities\production.json'),
        @('Resources\Trace\insights-actions.5.3.json',
            'Resources\Trace\insights-actions.5.3.json'),
        @('Resources\Trace\launch-profiles.json',
            'Resources\Trace\launch-profiles.json'),
        @('Resources\Trace\worker-protocol.v1.json',
            'Resources\Trace\worker-protocol.v1.json')
    )
    foreach ($Pair in $Copies) {
        $Destination = Join-Path $Staging $Pair[1]
        New-Item -ItemType Directory -Path (
            Split-Path -Parent $Destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $SourceRoot $Pair[0]) `
            -Destination $Destination
    }

    $Binaries = Join-Path $Staging 'Binaries\Win64'
    Write-Bytes (Join-Path $Binaries 'UnrealEditor-UE_AI_integration.dll') ([byte[]]@(1))
    Write-Bytes (Join-Path $Binaries 'UnrealEditor-UE_AI_integration.pdb') ([byte[]]@(2))
    Write-Bytes (Join-Path $Binaries 'UnrealEditor-UEAITraceAnalysisCore.dll') ([byte[]]@(3))
    Write-Bytes (Join-Path $Binaries 'UnrealEditor-UEAITraceAnalysisCore.pdb') ([byte[]]@(4))

    $RuntimePrecompiled = Join-Path $Staging `
        'Intermediate\Build\Win64\UnrealGame\Development\UEAITraceRuntime\UEAITraceRuntime.precompiled'
    $RuntimeObject = Join-Path $Staging `
        'Intermediate\Build\Win64\x64\UnrealGame\Development\UEAITraceRuntime\Module.UEAITraceRuntime.cpp.obj'
    Write-Bytes $RuntimePrecompiled ([byte[]]@(6))
    Write-Bytes $RuntimeObject ([byte[]]@(7))

    $WorkerDirectory = Join-Path $Staging 'Tools\Trace\Win64\5.3'
    New-Item -ItemType Directory -Path $WorkerDirectory -Force | Out-Null
    Copy-Item -LiteralPath $WorkerFixture `
        -Destination (Join-Path $WorkerDirectory 'UEAITraceWorker.exe')
    Write-Bytes (Join-Path $WorkerDirectory 'UEAITraceWorker.pdb') ([byte[]]@(5))
    Copy-Item -LiteralPath (Join-Path $Staging 'Resources\Trace\insights-actions.5.3.json') `
        -Destination (Join-Path $WorkerDirectory 'insights-actions.5.3.json')
    Copy-Item -LiteralPath (Join-Path $Staging 'Resources\Trace\launch-profiles.json') `
        -Destination (Join-Path $WorkerDirectory 'launch-profiles.json')
    Copy-Item -LiteralPath (Join-Path $Staging 'Resources\Trace\worker-protocol.v1.json') `
        -Destination (Join-Path $WorkerDirectory 'worker-protocol.v1.json')

    $Evidence = Get-ContractEvidence $Staging '5.3'
    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_CONTRACT_DIGEST', $Evidence.contractDigest, 'Process')
    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_PROVIDER_DIGEST', $Evidence.providerSchemaDigest, 'Process')
    Write-BundleManifest $WorkerDirectory '5.3' $Evidence

    $Result = Invoke-Preflight $Staging $Install '5.3'
    if ($Result.schema -ne 'ue.plugin-install-preflight.v1' -or
        $Result.version -ne '0.9.0' -or
        $Result.engineVersion -ne '5.3' -or
        [IO.Path]::GetFullPath([string]$Result.engineDirectory) -ne
            [IO.Path]::GetFullPath($FakeEngineDirectory) -or
        [IO.Path]::GetFullPath([string]$Result.unrealInsightsPath) -ne
            [IO.Path]::GetFullPath((
                Join-Path $FakeEngineDirectory 'Binaries\Win64\UnrealInsights.exe')) -or
        $Result.contractDigest -ne $Evidence.contractDigest -or
        $Result.providerSchemaDigest -ne $Evidence.providerSchemaDigest) {
        throw 'Positive install preflight evidence does not match the fixture.'
    }

    $BuildVersionPath = Join-Path $FakeEngineDirectory 'Build\Build.version'
    $OriginalBuildVersion = [IO.File]::ReadAllBytes($BuildVersionPath)
    [IO.File]::WriteAllText(
        $BuildVersionPath,
        (@{ MajorVersion = 5; MinorVersion = 4; PatchVersion = 0 } |
            ConvertTo-Json -Compress),
        [Text.UTF8Encoding]::new($false))
    Assert-Throws {
        Invoke-Preflight $Staging $Install '5.3'
    } 'does not match Worker 5.3'
    [IO.File]::WriteAllBytes($BuildVersionPath, $OriginalBuildVersion)

    $InsightsPath = Join-Path $FakeEngineDirectory `
        'Binaries\Win64\UnrealInsights.exe'
    Remove-Item -LiteralPath $InsightsPath
    Assert-Throws {
        Invoke-Preflight $Staging $Install '5.3'
    } 'has no UnrealInsights.exe'
    Write-Bytes $InsightsPath ([byte[]]@(10, 11, 12))

    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_INSIGHTS_AVAILABLE', 'false', 'Process')
    Assert-Throws {
        Invoke-Preflight $Staging $Install '5.3'
    } 'did not bind to the selected Engine Unreal Insights'
    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_INSIGHTS_AVAILABLE', $null, 'Process')

    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_INSIGHTS_PATH', (Join-Path $TemporaryRoot 'wrong.exe'), 'Process')
    Assert-Throws {
        Invoke-Preflight $Staging $Install '5.3'
    } 'did not bind to the selected Engine Unreal Insights'
    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_INSIGHTS_PATH', $null, 'Process')

    $PollutedSaved = Join-Path $Staging 'Tools\UEAITraceWorker\Saved\Logs'
    New-Item -ItemType Directory -Path $PollutedSaved -Force | Out-Null
    Write-Bytes (Join-Path $PollutedSaved 'UEAITraceWorker.log') ([byte[]]@(13))
    Assert-Throws {
        Invoke-Preflight $Staging $Install '5.3'
    } 'contains generated Tools/UEAITraceWorker/Saved'
    Remove-Item -LiteralPath (Join-Path $Staging 'Tools\UEAITraceWorker\Saved') `
        -Recurse -Force

    $AnalysisPdb = Join-Path $Binaries 'UnrealEditor-UEAITraceAnalysisCore.pdb'
    Remove-Item -LiteralPath $AnalysisPdb
    Assert-Throws {
        Invoke-Preflight $Staging $Install '5.3'
    } 'UnrealEditor-UEAITraceAnalysisCore.pdb'
    Write-Bytes $AnalysisPdb ([byte[]]@(4))

    Remove-Item -LiteralPath $RuntimePrecompiled
    Assert-Throws {
        Invoke-Preflight $Staging $Install '5.3'
    } 'UEAITraceRuntime.precompiled'
    Write-Bytes $RuntimePrecompiled ([byte[]]@(6))

    $ShippingRuntime = Join-Path $Staging `
        'Intermediate\Build\Win64\UnrealGame\Shipping\UEAITraceRuntime\UEAITraceRuntime.precompiled'
    Write-Bytes $ShippingRuntime ([byte[]]@(8))
    Assert-Throws {
        Invoke-Preflight $Staging $Install '5.3'
    } 'must not be present in a Shipping build artifact'
    Remove-Item -LiteralPath $ShippingRuntime

    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_CONTRACT_DIGEST',
        ('sha256:' + ('f' * 64)),
        'Process')
    Assert-Throws {
        Invoke-Preflight $Staging $Install '5.3'
    } 'handshake does not match'
    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_CONTRACT_DIGEST', $Evidence.contractDigest, 'Process')

    $OtherWorkerDirectory = Join-Path $Staging 'Tools\Trace\Win64\5.4'
    New-Item -ItemType Directory -Path $OtherWorkerDirectory -Force | Out-Null
    Copy-Item -LiteralPath $WorkerFixture `
        -Destination (Join-Path $OtherWorkerDirectory 'UEAITraceWorker.exe')
    Assert-Throws {
        Invoke-Preflight $Staging $Install ''
    } 'Multiple staged Trace Worker Engine versions'

    $ExplicitResult = Invoke-Preflight $Staging $Install '5.3'
    if ($ExplicitResult.engineVersion -ne '5.3') {
        throw 'Explicit Engine selection did not remain bound to the 5.3 Worker.'
    }

    # Exercise the complete Windows PowerShell 5.1 activation path as well as
    # preflight. This covers canonical relative hashing, staging-copy evidence,
    # backup/replace, and the post-activation Worker handshake.
    Remove-Item -LiteralPath $OtherWorkerDirectory -Recurse -Force
    New-Item -ItemType Directory -Path (Split-Path -Parent $Install) -Force |
        Out-Null
    New-Item -ItemType Directory -Path $Install -Force | Out-Null
    Write-Bytes (Join-Path $Install 'legacy.bin') ([byte[]]@(9, 8, 7))
    $BackupRoot = Join-Path $TemporaryRoot 'Backups'
    $InstallResult = (& $Installer `
        -StagingPluginRoot $Staging `
        -InstallPluginRoot $Install `
        -BackupRoot $BackupRoot `
        -EngineVersion '5.3' `
        -EngineRoot $FakeEngineRoot | Out-String) | ConvertFrom-Json
    if ($InstallResult.schema -ne 'ue.plugin-install.v1' -or
        $InstallResult.version -ne '0.9.0' -or
        $InstallResult.engineVersion -ne '5.3' -or
        -not (Test-Path -LiteralPath (
            Join-Path $Install 'UE_AI_integration.uplugin') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (
            Join-Path $InstallResult.backupRoot 'legacy.bin') -PathType Leaf)) {
        throw 'Full install activation did not replace and back up the fixture atomically.'
    }
    Write-Output 'install plugin preflight tests passed'
}
finally {
    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_CONTRACT_DIGEST', $PreviousContract, 'Process')
    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_PROVIDER_DIGEST', $PreviousProvider, 'Process')
    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_INSIGHTS_AVAILABLE', $PreviousInsightsAvailable, 'Process')
    [Environment]::SetEnvironmentVariable(
        'FAKE_TRACE_INSIGHTS_PATH', $PreviousInsightsPath, 'Process')
    if (Test-Path -LiteralPath $TemporaryRoot) {
        $Resolved = [IO.Path]::GetFullPath($TemporaryRoot)
        $Prefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if ($Resolved.StartsWith($Prefix, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($Resolved).StartsWith(
                'ue-plugin-install-preflight-', [StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $Resolved -Recurse -Force
        }
    }
}
