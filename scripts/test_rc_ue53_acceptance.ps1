[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Archive,

    [Parameter(Mandatory = $true)]
    [string]$EngineRoot,

    [string]$EvidencePath,
    [switch]$KeepFixture
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-JsonCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int[]]$AcceptedExitCodes = @(0)
    )
    $lines = @(& $FilePath @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    Assert-Condition ($AcceptedExitCodes -contains $exitCode) (
        "Command failed with exit code ${exitCode}: $FilePath $($Arguments -join ' ')`n$($lines -join [Environment]::NewLine)")
    $text = $lines | Out-String
    try { return $text | ConvertFrom-Json }
    catch { throw "Command did not return JSON: $FilePath`n$text" }
}

function Invoke-NodeAcceptance {
    param([string]$Node, [string]$Script, [string[]]$Arguments)
    $lines = @(& $Node $Script @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    $jsonLine = $lines | Where-Object { ([string]$_).TrimStart().StartsWith('{') } | Select-Object -Last 1
    Assert-Condition ($null -ne $jsonLine) "Acceptance helper returned no JSON:`n$($lines -join [Environment]::NewLine)"
    try { $result = ([string]$jsonLine) | ConvertFrom-Json }
    catch { throw "Acceptance helper returned invalid JSON:`n$($lines -join [Environment]::NewLine)" }
    Assert-Condition ($exitCode -eq 0 -and $result.ok -eq $true) (
        "Acceptance helper failed with exit code ${exitCode}:`n$($lines -join [Environment]::NewLine)")
    return $result.data
}

function Get-FreeLoopbackPort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    try {
        $listener.Start()
        return ([Net.IPEndPoint]$listener.LocalEndpoint).Port
    }
    finally { $listener.Stop() }
}

function Start-OwnedEditor {
    param([string]$Editor, [string]$Project, [string]$LogPath)
    $arguments = @(
        ('"' + $Project + '"'),
        '-NoSplash',
        '-NoSound',
        '-NoLiveCoding',
        '-log',
        ('-abslog="' + $LogPath + '"')
    )
    $process = Start-Process -FilePath $Editor -ArgumentList $arguments -PassThru -WindowStyle Hidden
    Assert-Condition ($null -ne $process -and $process.Id -gt 0) 'The owned Unreal Editor process did not start.'
    return [ordered]@{
        Process = $process
        Id = $process.Id
        StartTimeUtc = $process.StartTime.ToUniversalTime()
    }
}

function Stop-OwnedEditor {
    param($Owned)
    if ($null -eq $Owned) { return }
    $candidate = Get-Process -Id ([int]$Owned.Id) -ErrorAction SilentlyContinue
    if ($null -eq $candidate) { return }
    $delta = [Math]::Abs(($candidate.StartTime.ToUniversalTime() - [DateTime]$Owned.StartTimeUtc).TotalSeconds)
    Assert-Condition ($delta -lt 2) "Refusing to terminate reused PID $($Owned.Id)."
    Stop-Process -Id $candidate.Id -Force
    $candidate.WaitForExit(10000) | Out-Null
}

