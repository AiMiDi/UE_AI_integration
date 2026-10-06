<#
.SYNOPSIS
    Runs Niagara system-spec export/import/save/read-back in three isolated
    Editor processes and gates the result on exact loaded-module identity.

    The native test creates a GUID-scoped /Game/Automation system in prepare,
    persists its exported JSON beside -WorkRoot, imports that JSON and saves in
    import, then reloads and verifies the real authored state in verify. The
    original project assets are never used or modified.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $PluginRoot,
    [string] $EngineRoot = $env:UEAI_ENGINE_ROOT,
    [Parameter(Mandatory)][string] $WorkRoot,
    [ValidateSet('isolated-nullrhi', 'nonnullrhi-editor')]
    [string] $VerificationLane = 'isolated-nullrhi',
    [switch] $AllowMissingDynamicInput
)
$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'run-host-automation.ps1'
$runId = [Guid]::NewGuid().ToString('N')
$artifact = Join-Path $WorkRoot "niagara-spec-persistence-$runId.json"
$phaseResults = @()

foreach ($phase in @('prepare', 'import', 'verify')) {
    & $runner -PluginRoot $PluginRoot -EngineRoot $EngineRoot -WorkRoot $WorkRoot `
        -SkipBuild -VerificationLane $VerificationLane `
        -TestFilter 'UE_AI_integration.Niagara.SystemSpec.CrossProcessPersistence' `
        -EditorEnvironment @{
            UEAI_NIAGARA_SPEC_RUN = $runId
            UEAI_NIAGARA_SPEC_PHASE = $phase
            UEAI_NIAGARA_SPEC_ARTIFACT = $artifact
        }
    if ($LASTEXITCODE -ne 0) { throw "Niagara system-spec persistence phase '$phase' failed." }
    $summaryFile = Get-ChildItem -LiteralPath $WorkRoot -File -Filter 'summary-*.json' |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if (-not $summaryFile) { throw "No host automation summary was produced for '$phase'." }
    $summary = Get-Content -LiteralPath $summaryFile.FullName -Raw | ConvertFrom-Json
    if ($summary.automation.skipped -or $summary.automation.exitCode -ne 0 `
        -or $summary.automation.gateReasons.Count -ne 0 `
        -or $summary.automation.successfulTestStates -ne 1 `
        -or $summary.automation.verification.moduleLoaded -ne $true) {
        throw "Niagara system-spec phase '$phase' lacks successful Automation and exact module identity proof."
    }
    $phaseResults += [ordered]@{
        phase = $phase
        processId = $summary.automation.editor.processId
        summary = $summaryFile.FullName
        moduleProof = $summary.automation.verification.moduleLoadProof
    }
}

if (@($phaseResults.processId | Sort-Object -Unique).Count -ne 3) {
    throw 'Niagara system-spec persistence requires three distinct isolated Editor PIDs.'
}
if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
    throw 'Native persistence test did not publish its artifact.'
}
$native = Get-Content -LiteralPath $artifact -Raw | ConvertFrom-Json
if ($native.importSaved -ne $true -or $native.verifyRoundTrip -ne $true) {
    throw 'Native spec import/save/round-trip evidence is incomplete.'
}
if (-not $AllowMissingDynamicInput -and $native.dynamicInputCovered -ne $true) {
    throw 'Dynamic input tree was not covered by the native persistence fixture.'
}

[ordered]@{
    schema = 'ue.niagara.system-spec-cross-process-acceptance.v1'
    runId = $runId
    artifact = $artifact
    phases = $phaseResults
    native = $native
    distinctEditorPids = $true
    moduleIdentityVerified = $true
    persistenceVerified = $true
    originalProjectAssetsTouched = $false
} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $WorkRoot "niagara-spec-persistence-$runId.acceptance.json") -Encoding utf8NoBOM
Write-Host "[niagara-spec] acceptance: $WorkRoot\niagara-spec-persistence-$runId.acceptance.json"
