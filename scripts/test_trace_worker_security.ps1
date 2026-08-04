[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $WorkerPath
)

$ErrorActionPreference = 'Stop'

if (-not $IsWindows -and $env:OS -ne 'Windows_NT') {
    throw 'This security regression currently targets the mandatory Win64 release gate.'
}

$WorkerPath = [System.IO.Path]::GetFullPath(
    (Resolve-Path -LiteralPath $WorkerPath -ErrorAction Stop).Path)
if (-not (Test-Path -LiteralPath $WorkerPath -PathType Leaf)) {
    throw "Trace Worker executable does not exist: $WorkerPath"
}

function Assert-True([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
}

function Invoke-WorkerRequest(
    [hashtable] $Request,
    [hashtable] $Environment,
    [string] $AdditionalArguments = '') {
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $WorkerPath
    $StartInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($WorkerPath)
    $StartInfo.Arguments = "--stdio $AdditionalArguments".Trim()
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardInput = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Name in $Environment.Keys) {
        $StartInfo.EnvironmentVariables[$Name] = [string] $Environment[$Name]
    }

    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    if (-not $Process.Start()) {
        throw 'Trace Worker process did not start.'
    }
    try {
        $Payload = $Request | ConvertTo-Json -Depth 12 -Compress
        $Process.StandardInput.Write($Payload)
        $Process.StandardInput.Close()
		$StdoutTask = $Process.StandardOutput.ReadToEndAsync()
		$StderrTask = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit(30000)) {
            $Process.Kill()
            throw 'Trace Worker request timed out.'
        }
		$Stdout = $StdoutTask.Result
		$Stderr = $StderrTask.Result
        if ([string]::IsNullOrWhiteSpace($Stdout)) {
            throw "Trace Worker returned no JSON. stderr: $Stderr"
        }
        return [pscustomobject]@{
            ExitCode = $Process.ExitCode
            Json = ($Stdout.Trim() | ConvertFrom-Json)
            Stderr = $Stderr
        }
    }
    finally {
        $Process.Dispose()
    }
}

function Invoke-Import(
    [string] $TracePath,
    [string] $CopyMode,
    [hashtable] $Environment,
    [string] $AdditionalArguments = '') {
    return Invoke-WorkerRequest -Environment $Environment `
        -AdditionalArguments $AdditionalArguments -Request @{
            schema = 'ue.trace-worker-request.v1'
            action = 'execute'
            requestId = [guid]::NewGuid().ToString('N')
            capability = 'production.trace.import'
            params = @{
                tracePath = $TracePath
                copyMode = $CopyMode
            }
        }
}

function New-DiagnosticTraceFixture(
    [string] $TracePath,
    [string] $EngineMarker = '') {
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $WorkerPath
    $StartInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($WorkerPath)
    $StartInfo.Arguments = "--generate-fixture=`"$TracePath`""
    if (-not [string]::IsNullOrEmpty($EngineMarker)) {
        $StartInfo.Arguments += " --fixtureEngineMarker=$EngineMarker"
    }
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    if (-not $Process.Start()) {
        throw 'ASSERTION FAILED: Diagnostic fixture process must start.'
    }
    try {
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit(30000)) {
            $Process.Kill()
            throw 'ASSERTION FAILED: Diagnostic fixture generation timed out.'
        }
        $Stdout = $StdoutTask.Result
        $Stderr = $StderrTask.Result
        Assert-True ($Process.ExitCode -eq 0) `
            "Diagnostic fixture generation failed: $Stderr"
        Assert-True ($Stderr -notmatch 'WriteError') `
            "Diagnostic fixture generation reported a Trace write failure: $Stderr"
        $Envelope = $Stdout.Trim() | ConvertFrom-Json
        Assert-True ($Envelope.ok -and (Test-Path -LiteralPath $TracePath)) `
            'Diagnostic fixture generation must create an analyzable trace.'
        return $Envelope
    }
    finally {
        $Process.Dispose()
    }
}

function Read-Exact(
    [System.IO.Stream] $Stream,
    [int] $Count) {
    $Buffer = [byte[]]::new($Count)
    $Offset = 0
    while ($Offset -lt $Count) {
        $Pending = $Stream.BeginRead(
            $Buffer,
            $Offset,
            $Count - $Offset,
            $null,
            $null)
        if (-not $Pending.AsyncWaitHandle.WaitOne(5000)) {
            throw 'Timed out waiting for the resident Named Pipe response.'
        }
        $Read = $Stream.EndRead($Pending)
        if ($Read -le 0) {
            throw 'Named Pipe closed before the complete response arrived.'
        }
        $Offset += $Read
    }
    return $Buffer
}

function Send-PipeRequest(
    [System.IO.Pipes.NamedPipeClientStream] $Pipe,
    [hashtable] $Request) {
    $Payload = [System.Text.Encoding]::UTF8.GetBytes(
        ($Request | ConvertTo-Json -Depth 12 -Compress))
    $Length = [System.BitConverter]::GetBytes([uint32] $Payload.Length)
    $Pipe.Write($Length, 0, $Length.Length)
    $Pipe.Write($Payload, 0, $Payload.Length)
    $Pipe.Flush()
    $ResponseLengthBytes = Read-Exact -Stream $Pipe -Count 4
    $ResponseLength = [System.BitConverter]::ToUInt32($ResponseLengthBytes, 0)
    Assert-True ($ResponseLength -gt 0 -and $ResponseLength -le 4MB) `
        'Resident response length must remain bounded.'
    $ResponseBytes = Read-Exact -Stream $Pipe -Count ([int] $ResponseLength)
    return ([System.Text.Encoding]::UTF8.GetString($ResponseBytes) |
        ConvertFrom-Json)
}

$TempBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$TestRoot = Join-Path $TempBase ('UEAITraceSecurity-' + [guid]::NewGuid().ToString('N'))
$TestRoot = [System.IO.Path]::GetFullPath($TestRoot)
$TestName = [System.IO.Path]::GetFileName($TestRoot)
if (-not $TestRoot.StartsWith(
        $TempBase,
        [System.StringComparison]::OrdinalIgnoreCase) -or
    -not $TestName.StartsWith(
        'UEAITraceSecurity-',
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing unsafe test directory: $TestRoot"
}

$AllowedRoot = Join-Path $TestRoot 'allowed'
$OutsideRoot = Join-Path $TestRoot 'outside'
$StoreRoot = Join-Path $TestRoot 'store'
$Junction = Join-Path $AllowedRoot 'escape'
$ArtifactJunction = Join-Path $StoreRoot 'artifact-escape'
$RelocatedAnalysisOwner = Join-Path $TestRoot 'relocated-analysis-owner'
$AnalysisOwnerJunction = $null
$AnalysisOwnerJunctionActive = $false
$ServerProcess = $null
$PipeClients = @()

try {
    New-Item -ItemType Directory -Path $AllowedRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $OutsideRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $StoreRoot -Force | Out-Null
    $InsideTrace = Join-Path $AllowedRoot 'inside.utrace'
    $OutsideTrace = Join-Path $OutsideRoot 'outside.utrace'
    $SameHashOutsideTrace = Join-Path $OutsideRoot 'same-hash.utrace'
    [System.IO.File]::WriteAllBytes(
        $InsideTrace,
        [System.Text.Encoding]::UTF8.GetBytes('inside-trace-fixture'))
    [System.IO.File]::WriteAllBytes(
        $OutsideTrace,
        [System.Text.Encoding]::UTF8.GetBytes('outside-trace-fixture'))
    [System.IO.File]::WriteAllBytes(
        $SameHashOutsideTrace,
        [System.IO.File]::ReadAllBytes($InsideTrace))
    New-Item -ItemType Junction -Path $Junction -Target $OutsideRoot | Out-Null

    $Environment = @{
        UEAI_TRACE_STORE = $StoreRoot
        UEAI_TRACE_ROOTS = $AllowedRoot
    }

    # The exact version-controlled profile shipped beside the Worker must be
    # accepted. Use a bounded fake project/executable here: launch.plan only
    # hashes and approves them; it does not start the process.
    $PlanProjectRoot = Join-Path $TestRoot 'plan-project'
    $PlanProjectPath = Join-Path $PlanProjectRoot 'TracePlanFixture.uproject'
    $PlanExecutableDirectory = Join-Path $PlanProjectRoot 'Binaries\Win64'
    $PlanExecutablePath = Join-Path $PlanExecutableDirectory 'TracePlanFixture.exe'
    New-Item -ItemType Directory -Path $PlanExecutableDirectory -Force | Out-Null
    [System.IO.File]::WriteAllText(
        $PlanProjectPath,
        '{"FileVersion":3}',
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllBytes(
        $PlanExecutablePath,
        [System.Text.Encoding]::UTF8.GetBytes('bounded launch-plan fixture'))
    $TargetList = Invoke-WorkerRequest -Environment $Environment -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.target.list'
        params = @{}
    }
    $DevelopmentTarget = $TargetList.Json.data.targets |
        Where-Object { $_.kind -eq 'development' } |
        Select-Object -First 1
    $InstalledProfile = $TargetList.Json.data.launchProfiles |
        Where-Object { $_.id -eq 'projectDevelopment' } |
        Select-Object -First 1
    Assert-True ($TargetList.Json.ok -and $null -ne $DevelopmentTarget -and
        $DevelopmentTarget.available -and $null -ne $InstalledProfile) `
        'The installed projectDevelopment launch profile must parse and be available.'
    $LaunchPlan = Invoke-WorkerRequest -Environment $Environment -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.launch.plan'
        params = @{
            launchProfileId = 'projectDevelopment'
            project = $PlanProjectPath
            map = '/Game/Test'
            preset = 'standard'
            postStop = 'artifactOnly'
        }
    }
    Assert-True ($LaunchPlan.Json.ok -and
        $LaunchPlan.Json.data.executionReady -and
        $LaunchPlan.Json.data.launchProfileId -eq 'projectDevelopment' -and
        $LaunchPlan.Json.data.planDigest -match '^sha256:[0-9a-f]{64}$') `
        'The installed projectDevelopment profile must produce an executable Launch Plan.'

    $InsideReference = Invoke-Import $InsideTrace 'reference' $Environment
    Assert-True $InsideReference.Json.ok `
        'A canonical in-root reference import must succeed.'

    $OutsideReference = Invoke-Import $OutsideTrace 'reference' $Environment
    Assert-True (-not $OutsideReference.Json.ok) `
        'An outside-root reference import must be rejected.'
    Assert-True ($OutsideReference.Json.error.code -eq 'trace_path_not_allowed') `
        'Outside-root reference rejection must use trace_path_not_allowed.'

    $JunctionReference = Invoke-Import `
        (Join-Path $Junction 'outside.utrace') 'reference' $Environment
    Assert-True (-not $JunctionReference.Json.ok) `
        'A junction that escapes an allowed root must be rejected.'
    Assert-True ($JunctionReference.Json.error.code -eq 'trace_path_not_allowed') `
        'Junction escape rejection must use trace_path_not_allowed.'

    $OutsideCopy = Invoke-Import $OutsideTrace 'copy' $Environment
    Assert-True $OutsideCopy.Json.ok `
        'An explicit copy import may originate outside configured roots.'
    Assert-True ($OutsideCopy.Json.data.copyMode -eq 'copy') `
        'Outside-root copy import must be recorded as copy.'

    $CopyOverReference = Invoke-Import $SameHashOutsideTrace 'copy' $Environment
    Assert-True $CopyOverReference.Json.ok `
        'A copy request sharing a prior reference hash must succeed.'
    Assert-True ($CopyOverReference.Json.data.copyMode -eq 'copy') `
        'A prior reference record must not satisfy a stronger copy request.'
    $CopyRecord = Get-Content -LiteralPath (
        Join-Path $StoreRoot ('records\' + $CopyOverReference.Json.data.traceId + '.json')) `
        -Raw | ConvertFrom-Json
    Assert-True ($CopyRecord.copyMode -eq 'copy') `
        'The shared hash registry record must be upgraded to a store copy.'
    $CanonicalStore = [System.IO.Path]::GetFullPath($StoreRoot)
    $CanonicalTrace = [System.IO.Path]::GetFullPath($CopyRecord.tracePath)
    Assert-True ($CanonicalTrace.StartsWith(
            $CanonicalStore + [System.IO.Path]::DirectorySeparatorChar,
            [System.StringComparison]::OrdinalIgnoreCase)) `
        'The upgraded record must point inside the Worker store.'

    $OversizedTrace = Join-Path $OutsideRoot 'oversized.utrace'
    [System.IO.File]::WriteAllBytes($OversizedTrace, [byte[]]::new(2MB))
    $Oversized = Invoke-Import `
        $OversizedTrace 'copy' $Environment '--maxImportMiB=1'
    Assert-True (-not $Oversized.Json.ok) `
        'The configurable import bound must reject oversized traces.'
    Assert-True ($Oversized.Json.error.code -eq 'trace_hash_failed') `
        'Oversized import rejection must be deterministic.'

    # Generate a real analyzable fixture so the resident gate proves that
    # TraceServices was loaded before request threads begin using providers.
    $AnalysisFixture = Join-Path $AllowedRoot 'resident-analysis.utrace'
    $FixtureEnvelope = New-DiagnosticTraceFixture $AnalysisFixture
    $FixtureReceipt = Get-Content -LiteralPath $FixtureEnvelope.receiptPath `
        -Raw | ConvertFrom-Json
    Assert-True ($FixtureReceipt.managedEngineMarker -match '^UEAI_TRACE_ENGINE_VERSION=\d+\.\d+$') `
        'The managed diagnostic fixture must publish an exact Engine marker.'

    # Analysis artifact records are persisted metadata and therefore treated as
    # untrusted on read. A lexical in-store path through a junction must not be
    # able to disclose a file outside the canonical Worker store.
    $ArtifactFixtureImport = Invoke-Import $AnalysisFixture 'reference' $Environment
    Assert-True $ArtifactFixtureImport.Json.ok `
        'The artifact ownership fixture trace must import successfully.'
    foreach ($InvalidCursor in @(1.5, '1x', '18446744073709551616')) {
        $InvalidCursorResult = Invoke-WorkerRequest `
            -Environment $Environment -Request @{
                schema = 'ue.trace-worker-request.v1'
                action = 'execute'
                requestId = [guid]::NewGuid().ToString('N')
                capability = 'production.trace.counter.query'
                params = @{
                    traceId = $ArtifactFixtureImport.Json.data.traceId
                    operation = 'list'
                    cursor = $InvalidCursor
                    limit = 1
                }
            }
        Assert-True (-not $InvalidCursorResult.Json.ok -and
            $InvalidCursorResult.Json.error.code -eq 'trace_query_invalid') `
            "Invalid cursor '$InvalidCursor' must be rejected without truncation."
    }
    $ArtifactAnalysis = Invoke-WorkerRequest -Environment $Environment -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.provider.list'
        params = @{ traceId = $ArtifactFixtureImport.Json.data.traceId }
    }
    Assert-True ($ArtifactAnalysis.Json.ok -and
        $ArtifactAnalysis.Json.data.analysisId -match '^trace-analysis-local-') `
        'The artifact ownership fixture must create a persisted analysis job.'
    $OwnedAnalysisArtifact = Invoke-WorkerRequest `
        -Environment $Environment -Request @{
            schema = 'ue.trace-worker-request.v1'
            action = 'execute'
            requestId = [guid]::NewGuid().ToString('N')
            capability = 'production.job.artifact.get'
            params = @{
                jobId = $ArtifactAnalysis.Json.data.analysisId
                artifactId = 'result'
                maxBytes = 1024
            }
        }
    Assert-True ($OwnedAnalysisArtifact.Json.ok -and
        $OwnedAnalysisArtifact.Json.data.contentBase64.Length -gt 0) `
        'A canonical analysis result artifact must remain readable.'
    $ExportAnalysis = Invoke-WorkerRequest -Environment $Environment -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.export'
        params = @{
            traceId = $ArtifactFixtureImport.Json.data.traceId
            provider = 'timing'
            operation = 'events'
            format = 'json'
            limit = 16
        }
    }
    Assert-True ($ExportAnalysis.Json.ok -and
        $ExportAnalysis.Json.data.analysisId -match '^trace-analysis-local-') `
        'The artifact ownership fixture must create a bounded export.'
    $OwnedExportArtifact = Invoke-WorkerRequest `
        -Environment $Environment -Request @{
            schema = 'ue.trace-worker-request.v1'
            action = 'execute'
            requestId = [guid]::NewGuid().ToString('N')
            capability = 'production.job.artifact.get'
            params = @{
                jobId = $ExportAnalysis.Json.data.analysisId
                artifactId = 'export'
                maxBytes = 1024
            }
        }
    Assert-True ($OwnedExportArtifact.Json.ok -and
        $OwnedExportArtifact.Json.data.contentBase64.Length -gt 0) `
        'A canonical trace export artifact must remain readable.'
    $OutsideArtifact = Join-Path $OutsideRoot 'artifact-secret.bin'
    [System.IO.File]::WriteAllBytes(
        $OutsideArtifact,
        [System.Text.Encoding]::UTF8.GetBytes('must-not-cross-artifact-root'))
    New-Item -ItemType Junction -Path $ArtifactJunction -Target $OutsideRoot |
        Out-Null
    $AnalysisRecordPath = Join-Path $StoreRoot (
        'analyses\' + $ArtifactAnalysis.Json.data.analysisId + '\result.json')
    $AnalysisRecord = Get-Content -LiteralPath $AnalysisRecordPath -Raw |
        ConvertFrom-Json
    $AnalysisRecord.artifacts[0].path = Join-Path $ArtifactJunction 'artifact-secret.bin'
    [System.IO.File]::WriteAllText(
        $AnalysisRecordPath,
        ($AnalysisRecord | ConvertTo-Json -Depth 20),
        [System.Text.UTF8Encoding]::new($false))
    $EscapedAnalysisArtifact = Invoke-WorkerRequest `
        -Environment $Environment -Request @{
            schema = 'ue.trace-worker-request.v1'
            action = 'execute'
            requestId = [guid]::NewGuid().ToString('N')
            capability = 'production.job.artifact.get'
            params = @{
                jobId = $ArtifactAnalysis.Json.data.analysisId
                artifactId = 'result'
                maxBytes = 1024
            }
        }
    Assert-True (-not $EscapedAnalysisArtifact.Json.ok -and
        $EscapedAnalysisArtifact.Json.error.code -eq 'artifact_not_found') `
        'Analysis artifact reads must reject a junction that escapes the canonical store.'
    [System.IO.Directory]::Delete($ArtifactJunction)

    $NoMarkerFixture = Join-Path $AllowedRoot 'no-engine-marker.utrace'
    New-DiagnosticTraceFixture $NoMarkerFixture 'none' | Out-Null
    $NoMarkerImport = Invoke-Import $NoMarkerFixture 'reference' $Environment
    Assert-True $NoMarkerImport.Json.ok `
        'A valid unmarked trace may be imported before analysis validation.'
    $NoMarkerQuery = Invoke-WorkerRequest -Environment $Environment -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.provider.list'
        params = @{ traceId = $NoMarkerImport.Json.data.traceId }
    }
    Assert-True (-not $NoMarkerQuery.Json.ok -and
        $NoMarkerQuery.Json.error.code -eq 'trace_engine_version_unknown') `
        'An unknown BuildVersion without a managed marker must fail closed.'

    $MarkerVersion = $FixtureReceipt.managedEngineMarker.Substring(
        'UEAI_TRACE_ENGINE_VERSION='.Length)
    $MarkerParts = $MarkerVersion.Split('.')
    $CrossMinorMarker = $MarkerParts[0] + '.' + ([int]$MarkerParts[1] + 1)
    $CrossMinorFixture = Join-Path $AllowedRoot 'cross-minor-marker.utrace'
    New-DiagnosticTraceFixture $CrossMinorFixture $CrossMinorMarker | Out-Null
    $CrossMinorImport = Invoke-Import $CrossMinorFixture 'reference' $Environment
    Assert-True $CrossMinorImport.Json.ok `
        'A cross-minor fixture may be imported before analysis validation.'
    $CrossMinorQuery = Invoke-WorkerRequest -Environment $Environment -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.provider.list'
        params = @{ traceId = $CrossMinorImport.Json.data.traceId }
    }
    Assert-True (-not $CrossMinorQuery.Json.ok -and
        $CrossMinorQuery.Json.error.code -eq 'trace_engine_version_mismatch') `
        'A managed marker from another Engine minor must be rejected.'

    $PipeName = 'UEAITraceSecurity-' + [guid]::NewGuid().ToString('N')
    $Endpoint = '\\.\pipe\' + $PipeName
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $WorkerPath
    $StartInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($WorkerPath)
    $StartInfo.Arguments = @(
        '--serve',
        "--endpoint=$Endpoint",
        '--idleSeconds=30',
        '--connectionIoTimeoutSeconds=1',
        '--maxSessions=2'
    ) -join ' '
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Name in $Environment.Keys) {
        $StartInfo.EnvironmentVariables[$Name] = [string] $Environment[$Name]
    }
    $ServerProcess = [System.Diagnostics.Process]::new()
    $ServerProcess.StartInfo = $StartInfo
    if (-not $ServerProcess.Start()) {
        $ServerProcess.Dispose()
        $ServerProcess = $null
        throw 'ASSERTION FAILED: Resident Trace Worker must start.'
    }

    foreach ($Index in 1..2) {
        $Client = [System.IO.Pipes.NamedPipeClientStream]::new(
            '.',
            $PipeName,
            [System.IO.Pipes.PipeDirection]::InOut,
            [System.IO.Pipes.PipeOptions]::None)
        $Client.Connect(5000)
        $Client.WriteByte(1)
        $Client.Flush()
        $PipeClients += $Client
    }
    Start-Sleep -Milliseconds 1600

    $HealthyClient = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.',
        $PipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    $HealthyClient.Connect(5000)
    $PipeClients += $HealthyClient
    $Handshake = Send-PipeRequest -Pipe $HealthyClient -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'handshake'
        requestId = [guid]::NewGuid().ToString('N')
    }
    Assert-True $Handshake.ok `
        'A valid client must be served after two partial clients time out.'
    Assert-True ($Handshake.data.analysisSessionCacheCapacity -eq 2) `
        'Resident analysis cache capacity must be exactly two trace hashes.'
    Assert-True ($Handshake.data.analysisSessionPolicy -eq 'sha256-lru') `
        'Resident analysis cache must use the declared SHA-256 LRU policy.'
    Assert-True ($Handshake.data.maximumConcurrentAnalyses -eq 1) `
        'Resident TraceServices analysis must remain serialized.'

    $ImportClient = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    $ImportClient.Connect(5000)
    $PipeClients += $ImportClient
    $ResidentImport = Send-PipeRequest -Pipe $ImportClient -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.import'
        params = @{
            tracePath = $AnalysisFixture
            copyMode = 'reference'
        }
    }
    Assert-True $ResidentImport.ok `
        'The resident Worker must import the diagnostic trace.'
    $ProviderClient = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    $ProviderClient.Connect(5000)
    $PipeClients += $ProviderClient
    $ResidentProviders = Send-PipeRequest -Pipe $ProviderClient -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.provider.list'
        params = @{ traceId = $ResidentImport.data.traceId }
    }
    Assert-True ($ResidentProviders.ok -and $ResidentProviders.data.total -ge 10) `
        'Resident provider discovery must execute through TraceServices.'
    Assert-True ($ResidentProviders.data.engineVersionStatus -eq 'matchedManagedMarker') `
        'Unknown source-build BuildVersion must be admitted only by the exact managed marker.'

    # Replace one persisted analysis-job directory with a junction. The same
    # final-path ownership primitive protects Development launch job folders.
    $AnalysisOwnerJunction = Join-Path $StoreRoot (
        'analyses\' + $ResidentProviders.data.analysisId)
    Move-Item -LiteralPath $AnalysisOwnerJunction `
        -Destination $RelocatedAnalysisOwner
    New-Item -ItemType Junction -Path $AnalysisOwnerJunction `
        -Target $RelocatedAnalysisOwner |
        Out-Null
    $AnalysisOwnerJunctionActive = $true
    Assert-True (((Get-Item -LiteralPath $AnalysisOwnerJunction).Attributes -band
            [System.IO.FileAttributes]::ReparsePoint) -ne 0) `
        'The ownership-root replacement fixture must be a real junction.'
    $ArtifactOwnerClient = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    $ArtifactOwnerClient.Connect(5000)
    $PipeClients += $ArtifactOwnerClient
    $ReplacedOwnerArtifact = Send-PipeRequest -Pipe $ArtifactOwnerClient -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.job.artifact.get'
        params = @{
            jobId = $ResidentProviders.data.analysisId
            artifactId = 'result'
            maxBytes = 1024
        }
    }
    Assert-True (-not $ReplacedOwnerArtifact.ok -and
        $ReplacedOwnerArtifact.error.code -eq 'artifact_not_found') `
        ("Artifact reads must reject replacement of their owning directory by a junction; " +
            "ok=$($ReplacedOwnerArtifact.ok) code=$($ReplacedOwnerArtifact.error.code) " +
            "path=$($ReplacedOwnerArtifact.data.path).")
    [System.IO.Directory]::Delete($AnalysisOwnerJunction)
    $AnalysisOwnerJunctionActive = $false
    Move-Item -LiteralPath $RelocatedAnalysisOwner `
        -Destination $AnalysisOwnerJunction
    $ScreenshotStatus = $ResidentProviders.data.providers |
        Where-Object { $_.id -eq 'screenshot' } |
        Select-Object -First 1
    Assert-True ($null -ne $ScreenshotStatus -and -not $ScreenshotStatus.recorded) `
        'The diagnostic fixture intentionally has no Screenshot provider data.'
    $ScreenshotClient = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    $ScreenshotClient.Connect(5000)
    $PipeClients += $ScreenshotClient
    $MissingScreenshot = Send-PipeRequest -Pipe $ScreenshotClient -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.screenshot.query'
        params = @{
            traceId = $ResidentImport.data.traceId
            operation = 'list'
            limit = 16
        }
    }
    Assert-True (-not $MissingScreenshot.ok -and
        $MissingScreenshot.error.code -eq 'trace_provider_unavailable') `
        'A missing Screenshot provider must not be returned as an empty success.'
    $TimingClient = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.', $PipeName, [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    $TimingClient.Connect(5000)
    $PipeClients += $TimingClient
    $ResidentTiming = Send-PipeRequest -Pipe $TimingClient -Request @{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = [guid]::NewGuid().ToString('N')
        capability = 'production.trace.timing.query'
        params = @{
            traceId = $ResidentImport.data.traceId
            operation = 'events'
            limit = 16
        }
    }
    Assert-True ($ResidentTiming.ok -and $ResidentTiming.data.query.total -gt 0) `
        'Resident timing query must return real diagnostic scope rows.'

    Write-Host 'Trace Worker security regression passed.'
}
finally {
    foreach ($Client in $PipeClients) {
        if ($null -ne $Client) {
            $Client.Dispose()
        }
    }
    if ($null -ne $ServerProcess) {
        if ($ServerProcess.HasExited -and $ServerProcess.ExitCode -ne 0) {
            $ServerStdout = $ServerProcess.StandardOutput.ReadToEnd()
            $ServerStderr = $ServerProcess.StandardError.ReadToEnd()
            Write-Warning (
                "Resident Trace Worker exited with $($ServerProcess.ExitCode). " +
                "stdout: $ServerStdout stderr: $ServerStderr")
        }
        if (-not $ServerProcess.HasExited) {
            $ServerProcess.Kill()
            $ServerProcess.WaitForExit(5000) | Out-Null
        }
        $ServerProcess.Dispose()
    }
    if ($AnalysisOwnerJunctionActive -and
        -not [string]::IsNullOrEmpty($AnalysisOwnerJunction) -and
        (Test-Path -LiteralPath $AnalysisOwnerJunction)) {
        [System.IO.Directory]::Delete($AnalysisOwnerJunction)
        $AnalysisOwnerJunctionActive = $false
    }
    if ((Test-Path -LiteralPath $RelocatedAnalysisOwner) -and
        -not [string]::IsNullOrEmpty($AnalysisOwnerJunction) -and
        -not (Test-Path -LiteralPath $AnalysisOwnerJunction)) {
        Move-Item -LiteralPath $RelocatedAnalysisOwner `
            -Destination $AnalysisOwnerJunction
    }
    if (Test-Path -LiteralPath $ArtifactJunction) {
        [System.IO.Directory]::Delete($ArtifactJunction)
    }
    if (Test-Path -LiteralPath $Junction) {
        [System.IO.Directory]::Delete($Junction)
    }
    if (Test-Path -LiteralPath $TestRoot) {
        $VerifiedRoot = [System.IO.Path]::GetFullPath($TestRoot)
        if (-not $VerifiedRoot.StartsWith(
                $TempBase,
                [System.StringComparison]::OrdinalIgnoreCase) -or
            -not ([System.IO.Path]::GetFileName($VerifiedRoot)).StartsWith(
                'UEAITraceSecurity-',
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove unsafe test directory: $VerifiedRoot"
        }
        Remove-Item -LiteralPath $VerifiedRoot -Recurse -Force
    }
}
