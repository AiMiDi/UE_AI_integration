<#
.SYNOPSIS
    Static contract test for the verification harness scripts.

.DESCRIPTION
    This test intentionally does not invoke Node, npm, UAT, UnrealEditor, or
    any process that could touch a project. It parses the harness scripts and
    checks portable input paths, source/binary identity and verification-lane
    fields when the runners evolve.
#>
[CmdletBinding()]
param(
    [string] $PluginRoot
)

$ErrorActionPreference = 'Stop'
if (-not $PluginRoot) {
    $PluginRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
}
$PluginRoot = (Resolve-Path -LiteralPath $PluginRoot).Path

function Assert-Contains {
    param(
        [Parameter(Mandatory)][string] $Text,
        [Parameter(Mandatory)][string] $Needle,
        [Parameter(Mandatory)][string] $Path
    )
    if ($Text.IndexOf($Needle, [StringComparison]::Ordinal) -lt 0) {
        throw "Harness contract '$Needle' is missing from $Path"
    }
}

function Assert-PowerShellParses {
    param([Parameter(Mandatory)][string] $Path)
    $tokens = $null
    $parseErrors = $null
    [System.Management.Automation.Language.Parser]::ParseFile(
        $Path,
        [ref]$tokens,
        [ref]$parseErrors) | Out-Null
    if ($parseErrors.Count -ne 0) {
        $messages = @($parseErrors | ForEach-Object { $_.Message }) -join '; '
        throw "PowerShell parse errors in ${Path}: $messages"
    }
}

function Assert-EnvironmentParameterDefault {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $ParameterName,
        [Parameter(Mandatory)][string] $EnvironmentName
    )
    $tokens = $null
    $parseErrors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile(
        $Path, [ref]$tokens, [ref]$parseErrors)
    $parameters = @($ast.ParamBlock.Parameters | Where-Object {
            $_.Name.VariablePath.UserPath -eq $ParameterName
        })
    if ($parameters.Count -ne 1 -or
        $parameters[0].DefaultValue.Extent.Text -cne ('$env:' + $EnvironmentName)) {
        throw "Harness parameter '$ParameterName' must default to environment variable '$EnvironmentName': $Path"
    }
}

$hostHarness = Join-Path $PluginRoot 'tests\hostproject\run-host-automation.ps1'
$contractHarness = Join-Path $PluginRoot 'tests\hostproject\run-contract-checks.ps1'
$replicaHarness = Join-Path $PluginRoot 'tests\hostproject\run-project-replica-acceptance.ps1'
$runtimeHarnesses = @(
    $hostHarness,
    (Join-Path $PluginRoot 'tests\hostproject\run-blueprint-persistence.ps1'),
    (Join-Path $PluginRoot 'tests\hostproject\run-niagara-spec-persistence.ps1'),
    $replicaHarness
)
foreach ($path in @($runtimeHarnesses) + @($contractHarness)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Harness script is missing: $path"
    }
    Assert-PowerShellParses -Path $path
}
foreach ($path in $runtimeHarnesses) {
    Assert-EnvironmentParameterDefault -Path $path -ParameterName 'EngineRoot' -EnvironmentName 'UEAI_ENGINE_ROOT'
}
Assert-EnvironmentParameterDefault -Path $replicaHarness -ParameterName 'OriginalProjectRoot' -EnvironmentName 'UEAI_PROJECT_ROOT'
$replicaText = Get-Content -LiteralPath $replicaHarness -Raw
foreach ($needle in @(
        "-Filter '*.uproject'",
        '$ProjectDescriptorName',
        '$originalDescriptorFiles.Count -eq 1',
        '-LiteralPath $originalDescriptorFile.FullName',
        'Join-Path $replica $originalDescriptorFile.Name',
        'Assert-IndependentDirectory $WorkRoot',
        'ReparsePoint',
        'originalSeedsUnchanged')) {
    Assert-Contains -Text $replicaText -Needle $needle -Path $replicaHarness
}
$tokens = $null
$parseErrors = $null
$replicaAst = [System.Management.Automation.Language.Parser]::ParseFile(
    $replicaHarness, [ref]$tokens, [ref]$parseErrors)
