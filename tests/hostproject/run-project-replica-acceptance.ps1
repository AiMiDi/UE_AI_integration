<#
.SYNOPSIS
    Runs real project asset edits in a new replica, using an already built plugin.
.DESCRIPTION
    Never builds, deploys to the original project, changes its ACLs, or touches
    a user-owned Editor. Junctions retain the original /Game paths but are not
    OS read-only mounts. The native case vetoes package saves outside its own
    directory; this script verifies the exact original seed bytes afterwards.
    Shader/runtime evidence requires NonNullRHI; this is not visual acceptance.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $PackagedPluginRoot,
    [Parameter(Mandatory)][string] $BuildSummaryPath,
    [Parameter(Mandatory)][string] $WorkRoot,
    [string] $OriginalProjectRoot = 'S:\SilverPalace\Project',
    [string] $EngineRoot = 'S:\SilverPalace\unrealengine',
    [switch] $PrepareOnly
)
$ErrorActionPreference = 'Stop'

function Assert-Condition([bool] $Condition, [string] $Message) {
    if (-not $Condition) { throw $Message }
}

function Get-FullPath([string] $Path) {
    [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Assert-IndependentDirectory([string] $Path) {
    $resolvedPath = Get-FullPath $Path
    Assert-Condition ($resolvedPath.StartsWith('S:\tmp\', [StringComparison]::OrdinalIgnoreCase)) `
        'Replica directories must be below S:\tmp.'
    $cursor = $resolvedPath
    while ($cursor -and $cursor -ne 'S:\') {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -Force -LiteralPath $cursor
            Assert-Condition (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) `
                "Independent directory has a reparse-point ancestor: $cursor"
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}

function Get-FileIdentity([string] $Path) {
    $file = Get-Item -LiteralPath $Path
    [ordered]@{
        path = $file.FullName
        bytes = [int64]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-PackageDigest([string] $Root) {
    $entries = @(
        foreach ($file in @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Sort-Object FullName)) {
            $relative = [IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/')
            $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            "$relative|$($file.Length)|$hash"
        }
    )
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData(
        [Text.Encoding]::UTF8.GetBytes($entries -join "`n"))).ToLowerInvariant()
}

function Get-OriginalSeedIdentities {
    foreach ($relative in @(
        'Content\LogicRes\FeatureTest\AudioTest\M_AudioDebugColor.uasset',
        'Content\ArtRes\SeqRes\Templete_BP_Res\Prop\BP_Int_Gen_Taskline_Photo_A_Paper_01.uasset'
    )) {
        $asset = Join-Path $OriginalProjectRoot $relative
        Get-FileIdentity $asset
        foreach ($extension in @('.uexp', '.ubulk', '.uptnl')) {
            $sidecar = [IO.Path]::ChangeExtension($asset, $extension)
            if (Test-Path -LiteralPath $sidecar -PathType Leaf) { Get-FileIdentity $sidecar }
        }
    }
}

function Write-Evidence([string] $Path, $Value) {
    # Publish only complete JSON. The native reader can observe this path as
    # soon as it exists, so the temporary file must be in the same directory.
    $temporary = "$Path.$([Guid]::NewGuid().ToString('N')).tmp"
    try {
        $json = $Value | ConvertTo-Json -Depth 18
        [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $Path)
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Resolve-ObservedPath([string] $Path) {
    if (-not [IO.Path]::IsPathRooted($Path)) {
        $Path = Join-Path (Join-Path $EngineRoot 'Engine\Binaries\Win64') $Path
    }
    Get-FullPath $Path
}

function Get-ModuleProof([Diagnostics.Process] $Process, [int] $Port, $Dll, $Pdb) {
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    $lastFailure = 'Editor module-proof endpoint is not ready.'
    while (-not $Process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        try {
            $schema = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/capabilities?operation=production.module.loaded.get&detail=full" `
                -Method Get -TimeoutSec 3
            $descriptors = @($schema.data.capabilities)
            Assert-Condition ($schema.ok -eq $true -and $descriptors.Count -eq 1 `
                -and $descriptors[0].id -eq 'production.module.loaded.get' `
                -and $descriptors[0].available -eq $true) 'Exact live module-proof schema is unavailable.'
            $body = @{ capability = 'production.module.loaded.get'; params = @{}; requestId = "replica-$([Guid]::NewGuid().ToString('N'))" } |
                ConvertTo-Json -Depth 4 -Compress
            $response = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/execute" `
                -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 3
            Assert-Condition ($response.ok -eq $true -and $null -ne $response.data) 'Native module proof failed.'
            $data = $response.data
            $identity = $data.loadedModuleIdentity
            Assert-Condition ($data.plugin -eq 'UE_AI_integration' -and $data.module -eq 'UE_AI_integration' `
                -and $data.loaded -eq $true -and $data.processId -eq $Process.Id) 'Native module or Editor PID differs.'
            Assert-Condition ($identity.schema -eq 'ue.loaded-module-identity.v1' `
                -and $identity.plugin -eq 'UE_AI_integration' -and $identity.module -eq 'UE_AI_integration' `
                -and $identity.processId -eq $Process.Id `
                -and (Resolve-ObservedPath $identity.modulePath).Equals($Dll.path, [StringComparison]::OrdinalIgnoreCase) `
                -and $identity.moduleSha256 -eq $Dll.sha256 `
                -and (Resolve-ObservedPath $identity.pdbPath).Equals($Pdb.path, [StringComparison]::OrdinalIgnoreCase) `
                -and $identity.pdbSha256 -eq $Pdb.sha256 `
                -and (Resolve-ObservedPath $identity.latestBuildArtifactPath).Equals($Dll.path, [StringComparison]::OrdinalIgnoreCase) `
                -and $identity.latestBuildArtifactSha256 -eq $Dll.sha256 `
                -and $identity.editorStartedAtUtc -eq $data.editorStartedAtUtc) 'Loaded-module identity tuple differs from the selected package.'
            foreach ($path in @($data.modulePath, $data.dll.path, $data.latestBuildArtifactPath)) {
                Assert-Condition ((Resolve-ObservedPath $path).Equals($Dll.path, [StringComparison]::OrdinalIgnoreCase)) `
                    'Loaded DLL provenance path differs from the copied package.'
            }
            Assert-Condition ((Resolve-ObservedPath $data.pdb.path).Equals($Pdb.path, [StringComparison]::OrdinalIgnoreCase)) `
                'PDB provenance path differs from the copied package.'
            Assert-Condition ($data.dll.exists -eq $true -and $data.pdb.exists -eq $true `
                -and $data.dll.sha256 -eq $Dll.sha256 -and $data.pdb.sha256 -eq $Pdb.sha256 `
                -and $data.latestBuildArtifactSha256 -eq $Dll.sha256 `
                -and $data.latestBuildArtifactExists -eq $true -and $data.matchesLatestBuildArtifact -eq $true) `
                'Exact native DLL/PDB/latest-artifact hashes differ.'
            Assert-Condition ($null -ne $data.liveCoding -and $data.liveCoding.compiling -ne $true `
                -and $data.liveCoding.enabledForSession -ne $true -and $data.liveCoding.lastPatchResult -eq 'none' `
                -and -not [string]::IsNullOrEmpty($data.editorStartedAtUtc)) 'Unpatched Editor start identity is not proven.'
            return $data
        }
        catch {
            # Do not echo HTTP responses, project config, or process credentials.
            $lastFailure = $_.Exception.Message
        }
        Start-Sleep -Milliseconds 250
        $Process.Refresh()
    }
    throw "Exact module identity could not be verified: $lastFailure"
}

function Save-SanitizedLog([string] $Path, [string] $Text) {
    $Text = [regex]::Replace($Text, '(?i)(Bearer\s+)[A-Za-z0-9\-\._~+/]+=*', '$1<redacted>')
    $Text = [regex]::Replace($Text, '(?im)^([^\r\n]*(?:password|HordeToken|api[_-]?key|access[_-]?token|authorization)\s*[=:]\s*)[^\r\n]*$', '$1<redacted>')
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Invoke-ReplicaPhase([string] $Phase, $Dll, $Pdb) {
    $phaseRoot = Join-Path $evidenceRoot $Phase
    New-Item -ItemType Directory -Path $phaseRoot | Out-Null
    $gate = Join-Path $phaseRoot 'module-before-gate.json'
    $result = Join-Path $phaseRoot 'native-result.json'
    $ack = Join-Path $phaseRoot 'module-after-ack.json'
    $report = Join-Path $phaseRoot 'Automation'
    $port = Get-Random -Minimum 39000 -Maximum 49000
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $editor
    $start.WorkingDirectory = $replica
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @(
        $replicaProject, '-unattended', '-nop4', '-SCCProvider=None', '-nosplash', '-NoSound',
        '-RenderOffscreen', '-NoLiveCoding', '-stdout', '-FullStdOutLogOutput',
        "-UserDir=$(Join-Path $replica 'User')", "-abslog=$(Join-Path $phaseRoot 'Editor.log')",
        '-ExecCmds=Automation RunTests UE_AI_integration.ProjectReplica.RealAssetsWriteRestoreReferencesAndRuntime',
        '-TestExit=Automation Test Queue Empty', "-ReportExportPath=$report"
    )) { $start.ArgumentList.Add($argument) }
    $start.Environment['UE_PORT'] = [string]$port
    foreach ($entry in @{
        UEAI_PROJECT_REPLICA_RUN = $runId; UEAI_PROJECT_REPLICA_PHASE = $Phase
        UEAI_PROJECT_REPLICA_ROOT = $replica; UEAI_PROJECT_REPLICA_GATE = $gate
        UEAI_PROJECT_REPLICA_RESULT = $result; UEAI_PROJECT_REPLICA_ACK = $ack
    }.GetEnumerator()) { $start.Environment[$entry.Key] = [string]$entry.Value }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $startedProcess = $false
    $stdout = $null
    $stderr = $null
    try {
        Assert-Condition ($process.Start()) 'Could not start the isolated replica Editor.'
        $startedProcess = $true
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $moduleBefore = Get-ModuleProof $process $port $Dll $Pdb
        Write-Evidence $gate @{
            runId = $runId; phase = $Phase; processId = $process.Id
            moduleVerified = $true; independentWriteRoot = $true; moduleProof = $moduleBefore
        }
        $deadline = [DateTime]::UtcNow.AddSeconds(360)
        while (-not (Test-Path -LiteralPath $result -PathType Leaf) `
            -and -not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 250
            $process.Refresh()
        }
        Assert-Condition (Test-Path -LiteralPath $result -PathType Leaf) `
            'Native replica acceptance did not publish a bounded result; inspect its Automation report and retained native logs for evidence-publication errors.'
        $native = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
        Assert-Condition ($native.schema -eq 'ue.project-replica-native-acceptance.v1' `
            -and $native.runId -eq $runId -and $native.phase -eq $Phase `
            -and $native.processId -eq $process.Id) 'Native evidence belongs to a different run or Editor.'
        $moduleAfter = Get-ModuleProof $process $port $Dll $Pdb
        Assert-Condition ($moduleAfter.editorStartedAtUtc -eq $moduleBefore.editorStartedAtUtc) 'Editor restarted during acceptance.'
        Write-Evidence $ack @{ processId = $process.Id; moduleVerified = $true; moduleProof = $moduleAfter }
        Assert-Condition ($process.WaitForExit(120000)) 'Isolated Editor did not exit after module-proof acknowledgement.'
        Assert-Condition ($process.ExitCode -eq 0) "Isolated Editor returned exit code $($process.ExitCode)."
        Assert-Condition ($native.passed -eq $true -and $native.originalSaveVetoVerified -eq $true) 'Native replica acceptance failed.'
        if ($Phase -eq 'prepare' -or $Phase -eq 'verify') {
            Assert-Condition ($native.materialWriteRestoreVerified -eq $true -and $native.materialCompileVerified -eq $true) `
                'Real material write/restore/fresh shader compilation/readback was not verified.'
        }
        if ($Phase -eq 'rename' -or $Phase -eq 'verify') {
            Assert-Condition ($native.blueprintReferencesVerified -eq $true) 'Persisted real-asset references were not verified.'
        }
        if ($Phase -eq 'verify') {
            Assert-Condition ($native.runtimeVerified -eq $true) 'Real duplicate runtime bytecode was not verified.'
        }
        $index = Join-Path $report 'index.json'
        Assert-Condition (Test-Path -LiteralPath $index -PathType Leaf) 'Native Automation report is missing.'
        $automation = Get-Content -LiteralPath $index -Raw | ConvertFrom-Json
        $tests = @($automation.tests)
        Assert-Condition ($tests.Count -eq 1 -and $tests[0].fullTestPath -eq 'UE_AI_integration.ProjectReplica.RealAssetsWriteRestoreReferencesAndRuntime' `
            -and $tests[0].state -in @('Success', 'SuccessWithWarnings')) 'The exact native acceptance test did not succeed.'
        [ordered]@{ phase = $Phase; processId = $process.Id; moduleBefore = $moduleBefore; moduleAfter = $moduleAfter; native = $native; report = $index }
    }
    finally {
        # This process object was created by this invocation; no other Editor is addressed.
        if ($startedProcess -and -not $process.HasExited) {
            $process.Kill()
            $null = $process.WaitForExit(10000)
        }
        if ($stdout) { Save-SanitizedLog (Join-Path $phaseRoot 'stdout.log') ($stdout.GetAwaiter().GetResult()) }
        if ($stderr) { Save-SanitizedLog (Join-Path $phaseRoot 'stderr.log') ($stderr.GetAwaiter().GetResult()) }
        $editorLog = Join-Path $phaseRoot 'Editor.log'
        if (Test-Path -LiteralPath $editorLog) { Save-SanitizedLog $editorLog ([IO.File]::ReadAllText($editorLog)) }
        $process.Dispose()
    }
}

$PackagedPluginRoot = Get-FullPath (Resolve-Path -LiteralPath $PackagedPluginRoot).Path
$OriginalProjectRoot = Get-FullPath (Resolve-Path -LiteralPath $OriginalProjectRoot).Path
$EngineRoot = Get-FullPath (Resolve-Path -LiteralPath $EngineRoot).Path
$WorkRoot = Get-FullPath $WorkRoot
Assert-IndependentDirectory $WorkRoot
$build = Get-Content -LiteralPath $BuildSummaryPath -Raw | ConvertFrom-Json
Assert-Condition ($build.build.skipped -ne $true -and $build.build.exitCode -eq 0 `
    -and $build.sourceStableForBuild -eq $true -and $build.packageBinding.verified -eq $true) `
    'A successful, stable, content-bound native build receipt is required.'
Assert-Condition ((Get-PackageDigest $PackagedPluginRoot) -eq $build.packageBinding.packageContent.contentSha256) `
    'The supplied plugin package differs from its successful build receipt.'
$engineBuildId = (Get-Content -Raw -LiteralPath (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.modules') | ConvertFrom-Json).BuildId
foreach ($manifest in @(
    (Join-Path $OriginalProjectRoot 'Binaries\Win64\UnrealEditor.modules'),
    (Join-Path $PackagedPluginRoot 'Binaries\Win64\UnrealEditor.modules')
)) {
    Assert-Condition ((Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json).BuildId -eq $engineBuildId) `
        "BuildId mismatch; no manifest is rewritten: $manifest"
}
$editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
Assert-Condition (Test-Path -LiteralPath $editor -PathType Leaf) 'Matching Editor executable is missing.'
$runId = [Guid]::NewGuid().ToString('N')
$replica = Join-Path $WorkRoot "replica-$runId"
Assert-Condition (-not (Test-Path -LiteralPath $replica)) 'Replica directory already exists.'
New-Item -ItemType Directory -Path $replica -Force | Out-Null
$content = Join-Path $replica 'Content'
$validation = Join-Path $content 'UEAIValidation'
$runDirectory = Join-Path $validation $runId
$evidenceRoot = Join-Path $replica 'Saved\UEAIReplica'
$pluginCopy = Join-Path $replica 'Plugins\UE_AI_integration'
foreach ($directory in @($runDirectory, $evidenceRoot, (Join-Path $replica 'Plugins'))) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    Assert-IndependentDirectory $directory
}
Copy-Item -LiteralPath $PackagedPluginRoot -Destination $pluginCopy -Recurse
New-Item -ItemType Directory -Path (Join-Path $replica 'Config') | Out-Null
New-Item -ItemType Junction -Path (Join-Path $replica 'Binaries') -Target (Join-Path $OriginalProjectRoot 'Binaries') | Out-Null
$mounts = @(
    foreach ($directory in @(Get-ChildItem -LiteralPath (Join-Path $OriginalProjectRoot 'Content') -Directory)) {
        if ($directory.Name -eq 'UEAIValidation') { continue }
        $link = Join-Path $content $directory.Name
        New-Item -ItemType Junction -Path $link -Target $directory.FullName | Out-Null
        [ordered]@{ mount = $link; original = $directory.FullName; osReadOnly = $false }
    }
)
foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $OriginalProjectRoot 'Content') -File)) {
    Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $content $file.Name)
}
$originalDescriptor = Get-Content -Raw -LiteralPath (Join-Path $OriginalProjectRoot 'SilverPalace.uproject') | ConvertFrom-Json -AsHashtable
# Retain the real game's native module and its existing binaries, but do not
# import project startup maps, Lua/cook tools, auto-enabled Editor extensions,
# user settings, Saved or Intermediate into the acceptance process.
$disabledProjectPlugins = @(
    Get-ChildItem -LiteralPath (Join-Path $OriginalProjectRoot 'Plugins') -Recurse -File -Filter '*.uplugin' |
        ForEach-Object { $_.BaseName } | Sort-Object -Unique |
        Where-Object { $_ -notin @('UE_AI_integration', 'Niagara') } |
        ForEach-Object { @{ Name = $_; Enabled = $false } }
)
$descriptor = @{
    FileVersion = 3; EngineAssociation = $originalDescriptor.EngineAssociation
    Modules = $originalDescriptor.Modules
    DisableEnginePluginsByDefault = $true
    AdditionalPluginDirectories = @((Join-Path $OriginalProjectRoot 'Plugins'))
    Plugins = $disabledProjectPlugins + @(
        @{ Name = 'UE_AI_integration'; Enabled = $true }, @{ Name = 'Niagara'; Enabled = $true }
    )
}
$replicaProject = Join-Path $replica 'SilverPalace.uproject'
Write-Evidence $replicaProject $descriptor
@'

[/Script/EngineSettings.GameMapsSettings]
EditorStartupMap=
GameDefaultMap=/Engine/Maps/Entry.Entry
GlobalDefaultGameMode=/Script/Engine.GameModeBase
'@ | Set-Content -LiteralPath (Join-Path $replica 'Config\DefaultEngine.ini') -Encoding utf8NoBOM
@'

[/Script/UnrealEd.EditorLoadingSavingSettings]
LoadLevelAtStartup=None
bAutoSaveEnable=False
bPromptForCheckoutOnAssetModification=False
bAutomaticallyCheckoutOnAssetModification=False
'@ | Set-Content -LiteralPath (Join-Path $replica 'Config\DefaultEditorPerProjectUserSettings.ini') -Encoding utf8NoBOM
$dll = Get-FileIdentity (Join-Path $pluginCopy 'Binaries\Win64\UnrealEditor-UE_AI_integration.dll')
$pdb = Get-FileIdentity (Join-Path $pluginCopy 'Binaries\Win64\UnrealEditor-UE_AI_integration.pdb')
Assert-Condition ($dll.sha256 -eq $build.packageBinding.packageContent.moduleArtifacts.dll.sha256 `
    -and $pdb.sha256 -eq $build.packageBinding.packageContent.moduleArtifacts.pdb.sha256) 'Copied DLL/PDB differ from the bound build.'
$originalBefore = @(Get-OriginalSeedIdentities)
$summaryPath = Join-Path $evidenceRoot 'summary.json'
$summary = [ordered]@{
    schema = 'ue.project-replica-acceptance.v1'; runId = $runId; project = $replicaProject
    buildSummary = (Get-FullPath $BuildSummaryPath); engineBuildId = $engineBuildId
    preparedOnly = [bool]$PrepareOnly; mounts = $mounts; writeDirectory = $runDirectory
    expectedDll = $dll; expectedPdb = $pdb; originalBefore = $originalBefore
    originalAfter = $null; originalSeedsUnchanged = $null; phases = @(); passed = $false
    verification = [ordered]@{
        staticVerified = $true; compiled = $true; moduleLoaded = $null
        assetReadback = $null; runtimeVerified = $null; visualVerified = $null
        unknownReasons = @(
            'Junctions are not OS read-only mounts; save veto covers package writes during the native test.',
            'Original byte verification covers the two seed packages and their sidecars, not the entire project.',
            'A Game-world test of duplicated assets does not prove original project gameplay or visual quality.'
        )
    }
}
try {
    if (-not $PrepareOnly) {
        foreach ($phase in @('prepare', 'rename', 'verify')) {
            $summary.phases += Invoke-ReplicaPhase $phase $dll $pdb
        }
        Assert-Condition (@($summary.phases.processId | Sort-Object -Unique).Count -eq 3) 'Acceptance requires three independent Editor PIDs.'
        $summary.verification.moduleLoaded = $true
        $summary.verification.assetReadback = $true
        $summary.verification.runtimeVerified = $true
        $summary.passed = $true
    }
}
finally {
    $summary.originalAfter = @(Get-OriginalSeedIdentities)
    $summary.originalSeedsUnchanged = ($originalBefore | ConvertTo-Json -Depth 4 -Compress) -eq `
        ($summary.originalAfter | ConvertTo-Json -Depth 4 -Compress)
    if (-not $summary.originalSeedsUnchanged) { $summary.passed = $false }
    Write-Evidence $summaryPath $summary
    Write-Host "[replica] evidence: $summaryPath"
}
Assert-Condition ($summary.originalSeedsUnchanged) 'Original project seed bytes changed; acceptance failed.'
if (-not $PrepareOnly) { Assert-Condition ($summary.passed) 'Project replica acceptance failed.' }
