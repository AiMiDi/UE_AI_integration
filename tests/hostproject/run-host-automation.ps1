<#
.SYNOPSIS
    Isolated HostProject build and Unreal Automation runner for UE_AI_integration.

.DESCRIPTION
    Verification-lane harness. It builds the plugin through UAT BuildPlugin into
    an isolated package under -WorkRoot and then runs UnrealEditor-Cmd with
    -NullRHI against that isolated copy, so the user's project DLL and Editor
    state stay untouched. This script never edits plugin sources.

    Evidence written under -WorkRoot:
        logs\buildplugin-<stamp>.log
        logs\automation-<stamp>.log
        reports\<stamp>\index.json
        summary-<stamp>.json

    Pass -SkipBuild to reuse an existing package (for example to rerun a
    different -TestFilter without paying for a rebuild).

.EXAMPLE
    pwsh -File tests\hostproject\run-host-automation.ps1 `
        -TestFilter UE_AI_integration.Niagara.GraphEdit
#>
[CmdletBinding()]
param(
    [string] $PluginRoot,

    [string] $EngineRoot = 'S:\SilverPalace\unrealengine',

    [string] $WorkRoot = 'S:\tmp\ueai-host-automation',

    [string] $PackageDir,

    [string[]] $TestFilter = @(),

    [switch] $SkipBuild,

    [switch] $Fresh,

    [ValidateRange(30, 7200)]
    [int] $AutomationTimeoutSeconds = 1800,

    [ValidateRange(30, 3600)]
    [int] $UbtIdleTimeoutSeconds = 1800
)

$ErrorActionPreference = 'Stop'

if (-not $PluginRoot) {
    $PluginRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
}
$PluginRoot = (Resolve-Path -LiteralPath $PluginRoot).Path
$EngineRoot = (Resolve-Path -LiteralPath $EngineRoot).Path
if (-not $PackageDir) {
    $PackageDir = Join-Path $WorkRoot 'package'
}

$PluginUproject = Join-Path $PluginRoot 'UE_AI_integration.uplugin'
$RunUat = Join-Path $EngineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$EditorCmd = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
foreach ($required in @($PluginUproject, $RunUat, $EditorCmd)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing required file: $required"
    }
}

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$LogsRoot = Join-Path $WorkRoot 'logs'
$ReportsRoot = Join-Path $WorkRoot 'reports'
foreach ($directory in @($WorkRoot, $LogsRoot, $ReportsRoot)) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

function Get-PluginSnapshot {
    $previous = Get-Location
    try {
        Set-Location -LiteralPath $PluginRoot
        $head = (git rev-parse HEAD).Trim()
        $entries = git status --porcelain=v1 --untracked-files=all
        $status = (@($entries) | Sort-Object) -join "`n"
        # Status text alone does not bind an untracked test or a dirty source
        # file to the package. Include the raw worktree hash for every path
        # changed from HEAD, including deleted paths.
        $changedPaths = @(
            git diff --name-only HEAD
            git ls-files --others --exclude-standard
        ) | Where-Object { $_ } | ForEach-Object { $_.Trim() } |
            Sort-Object -Unique
        $contentEntries = @(
            foreach ($relativePath in $changedPaths) {
                $fullPath = Join-Path $PluginRoot ($relativePath -replace '/', '\')
                if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
                    $fileHash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
                    "$relativePath|$fileHash"
                }
                else {
                    "$relativePath|<missing>"
                }
            }
        )
        # Keep every changed-file hash as its own line. Passing the nested
        # array directly to @(...)-join stringifies it as System.Object[] and
        # would make unrelated worktree changes invisible to the snapshot.
        $snapshotLines = @($head, $status)
        $snapshotLines += @($contentEntries)
        $snapshotText = $snapshotLines -join "`n"
        $hash = [BitConverter]::ToString(
            [Security.Cryptography.SHA256]::Create().ComputeHash(
                [Text.Encoding]::UTF8.GetBytes($snapshotText))).Replace('-', '').ToLowerInvariant()
        return [ordered]@{
            repository = $PluginRoot
            head = $head
            snapshotSha256 = $hash
            modifiedCount = @($entries | Where-Object { $_ -match '^ M|^M ' }).Count
            untrackedCount = @($entries | Where-Object { $_ -match '^\?\?' }).Count
            changedFileCount = $contentEntries.Count
            changedFiles = $contentEntries
        }
    }
    finally {
        Set-Location -LiteralPath $previous
    }
}