$literalProjectNames = @($replicaAst.FindAll({
            param($node)
            $node -is [System.Management.Automation.Language.StringConstantExpressionAst] -and
            $node.Value -match '\.uproject$' -and $node.Value -notin @('*.uproject', '.uproject')
        }, $true))
if ($literalProjectNames.Count -ne 0) {
    throw "Replica harness must derive the project descriptor filename from its source project: $replicaHarness"
}

$hostText = Get-Content -LiteralPath $hostHarness -Raw
foreach ($needle in @(
        'VerificationLane',
        'isolated-nullrhi',
        'nonnullrhi-editor',
        'sourceSnapshotAfterBuild',
        'sourceStableForBuild',
        'source_changed_during_build',
        'source_changed_during_automation',
        'moduleArtifacts',
        'Get-FileIdentity',
        'processId',
        'staticVerified',
        'compiled',
        'moduleLoaded',
        'moduleLoadProof',
        'production.module.loaded.get',
        'UE_PORT',
        'expectedDll',
        'expectedPdb',
        'observedProcessId',
        'processMatches',
        'pathMatches',
        'hashMatches',
        'pdbMatches',
        'latestBuildArtifactPath',
        'latestBuildArtifactSha256',
        'latestBuildArtifactPathMatches',
        'latestBuildArtifactHashMatches',
        'identityComplete',
        'exactIdentityComplete',
        'exactIdentityFailureReasons',
        'pdbSha256Complete',
        'latestArtifactIdentityComplete',
        'loadedModuleIdentity',
        'loadedModuleIdentityMatches',
        'verificationFailures',
        'failureClass',
        'lastFailureMessage',
        'attempts',
        'assetReadback',
        'runtimeVerified',
        'visualVerified')) {
    Assert-Contains -Text $hostText -Needle $needle -Path $hostHarness
}

$productionController = Join-Path $PluginRoot 'Source\UE_AI_integration\Private\Infrastructure\ProductionRuntimeController.cpp'
if (-not (Test-Path -LiteralPath $productionController -PathType Leaf)) {
    throw "Production module controller is missing: $productionController"
}
$productionText = Get-Content -LiteralPath $productionController -Raw
foreach ($needle in @(
        'processId',
        'latestBuildArtifactPath',
        'latestBuildArtifactSha256',
        'latestBuildArtifactExists',
        'latestBuildArtifactTimestampUtc',
        'latestBuildArtifactPathMatchesLoaded',
        'matchesLatestBuildArtifact',
        'identityComplete',
        'exactIdentityComplete',
        'exactIdentityFailureReasons',
        'pdbSha256Complete',
        'latestArtifactIdentityComplete',
        'ue.loaded-module-identity.v1',
        'moduleSha256',
        'pdbSha256')) {
    Assert-Contains -Text $productionText -Needle $needle -Path $productionController
}

