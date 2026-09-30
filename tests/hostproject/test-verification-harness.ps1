<#
.SYNOPSIS
    Static contract test for the verification harness scripts.

.DESCRIPTION
    This test intentionally does not invoke Node, npm, UAT, UnrealEditor, or
    any process that could touch a project. It parses the two harness scripts
    and checks that source/binary identity and verification-lane fields remain
    present when the runners evolve.
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

$hostHarness = Join-Path $PluginRoot 'tests\hostproject\run-host-automation.ps1'
$contractHarness = Join-Path $PluginRoot 'tests\hostproject\run-contract-checks.ps1'
foreach ($path in @($hostHarness, $contractHarness)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Harness script is missing: $path"
    }
    Assert-PowerShellParses -Path $path
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
        'assetReadback',
        'runtimeVerified',
        'visualVerified')) {
    Assert-Contains -Text $hostText -Needle $needle -Path $hostHarness
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
        'assetReadback',
        'runtimeVerified',
        'visualVerified')) {
    Assert-Contains -Text $contractText -Needle $needle -Path $contractHarness
}

Write-Host '[harness-test] verification harness contracts passed (static parse only).'
exit 0
