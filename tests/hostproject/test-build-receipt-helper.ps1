[CmdletBinding()]
param([string] $PluginRoot)

$ErrorActionPreference = 'Stop'
if (-not $PluginRoot) { $PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$helperPath = Join-Path $PluginRoot 'tests\hostproject\build-receipt-helper.ps1'
. $helperPath

$tokens = $null
$errors = $null
[System.Management.Automation.Language.Parser]::ParseFile($helperPath, [ref]$tokens, [ref]$errors) | Out-Null
if ($errors.Count -ne 0) { throw 'Build receipt helper does not parse.' }

function Assert-ReceiptRejected {
    param([string] $ReceiptPath, [string] $PackageRoot, [string] $EngineRoot, [string] $Case)
    $rejected = $false
    try {
        Get-VerifiedBuildBinding -BuildSummaryPath $ReceiptPath -PackagedPluginRoot $PackageRoot -EngineRoot $EngineRoot | Out-Null
    } catch {
        $rejected = $true
    }
    if (-not $rejected) { throw "Negative receipt case unexpectedly passed: $Case" }
}

$temp = Join-Path ([IO.Path]::GetTempPath()) "ueai-receipt-$([Guid]::NewGuid().ToString('N'))"
$engineRoot = Join-Path $temp 'Engine'
$packageRoot = Join-Path $temp 'Package'
$engineBin = Join-Path $engineRoot 'Engine\Binaries\Win64'
$packageBin = Join-Path $packageRoot 'Binaries\Win64'
New-Item -ItemType Directory -Path $engineBin, $packageBin -Force | Out-Null

try {
    $buildId = 'test-build-id'
    $manifest = @{ BuildId = $buildId; Modules = @{} }
    $engineManifestPath = Join-Path $engineBin 'UnrealEditor.modules'
    $packageManifestPath = Join-Path $packageBin 'UnrealEditor.modules'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $engineManifestPath -Encoding utf8NoBOM
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $packageManifestPath -Encoding utf8NoBOM
    $dllPath = Join-Path $packageBin 'UnrealEditor-UE_AI_integration.dll'
    $pdbPath = Join-Path $packageBin 'UnrealEditor-UE_AI_integration.pdb'
    [IO.File]::WriteAllBytes($dllPath, [Text.Encoding]::UTF8.GetBytes('test dll'))
    [IO.File]::WriteAllBytes($pdbPath, [Text.Encoding]::UTF8.GetBytes('test pdb'))

    $startedUtc = [DateTime]::Parse('2026-01-01T00:00:00Z').ToUniversalTime()
    $finishedUtc = [DateTime]::Parse('2026-01-01T00:01:00Z').ToUniversalTime()
    [IO.File]::SetLastWriteTimeUtc($dllPath, $startedUtc.AddSeconds(10))
    [IO.File]::SetLastWriteTimeUtc($pdbPath, $startedUtc.AddSeconds(10))

    $sourcePath = Join-Path $temp 'source.cpp'
    [IO.File]::WriteAllText($sourcePath, 'stable', [Text.Encoding]::UTF8)
    $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $receiptPath = Join-Path $temp 'build-receipt.json'
    $beforePath = Join-Path $temp 'source-before.json'
    $afterPath = Join-Path $temp 'source-after.json'
    $sourcePin = @(@{ Path = $sourcePath; SHA256 = $sourceHash })
    $sourcePin | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $beforePath -Encoding utf8NoBOM
    $sourcePin | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $afterPath -Encoding utf8NoBOM

    $receipt = [ordered]@{
        Kind = 'test whole project'
        StartedUTC = $startedUtc.ToString('o')
        FinishedUTC = $finishedUtc.ToString('o')
        ExitCode = 0
        SourceStableDuringBuild = $true
        SourceCount = 1
        ModulePins = @(
            @{ Path = $engineManifestPath; BuildId = $buildId; SHA256 = (Get-FileHash -LiteralPath $engineManifestPath -Algorithm SHA256).Hash }
            @{ Path = $packageManifestPath; BuildId = $buildId; SHA256 = (Get-FileHash -LiteralPath $packageManifestPath -Algorithm SHA256).Hash }
        )
    }
    $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM
    $binding = Get-VerifiedBuildBinding -BuildSummaryPath $receiptPath -PackagedPluginRoot $packageRoot -EngineRoot $engineRoot
    if ($binding.mode -ne 'whole-project' -or $binding.buildId -ne $buildId) { throw 'Positive whole-project receipt binding failed.' }

    $bad = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    $bad.SourceStableDuringBuild = $false
    $bad | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM
    Assert-ReceiptRejected $receiptPath $packageRoot $engineRoot 'source stability'
    $bad.SourceStableDuringBuild = $true
    $bad | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM

    $badAfter = @(@{ Path = $sourcePath; SHA256 = ('0' * 64) })
    $badAfter | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $afterPath -Encoding utf8NoBOM
    Assert-ReceiptRejected $receiptPath $packageRoot $engineRoot 'source-before/after mismatch'
    $sourcePin | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $afterPath -Encoding utf8NoBOM

    [IO.File]::WriteAllText($sourcePath, 'mutated', [Text.Encoding]::UTF8)
    Assert-ReceiptRejected $receiptPath $packageRoot $engineRoot 'source pin drift'
    [IO.File]::WriteAllText($sourcePath, 'stable', [Text.Encoding]::UTF8)

    $badModulePin = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    $badModulePin.ModulePins[0].BuildId = 'different-build-id'
    $badModulePin | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM
    Assert-ReceiptRejected $receiptPath $packageRoot $engineRoot 'module BuildId mismatch'
    $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM

    $badModuleHash = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    $badModuleHash.ModulePins[0].SHA256 = '0' * 64
    $badModuleHash | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM
    Assert-ReceiptRejected $receiptPath $packageRoot $engineRoot 'module manifest hash mismatch'
    $receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath -Encoding utf8NoBOM

    [IO.File]::SetLastWriteTimeUtc($dllPath, $startedUtc.AddHours(-2))
    Assert-ReceiptRejected $receiptPath $packageRoot $engineRoot 'stale DLL artifact'
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host '[receipt-test] build receipt helper parse, positive binding, and negative gates passed.'
