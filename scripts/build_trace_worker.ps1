[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $EngineRoot,

    [Parameter(Mandatory = $true)]
    [string] $PluginRoot,

    [Parameter(Mandatory = $true)]
    [string] $StagingPluginRoot
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingDirectory([string] $Path, [string] $Label) {
    $Resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $Resolved.Path -PathType Container)) {
        throw "$Label is not a directory: $Path"
    }
    return [System.IO.Path]::GetFullPath($Resolved.Path)
}

$EngineRoot = Resolve-ExistingDirectory $EngineRoot 'Engine root'
$PluginRoot = Resolve-ExistingDirectory $PluginRoot 'Plugin root'
$StagingPluginRoot = Resolve-ExistingDirectory $StagingPluginRoot 'Staging plugin root'
$GeneratedWorkerSaved = Join-Path $StagingPluginRoot 'Tools\UEAITraceWorker\Saved'
if (Test-Path -LiteralPath $GeneratedWorkerSaved) {
    throw 'Staging contains generated Tools/UEAITraceWorker/Saved output; rebuild from a clean staging directory.'
}

$BuildBat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'
$HostTemplate = Join-Path $PluginRoot 'Programs\UEAITraceWorker\UEAITraceWorker.uproject'
$TargetTemplate = Join-Path $PluginRoot 'Programs\UEAITraceWorker\Source\UEAITraceWorker.Target.cs'
$PluginDescriptor = Join-Path $PluginRoot 'UE_AI_integration.uplugin'
foreach ($RequiredFile in @($BuildBat, $HostTemplate, $TargetTemplate, $PluginDescriptor)) {
    if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
        throw "Required Trace Worker build input is missing: $RequiredFile"
    }
}

$TempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$HostRoot = Join-Path $TempRoot ('UEAITraceWorkerHost-' + [guid]::NewGuid().ToString('N'))
$HostRoot = [System.IO.Path]::GetFullPath($HostRoot)
$Comparison = [System.StringComparison]::OrdinalIgnoreCase
if (-not $HostRoot.StartsWith($TempRoot, $Comparison) -or
    -not ([System.IO.Path]::GetFileName($HostRoot)).StartsWith('UEAITraceWorkerHost-', $Comparison)) {
    throw "Refusing to create an unsafe Trace Worker host path: $HostRoot"
}

$PluginLink = Join-Path $HostRoot 'Plugins\UE_AI_integration'
$HostProject = Join-Path $HostRoot 'UEAITraceWorker.uproject'
$WorkerBinaryDirectory = Join-Path $HostRoot 'Binaries\Win64'

