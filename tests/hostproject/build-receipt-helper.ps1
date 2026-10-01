<#
    Shared build-receipt verification for project-replica acceptance.
    This file only defines functions; callers dot-source it before running.
#>

function Get-ReceiptFullPath([string] $Path) {
    [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

function Get-ReceiptFileIdentity([string] $Path) {
    $file = Get-Item -LiteralPath $Path -Force
    [ordered]@{
        path = $file.FullName
        bytes = [int64]$file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        lastWriteUtc = $file.LastWriteTimeUtc
    }
}

function Convert-ReceiptUtc($Value, [string] $Label) {
    if ($Value -is [DateTimeOffset]) { return $Value.UtcDateTime }
    if ($Value -is [DateTime]) { return $Value.ToUniversalTime() }
    try {
        return [DateTime]::Parse(
            [string]$Value,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal)
    } catch {
        throw "$Label is not a valid UTC timestamp."
    }
}

function Get-ReceiptPackageDigest([string] $Root) {
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

function Convert-ReceiptSourceList($Value, [string] $Label) {
    $items = @($Value)
    if ($items.Count -eq 0) { throw "$Label is empty." }
    $records = @(
        foreach ($item in $items) {
            $missingPath = [string]::IsNullOrWhiteSpace([string]$item.Path)
            $missingHash = [string]::IsNullOrWhiteSpace([string]$item.SHA256)
            if ($missingPath -or $missingHash) {
                throw "$Label contains an incomplete source identity."
            }
            [ordered]@{ path = (Get-ReceiptFullPath ([string]$item.Path)).ToLowerInvariant(); sha256 = ([string]$item.SHA256).ToLowerInvariant() }
        }
    )
    return @($records | Sort-Object path)
}

function Assert-ReceiptSourceSnapshot($Snapshot, [string] $Label) {
    foreach ($item in @($Snapshot)) {
        if (-not (Test-Path -LiteralPath $item.path -PathType Leaf)) {
            throw "$Label references a missing source file: $($item.path)"
        }
        $actual = (Get-FileHash -LiteralPath $item.path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $item.sha256) {
            throw "$Label source hash differs from the recorded pin: $($item.path)"
        }
    }
}

function Get-VerifiedBuildBinding {
    param(
        [Parameter(Mandatory)][string] $BuildSummaryPath,
        [Parameter(Mandatory)][string] $PackagedPluginRoot,
        [Parameter(Mandatory)][string] $EngineRoot
    )
    $receiptPath = Get-ReceiptFullPath $BuildSummaryPath
    $packageRoot = Get-ReceiptFullPath $PackagedPluginRoot
    $engineRootPath = Get-ReceiptFullPath $EngineRoot
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    $engineManifestPath = Join-Path $engineRootPath 'Engine\Binaries\Win64\UnrealEditor.modules'
    $packageManifestPath = Join-Path $packageRoot 'Binaries\Win64\UnrealEditor.modules'
    $missingEngineManifest = -not (Test-Path -LiteralPath $engineManifestPath -PathType Leaf)
    $missingPackageManifest = -not (Test-Path -LiteralPath $packageManifestPath -PathType Leaf)
    if ($missingEngineManifest -or $missingPackageManifest) {
        throw 'Engine and packaged plugin module manifests are required.'
    }
    $engineBuildId = (Get-Content -LiteralPath $engineManifestPath -Raw | ConvertFrom-Json).BuildId
    $packageBuildId = (Get-Content -LiteralPath $packageManifestPath -Raw | ConvertFrom-Json).BuildId
    if ([string]::IsNullOrWhiteSpace([string]$engineBuildId) -or $engineBuildId -ne $packageBuildId) {
        throw 'Engine and packaged plugin BuildId values differ.'
    }
    $dll = Get-ReceiptFileIdentity (Join-Path $packageRoot 'Binaries\Win64\UnrealEditor-UE_AI_integration.dll')
    $pdb = Get-ReceiptFileIdentity (Join-Path $packageRoot 'Binaries\Win64\UnrealEditor-UE_AI_integration.pdb')

    if ($null -ne $receipt.packageBinding -and $receipt.packageBinding.verified -eq $true) {
        $packageBuildInvalid = $receipt.build.skipped -eq $true -or $receipt.build.exitCode -ne 0 -or $receipt.sourceStableForBuild -ne $true
        if ($packageBuildInvalid) { throw 'Package build receipt is not successful and source-stable.' }
        $packageDigest = Get-ReceiptPackageDigest $packageRoot
        if ($packageDigest -ne [string]$receipt.packageBinding.packageContent.contentSha256) {
            throw 'Packaged plugin content differs from the package build receipt.'
        }
        $packageArtifactsMismatch = $dll.sha256 -ne [string]$receipt.packageBinding.packageContent.moduleArtifacts.dll.sha256 -or $pdb.sha256 -ne [string]$receipt.packageBinding.packageContent.moduleArtifacts.pdb.sha256
        if ($packageArtifactsMismatch) {
            throw 'Packaged plugin DLL/PDB differs from the package build receipt.'
        }
        return [ordered]@{ mode = 'plugin-package'; packageDigest = $packageDigest; dll = $dll; pdb = $pdb; buildId = $engineBuildId; sourceStable = $true; receipt = $receiptPath }
    }

    # Whole-project receipts intentionally bind by source snapshots, module
    # pins and artifact time window; accepting ExitCode alone is prohibited.
    $missingWholeProjectProof = [string]::IsNullOrWhiteSpace([string]$receipt.Kind) -or $receipt.ExitCode -ne 0 -or $receipt.SourceStableDuringBuild -ne $true -or @($receipt.ModulePins).Count -eq 0
    if ($missingWholeProjectProof) {
        throw 'A whole-project receipt must prove successful exit, source stability and module pins.'
    }
    $receiptDirectory = Split-Path -Parent $receiptPath
    $beforePath = Join-Path $receiptDirectory 'source-before.json'
    $afterPath = Join-Path $receiptDirectory 'source-after.json'
    $missingSourceBefore = -not (Test-Path -LiteralPath $beforePath -PathType Leaf)
    $missingSourceAfter = -not (Test-Path -LiteralPath $afterPath -PathType Leaf)
    if ($missingSourceBefore -or $missingSourceAfter) {
        throw 'Whole-project receipt requires source-before.json and source-after.json.'
    }
    $before = Convert-ReceiptSourceList (Get-Content -LiteralPath $beforePath -Raw | ConvertFrom-Json) 'source-before'
    $after = Convert-ReceiptSourceList (Get-Content -LiteralPath $afterPath -Raw | ConvertFrom-Json) 'source-after'
    if ((@($before | ConvertTo-Json -Compress) -join '') -ne (@($after | ConvertTo-Json -Compress) -join '')) {
        throw 'Whole-project source-before and source-after pins differ.'
    }
    if ($receipt.SourceCount -ne @($before).Count) { throw 'Whole-project source count differs from source pin snapshot.' }
    Assert-ReceiptSourceSnapshot $after 'source-after'
    $pinIds = @(
        foreach ($pin in @($receipt.ModulePins)) {
            $pinPath = [string]$pin.Path
            $pinBuildId = [string]$pin.BuildId
            $pinHash = [string]$pin.SHA256
            if ([string]::IsNullOrWhiteSpace($pinPath) -or [string]::IsNullOrWhiteSpace($pinBuildId) -or [string]::IsNullOrWhiteSpace($pinHash)) {
                throw 'Whole-project module pins must include Path, BuildId and SHA256.'
            }
            $pinFile = Get-ReceiptFullPath $pinPath
            if ([IO.Path]::GetFileName($pinFile) -ne 'UnrealEditor.modules') {
                throw "Whole-project module pin is not an UnrealEditor.modules manifest: $pinFile"
            }
            if (-not (Test-Path -LiteralPath $pinFile -PathType Leaf)) {
                throw "Whole-project module pin references a missing manifest: $pinFile"
            }
            $pinIdentity = Get-ReceiptFileIdentity $pinFile
            if ($pinIdentity.sha256 -ne $pinHash.ToLowerInvariant()) {
                throw "Whole-project module manifest hash differs from the recorded pin: $pinFile"
            }
            $pinManifest = Get-Content -LiteralPath $pinFile -Raw | ConvertFrom-Json
            if ([string]$pinManifest.BuildId -ne $pinBuildId -or $pinBuildId -ne [string]$engineBuildId) {
                throw "Whole-project module pin BuildId differs from the engine BuildId: $pinFile"
            }
            $pinBuildId
        }
    )
    $pinIds = @($pinIds | Sort-Object -Unique)
    if ($pinIds.Count -ne 1 -or $pinIds[0] -ne $engineBuildId) { throw 'Whole-project module pins do not match the engine BuildId.' }
    $started = Convert-ReceiptUtc $receipt.StartedUTC 'StartedUTC'
    $finished = Convert-ReceiptUtc $receipt.FinishedUTC 'FinishedUTC'
    if ($finished -lt $started) { throw 'Whole-project receipt has an invalid UTC build window.' }
    foreach ($artifact in @($dll, $pdb)) {
        if ($artifact.lastWriteUtc -lt $started.AddMinutes(-1) -or $artifact.lastWriteUtc -gt $finished.AddMinutes(1)) {
            throw "Whole-project artifact was not produced in the recorded build window: $($artifact.path)"
        }
    }
    [ordered]@{ mode = 'whole-project'; packageDigest = (Get-ReceiptPackageDigest $packageRoot); dll = $dll; pdb = $pdb; buildId = $engineBuildId; sourceStable = $true; sourceCount = @($after).Count; receipt = $receiptPath }
}
