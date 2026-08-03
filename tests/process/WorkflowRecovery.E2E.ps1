param(
    [Parameter(Mandatory = $true)][string]$Editor,
    [Parameter(Mandatory = $true)][string]$Project,
    [Parameter(Mandatory = $true)][string]$WorkflowFile,
    [ValidateSet('beforeFirstOperation','afterHandlerBeforeCheckpoint','afterCheckpoint','duringFinalSave','afterSuccessBeforeRollback')]
    [string]$FaultPoint = 'afterCheckpoint',
    [switch]$RunAllFaultPoints,
    [switch]$VerifyConflict,
    [int]$Port = 19847,
    [int]$StartupTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$oldPort = [Environment]::GetEnvironmentVariable('UE_PORT', 'Process')
$ownedProcesses = [Collections.Generic.List[Diagnostics.Process]]::new()
$markers = [Collections.Generic.List[string]]::new()

function Stop-OwnedEditor([Diagnostics.Process]$Process) {
    if ($Process -and -not $Process.HasExited) {
        # Only processes created by this script are ever force-terminated. The
        # dedicated test switch never targets an interactive Editor instance.
        Stop-Process -Id $Process.Id -Force
        $Process.WaitForExit()
    }
}

function Start-TestEditor([int]$TestPort, [string]$CurrentFaultPoint, [string]$Marker) {
    [Environment]::SetEnvironmentVariable('UE_PORT', [string]$TestPort, 'Process')
    $arguments = @($Project, '-unattended', '-nop4', '-nosplash', '-NoSound', '-NullRHI')
    if ($CurrentFaultPoint) {
        $arguments += "-UEAIWorkflowFaultPoint=$CurrentFaultPoint"
        $arguments += "-UEAIWorkflowFaultMarker=$Marker"
    }
    $process = Start-Process -FilePath $Editor -ArgumentList $arguments -PassThru -WindowStyle Hidden
    $ownedProcesses.Add($process)
    return $process
}

function Wait-Ready([Diagnostics.Process]$Process, [string]$BaseUrl) {
    $deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) { throw "Editor exited during startup with $($Process.ExitCode)" }
        try {
            $health = Invoke-RestMethod -Uri "$BaseUrl/api/health" -TimeoutSec 2
            if ($health.data.state -eq 'ready') { return }
        } catch { Start-Sleep -Milliseconds 250 }
    }
    throw 'Editor did not become ready within startupTimeoutSeconds.'
}

function Invoke-Workflow([string]$BaseUrl, [hashtable]$Body) {
    $json = $Body | ConvertTo-Json -Depth 100 -Compress
    return Invoke-RestMethod -Method Post -Uri "$BaseUrl/api/v1/workflow" -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($json)) -TimeoutSec 120
}

function Invoke-Capability([string]$BaseUrl, [string]$Capability, [hashtable]$Params) {
    $body = @{
        capability = $Capability
        requestId = [guid]::NewGuid().ToString('D')
        params = $Params
    }
    $json = $body | ConvertTo-Json -Depth 100 -Compress
    return Invoke-RestMethod -Method Post -Uri "$BaseUrl/api/execute" -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($json)) -TimeoutSec 120
}

function Get-WorkflowScopes([object]$Workflow) {
    $scopes = @()
    if ([string]$Workflow.dslVersion -eq '2.0') {
        foreach ($property in $Workflow.scopes.PSObject.Properties) {
            $scopes += $property.Value
        }
    } elseif ($Workflow.scope) {
        $scopes += $Workflow.scope
    }
    return $scopes
}

function Get-PackageFilename([string]$AssetPath) {
    $packagePath = $AssetPath
    $slash = $packagePath.LastIndexOf('/')
    $dot = $packagePath.IndexOf('.', [Math]::Max(0, $slash))
    if ($dot -ge 0) { $packagePath = $packagePath.Substring(0, $dot) }
    if (-not $packagePath.StartsWith('/Game/')) {
        throw "Process recovery fixture only supports project assets: $AssetPath"
    }
    $projectRoot = Split-Path -Parent $Project
    $relative = $packagePath.Substring('/Game/'.Length).Replace('/', [IO.Path]::DirectorySeparatorChar)
    $uasset = Join-Path $projectRoot (Join-Path 'Content' ($relative + '.uasset'))
    $umap = Join-Path $projectRoot (Join-Path 'Content' ($relative + '.umap'))
    if (Test-Path -LiteralPath $umap) { return $umap }
    return $uasset
}