try {
    New-Item -ItemType Directory -Path (Join-Path $HostRoot 'Source') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $HostRoot 'Plugins') -Force | Out-Null
    Copy-Item -LiteralPath $HostTemplate -Destination $HostProject
    Copy-Item -LiteralPath $TargetTemplate -Destination (Join-Path $HostRoot 'Source\UEAITraceWorker.Target.cs')
    New-Item -ItemType Junction -Path $PluginLink -Target $PluginRoot | Out-Null

    & $BuildBat UEAITraceWorker Win64 Development `
        "-Project=$HostProject" -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles
    if ($LASTEXITCODE -ne 0) {
        throw "UEAITraceWorker build failed with exit code $LASTEXITCODE."
    }

    $WorkerExe = Join-Path $WorkerBinaryDirectory 'UEAITraceWorker.exe'
    if (-not (Test-Path -LiteralPath $WorkerExe -PathType Leaf)) {
        throw "UEAITraceWorker linked no executable at $WorkerExe"
    }

    $BuildVersion = Get-Content -LiteralPath (Join-Path $EngineRoot 'Engine\Build\Build.version') -Raw | ConvertFrom-Json
    $EngineVersion = "$($BuildVersion.MajorVersion).$($BuildVersion.MinorVersion)"
    $TraceStage = Join-Path $StagingPluginRoot "Tools\Trace\Win64\$EngineVersion"
    New-Item -ItemType Directory -Path $TraceStage -Force | Out-Null
    Copy-Item -LiteralPath $WorkerExe -Destination (Join-Path $TraceStage 'UEAITraceWorker.exe') -Force
    $WorkerPdb = Join-Path $WorkerBinaryDirectory 'UEAITraceWorker.pdb'
    if (-not (Test-Path -LiteralPath $WorkerPdb -PathType Leaf)) {
        throw "UEAITraceWorker linked no required Win64 PDB at $WorkerPdb"
    }
    Copy-Item -LiteralPath $WorkerPdb -Destination (Join-Path $TraceStage 'UEAITraceWorker.pdb') -Force
    $WorkerReceipt = Join-Path $WorkerBinaryDirectory 'UEAITraceWorker.target'
    if (Test-Path -LiteralPath $WorkerReceipt -PathType Leaf) {
        Copy-Item -LiteralPath $WorkerReceipt -Destination (Join-Path $TraceStage 'UEAITraceWorker.target') -Force
    }

    # These are the non-system dependencies reported by the UE 5.3 Program
    # receipt/PE import table. Keep them next to the Worker so it can start on
    # a machine where the Engine binary directories are not on PATH.
    $RuntimeDependencies = @(
        (Join-Path $EngineRoot 'Engine\Binaries\ThirdParty\Intel\TBB\Win64\tbb.dll'),
        (Join-Path $EngineRoot 'Engine\Binaries\ThirdParty\Intel\TBB\Win64\tbbmalloc.dll'),
        (Join-Path $EngineRoot 'Engine\Binaries\ThirdParty\AppLocalDependencies\Win64\DirectX\xinput1_3.dll'),
        (Join-Path $EngineRoot 'Engine\Binaries\ThirdParty\Windows\WinPixEventRuntime\x64\WinPixEventRuntime.dll'),
        (Join-Path $EngineRoot 'Engine\Binaries\ThirdParty\DbgHelp\dbghelp.dll')
    )
    foreach ($RuntimeDependency in $RuntimeDependencies) {
        if (-not (Test-Path -LiteralPath $RuntimeDependency -PathType Leaf)) {
            throw "Required Trace Worker runtime dependency is missing: $RuntimeDependency"
        }
        Copy-Item -LiteralPath $RuntimeDependency `
            -Destination (Join-Path $TraceStage ([System.IO.Path]::GetFileName($RuntimeDependency))) -Force
    }
    foreach ($ResourceName in @(
        "insights-actions.$EngineVersion.json",
        'launch-profiles.json',
        'worker-protocol.v1.json')) {
        Copy-Item -LiteralPath (Join-Path $PluginRoot "Resources\Trace\$ResourceName") `
            -Destination (Join-Path $TraceStage $ResourceName) -Force
    }

    $StagedWorker = Join-Path $TraceStage 'UEAITraceWorker.exe'
    $PluginDescriptor = Get-Content -LiteralPath $PluginDescriptor -Raw | ConvertFrom-Json
    $RequestId = 'package-' + [guid]::NewGuid().ToString('N')
    $Request = @{
        schema = 'ue.trace-worker-request.v1'
        action = 'handshake'
        requestId = $RequestId
    } | ConvertTo-Json -Compress
    $EngineDirectory = Join-Path $EngineRoot 'Engine'
    $ExpectedInsights = [IO.Path]::GetFullPath(
        (Join-Path $EngineDirectory 'Binaries\Win64\UnrealInsights.exe'))
    $RawHandshake = $Request | & $StagedWorker --stdio -NoLog -NoDefaultLog -SaveToUserDir `
        "-EngineDir=$EngineDirectory"
    if ($LASTEXITCODE -ne 0) {
        throw "Staged Trace Worker handshake failed with exit code $LASTEXITCODE."
    }
    $Handshake = $RawHandshake | ConvertFrom-Json
    if ($Handshake.schema -ne 'ue.trace-worker-response.v1' -or
        $Handshake.ok -ne $true -or
        $Handshake.meta.requestId -ne $RequestId -or
        $Handshake.data.schema -ne 'ue.trace-worker-handshake.v1' -or
        $Handshake.data.protocolVersion -ne 1 -or
        $Handshake.data.workerVersion -ne $PluginDescriptor.VersionName -or
        -not $Handshake.data.engineVersion.StartsWith($EngineVersion) -or
        $Handshake.data.contractBound -ne $true -or
        $Handshake.data.unrealInsightsAvailable -ne $true -or
        $Handshake.data.unrealInsightsEngineVersion -ne $EngineVersion -or
        [IO.Path]::GetFullPath([string]$Handshake.data.unrealInsightsPath) -ne $ExpectedInsights) {
        throw 'Staged Trace Worker handshake does not match the plugin/Engine contract.'
    }

    $TargetRequestId = 'package-targets-' + [guid]::NewGuid().ToString('N')
    $TargetRequest = @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = $TargetRequestId
        capability = 'production.trace.target.list'
        params = @{}
    } | ConvertTo-Json -Depth 5 -Compress
    $RawTargets = $TargetRequest | & $StagedWorker --stdio -NoLog -NoDefaultLog -SaveToUserDir `
        "-EngineDir=$EngineDirectory"
    if ($LASTEXITCODE -ne 0) {
        throw "Staged Trace Worker target.list failed with exit code $LASTEXITCODE."
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
        throw 'Staged Trace Worker does not expose the installed Development launch profile.'
    }

    # The Worker contract includes the launch profile bytes, so a valid-JSON
    # post-build mutation must fail closed instead of changing fixedArguments
    # behind an otherwise matching handshake digest.
    $StagedProfiles = Join-Path $TraceStage 'launch-profiles.json'
    $OriginalProfiles = [IO.File]::ReadAllBytes($StagedProfiles)
    try {
        $TamperedProfiles = New-Object byte[] ($OriginalProfiles.Length + 1)
        [Array]::Copy($OriginalProfiles, $TamperedProfiles, $OriginalProfiles.Length)
        $TamperedProfiles[$TamperedProfiles.Length - 1] = 0x20
        [IO.File]::WriteAllBytes($StagedProfiles, $TamperedProfiles)

        $TamperRequestId = 'package-profile-tamper-' + [guid]::NewGuid().ToString('N')
        $TamperRequest = @{
            schema = 'ue.trace-worker-request.v1'
            action = 'execute'
            requestId = $TamperRequestId
            capability = 'production.trace.target.list'
            params = @{}
        } | ConvertTo-Json -Depth 5 -Compress
        $RawTamperResult = $TamperRequest | & $StagedWorker --stdio -NoLog -NoDefaultLog -SaveToUserDir `
            "-EngineDir=$EngineDirectory"
        if ($LASTEXITCODE -ne 0) {
            throw "Staged Trace Worker tamper probe failed with exit code $LASTEXITCODE."
        }
        $TamperResult = $RawTamperResult | ConvertFrom-Json
        $TamperedDevelopmentTarget = @($TamperResult.data.targets |
            Where-Object { $_.kind -eq 'development' }) | Select-Object -First 1
        if ($TamperResult.schema -ne 'ue.trace-worker-response.v1' -or
            $TamperResult.ok -ne $true -or
            $TamperResult.meta.requestId -ne $TamperRequestId -or
            $null -eq $TamperedDevelopmentTarget -or
            $TamperedDevelopmentTarget.available -ne $false -or
            $TamperedDevelopmentTarget.reason -notmatch 'build contract') {
            throw 'Staged Trace Worker accepted a launch-profiles.json mutation.'
        }
    }
    finally {
        [IO.File]::WriteAllBytes($StagedProfiles, $OriginalProfiles)
    }

    $BundleFiles = Get-ChildItem -LiteralPath $TraceStage -File |
        Sort-Object Name |
        ForEach-Object {
            [ordered]@{
                name = $_.Name
                size = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        }
    $BundleManifest = [ordered]@{
        schema = 'ue.trace-worker-bundle.v1'
        workerVersion = $PluginDescriptor.VersionName
        engineVersion = $EngineVersion
        protocolVersion = 1
        contractDigest = $Handshake.data.contractDigest
        providerSchemaDigest = $Handshake.data.providerSchemaDigest
        files = @($BundleFiles)
    }
    [IO.File]::WriteAllText(
        (Join-Path $TraceStage 'bundle-manifest.json'),
        ($BundleManifest | ConvertTo-Json -Depth 5),
        [Text.UTF8Encoding]::new($false))

    if (Test-Path -LiteralPath $GeneratedWorkerSaved) {
        throw 'Trace Worker validation generated Tools/UEAITraceWorker/Saved despite file logging being disabled.'
    }

    Write-Host "UEAITraceWorker staged to $TraceStage"
}
finally {
    if (Test-Path -LiteralPath $PluginLink) {
        # Windows PowerShell 5.1 can throw a NullReferenceException when
        # Remove-Item targets a directory junction. Directory.Delete removes
        # the link itself and never traverses into the plugin source tree.
        [System.IO.Directory]::Delete($PluginLink)
    }
    if (Test-Path -LiteralPath $HostRoot) {
        $VerifiedHostRoot = [System.IO.Path]::GetFullPath($HostRoot)
        if (-not $VerifiedHostRoot.StartsWith($TempRoot, $Comparison) -or
            -not ([System.IO.Path]::GetFileName($VerifiedHostRoot)).StartsWith('UEAITraceWorkerHost-', $Comparison)) {
            throw "Refusing to remove an unsafe Trace Worker host path: $VerifiedHostRoot"
        }
        Remove-Item -LiteralPath $VerifiedHostRoot -Recurse -Force
    }
}
