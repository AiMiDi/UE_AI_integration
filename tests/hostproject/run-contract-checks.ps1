<#
.SYNOPSIS
    Contract validation for UE_AI_integration: capability manifests, skill
    recipes, and the MCP TypeScript adapter build and tests.

.DESCRIPTION
    Runs the documented contract checks and captures their output as evidence
    under -WorkRoot\contracts\<stamp>\*.log. Exits non-zero when any check
    fails, so the caller can treat this as a gate.

    Checks:
        node scripts/validate_capabilities.mjs
        node scripts/validate_skills.mjs
        npm run build   (inside MCP)
        npm test        (inside MCP)

.EXAMPLE
    pwsh -File tests\hostproject\run-contract-checks.ps1
#>
[CmdletBinding()]
param(
    [string] $PluginRoot,

    [string] $WorkRoot = 'S:\tmp\ueai-host-automation',

    [switch] $ForceNpmCi
)

$ErrorActionPreference = 'Stop'

if (-not $PluginRoot) {
    $PluginRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
}
$PluginRoot = (Resolve-Path -LiteralPath $PluginRoot).Path
$MCPRoot = Join-Path $PluginRoot 'MCP'

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$EvidenceRoot = Join-Path $WorkRoot "contracts\$Stamp"
New-Item -ItemType Directory -Force -Path $EvidenceRoot | Out-Null

function Invoke-Check {
    param(
        [string] $Name,
        [string] $WorkingDirectory,
        [string[]] $Command
    )
    $log = Join-Path $EvidenceRoot "$Name.log"
    Write-Host "[contracts] $Name -> $log"
    $previous = Get-Location
    try {
        Set-Location -LiteralPath $WorkingDirectory
        & $Command[0] @($Command[1..($Command.Count - 1)]) *>&1 | Tee-Object -FilePath $log | Out-Host
        $exitCode = $LASTEXITCODE
    }
    finally {
        Set-Location -LiteralPath $previous
    }
    $tail = if (Test-Path -LiteralPath $log) {
        @(Get-Content -LiteralPath $log -Tail 6)
    }
    else { @() }
    Write-Host "[contracts] $Name exit=$exitCode"
    foreach ($line in $tail) { Write-Host "    $line" }
    return [ordered]@{
        name = $Name
        command = ($Command -join ' ')
        workingDirectory = $WorkingDirectory
        exitCode = $exitCode
        log = $log
    }
}

$checks = @()
$checks += Invoke-Check -Name 'validate-capabilities' -WorkingDirectory $PluginRoot `
    -Command @('node', 'scripts/validate_capabilities.mjs')
$checks += Invoke-Check -Name 'validate-skills' -WorkingDirectory $PluginRoot `
    -Command @('node', 'scripts/validate_skills.mjs')

if ($ForceNpmCi -or -not (Test-Path -LiteralPath (Join-Path $MCPRoot 'node_modules'))) {
    $checks += Invoke-Check -Name 'npm-ci' -WorkingDirectory $MCPRoot -Command @('npm', 'ci')
}
$checks += Invoke-Check -Name 'mcp-build' -WorkingDirectory $MCPRoot -Command @('npm', 'run', 'build')
$checks += Invoke-Check -Name 'mcp-test' -WorkingDirectory $MCPRoot -Command @('npm', 'test')

$failed = @($checks | Where-Object { $_.exitCode -ne 0 })
$summaryPath = Join-Path $EvidenceRoot 'summary.json'
[ordered]@{
    schema = 'ue.contract-checks.v1'
    utcStarted = [DateTime]::UtcNow.ToString('o')
    pluginRoot = $PluginRoot
    checks = $checks
    failed = @($failed | ForEach-Object { $_.name })
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $summaryPath -Encoding utf8NoBOM
Write-Host "[contracts] summary: $summaryPath (failed: $($failed.Count))"

if ($failed.Count -gt 0) {
    exit 1
}
exit 0