function Get-DirectoryContentSnapshot {
    param([Parameter(Mandatory)][string] $Root)
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        return [ordered]@{ present = $false; fileCount = 0; totalBytes = 0; contentSha256 = $null }
    }
    $resolvedRoot = (Resolve-Path -LiteralPath $Root).Path
    $files = @(Get-ChildItem -LiteralPath $resolvedRoot -Recurse -File -Force |
        Sort-Object FullName)
    $entries = @(
        foreach ($file in $files) {
            $relativePath = [IO.Path]::GetRelativePath($resolvedRoot, $file.FullName).Replace('\', '/')
            $fileHash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            "$relativePath|$($file.Length)|$fileHash"
        }
    )
    $contentText = $entries -join "`n"
    $contentHash = [BitConverter]::ToString(
        [Security.Cryptography.SHA256]::Create().ComputeHash(
            [Text.Encoding]::UTF8.GetBytes($contentText))).Replace('-', '').ToLowerInvariant()
    return [ordered]@{
        present = $true
        fileCount = $files.Count
        totalBytes = [int64](($files | Measure-Object -Property Length -Sum).Sum)
        contentSha256 = $contentHash
    }
}

function Get-PackageContentSnapshot {
    param([Parameter(Mandatory)][string] $PackageDirectory)
    $hostPlugin = Join-Path $PackageDirectory 'HostProject\Plugins\UE_AI_integration'
    $pluginDirectory = if (Test-Path -LiteralPath $hostPlugin -PathType Container) {
        $hostPlugin
    }
    else {
        $PackageDirectory
    }
    $snapshot = Get-DirectoryContentSnapshot -Root $pluginDirectory
    $snapshot.pluginDirectory = $pluginDirectory
    return $snapshot
}

function Protect-LogFile {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $text = [IO.File]::ReadAllText($Path)
    $clean = [regex]::Replace($text, '(?im)^(.*HordeToken\s*[=:]\s*)\S+(.*)$', '$1<redacted>$2')
    $clean = [regex]::Replace($clean, '(?i)(Bearer\s+)[A-Za-z0-9\-\._~\+\/]+=*', '$1<redacted>')
    if ($clean -ne $text) {
        [IO.File]::WriteAllText($Path, $clean, [Text.UTF8Encoding]::new($false))
    }
}

function Get-ErrorLines {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return @()
    }
    return @(Select-String -Path $Path -Pattern ': (error|fatal error) [A-Z]+\d+:' |
        ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique)
}

$Snapshot = Get-PluginSnapshot
Write-Host "[harness] plugin snapshot head=$($Snapshot.head) sha256=$($Snapshot.snapshotSha256)"