function Capture-Baseline([object]$Workflow) {
    $entries = @()
    foreach ($scope in (Get-WorkflowScopes $Workflow)) {
        $filename = Get-PackageFilename ([string]$scope.asset)
        $exists = Test-Path -LiteralPath $filename
        $entries += [pscustomobject]@{
            asset = [string]$scope.asset
            filename = $filename
            existed = $exists
            sha256 = if ($exists) { (Get-FileHash -Algorithm SHA256 -LiteralPath $filename).Hash } else { $null }
        }
    }
    return $entries
}

function Assert-BaselineRestored([object[]]$Baseline) {
    foreach ($entry in $Baseline) {
        $exists = Test-Path -LiteralPath $entry.filename
        if ($entry.existed -ne $exists) {
            throw "Rollback existence mismatch for $($entry.asset): expected=$($entry.existed), actual=$exists"
        }
        if ($exists) {
            $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $entry.filename).Hash
            if ($actual -ne $entry.sha256) {
                throw "Rollback SHA-256 mismatch for $($entry.asset)."
            }
        }
    }
}

function Get-HttpErrorCode([Management.Automation.ErrorRecord]$Record) {
    $text = [string]$Record.ErrorDetails.Message
    if (-not $text -and $Record.Exception.Response) {
        try {
            $reader = [IO.StreamReader]::new($Record.Exception.Response.GetResponseStream())
            try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
        } catch { }
    }
    if ($text) {
        try { return [string](($text | ConvertFrom-Json).error.code) } catch { }
    }
    return $null
}

