[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $CliPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $WorkerPath,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string] $ContractRoot,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Leaf })]
    [string] $Project,

    [Parameter(Mandatory = $true)]
    [ValidateScript({ Test-Path -LiteralPath $_ -PathType Container })]
    [string] $WorkerStore,

    [string[]] $TraceRoots = @(),
    [string] $LaunchProfileId = "projectDevelopment",
    [string] $Map = "/Game/Maps/TraceFixture",
    [ValidateSet("standard", "cpu", "fullInsights")]
    [string[]] $Presets = @("standard", "cpu", "fullInsights"),
    [ValidateRange(5, 3600)]
    [int] $MaxDurationSeconds = 12,
    [ValidateRange(64, 16384)]
    [int] $MaxFileSizeMiB = 2048,
    [ValidateRange(30, 1800)]
    [int] $JobTimeoutSeconds = 300,
    [string] $EvidencePath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

function Assert-Condition {
    param(
        [bool] $Condition,
        [string] $Message
    )
    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-UeCapability {
    param(
        [string] $Capability,
        [hashtable] $Parameters
    )

    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $script:ResolvedCliPath
    $startInfo.Arguments = "$Capability --params-file - --json"
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    Assert-Condition ($process.Start()) "Failed to start ue CLI."
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $serialized = $Parameters | ConvertTo-Json -Depth 64 -Compress
    $process.StandardInput.Write($serialized)
    $process.StandardInput.Close()
    $process.WaitForExit()
    $stdout = $stdoutTask.Result.Trim()
    $stderr = $stderrTask.Result.Trim()
    if ($process.ExitCode -ne 0) {
        throw "ue $Capability failed with exit $($process.ExitCode). stderr=$stderr stdout=$stdout"
    }
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($stdout)) `
        "ue $Capability returned no JSON. stderr=$stderr"
    try {
        $envelope = $stdout | ConvertFrom-Json
    }
    catch {
        throw "ue $Capability returned invalid JSON. stderr=$stderr stdout=$stdout"
    }
    Assert-Condition ([bool]$envelope.ok) `
        "ue $Capability returned ok=false. stderr=$stderr stdout=$stdout"
    return $envelope
}

