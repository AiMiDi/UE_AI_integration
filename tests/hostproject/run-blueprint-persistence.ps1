<#
.SYNOPSIS
    Verifies Blueprint save, reference/redirector stability and runtime behavior
    across three sequential isolated Editor processes using a bound package.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $PluginRoot,
    [string] $EngineRoot = $env:UEAI_ENGINE_ROOT,
    [Parameter(Mandatory)][string] $WorkRoot,
    [ValidateSet('isolated-nullrhi', 'nonnullrhi-editor')]
    [string] $VerificationLane = 'isolated-nullrhi'
)
$ErrorActionPreference = 'Stop'
$runId = [Guid]::NewGuid().ToString('N')
$runner = Join-Path $PSScriptRoot 'run-host-automation.ps1'
$results = @()
foreach ($phase in @('prepare', 'rename', 'verify')) {
    & $runner -PluginRoot $PluginRoot -EngineRoot $EngineRoot -WorkRoot $WorkRoot `
        -SkipBuild -VerificationLane $VerificationLane `
        -TestFilter 'UE_AI_integration.Blueprint.CrossProcess.PersistenceReferencesAndRuntime' `
        -EditorEnvironment @{
            UEAI_BP_PERSISTENCE_RUN = $runId
            UEAI_BP_PERSISTENCE_PHASE = $phase
        }
    if ($LASTEXITCODE -ne 0) { throw "Blueprint persistence phase '$phase' failed." }
    $summaryFile = Get-ChildItem -LiteralPath $WorkRoot -File -Filter 'summary-*.json' |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    $summary = Get-Content -LiteralPath $summaryFile.FullName -Raw | ConvertFrom-Json
    if ($summary.automation.skipped -or $summary.automation.exitCode -ne 0 `
        -or $summary.automation.gateReasons.Count -ne 0 `
        -or $summary.automation.successfulTestStates -ne 1 `
        -or $summary.automation.verification.moduleLoaded -ne $true) {
        throw "Blueprint persistence phase '$phase' lacks successful Automation and module identity proof."
    }
    $results += [ordered]@{
        phase = $phase
        processId = $summary.automation.editor.processId
        summary = $summaryFile.FullName
    }
}
if (@($results.processId | Sort-Object -Unique).Count -ne 3) {
    throw 'Persistence acceptance requires three distinct isolated Editor PIDs.'
}
[ordered]@{
    schema = 'ue.blueprint-cross-process-acceptance.v1'
    runId = $runId
    phases = $results
    persistenceVerified = $true
    renamedReferencesVerified = $true
    runtimeBehaviorVerified = $true
    projectAssetAcceptance = $null
    visualVerified = $null
} | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $WorkRoot "blueprint-persistence-$runId.json") -Encoding utf8NoBOM
