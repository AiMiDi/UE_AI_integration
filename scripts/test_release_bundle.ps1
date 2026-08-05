[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Archive,

    [string]$EngineRoot,
    [string]$EditorEndpoint,
    [string]$ExpectedVersion,
    [switch]$KeepExtracted
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-JsonCommand {
    param([string]$FilePath, [string[]]$Arguments)
    $output = & $FilePath @Arguments 2>&1
    Assert-Condition ($LASTEXITCODE -eq 0) "Command failed: $FilePath $($Arguments -join ' ')`n$output"
    try { return ($output | Out-String | ConvertFrom-Json) }
    catch { throw "Command did not return JSON: $FilePath`n$output" }
}

$archivePath = [IO.Path]::GetFullPath($Archive)
Assert-Condition (Test-Path -LiteralPath $archivePath -PathType Leaf) "Release archive not found: $archivePath"

$extractRoot = Join-Path ([IO.Path]::GetTempPath()) ('ueai-release-smoke-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $extractRoot | Out-Null
try {
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot
    $roots = @(Get-ChildItem -LiteralPath $extractRoot -Directory)
    Assert-Condition ($roots.Count -eq 1) 'Release archive must contain exactly one top-level directory.'
    $bundleRoot = $roots[0].FullName

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
        'skills/setup-ue5/SKILL.md',
        'skills/ue-recovery-operator/SKILL.md',
        'Recipes/ue-recovery-operator.recipe.json',
        'Release/release-files.sha256.json'
    )
    foreach ($relative in $required) {
        Assert-Condition (Test-Path -LiteralPath (Join-Path $bundleRoot $relative)) "Required release file is missing: $relative"
    }
    $traceWorkers = @(Get-ChildItem -LiteralPath (Join-Path $bundleRoot 'Tools/Trace/Win64') -Recurse -File -Filter 'UEAITraceWorker.exe' -ErrorAction SilentlyContinue)
    Assert-Condition ($traceWorkers.Count -eq 1) 'Release bundle must contain exactly one Win64 UEAITraceWorker.exe.'

    $manifestPath = Join-Path $bundleRoot 'Release/release-files.sha256.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-Condition ($manifest.schema -eq 'ue.release-files.v1') 'Unsupported release file manifest schema.'
    Assert-Condition ($manifest.files.Count -gt 0) 'Release file manifest is empty.'
    foreach ($entry in $manifest.files) {
        $candidate = Join-Path $bundleRoot ([string]$entry.path)
        Assert-Condition (Test-Path -LiteralPath $candidate -PathType Leaf) "Manifest file is missing: $($entry.path)"
        $actual = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert-Condition ($actual -eq ([string]$entry.sha256).ToLowerInvariant()) "SHA-256 mismatch: $($entry.path)"
    }

    $descriptor = Get-Content -LiteralPath (Join-Path $bundleRoot 'UE_AI_integration.uplugin') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($ExpectedVersion) {
        Assert-Condition ($descriptor.VersionName -eq $ExpectedVersion) "Expected version $ExpectedVersion, found $($descriptor.VersionName)."
    }

    $ue = Join-Path $bundleRoot 'CLI/bin/ue.exe'
    $workflow = Join-Path $bundleRoot 'CLI/bin/ue-workflow.exe'
    $previousTraceTransport = $env:UEAI_TRACE_TRANSPORT
    try {
        # Black-box validation owns a one-shot Worker. It must not leave a
        # resident service holding DLLs in the extracted directory.
        $env:UEAI_TRACE_TRANSPORT = 'stdio'
        $doctorArgs = @('doctor', '--full', '--bundle', $bundleRoot, '--json')
        if ($EditorEndpoint) { $doctorArgs += @('--endpoint', $EditorEndpoint) }
        $doctor = Invoke-JsonCommand $ue $doctorArgs
        Assert-Condition ($doctor.ok -eq $true) 'ue doctor --full did not succeed.'

        $testTools = $null
        if ($EditorEndpoint) {
            $testToolsArgs = @('test-tools', '--bundle', $bundleRoot, '--endpoint', $EditorEndpoint, '--json')
            $testTools = Invoke-JsonCommand $ue $testToolsArgs
            Assert-Condition ($testTools.ok -eq $true) 'ue test-tools did not succeed.'
        }
    }
    finally { $env:UEAI_TRACE_TRANSPORT = $previousTraceTransport }

    $workflowDoctor = Invoke-JsonCommand $workflow @('doctor', '--json')
    Assert-Condition ($workflowDoctor.ok -eq $true) 'ue-workflow doctor did not succeed.'
    $capabilities = Invoke-JsonCommand $ue @('capabilities', '--limit', '1', '--json')
    Assert-Condition ($capabilities.ok -eq $true) 'Packaged capability query failed.'
    $skills = Invoke-JsonCommand $ue @('skills', '--limit', '1', '--json')
    Assert-Condition ($skills.ok -eq $true) 'Packaged Skills query failed.'

    $offlineFixture = Join-Path $extractRoot 'offline-project'
    $offlineConfig = Join-Path $offlineFixture 'Config'
    New-Item -ItemType Directory -Path $offlineConfig -Force | Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $offlineFixture 'Fixture.uproject'),
        '{"EngineAssociation":"5.3","Plugins":[]}',
        [Text.UTF8Encoding]::new($false))
    $offlineSecret = 'release-smoke-must-not-leak-token'
    [IO.File]::WriteAllText(
        (Join-Path $offlineConfig 'DefaultEngine.ini'),
        "[Online]`nApiToken=$offlineSecret`nEndpoint=https://example.invalid`n",
        [Text.UTF8Encoding]::new($false))
    $offlineParams = Join-Path $extractRoot 'offline-params.json'
    [IO.File]::WriteAllText(
        $offlineParams,
        (@{ projectRoot = $offlineFixture } | ConvertTo-Json -Compress),
        [Text.UTF8Encoding]::new($false))
    $offline = Invoke-JsonCommand $ue @(
        'production.project.config.get',
        '--params-file', $offlineParams,
        '--json')
    Assert-Condition ($offline.ok -eq $true) 'Packaged localProject CLI query failed.'
    Assert-Condition ($offline.meta.executionBackend -eq 'localProject') 'Packaged CLI did not select localProject.'
    $offlineJson = $offline | ConvertTo-Json -Depth 20 -Compress
    Assert-Condition (-not $offlineJson.Contains($offlineSecret)) 'Packaged localProject CLI leaked a sensitive .ini value.'
    Assert-Condition ($offline.data.merged.Online.ApiToken.value -eq '<redacted>') 'Packaged localProject CLI did not redact ApiToken.'

    $node = Get-Command node -ErrorAction Stop
    $mcpSmoke = Join-Path $bundleRoot 'scripts/mcp_stdio_smoke.mjs'
    $mcpOutput = & $node.Source $mcpSmoke $bundleRoot 2>&1
    Assert-Condition ($LASTEXITCODE -eq 0) "MCP initialize/list-tools smoke failed:`n$mcpOutput"
    $mcp = $mcpOutput | Out-String | ConvertFrom-Json
    Assert-Condition ($mcp.toolCount -eq 12) 'MCP surface must contain exactly 12 tools.'

    if ($EngineRoot) {
        $previousEngineRoot = $env:UEAI_ENGINE_ROOT
        $previousTraceTransport = $env:UEAI_TRACE_TRANSPORT
        try {
            $env:UEAI_ENGINE_ROOT = [IO.Path]::GetFullPath($EngineRoot)
            $env:UEAI_TRACE_TRANSPORT = 'stdio'
            $worker = Invoke-JsonCommand $ue @('trace', 'doctor', '--json')
            Assert-Condition ($worker.ok -eq $true) 'Trace Worker handshake failed.'
        }
        finally {
            $env:UEAI_ENGINE_ROOT = $previousEngineRoot
            $env:UEAI_TRACE_TRANSPORT = $previousTraceTransport
        }
    }

    if ($EditorEndpoint) {
        $health = Invoke-JsonCommand $ue @('status', '--endpoint', $EditorEndpoint, '--json')
        Assert-Condition ($health.ok -eq $true) 'UE 5.3 fixture health failed.'
        $assets = Invoke-JsonCommand $ue @('blueprint.asset.list', '--endpoint', $EditorEndpoint, '--json')
        Assert-Condition ($assets.ok -eq $true) 'Read-only blueprint.asset.list smoke failed.'
    }

    [ordered]@{
        ok = $true
        schema = 'ue.release-smoke.v1'
        version = $descriptor.VersionName
        fileCount = $manifest.files.Count
        mcpToolCount = $mcp.toolCount
        localProjectCliChecked = $true
        workerChecked = [bool]$EngineRoot
        editorChecked = [bool]$EditorEndpoint
        testToolsChecked = [bool]$EditorEndpoint
        extractedRoot = if ($KeepExtracted) { $bundleRoot } else { $null }
    } | ConvertTo-Json -Depth 5
}
finally {
    if (-not $KeepExtracted -and (Test-Path -LiteralPath $extractRoot)) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
}