function Invoke-FaultRecovery(
    [object]$Workflow,
    [string]$CurrentFaultPoint,
    [int]$TestPort) {
    $baseUrl = "http://127.0.0.1:$TestPort"
    $marker = Join-Path ([IO.Path]::GetTempPath()) ("ue-workflow-fault-" + [guid]::NewGuid().ToString('N') + '.json')
    $markers.Add($marker)
    $baseline = Capture-Baseline $Workflow
    $first = $null
    $second = $null
    try {
        $first = Start-TestEditor $TestPort $CurrentFaultPoint $marker
        Wait-Ready $first $baseUrl
        $plan = Invoke-Workflow $baseUrl @{ action = 'plan'; workflow = $Workflow; detailLevel = 'summary' }
        $digest = $plan.data.planDigest
        if (-not $digest) { throw 'Workflow plan did not return planDigest.' }
        try {
            Invoke-Workflow $baseUrl @{
                action = 'execute'
                workflow = $Workflow
                approvePlanDigest = $digest
                saveOnSuccess = $true
                detailLevel = 'summary'
            } | Out-Null
        } catch {
            # The test-only interruption returns 503 after the marker is durable.
        }
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        while (-not (Test-Path -LiteralPath $marker) -and [DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 100
        }
        if (-not (Test-Path -LiteralPath $marker)) { throw 'Fault marker was not published.' }
        $fault = Get-Content -LiteralPath $marker -Raw -Encoding UTF8 | ConvertFrom-Json
        Stop-OwnedEditor $first

        $second = Start-TestEditor $TestPort $null $null
        Wait-Ready $second $baseUrl
        $status = Invoke-Workflow $baseUrl @{ action = 'status'; runId = $fault.runId; detailLevel = 'summary' }
        if ($status.data.recoveryState -notin @('prepared','checkpointed','completed')) {
            throw "Unexpected recoveryState: $($status.data.recoveryState)"
        }
        $resume = Invoke-Workflow $baseUrl @{ action = 'resume'; runId = $fault.runId; detailLevel = 'summary' }
        if ($resume.data.status -ne 'completed') { throw "Resume did not complete: $($resume.data.status)" }
        $rollback = Invoke-Workflow $baseUrl @{
            action = 'rollback'
            runId = $fault.runId
            approvePlanDigest = $digest
            detailLevel = 'summary'
        }
        if (-not $rollback.data.rollbackVerified) { throw 'Cross-restart rollback was not structurally verified.' }
        Stop-OwnedEditor $second
        Assert-BaselineRestored $baseline
        return [pscustomobject]@{
            ok = $true
            faultPoint = $CurrentFaultPoint
            runId = $fault.runId
            checkpointId = $fault.checkpointId
            resumeMode = $resume.data.resumeMode
            status = $resume.data.status
            rollbackVerified = $rollback.data.rollbackVerified
        }
    } finally {
        Stop-OwnedEditor $first
        Stop-OwnedEditor $second
    }
}

try {
    $workflow = Get-Content -LiteralPath $WorkflowFile -Raw -Encoding UTF8 | ConvertFrom-Json
    $faultPoints = if ($RunAllFaultPoints) {
        @('beforeFirstOperation','afterHandlerBeforeCheckpoint','afterCheckpoint','duringFinalSave','afterSuccessBeforeRollback')
    } else {
        @($FaultPoint)
    }
    $results = @()
    for ($index = 0; $index -lt $faultPoints.Count; ++$index) {
        $results += Invoke-FaultRecovery $workflow $faultPoints[$index] ($Port + $index)
    }

    if ($VerifyConflict) {
        $scope = @(Get-WorkflowScopes $workflow | Where-Object { [string]$_.kind -eq 'blueprint' } | Select-Object -First 1)
        if ($scope.Count -ne 1) { throw 'Conflict verification requires one Blueprint scope.' }
        $testPort = $Port + $faultPoints.Count
        $baseUrl = "http://127.0.0.1:$testPort"
        $baseline = Capture-Baseline $workflow
        $backupRoot = Join-Path ([IO.Path]::GetTempPath()) ('ue-workflow-conflict-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $backupRoot | Out-Null
        foreach ($entry in $baseline) {
            if ($entry.existed) {
                $entry | Add-Member -NotePropertyName backup -NotePropertyValue (Join-Path $backupRoot ([guid]::NewGuid().ToString('N') + [IO.Path]::GetExtension($entry.filename)))
                Copy-Item -LiteralPath $entry.filename -Destination $entry.backup
            }
        }
        $editorProcess = Start-TestEditor $testPort $null $null
        try {
            Wait-Ready $editorProcess $baseUrl
            $plan = Invoke-Workflow $baseUrl @{ action = 'plan'; workflow = $workflow; detailLevel = 'summary' }
            $digest = $plan.data.planDigest
            $execution = Invoke-Workflow $baseUrl @{
                action = 'execute'; workflow = $workflow; approvePlanDigest = $digest
                saveOnSuccess = $true; detailLevel = 'summary'
            }
            $variableName = 'ExternalConflict_' + [guid]::NewGuid().ToString('N').Substring(0, 12)
            Invoke-Capability $baseUrl 'blueprint.variable.add' @{
                blueprint = [string]$scope[0].asset
                variableName = $variableName
                variableType = 'Boolean'
            } | Out-Null
            try {
                Invoke-Workflow $baseUrl @{
                    action = 'rollback'; runId = $execution.data.runId
                    approvePlanDigest = $digest; detailLevel = 'summary'
                } | Out-Null
                throw 'Rollback unexpectedly overwrote an external modification.'
            } catch {
                if ($_.Exception.Message -eq 'Rollback unexpectedly overwrote an external modification.') { throw }
                $errorCode = Get-HttpErrorCode $_
                if ($errorCode -notin @('resume_conflict','rollback_conflict')) {
                    throw "Rollback failed with unexpected error code '$errorCode'."
                }
                $results += [pscustomobject]@{
                    ok = $true
                    check = 'externalModificationConflict'
                    runId = $execution.data.runId
                    rollbackRejected = $true
                    errorCode = $errorCode
                }
            }
        } finally {
            Stop-OwnedEditor $editorProcess
            foreach ($entry in $baseline) {
                if ($entry.existed) {
                    Copy-Item -LiteralPath $entry.backup -Destination $entry.filename -Force
                } elseif (Test-Path -LiteralPath $entry.filename) {
                    Remove-Item -LiteralPath $entry.filename -Force
                }
            }
            Assert-BaselineRestored $baseline
            Remove-Item -LiteralPath $backupRoot -Recurse -Force
        }
    }

    [pscustomobject]@{ ok = $true; results = $results } | ConvertTo-Json -Depth 12
} finally {
    foreach ($process in $ownedProcesses) { Stop-OwnedEditor $process }
    [Environment]::SetEnvironmentVariable('UE_PORT', $oldPort, 'Process')
    foreach ($marker in $markers) {
        if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker -Force }
    }
}