function Wait-EditorHealth {
    param([string]$Endpoint, $Owned, [int]$TimeoutSeconds = 120)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $candidate = Get-Process -Id ([int]$Owned.Id) -ErrorAction SilentlyContinue
        if ($null -eq $candidate) { throw "Owned Unreal Editor PID $($Owned.Id) exited before health became ready." }
        try {
            $response = Invoke-RestMethod -Uri ($Endpoint + '/api/health') -Method Get -TimeoutSec 2
            if ($response.ok -eq $true -and $response.data.status -in @('ready', 'degraded')) {
                Assert-Condition ([int]$response.data.processId -eq [int]$Owned.Id) 'Health belongs to a different Editor process.'
                Assert-Condition ([string]$response.data.engineVersion -match '^5\.3(?:\.|$)') 'Fixture health is not from UE 5.3.'
                return $response.data
            }
        }
        catch {
            Start-Sleep -Milliseconds 500
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "UE 5.3 fixture health did not become ready within $TimeoutSeconds seconds."
}

function Get-PersistentFingerprint {
    param([string]$ProjectRoot)
    $result = [ordered]@{}
    $prefix = $ProjectRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    foreach ($file in Get-ChildItem -LiteralPath $ProjectRoot -Recurse -File | Sort-Object FullName) {
        $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
        if ($relative -match '^(Saved|Intermediate|DerivedDataCache)/') { continue }
        $result[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $result
}

function Assert-FingerprintEqual {
    param($Before, $After)
    $beforeKeys = @($Before.Keys | Sort-Object)
    $afterKeys = @($After.Keys | Sort-Object)
    Assert-Condition (($beforeKeys -join "`n") -ceq ($afterKeys -join "`n")) 'Fixture persistent file set changed after rollback.'
    foreach ($key in $beforeKeys) {
        Assert-Condition ([string]$Before[$key] -ceq [string]$After[$key]) "Fixture persistent file hash changed: $key"
    }
}

function Stop-OwnedRecipeWorkers {
    param([string]$RecipeRoot, [string]$BundleRoot)
    if (-not (Test-Path -LiteralPath $RecipeRoot -PathType Container)) { return }
    foreach ($journal in Get-ChildItem -LiteralPath $RecipeRoot -Recurse -File -Filter run.json -ErrorAction SilentlyContinue) {
        try { $state = Get-Content -LiteralPath $journal.FullName -Raw -Encoding UTF8 | ConvertFrom-Json }
        catch { continue }
        if ($null -eq $state.workerPid -or [int]$state.workerPid -le 0) { continue }
        $pidValue = [int]$state.workerPid
        $process = Get-Process -Id $pidValue -ErrorAction SilentlyContinue
        if ($null -eq $process) { continue }
        $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$pidValue" -ErrorAction SilentlyContinue
        $commandLine = if ($null -ne $cim) { [string]$cim.CommandLine } else { '' }
        $expectedModule = (Join-Path $BundleRoot 'MCP\dist\recipe-runner.js')
        if ($commandLine.Contains($expectedModule, [StringComparison]::OrdinalIgnoreCase) -and
            $commandLine.Contains([string]$state.runId, [StringComparison]::OrdinalIgnoreCase)) {
            Stop-Process -Id $pidValue -Force
        }
    }
}

$archivePath = [IO.Path]::GetFullPath($Archive)
$enginePath = [IO.Path]::GetFullPath($EngineRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
Assert-Condition (Test-Path -LiteralPath $archivePath -PathType Leaf) "Release archive not found: $archivePath"
$buildVersionPath = Join-Path $enginePath 'Engine\Build\Build.version'
$enginePlugins = Join-Path $enginePath 'Engine\Plugins'
$editorPath = Join-Path $enginePath 'Engine\Binaries\Win64\UnrealEditor.exe'
Assert-Condition (Test-Path -LiteralPath $buildVersionPath -PathType Leaf) 'EngineRoot must contain Engine/Build/Build.version.'
Assert-Condition (Test-Path -LiteralPath $enginePlugins -PathType Container) 'EngineRoot must contain Engine/Plugins.'
Assert-Condition (Test-Path -LiteralPath $editorPath -PathType Leaf) 'EngineRoot must contain UnrealEditor.exe.'
$buildVersion = Get-Content -LiteralPath $buildVersionPath -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-Condition ([int]$buildVersion.MajorVersion -eq 5 -and [int]$buildVersion.MinorVersion -eq 3) (
    "This gate requires an independent UE 5.3 installation; found $($buildVersion.MajorVersion).$($buildVersion.MinorVersion).")

$node = (Get-Command node -ErrorAction Stop).Source
$acceptanceHelper = Join-Path $PSScriptRoot 'rc_online_acceptance.mjs'
Assert-Condition (Test-Path -LiteralPath $acceptanceHelper -PathType Leaf) 'RC acceptance helper is missing.'

$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
$testRoot = Join-Path $temporaryBase ('ueai-rc-ue53-' + [Guid]::NewGuid().ToString('N'))
$extractRoot = Join-Path $testRoot 'archive'
$fixtureRoot = Join-Path $testRoot 'fixture'
$recipeRoot = Join-Path $testRoot 'recipes'
$evidence = $null
$ownedEditor = $null
$previousPort = $env:UE_PORT
$previousEngineRoot = $env:UEAI_ENGINE_ROOT

New-Item -ItemType Directory -Path $extractRoot, $fixtureRoot, $recipeRoot -Force | Out-Null
try {
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot
    $roots = @(Get-ChildItem -LiteralPath $extractRoot -Directory)
    Assert-Condition ($roots.Count -eq 1) 'Release archive must contain exactly one top-level directory.'
    $bundleRoot = $roots[0].FullName

    $manifestPath = Join-Path $bundleRoot 'Release\release-files.sha256.json'
    Assert-Condition (Test-Path -LiteralPath $manifestPath -PathType Leaf) 'Release SHA-256 manifest is missing.'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-Condition ($manifest.schema -eq 'ue.release-files.v1') 'Unsupported release manifest schema.'
    $manifestPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $manifest.files) {
        $relative = ([string]$entry.path).Replace('/', [IO.Path]::DirectorySeparatorChar)
        $candidate = [IO.Path]::GetFullPath((Join-Path $bundleRoot $relative))
        Assert-Condition ($candidate.StartsWith($bundleRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) "Manifest path escaped bundle: $($entry.path)"
        Assert-Condition (Test-Path -LiteralPath $candidate -PathType Leaf) "Manifest file is missing: $($entry.path)"
        $actual = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert-Condition ($actual -eq ([string]$entry.sha256).ToLowerInvariant()) "SHA-256 mismatch: $($entry.path)"
        [void]$manifestPaths.Add(([string]$entry.path).Replace('\', '/'))
    }
    $bundlePrefix = $bundleRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    foreach ($file in Get-ChildItem -LiteralPath $bundleRoot -Recurse -File) {
        $relative = $file.FullName.Substring($bundlePrefix.Length).Replace('\', '/')
        if ($relative -ieq 'Release/release-files.sha256.json') { continue }
        Assert-Condition ($manifestPaths.Contains($relative)) "Archive contains an unmanifested file: $relative"
    }

    $projectFile = Join-Path $fixtureRoot 'UEAIRCAcceptance.uproject'
    $projectDefinition = [ordered]@{
        FileVersion = 3
        EngineAssociation = '5.3'
        Category = ''
        Description = 'UE-AI-CLI owned RC acceptance fixture'
        Plugins = @(@{ Name = 'UE_AI_integration'; Enabled = $true })
    }
    [IO.File]::WriteAllText($projectFile, ($projectDefinition | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
    New-Item -ItemType Directory -Path (Join-Path $fixtureRoot 'Content'), (Join-Path $fixtureRoot 'Config'), (Join-Path $fixtureRoot 'Plugins') -Force | Out-Null
    Copy-Item -LiteralPath $bundleRoot -Destination (Join-Path $fixtureRoot 'Plugins\UE_AI_integration') -Recurse
    $baseline = Get-PersistentFingerprint $fixtureRoot

    $port = Get-FreeLoopbackPort
    $endpoint = "http://127.0.0.1:$port"
    $env:UE_PORT = [string]$port
    $env:UEAI_ENGINE_ROOT = $enginePath
    $firstLog = Join-Path $testRoot 'UnrealEditor-first.log'
    $ownedEditor = Start-OwnedEditor $editorPath $projectFile $firstLog
    $firstHealth = Wait-EditorHealth $endpoint $ownedEditor

    $ue = Join-Path $bundleRoot 'CLI\bin\ue-cli.exe'
    $doctor = Invoke-JsonCommand $ue @('doctor', '--full', '--bundle', $bundleRoot, '--endpoint', $endpoint, '--json')
    Assert-Condition ($doctor.ok -eq $true -and $doctor.data.schema -eq 'ue.doctor.v3') 'Online doctor v3 failed.'
    $offlineTools = Invoke-JsonCommand $ue @('test-tools', '--bundle', $bundleRoot, '--json')
    Assert-Condition ($offlineTools.ok -eq $true -and $offlineTools.data.schema -eq 'ue.test-tools.v2' -and $offlineTools.data.status -eq 'partial') 'Offline test-tools did not return partial success.'
    $onlineTools = Invoke-JsonCommand $ue @('test-tools', '--require-editor', '--bundle', $bundleRoot, '--endpoint', $endpoint, '--json')
    Assert-Condition ($onlineTools.ok -eq $true -and $onlineTools.data.status -eq 'passed') 'Online test-tools --require-editor failed.'

    $online = Invoke-NodeAcceptance $node $acceptanceHelper @('online', $bundleRoot, $endpoint)
    $assetRelative = ([string]$online.workflow.asset).TrimStart('/').Replace('/', [IO.Path]::DirectorySeparatorChar) + '.uasset'
    $assetFile = Join-Path (Join-Path $fixtureRoot 'Content') ($assetRelative -replace '^Game[\\/]', '')
    Assert-Condition (-not (Test-Path -LiteralPath $assetFile)) "Rollback left the temporary asset on disk: $assetFile"

    $recipeStart = Invoke-NodeAcceptance $node $acceptanceHelper @('recipe-start', $bundleRoot, $endpoint, $recipeRoot)
    Stop-OwnedEditor $ownedEditor
    $ownedEditor = $null

    $secondLog = Join-Path $testRoot 'UnrealEditor-second.log'
    $ownedEditor = Start-OwnedEditor $editorPath $projectFile $secondLog
    $secondHealth = Wait-EditorHealth $endpoint $ownedEditor
    $recipeResume = Invoke-NodeAcceptance $node $acceptanceHelper @(
        'recipe-resume', $bundleRoot, $endpoint, $recipeRoot,
        [string]$recipeStart.runId, [string]$recipeStart.planDigest)

    $after = Get-PersistentFingerprint $fixtureRoot
    Assert-FingerprintEqual $baseline $after
    $evidence = [ordered]@{
        ok = $true
        schema = 'ue.rc-ue53-acceptance.v1'
        archive = [IO.Path]::GetFileName($archivePath)
        archiveSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        releaseManifestSchema = $manifest.schema
        releaseFileCount = $manifest.files.Count
        engineVersion = "5.3.$($buildVersion.PatchVersion)"
        fixtureProjectSha256 = (Get-FileHash -LiteralPath $projectFile -Algorithm SHA256).Hash.ToLowerInvariant()
        firstServerInstanceId = $firstHealth.serverInstanceId
        secondServerInstanceId = $secondHealth.serverInstanceId
        doctor = $doctor.data
        testToolsOffline = $offlineTools.data
        testToolsOnline = $onlineTools.data
        cancel = $online.cancel
        lease = $online.lease
        workflow = $online.workflow
        recipe = $recipeResume
        logs = @(
            @{ name = 'first-editor'; sha256 = (Get-FileHash -LiteralPath $firstLog -Algorithm SHA256).Hash.ToLowerInvariant() },
            @{ name = 'second-editor'; sha256 = (Get-FileHash -LiteralPath $secondLog -Algorithm SHA256).Hash.ToLowerInvariant() }
        )
    }
    $json = $evidence | ConvertTo-Json -Depth 30
    if ($EvidencePath) {
        $fullEvidencePath = [IO.Path]::GetFullPath($EvidencePath)
        $parent = Split-Path $fullEvidencePath -Parent
        if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        [IO.File]::WriteAllText($fullEvidencePath, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    }
    $json
}
finally {
    try { Stop-OwnedEditor $ownedEditor }
    finally {
        if ($recipeRoot -and (Test-Path -LiteralPath $recipeRoot)) {
            Stop-OwnedRecipeWorkers $recipeRoot $(if (Test-Path variable:bundleRoot) { $bundleRoot } else { '' })
        }
        $env:UE_PORT = $previousPort
        $env:UEAI_ENGINE_ROOT = $previousEngineRoot
        if (-not $KeepFixture -and (Test-Path -LiteralPath $testRoot)) {
            $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
            Assert-Condition ($resolvedTestRoot.StartsWith($temporaryBase + [IO.Path]::DirectorySeparatorChar + 'ueai-rc-ue53-', [StringComparison]::OrdinalIgnoreCase)) 'Refusing to remove a non-owned test directory.'
            Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
        }
    }
}