# Tests that create native Material/Blueprint editors or exercise shader/PIE
# rendering must be selected only in a NonNullRHI lane.  A NullRHI process may
# still report an Automation "Success" for a test body that exits early, which
# is a false runtime/visual acceptance signal.  Keep this list explicit so a
# new rendering test cannot silently regress to a NullRHI pass.  Keep the
# exact test paths here so a different test in the same file cannot satisfy the
# assertion accidentally.
$nonNullRhiTests = @(
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialEditorPreviewTests.cpp'; Test = 'UE_AI_integration.MaterialEditor.PreviewEditing' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialApplyReviewTests.cpp'; Test = 'UE_AI_integration.MaterialEditor.ApplyReviewAndReadback' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialCustomEditingTests.cpp'; Test = 'UE_AI_integration.MaterialCustom.NativeCompilerCorrection' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialDiagnosticSourceMapTests.cpp'; Test = 'UE_AI_integration.MaterialSourceMap.CompilerCorrection' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialFunctionCallTests.cpp'; Test = 'UE_AI_integration.MaterialFunctionCall.HostShaderCorrection' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialPreviewGraphTests.cpp'; Test = 'UE_AI_integration.MaterialEditor.GraphCrudAndSnapshots' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialPreviewGraphTests.cpp'; Test = 'UE_AI_integration.MaterialEditor.BatchAtomicity' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialPreviewGraphTests.cpp'; Test = 'UE_AI_integration.MaterialEditor.DeletionBatchRefresh' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialPreviewGraphTests.cpp'; Test = 'UE_AI_integration.MaterialEditor.NamedReroutePreview' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialSourceFingerprintTests.cpp'; Test = 'UE_AI_integration.MaterialSource.IncludeFreshness' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/MaterialSourceFingerprintTests.cpp'; Test = 'UE_AI_integration.MaterialSource.IncludeCompilerCorrection' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/BlueprintEditorLayoutTests.cpp'; Test = 'UE_AI_integration.Blueprint.EditorLayout.NativeCommandLoop' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/BlueprintDebugPIEHttpTests.cpp'; Test = 'UE_AI_integration.BlueprintDebug.RealPIEHttpStepWatchContinue' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/ProjectAssetReplicaAcceptanceTests.cpp'; Test = 'UE_AI_integration.ProjectReplica.RealAssetsWriteRestoreReferencesAndRuntime' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/NiagaraSystemRuntimeAcceptanceTests.cpp'; Test = 'UE_AI_integration.Niagara.SystemSpec.NonNullRHIRuntimeAcceptance' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/NiagaraSimCacheObservationTests.cpp'; Test = 'UE_AI_integration.Niagara.SimCache.CpuLifecycle' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/NiagaraSimCacheObservationTests.cpp'; Test = 'UE_AI_integration.Niagara.SimCache.GpuLifecycle' },
    @{ Path = 'Source/UE_AI_integration/Private/Tests/RuntimeSessionTests.cpp'; Test = 'UE_AI_integration.Runtime.Viewport.RealPIECapture' }
)
foreach ($test in $nonNullRhiTests) {
    $relativePath = [string]$test.Path
    $sourcePath = Join-Path $PluginRoot $relativePath
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "NonNullRHI test source is missing: $sourcePath"
    }
    $sourceText = Get-Content -LiteralPath $sourcePath -Raw
    $macroMatches = [regex]::Matches(
        $sourceText,
        '(?s)IMPLEMENT_SIMPLE_AUTOMATION_TEST\s*\((?<body>.*?)\)')
    $matchingMacros = @($macroMatches | Where-Object {
            $_.Groups['body'].Value.IndexOf(
                ('"' + [string]$test.Test + '"'),
                [StringComparison]::Ordinal) -ge 0
        })
    if ($matchingMacros.Count -ne 1) {
        throw "Expected exactly one Automation declaration for '$($test.Test)' in $sourcePath"
    }
    Assert-Contains `
        -Text $matchingMacros[0].Groups['body'].Value `
        -Needle 'EAutomationTestFlags::NonNullRHI' `
        -Path "$sourcePath ($($test.Test))"
}

$contractText = Get-Content -LiteralPath $contractHarness -Raw
foreach ($needle in @(
        'Get-SourceIdentity',
        'sourceIdentity',
        'toolIdentity',
        "verificationLane = 'manifest-cli-mcp-contract'",
        'staticVerified',
        'compiled',
        'moduleLoaded',
        'moduleLoadProof',
        'production.module.loaded.get',
        'assetReadback',
        'runtimeVerified',
        'visualVerified')) {
    Assert-Contains -Text $contractText -Needle $needle -Path $contractHarness
}

Write-Host '[harness-test] verification harness contracts passed (static parse only).'
exit 0
