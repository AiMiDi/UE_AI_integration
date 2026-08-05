[CmdletBinding()]
param(
    [string]$EngineRoot,
    [string]$OutputRoot,
    [string]$StagingRoot,
    [string]$EditorEndpoint,
    [switch]$SkipBuild,
    [switch]$SkipSmoke,
    [switch]$AllowDirtySource
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$pluginRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd([IO.Path]::DirectorySeparatorChar)
$descriptor = Get-Content -LiteralPath (Join-Path $pluginRoot 'UE_AI_integration.uplugin') -Raw -Encoding UTF8 | ConvertFrom-Json
$version = [string]$descriptor.VersionName
$sourceStatus = @(& git -C $pluginRoot status --porcelain=v1 --untracked-files=all)
if ($sourceStatus.Count -gt 0 -and -not $AllowDirtySource) {
    throw 'Official release packaging requires a clean source tree. Use -AllowDirtySource only for a non-release verification artifact.'
}
if (-not $OutputRoot) { $OutputRoot = Join-Path (Split-Path $pluginRoot -Parent) 'UE_AI_integration-Releases' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (-not $StagingRoot) { $StagingRoot = Join-Path $OutputRoot ("UE_AI_integration-$version") }
$StagingRoot = [IO.Path]::GetFullPath($StagingRoot)

if ($StagingRoot -eq $pluginRoot -or $StagingRoot.StartsWith($pluginRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Release staging must be outside the source checkout.'
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

if (-not $SkipBuild) {
    if (-not $EngineRoot) { throw '-EngineRoot is required unless -SkipBuild is used.' }
    $previousPortable = $env:UEAI_RUN_PORTABLE_TESTS
    try {
        $env:UEAI_RUN_PORTABLE_TESTS = '1'
        & (Join-Path $pluginRoot 'scripts/build_plugin.bat') $EngineRoot $StagingRoot
        if ($LASTEXITCODE -ne 0) { throw "BuildPlugin staging failed with exit code $LASTEXITCODE." }
    }
    finally { $env:UEAI_RUN_PORTABLE_TESTS = $previousPortable }
}

if (-not (Test-Path -LiteralPath $StagingRoot -PathType Container)) {
    throw "Staging root not found: $StagingRoot"
}

$required = @(
    'UE_AI_integration.uplugin',
    'CLI/bin/ue.exe',
    'CLI/bin/ue-workflow.exe',
    'MCP/package.json',
    'MCP/dist/index.js',
    'MCP/dist/local-capability-cli.js',
    'Resources/Capabilities/blueprint.json',
    'Resources/Capabilities/production.json',
    'Workflow/Contracts/contract-set.v1.json',
    'Resources/Trace/worker-protocol.v1.json',
    'Resources/Contracts/recipe.schema.v2.json',
    'Resources/Contracts/development-bridge.v1.json',
    'skills/ue-ai/SKILL.md',
    'skills/ue-ai/agents/openai.yaml',
    'skills/ue-ai/references/skill-routing.md',
    'skills/setup-ue5/SKILL.md',
    'skills/ue-recovery-operator/SKILL.md',
    'Recipes/ue-recovery-operator.recipe.json',
    'scripts/install_entry_skill.ps1',
    'scripts/install_entry_skill.sh',
    'scripts/mcp_stdio_smoke.mjs'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $StagingRoot $relative))) {
        throw "Release staging is incomplete: $relative"
    }
}
$traceWorkers = @(Get-ChildItem -LiteralPath (Join-Path $StagingRoot 'Tools/Trace/Win64') -Recurse -File -Filter 'UEAITraceWorker.exe' -ErrorAction SilentlyContinue)
if ($traceWorkers.Count -ne 1) { throw 'Release staging must contain exactly one Win64 UEAITraceWorker.exe.' }

$releaseDir = Join-Path $StagingRoot 'Release'
New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
$manifestPath = Join-Path $releaseDir 'release-files.sha256.json'
if (Test-Path -LiteralPath $manifestPath) { Remove-Item -LiteralPath $manifestPath -Force }
$stagingPrefix = $StagingRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$files = @(
    Get-ChildItem -LiteralPath $StagingRoot -Recurse -File |
        Sort-Object FullName |
        ForEach-Object {
            if (-not $_.FullName.StartsWith($stagingPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Release file escaped staging root: $($_.FullName)"
            }
            $relative = $_.FullName.Substring($stagingPrefix.Length).Replace('\', '/')
            [ordered]@{
                path = $relative
                size = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
)
$manifest = [ordered]@{
    schema = 'ue.release-files.v1'
    version = $version
    generatedAt = [DateTime]::UtcNow.ToString('o')
    sourceCommit = (& git -C $pluginRoot rev-parse HEAD).Trim()
    sourceTreeDirty = ($sourceStatus.Count -gt 0)
    files = $files
}
$manifestJson = $manifest | ConvertTo-Json -Depth 5
[IO.File]::WriteAllText(
    $manifestPath,
    $manifestJson + [Environment]::NewLine,
    (New-Object Text.UTF8Encoding($false)))

$archive = Join-Path $OutputRoot ("UE_AI_integration-$version.zip")
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
Compress-Archive -LiteralPath $StagingRoot -DestinationPath $archive -CompressionLevel Optimal

if (-not $SkipSmoke) {
    & (Join-Path $pluginRoot 'scripts/test_release_bundle.ps1') -Archive $archive -EngineRoot $EngineRoot -EditorEndpoint $EditorEndpoint -ExpectedVersion $version
    if ($LASTEXITCODE -ne 0) { throw "Release bundle smoke failed with exit code $LASTEXITCODE." }
}

[ordered]@{
    ok = $true
    schema = 'ue.release-package.v1'
    version = $version
    stagingRoot = $StagingRoot
    archive = $archive
    archiveSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    fileCount = $files.Count
    smokeRan = -not $SkipSmoke
    sourceTreeDirty = ($sourceStatus.Count -gt 0)
} | ConvertTo-Json -Depth 4