function Wait-TraceJob {
    param(
        [string] $JobId,
        [int] $TimeoutSeconds
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $phases = New-Object System.Collections.Generic.List[string]
    do {
        $statusEnvelope = Invoke-UeCapability `
            "production.job.status" `
            @{ jobId = $JobId }
        $status = $statusEnvelope.data
        if ($null -ne $status.phase -and -not $phases.Contains([string]$status.phase)) {
            $phases.Add([string]$status.phase)
        }
        if ([bool]$status.terminal -or [string]$status.status -ne "running") {
            return @{
                Status = $status
                Phases = @($phases)
            }
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    try {
        Invoke-UeCapability "production.job.cancel" @{ jobId = $JobId } | Out-Null
    }
    catch {
        Write-Warning "Could not request cancellation for timed-out job $JobId`: $($_.Exception.Message)"
    }
    throw "Trace job $JobId did not reach a terminal state in $TimeoutSeconds seconds."
}

function Get-ProviderMap {
    param([object[]] $Providers)
    $map = @{}
    foreach ($provider in @($Providers)) {
        $map[[string]$provider.id] = $provider
    }
    return $map
}

function Assert-NoOwnedProcess {
    param(
        [string] $Executable,
        [int] $ProcessId
    )
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $owned = @(Get-Process -Id $ProcessId -ErrorAction SilentlyContinue | Where-Object {
            try {
                [IO.Path]::GetFullPath($_.Path) -eq [IO.Path]::GetFullPath($Executable)
            }
            catch {
                $false
            }
        })
        if ($owned.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "AI-owned Development process $ProcessId remained alive after the job completed."
}

function Assert-NoExecutableProcesses {
    param([string] $Executable)

    $expected = [IO.Path]::GetFullPath($Executable)
    $deadline = [DateTime]::UtcNow.AddSeconds(35)
    do {
        $remaining = @(Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($expected)) `
            -ErrorAction SilentlyContinue | Where-Object {
                try {
                    [IO.Path]::GetFullPath($_.Path) -eq $expected
                }
                catch {
                    $false
                }
            })
        if ($remaining.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Development fixture left process(es) alive for $expected`: $(@($remaining.Id) -join ',')."
}

$script:ResolvedCliPath = (Resolve-Path -LiteralPath $CliPath).Path
$resolvedWorkerPath = (Resolve-Path -LiteralPath $WorkerPath).Path
$resolvedContractRoot = (Resolve-Path -LiteralPath $ContractRoot).Path
$resolvedProject = (Resolve-Path -LiteralPath $Project).Path
$resolvedWorkerStore = (Resolve-Path -LiteralPath $WorkerStore).Path
if ($TraceRoots.Count -eq 0) {
    $TraceRoots = @((Split-Path -Parent (Split-Path -Parent $resolvedProject)))
}
$resolvedTraceRoots = @($TraceRoots | ForEach-Object {
    (Resolve-Path -LiteralPath $_).Path
})

$previousEnvironment = @{
    UEAI_TRACE_WORKER = [Environment]::GetEnvironmentVariable("UEAI_TRACE_WORKER", "Process")
    UEAI_TRACE_CONTRACT_ROOT = [Environment]::GetEnvironmentVariable("UEAI_TRACE_CONTRACT_ROOT", "Process")
    UEAI_TRACE_STORE = [Environment]::GetEnvironmentVariable("UEAI_TRACE_STORE", "Process")
    UEAI_TRACE_ROOTS = [Environment]::GetEnvironmentVariable("UEAI_TRACE_ROOTS", "Process")
}

try {
    $env:UEAI_TRACE_WORKER = $resolvedWorkerPath
    $env:UEAI_TRACE_CONTRACT_ROOT = $resolvedContractRoot
    $env:UEAI_TRACE_STORE = $resolvedWorkerStore
    $env:UEAI_TRACE_ROOTS = [string]::Join(";", $resolvedTraceRoots)

    $doctor = Invoke-UeCapability "production.trace.channel.list" @{ backend = "local" }
    Assert-Condition ($doctor.data.presets.Count -ge 3) `
        "Trace channel discovery did not return the expected presets."

    $evidence = [ordered]@{
        schema = "ue.trace-development-e2e.v1"
        generatedAtUtc = [DateTime]::UtcNow.ToString("o")
        cliPath = $script:ResolvedCliPath
        cliSha256 = (Get-FileHash -LiteralPath $script:ResolvedCliPath -Algorithm SHA256).Hash
        workerPath = $resolvedWorkerPath
        workerSha256 = (Get-FileHash -LiteralPath $resolvedWorkerPath -Algorithm SHA256).Hash
        project = $resolvedProject
        presets = @()
    }
	$pidReuseJobId = ""

    foreach ($preset in $Presets) {
        $planParams = @{
            backend = "local"
            launchProfileId = $LaunchProfileId
            project = $resolvedProject
            map = $Map
            preset = $preset
            maxDurationSeconds = $MaxDurationSeconds
            maxFileSizeMiB = $MaxFileSizeMiB
            postStop = "analyzeSummary"
        }
        $planEnvelope = Invoke-UeCapability "production.trace.launch.plan" $planParams
        $plan = $planEnvelope.data
        Assert-Condition ([bool]$plan.executionReady) `
            "Launch plan for $preset was not execution-ready."
        Assert-Condition ((Get-FileHash -LiteralPath $plan.executable -Algorithm SHA256).Hash.ToLowerInvariant() -eq ([string]$plan.executableSha256).ToLowerInvariant()) `
            "Launch plan executable hash did not match for $preset."
        Assert-Condition ((Get-FileHash -LiteralPath $resolvedProject -Algorithm SHA256).Hash.ToLowerInvariant() -eq ([string]$plan.projectSha256).ToLowerInvariant()) `
            "Launch plan project hash did not match for $preset."
        $fixedArguments = @($plan.fixedArguments | ForEach-Object { [string]$_ })
        if ($preset -eq "fullInsights") {
            Assert-Condition ($fixedArguments -contains "-trace=memory") `
                "fullInsights did not approve the required process-startup -trace=memory argument."
			Assert-Condition ($fixedArguments -contains "-NetTrace=1") `
				"fullInsights did not approve the required process-startup -NetTrace=1 argument."
        }
        else {
            Assert-Condition (-not ($fixedArguments -contains "-trace=memory")) `
                "$preset unexpectedly enabled allocation tracing at process startup."
			Assert-Condition (-not ($fixedArguments -contains "-NetTrace=1")) `
				"$preset unexpectedly enabled Network trace verbosity."
        }

        $startParams = @{
            backend = "local"
            target = @{
                kind = "development"
                launchProfileId = $LaunchProfileId
                map = $Map
            }
            project = $resolvedProject
            preset = $preset
            maxDurationSeconds = $MaxDurationSeconds
            maxFileSizeMiB = $MaxFileSizeMiB
            postStop = "analyzeSummary"
            approvePlanDigest = [string]$plan.approvePlanDigest
            confirmLaunch = $true
        }
        $startEnvelope = Invoke-UeCapability "production.trace.start" $startParams
        $jobId = if ($null -ne $startEnvelope.data.traceId) {
            [string]$startEnvelope.data.traceId
        }
        else {
            [string]$startEnvelope.data.jobId
        }
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($jobId)) `
            "Trace start for $preset returned no job ID."

        $wait = Wait-TraceJob $jobId $JobTimeoutSeconds
        $resultEnvelope = Invoke-UeCapability `
            "production.job.result.get" `
            @{ jobId = $jobId }
        $result = $resultEnvelope.data
        Assert-Condition ([bool]$result.terminal) `
            "Trace job $jobId did not publish terminal=true."
        Assert-Condition ([string]$result.status -eq "succeeded") `
            "Trace job $jobId failed: $($result.errorCode) $($result.message)"
        $expectedPhaseHistory = @(
            "launching",
            "loading",
            "recording",
            "finalizing",
            "analyzing",
            "completed"
        )
        $actualPhaseHistory = @($result.phaseHistory | ForEach-Object { [string]$_ })
        Assert-Condition ($actualPhaseHistory.Count -eq $expectedPhaseHistory.Count) `
            "Trace job $jobId returned an incomplete persisted phaseHistory: $($actualPhaseHistory -join ' -> ')"
        for ($phaseIndex = 0; $phaseIndex -lt $expectedPhaseHistory.Count; ++$phaseIndex) {
            Assert-Condition ($actualPhaseHistory[$phaseIndex] -eq $expectedPhaseHistory[$phaseIndex]) `
                "Trace job $jobId phaseHistory was not canonical at index $phaseIndex`: expected=$($expectedPhaseHistory[$phaseIndex]) actual=$($actualPhaseHistory[$phaseIndex]) history=$($actualPhaseHistory -join ' -> ')"
        }
        Assert-Condition ([string]$result.runtimeReceipt.status -eq "succeeded") `
            "Runtime receipt for $jobId did not succeed."
        Assert-Condition ([string]$result.runtimeReceipt.traceFinalizationStatus -eq "completed") `
            "Runtime receipt for $jobId did not confirm writer finalization."
        Assert-Condition (-not [bool]$result.runtimeReceipt.partial) `
            "Runtime receipt for $jobId was partial."
        Assert-Condition ([string]$result.runtimeReceipt.launchPlanDigest -eq [string]$plan.approvePlanDigest) `
            "Runtime receipt for $jobId did not bind the approved plan digest."

        $tracePath = [string]$result.traceArtifact.path
        $logPath = [string]$result.logArtifact.path
        $receiptArtifact = @($result.artifacts | Where-Object { $_.artifactId -eq "receipt" }) | Select-Object -First 1
        $receiptPath = [string]$receiptArtifact.path
        foreach ($requiredPath in @($tracePath, $logPath, $receiptPath)) {
            Assert-Condition (Test-Path -LiteralPath $requiredPath -PathType Leaf) `
                "Required artifact was missing: $requiredPath"
        }
        Assert-Condition ((Get-Item -LiteralPath $tracePath).Length -gt 0) `
            "Trace artifact for $jobId was empty."
        $logText = Get-Content -LiteralPath $logPath -Raw
        Assert-Condition ($logText -notmatch "WriteError|ERROR_INVALID_HANDLE") `
            "Development log for $jobId contains an invalid-handle/write error."
        Assert-NoOwnedProcess ([string]$plan.executable) ([int]$result.processId)
		Assert-NoExecutableProcesses ([string]$plan.executable)

        $analysisTraceId = [string]$result.artifactTraceId
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($analysisTraceId)) `
            "Trace job $jobId did not register an offline analysis trace ID."
        $providerEnvelope = Invoke-UeCapability `
            "production.trace.provider.list" `
            @{ backend = "local"; traceId = $analysisTraceId }
        $providerMap = Get-ProviderMap @($providerEnvelope.data.providers)
        foreach ($requiredProvider in @("timing", "bookmark", "region")) {
            Assert-Condition ($providerMap.ContainsKey($requiredProvider) -and [bool]$providerMap[$requiredProvider].recorded) `
                "$preset trace did not record required provider '$requiredProvider'."
        }
        if ($preset -eq "standard" -or $preset -eq "fullInsights") {
            foreach ($requiredProvider in @("counter", "log")) {
                Assert-Condition ($providerMap.ContainsKey($requiredProvider) -and [bool]$providerMap[$requiredProvider].recorded) `
                    "$preset trace did not record required provider '$requiredProvider'."
            }
        }
        if ($preset -eq "fullInsights") {
            foreach ($requiredProvider in @("memory", "loading", "network", "io", "tasks")) {
                Assert-Condition ($providerMap.ContainsKey($requiredProvider) -and [bool]$providerMap[$requiredProvider].recorded) `
                    "fullInsights trace did not record required provider '$requiredProvider'."
            }
        }

        $timingEnvelope = Invoke-UeCapability `
            "production.trace.timing.query" `
            @{ backend = "local"; traceId = $analysisTraceId; operation = "frames"; limit = 10 }
        Assert-Condition ([int64]$timingEnvelope.data.query.total -gt 0) `
            "$preset trace contained no frame rows."
        $regionEnvelope = Invoke-UeCapability `
            "production.trace.region.query" `
            @{ backend = "local"; traceId = $analysisTraceId; operation = "list"; filter = "UEAI.Trace.$jobId"; limit = 10 }
        $regionRows = @($regionEnvelope.data.query.rows)
        Assert-Condition ($regionRows.Count -eq 1) `
            "$preset trace did not contain exactly one managed region for $jobId."
        Assert-Condition ([string]$regionRows[0].name -eq "UEAI.Trace.$jobId") `
            "$preset trace region name did not match the job ID."
        Assert-Condition ([double]$regionRows[0].durationMs -gt 0.0) `
            "$preset managed region had no positive duration."

        $gpuTimerCount = 0
        $networkConnectionCount = 0
		$networkPacketCount = 0
		$networkContentEventCount = 0
		$networkNamedConnectionCount = 0
        $loadingPackageCount = 0
        if ($preset -eq "fullInsights") {
            $recordedTimingChannels = @($providerMap["timing"].recordedChannels |
                ForEach-Object { [string]$_ })
            Assert-Condition ($recordedTimingChannels -contains "Gpu") `
                "fullInsights did not record the GPU Timing channel. reason=$($providerMap['timing'].unavailableReason)"
            $gpuTimerEnvelope = Invoke-UeCapability `
                "production.trace.timing.query" `
                @{ backend = "local"; traceId = $analysisTraceId; operation = "timers"; includeGpu = $true; limit = 1000 }
            $gpuTimerRows = @($gpuTimerEnvelope.data.query.rows |
                Where-Object { [bool]$_.gpu })
            $gpuTimerCount = $gpuTimerRows.Count
            Assert-Condition ($gpuTimerCount -gt 0) `
                "fullInsights recorded no GPU timer rows despite enabling the GPU channel."

            $networkEnvelope = Invoke-UeCapability `
                "production.trace.network.query" `
                @{ backend = "local"; traceId = $analysisTraceId; operation = "connections"; limit = 100 }
            $networkConnectionCount = [int64]$networkEnvelope.data.query.total
            Assert-Condition ($networkConnectionCount -gt 0) `
                "fullInsights recorded no Loopback Network connection evidence. reason=$($providerMap['network'].unavailableReason)"
			$networkConnectionRows = @($networkEnvelope.data.query.rows)
			$networkNamedConnectionCount = @($networkConnectionRows |
				Where-Object {
					-not [string]::IsNullOrWhiteSpace([string]$_.address) -and
					-not [string]::IsNullOrWhiteSpace([string]$_.name) -and
					[string]$_.gameInstance -ne "<unnamed>" -and
					[string]$_.name -ne "<unnamed>" -and
					[string]$_.name -ne "None"
				}).Count
			Assert-Condition ($networkNamedConnectionCount -gt 0) `
				"fullInsights Network connection evidence lacked post-recording instance/name/address metadata."

			$networkPacketEnvelope = Invoke-UeCapability `
				"production.trace.network.query" `
				@{ backend = "local"; traceId = $analysisTraceId; operation = "packets"; limit = 100 }
			$networkPacketCount = [int64]$networkPacketEnvelope.data.query.total
			Assert-Condition ($networkPacketCount -gt 0) `
				"fullInsights recorded no Loopback Network packet evidence."

			$networkContentEnvelope = Invoke-UeCapability `
				"production.trace.network.query" `
				@{ backend = "local"; traceId = $analysisTraceId; operation = "contentEvents"; limit = 100 }
			$networkContentEventCount = [int64]$networkContentEnvelope.data.query.total
			Assert-Condition ($networkContentEventCount -gt 0) `
				"fullInsights recorded no Loopback Network content-event evidence."
			$namedContentEvents = @($networkContentEnvelope.data.query.rows |
				Where-Object {
					[string]$_.name -ne "<unknown>" -and
					[string]$_.name -ne "<unnamed>"
				}).Count
			Assert-Condition ($namedContentEvents -gt 0) `
				"fullInsights Network content evidence contained only unknown names."

            $loadingEnvelope = Invoke-UeCapability `
                "production.trace.loading.query" `
                @{ backend = "local"; traceId = $analysisTraceId; operation = "packages"; limit = 100 }
            $loadingPackageCount = [int64]$loadingEnvelope.data.query.total
            Assert-Condition ($loadingPackageCount -gt 0) `
                "fullInsights recorded no Asset Loading package evidence. reason=$($providerMap['loading'].unavailableReason)"
        }

        $presetEvidence = [ordered]@{
            preset = $preset
            jobId = $jobId
            planDigest = [string]$plan.approvePlanDigest
            executable = [string]$plan.executable
            executableSha256 = [string]$plan.executableSha256
            projectSha256 = [string]$plan.projectSha256
            fixedArguments = $fixedArguments
            phases = $actualPhaseHistory
            sampledPhases = @($wait.Phases)
            traceId = $analysisTraceId
            tracePath = $tracePath
            traceSha256 = (Get-FileHash -LiteralPath $tracePath -Algorithm SHA256).Hash
            traceSizeBytes = (Get-Item -LiteralPath $tracePath).Length
            logPath = $logPath
            logSha256 = (Get-FileHash -LiteralPath $logPath -Algorithm SHA256).Hash
            receiptPath = $receiptPath
            receiptSha256 = (Get-FileHash -LiteralPath $receiptPath -Algorithm SHA256).Hash
            processId = [int]$result.processId
            recordedProviders = @($providerMap.Keys | Where-Object { [bool]$providerMap[$_].recorded } | Sort-Object)
            regionDurationMs = [double]$regionRows[0].durationMs
            gpuTimerCount = $gpuTimerCount
            networkConnectionCount = $networkConnectionCount
			networkNamedConnectionCount = $networkNamedConnectionCount
			networkPacketCount = $networkPacketCount
			networkContentEventCount = $networkContentEventCount
            loadingPackageCount = $loadingPackageCount
        }
        $evidence.presets += $presetEvidence
		$pidReuseJobId = $jobId
    }

	# A stale durable PID must never make the Worker wait on or terminate an
	# unrelated process after Windows reuses the numeric PID. Rebind a completed
	# job record to a test-owned ping process while preserving the matching
	# terminal runtime receipt and approved launch identity. The Worker must use
	# creation/path/hash identity, accept the receipt as owned-process exit
	# evidence, and leave the decoy alive.
	Assert-Condition (-not [string]::IsNullOrWhiteSpace($pidReuseJobId)) `
		"PID reuse safety requires at least one completed Development Trace job."
	$pidReuseJobPath = Join-Path $resolvedWorkerStore `
		("launch-jobs\{0}.json" -f $pidReuseJobId)
	Assert-Condition (Test-Path -LiteralPath $pidReuseJobPath -PathType Leaf) `
		"PID reuse safety could not find the persisted launch job $pidReuseJobId."
	$originalPidReuseJob = [IO.File]::ReadAllBytes($pidReuseJobPath)
	$decoyProcess = $null
	try {
		$pingPath = Join-Path $env:SystemRoot "System32\PING.EXE"
		$decoyStartInfo = [Diagnostics.ProcessStartInfo]::new()
		$decoyStartInfo.FileName = $pingPath
		$decoyStartInfo.Arguments = "-t 127.0.0.1"
		$decoyStartInfo.UseShellExecute = $false
		$decoyStartInfo.CreateNoWindow = $true
		$decoyStartInfo.RedirectStandardOutput = $true
		$decoyStartInfo.RedirectStandardError = $true
		$decoyProcess = [Diagnostics.Process]::new()
		$decoyProcess.StartInfo = $decoyStartInfo
		Assert-Condition ($decoyProcess.Start()) `
			"PID reuse safety could not start its bounded decoy process."

		$pidReuseRecord = Get-Content -LiteralPath $pidReuseJobPath -Raw |
			ConvertFrom-Json
		$pidReuseRecord.status = "running"
		$pidReuseRecord.phase = "finalizing"
		$pidReuseRecord.phaseHistory = @(
			"launching", "loading", "recording", "finalizing")
		$pidReuseRecord.processId = [int]$decoyProcess.Id
		$pidReuseRecord.processIdentityStatus = "verified"
		$pidReuseRecord.errorCode = ""
		$pidReuseRecord.errorMessage = ""
		$pidReuseRecord.partial = $false
		[IO.File]::WriteAllText(
			$pidReuseJobPath,
			($pidReuseRecord | ConvertTo-Json -Depth 64),
			[Text.UTF8Encoding]::new($false))

		$pidReuseEnvelope = Invoke-UeCapability `
			"production.job.status" `
			@{ jobId = $pidReuseJobId }
		$pidReuseStatus = $pidReuseEnvelope.data
		$decoyProcess.Refresh()
		Assert-Condition (-not $decoyProcess.HasExited) `
			"The Worker terminated or waited away an unrelated PID-reuse decoy."
		Assert-Condition ([string]$pidReuseStatus.status -eq "succeeded") `
			"A matching terminal receipt did not finalize safely after PID reuse: $($pidReuseStatus.errorCode) $($pidReuseStatus.message)"
		Assert-Condition ([string]$pidReuseStatus.processIdentityStatus -eq `
			"verifiedExitedPidReused") `
			"The Worker did not publish PID-reuse-safe exit evidence."
		$evidence.pidReuseSafety = [ordered]@{
			jobId = $pidReuseJobId
			decoyExecutable = $pingPath
			decoyProcessId = [int]$decoyProcess.Id
			processIdentityStatus = [string]$pidReuseStatus.processIdentityStatus
			decoySurvivedWorkerStatus = $true
		}
	}
	finally {
		[IO.File]::WriteAllBytes($pidReuseJobPath, $originalPidReuseJob)
		if ($null -ne $decoyProcess) {
			$decoyProcess.Refresh()
			if (-not $decoyProcess.HasExited) {
				$decoyProcess.Kill()
				$decoyProcess.WaitForExit(5000) | Out-Null
			}
			$decoyProcess.Dispose()
		}
	}

    if ([string]::IsNullOrWhiteSpace($EvidencePath)) {
        $EvidencePath = Join-Path $resolvedWorkerStore `
            ("development-trace-e2e-{0}.json" -f [DateTime]::UtcNow.ToString("yyyyMMdd-HHmmss"))
    }
    $evidenceDirectory = Split-Path -Parent $EvidencePath
    if (-not [string]::IsNullOrWhiteSpace($evidenceDirectory)) {
        New-Item -ItemType Directory -Path $evidenceDirectory -Force | Out-Null
    }
    $evidence | ConvertTo-Json -Depth 64 | Set-Content `
        -LiteralPath $EvidencePath `
        -Encoding UTF8
    Write-Output "Development Trace E2E passed: $EvidencePath"
}
finally {
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable(
            $name,
            $previousEnvironment[$name],
            "Process")
    }
}