$BuildExit = 0
$BuildLog = $null
$BuildErrors = @()
if (-not $SkipBuild) {
    if ($Fresh -and (Test-Path -LiteralPath $PackageDir)) {
        $resolvedPackage = [IO.Path]::GetFullPath($PackageDir)
        $resolvedWork = [IO.Path]::GetFullPath($WorkRoot)
        if (-not $resolvedPackage.StartsWith($resolvedWork, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove package outside -WorkRoot: $resolvedPackage"
        }
        Remove-Item -LiteralPath $resolvedPackage -Recurse -Force
    }

    $waitScript = Join-Path $PluginRoot 'scripts\wait_for_ubt_idle.ps1'
    if (Test-Path -LiteralPath $waitScript) {
        Write-Host '[harness] waiting for UnrealBuildTool to become idle...'
        & pwsh -NoProfile -File $waitScript -EngineRoot $EngineRoot `
            -TimeoutSeconds $UbtIdleTimeoutSeconds | Out-Null
    }

    $BuildLog = Join-Path $LogsRoot "buildplugin-$Stamp.log"
    Write-Host "[harness] UAT BuildPlugin (log: $BuildLog)"
    & $RunUat BuildPlugin "-Plugin=$($PluginUproject.Replace('\', '/'))" `
        "-Package=$($PackageDir.Replace('\', '/'))" `
        '-HostPlatforms=Win64' '-NoTargetPlatforms' *>&1 |
        Tee-Object -FilePath $BuildLog | Out-Null
    $BuildExit = $LASTEXITCODE
    Protect-LogFile -Path $BuildLog
    $BuildErrors = Get-ErrorLines -Path $BuildLog
    Write-Host "[harness] BuildPlugin exit=$BuildExit errors=$($BuildErrors.Count)"
    foreach ($errorLine in $BuildErrors) {
        Write-Host "  BUILD ERROR: $errorLine"
    }
}

$PackageBindingPath = Join-Path $WorkRoot 'package-binding.json'
$PackageBinding = [ordered]@{
    verified = $false
    sourceSnapshotSha256 = $Snapshot.snapshotSha256
    packageDir = $PackageDir
    packageContent = $null
    reason = 'not evaluated'
    metadata = $PackageBindingPath
}
$CanRunAutomation = $true
if (-not $SkipBuild -and $BuildExit -ne 0) {
    $CanRunAutomation = $false
    $PackageBinding.reason = 'build_failed'
}
elseif ($SkipBuild) {
    # A reused package is valid only when a previous run recorded the same
    # content-bound source snapshot. Otherwise the result would be evidence
    # for an unknown source revision.
    if (Test-Path -LiteralPath $PackageBindingPath -PathType Leaf) {
        $previousBinding = Get-Content -LiteralPath $PackageBindingPath -Raw | ConvertFrom-Json
        $PackageBinding.packageContent = Get-PackageContentSnapshot -PackageDirectory $PackageDir
        $PackageBinding.previousSourceSnapshotSha256 = [string]$previousBinding.sourceSnapshotSha256
        $sourceSnapshotMatches = [string]$previousBinding.sourceSnapshotSha256 -eq $Snapshot.snapshotSha256
        $packageSnapshotMatches = $PackageBinding.packageContent.present -and
            $PackageBinding.packageContent.contentSha256 -eq [string]$previousBinding.packageContent.contentSha256
        if ($sourceSnapshotMatches -and $packageSnapshotMatches) {
            $PackageBinding.verified = $true
            $PackageBinding.reason = 'reused_package_matches_source_and_package_snapshots'
        }
        else {
            $CanRunAutomation = $false
            $PackageBinding.reason = 'reused_package_snapshot_mismatch'
        }
    }
    else {
        $CanRunAutomation = $false
        $PackageBinding.reason = 'reused_package_has_no_binding_metadata'
    }
}
elseif (Test-Path -LiteralPath $PackageDir -PathType Container) {
    $PackageBinding.reason = 'fresh_build_pending_package_hash'
}
else {
    $CanRunAutomation = $false
    $PackageBinding.reason = 'build_succeeded_but_package_missing'
}
# BuildPlugin finalizes the compiled plugin at -Package and discards its
# temporary HostProject. Recreate a minimal HostProject (same shape UAT uses
# internally) so the automation step below can run the freshly built plugin DLL.
if ($CanRunAutomation -and $TestFilter.Count -gt 0) {
    $hostDir = Join-Path $PackageDir 'HostProject'
    $hostPluginDir = Join-Path $hostDir 'Plugins\UE_AI_integration'
    $hostUproject = Join-Path $hostDir 'HostProject.uproject'
    if (-not (Test-Path -LiteralPath $hostUproject)) {
        New-Item -ItemType Directory -Force -Path $hostPluginDir | Out-Null
        $packagedItems = @(Get-ChildItem -LiteralPath $PackageDir -Force |
            Where-Object { $_.Name -ne 'HostProject' })
        foreach ($item in $packagedItems) {
            Move-Item -LiteralPath $item.FullName -Destination (Join-Path $hostPluginDir $item.Name)
        }
        Set-Content -LiteralPath $hostUproject `
            -Value '{ "FileVersion": 3, "Plugins": [ { "Name": "UE_AI_integration", "Enabled": true }, { "Name": "Niagara", "Enabled": true }, { "Name": "ResonanceAudio", "Enabled": false }, { "Name": "SoundFields", "Enabled": false }, { "Name": "AudioCapture", "Enabled": false }, { "Name": "Synthesis", "Enabled": false } ] }' `
            -Encoding ASCII
        Write-Host "[harness] recreated HostProject at $hostUproject"
    }
}

if (-not $SkipBuild -and $CanRunAutomation) {
    $PackageBinding.packageContent = Get-PackageContentSnapshot -PackageDirectory $PackageDir
    if (-not $PackageBinding.packageContent.present -or $PackageBinding.packageContent.fileCount -eq 0) {
        $CanRunAutomation = $false
        $PackageBinding.reason = 'built_package_has_no_files'
    }
    else {
        $PackageBinding.verified = $true
        $PackageBinding.reason = 'fresh_build_package_hashed'
        $PackageBinding | ConvertTo-Json -Depth 8 |
            Set-Content -LiteralPath $PackageBindingPath -Encoding utf8NoBOM
    }
}

$Automation = [ordered]@{
    skipped = $true
    reason = if ($TestFilter.Count -eq 0) { 'no_test_filter' } elseif (-not $CanRunAutomation) { $PackageBinding.reason } else { 'not_started' }
}
$AutomationExit = $null
$AutomationOk = ($TestFilter.Count -eq 0)
if ($CanRunAutomation -and $TestFilter.Count -gt 0) {
    $project = Join-Path $PackageDir 'HostProject\HostProject.uproject'
    if (-not (Test-Path -LiteralPath $project)) {
        throw "Host project not found: $project (run a build first)"
    }

    $reportDir = Join-Path $ReportsRoot $Stamp
    New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
    $autoLog = Join-Path $LogsRoot "automation-$Stamp.log"
    # Accept either a string array or one comma-separated argument. Strip
    # shell-added quote characters before composing the Unreal Automation
    # expression; nested quotes make the editor treat all filters as one name.
    $filterTokens = @(
        foreach ($filter in $TestFilter) {
            if ($null -ne $filter) {
                foreach ($token in ([string]$filter -split ',')) {
                    $cleanToken = $token.Trim().Trim('"')
                    if ($cleanToken) {
                        $cleanToken
                    }
                }
            }
        }
    )
    $filterArgument = $filterTokens -join '+'
    $arguments = @(
        "`"$project`"",
        '-unattended',
        '-nop4',
        '-nosplash',
        '-NoSound',
        '-NullRHI',
        '-NoLiveCoding',
        '-UEAIDisableServer',
        '-stdout',
        '-FullStdOutLogOutput',
        '-Verbose',
        "-ExecCmds=`"Automation RunTests $filterArgument`"",
        '-TestExit="Automation Test Queue Empty"',
        "-ReportExportPath=`"$reportDir`"",
        "-abslog=`"$autoLog`""
    ) -join ' '

    Write-Host "[harness] Automation RunTests $filterArgument (log: $autoLog)"
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $EditorCmd
    $startInfo.WorkingDirectory = Split-Path -Parent $project
    $startInfo.Arguments = $arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    if (-not $process.Start()) {
        throw 'UnrealEditor-Cmd did not start.'
    }
    try {
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($AutomationTimeoutSeconds * 1000)) {
            $process.Kill()
            throw "Automation run timed out after $AutomationTimeoutSeconds seconds."
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        $AutomationExit = $process.ExitCode
    }
    finally {
        $process.Dispose()
    }
    [IO.File]::WriteAllText(
        (Join-Path $LogsRoot "automation-$Stamp.stdout.log"), $stdout, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText(
        (Join-Path $LogsRoot "automation-$Stamp.stderr.log"), $stderr, [Text.UTF8Encoding]::new($false))
    Protect-LogFile -Path $autoLog
    Protect-LogFile -Path (Join-Path $LogsRoot "automation-$Stamp.stdout.log")
    Protect-LogFile -Path (Join-Path $LogsRoot "automation-$Stamp.stderr.log")

    $reportPath = Join-Path $reportDir 'index.json'
    if (-not (Test-Path -LiteralPath $reportPath)) {
        $candidate = Get-ChildItem -LiteralPath $reportDir -Filter '*.json' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $reportPath = if ($candidate) { $candidate.FullName } else { $null }
    }
    if (-not $reportPath) {
        throw "Automation report not found under $reportDir (see $autoLog)."
    }

    $report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
    $reportTests = @($report.tests)
    $failedTests = @($reportTests | Where-Object { $_.state -ne 'Success' } |
        ForEach-Object {
            [ordered]@{
                path = $_.fullTestPath
                state = $_.state
                messages = @($_.entries | ForEach-Object {
                    if ($_.event -and $_.event.message) {
                        [string]$_.event.message
                    }
                    else {
                        [string]$_.message
                    }
                })
            }
        })
    $reportSucceeded = [int]$report.succeeded
    $reportSucceededWithWarnings = [int]$report.succeededWithWarnings
    $reportFailed = [int]$report.failed
    $reportNotRun = [int]$report.notRun
    # Unreal's report counters put warning-bearing successes in
    # succeededWithWarnings and may leave succeeded at zero. The per-test
    # state is the authoritative success signal for the requested filters.
    $successfulTestStates = @($reportTests | Where-Object { $_.state -eq 'Success' })
    $successfulTestCount = $successfulTestStates.Count
    $testPaths = @($reportTests | ForEach-Object { [string]$_.fullTestPath })
    # Command-line array arguments arrive as one comma-quoted string; split so
    # each requested filter is checked against the report individually.
    $normalizedFilters = @($TestFilter | ForEach-Object { $_ -split ',' }) |
        Where-Object { $_ } | ForEach-Object { $_.Trim().Trim('"') }
    $unmatchedFilters = @($normalizedFilters | Where-Object {
        $requested = $_
        -not @($testPaths | Where-Object { $_ -like "$requested*" }).Count
    })
    $gateReasons = @()
    if ($AutomationExit -ne 0) { $gateReasons += "editor_exit_$AutomationExit" }
    if ($reportFailed -ne 0) { $gateReasons += "report_failed_$reportFailed" }
    if ($successfulTestCount -le 0) { $gateReasons += 'no_successful_tests' }
    if ($reportNotRun -ne 0) { $gateReasons += "report_not_run_$reportNotRun" }
    if ($failedTests.Count -ne 0) { $gateReasons += "non_success_test_states_$($failedTests.Count)" }
    if ($reportTests.Count -eq 0) { $gateReasons += 'empty_test_report' }
    if ($unmatchedFilters.Count -ne 0) { $gateReasons += 'requested_filter_unmatched' }
    $AutomationOk = $gateReasons.Count -eq 0
    Write-Host ("[harness] automation exit={0} succeeded={1} succeededWithWarnings={2} successfulTestStates={3} failed={4} notRun={5} tests={6} report={7}" -f $AutomationExit, $reportSucceeded, $reportSucceededWithWarnings, $successfulTestCount, $reportFailed, $reportNotRun, $reportTests.Count, $reportPath)
    if ($gateReasons.Count -gt 0) {
        Write-Host "  GATE FAILED: $($gateReasons -join ', ')"
    }
    foreach ($failed in $failedTests) {
        Write-Host "  FAILED: $($failed.path) [$($failed.state)]"
    }

    $Automation = [ordered]@{
        skipped = $false
        filters = $TestFilter
        exitCode = $AutomationExit
        log = $autoLog
        report = $reportPath
        succeeded = $reportSucceeded
        succeededWithWarnings = $reportSucceededWithWarnings
        successfulTestStates = $successfulTestCount
        failed = $reportFailed
        notRun = $reportNotRun
        testCount = $reportTests.Count
        unmatchedFilters = $unmatchedFilters
        gateReasons = $gateReasons
        failedTests = $failedTests
    }
}

$summaryPath = Join-Path $WorkRoot "summary-$Stamp.json"
$summary = [ordered]@{
    schema = 'ue.host-automation-run.v1'
    utcStarted = [DateTime]::UtcNow.ToString('o')
    pluginSnapshot = $Snapshot
    engineRoot = $EngineRoot
    packageDir = $PackageDir
    packageBinding = $PackageBinding
    build = [ordered]@{
        skipped = [bool]$SkipBuild
        exitCode = $BuildExit
        log = $BuildLog
        errors = $BuildErrors
    }
    automation = $Automation
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding utf8NoBOM
Write-Host "[harness] summary: $summaryPath"

if (-not $SkipBuild -and $BuildExit -ne 0) {
    exit 1
}
if ($TestFilter.Count -gt 0 -and -not $CanRunAutomation) {
    exit 3
}
if ($TestFilter.Count -gt 0 -and -not $AutomationOk) {
    exit 2
}
exit 0
