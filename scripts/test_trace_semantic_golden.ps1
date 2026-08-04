[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $WorkerPath,

    [Parameter(Mandatory = $true)]
    [string] $UnrealEditorCmd,

    [Parameter(Mandatory = $true)]
    [string] $Project,

    [ValidateRange(30, 900)]
    [int] $TimeoutSeconds = 240,

    [switch] $KeepArtifacts
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingFile([string] $Path, [string] $Label) {
    $Resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if (-not (Test-Path -LiteralPath $Resolved.Path -PathType Leaf)) {
        throw "$Label is not a file: $Path"
    }
    return [System.IO.Path]::GetFullPath($Resolved.Path)
}

function Assert-Condition([bool] $Condition, [string] $Message) {
    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
}

$WorkerPath = Resolve-ExistingFile $WorkerPath 'Trace Worker'
$UnrealEditorCmd = Resolve-ExistingFile $UnrealEditorCmd 'UnrealEditor-Cmd'
$Project = Resolve-ExistingFile $Project 'Automation project'

$TemporaryBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$RunRoot = [System.IO.Path]::GetFullPath((Join-Path $TemporaryBase (
    'UEAITraceSemanticGolden-' + [guid]::NewGuid().ToString('N'))))
$RunName = [System.IO.Path]::GetFileName($RunRoot)
if (-not $RunRoot.StartsWith(
        $TemporaryBase,
        [System.StringComparison]::OrdinalIgnoreCase) -or
    -not $RunName.StartsWith(
        'UEAITraceSemanticGolden-',
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use unsafe Trace semantic golden root: $RunRoot"
}

$AllowedRoot = Join-Path $RunRoot 'allowed'
$StoreRoot = Join-Path $RunRoot 'store'
$ReportRoot = Join-Path $RunRoot 'report'
$TracePath = Join-Path $AllowedRoot 'semantic-golden.utrace'
$ExpectedPath = Join-Path $RunRoot 'worker-expected.json'
$ActualPath = Join-Path $RunRoot 'editor-actual.json'
$LogPath = Join-Path $RunRoot 'editor.log'
$Succeeded = $false

function Invoke-WorkerRequest(
    [System.Collections.IDictionary] $Request,
    [System.Collections.IDictionary] $Environment) {
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $WorkerPath
    $StartInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($WorkerPath)
    $StartInfo.Arguments = '--stdio'
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardInput = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true
    foreach ($Entry in $Environment.GetEnumerator()) {
        $StartInfo.EnvironmentVariables[[string] $Entry.Key] = [string] $Entry.Value
    }

    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    if (-not $Process.Start()) {
        throw 'Trace Worker process did not start.'
    }
    try {
        $Payload = $Request | ConvertTo-Json -Depth 20 -Compress
        $Process.StandardInput.Write($Payload)
        $Process.StandardInput.Close()
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit(60000)) {
            $Process.Kill()
            throw 'Trace Worker semantic request timed out.'
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

function New-DiagnosticFixture([string] $Destination) {
    $StartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $WorkerPath
    $StartInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($WorkerPath)
    $StartInfo.Arguments = "--generate-fixture=`"$Destination`""
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.RedirectStandardOutput = $true
    $StartInfo.RedirectStandardError = $true

    $Process = [System.Diagnostics.Process]::new()
    $Process.StartInfo = $StartInfo
    if (-not $Process.Start()) {
        throw 'Trace Worker fixture process did not start.'
    }
    try {
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit(60000)) {
            $Process.Kill()
            throw 'Trace Worker fixture generation timed out.'
        }
        $Stdout = $StdoutTask.Result
        $Stderr = $StderrTask.Result
        Assert-Condition ($Process.ExitCode -eq 0) `
            "Trace fixture generation failed: $Stderr"
        Assert-Condition (-not [string]::IsNullOrWhiteSpace($Stdout)) `
            'Trace fixture generation returned no receipt envelope.'
        $Envelope = $Stdout.Trim() | ConvertFrom-Json
        Assert-Condition ($Envelope.ok -and
            (Test-Path -LiteralPath $Destination -PathType Leaf)) `
            'Trace fixture generation did not create an analyzable .utrace.'
        return $Envelope
    }
    finally {
        $Process.Dispose()
    }
}

function Copy-Dictionary([System.Collections.IDictionary] $Source) {
    $Copy = [ordered]@{}
    foreach ($Entry in $Source.GetEnumerator()) {
        $Copy[[string] $Entry.Key] = $Entry.Value
    }
    return $Copy
}

function New-GoldenVector(
    [string] $Id,
    [string] $Capability,
    [System.Collections.IDictionary] $WorkerParams,
    [string] $CoreProvider,
    [System.Collections.IDictionary] $CoreParams,
    [string] $TraceId,
    [System.Collections.IDictionary] $Environment) {
    $BoundWorkerParams = Copy-Dictionary $WorkerParams
    $BoundWorkerParams.traceId = $TraceId
    $Response = Invoke-WorkerRequest -Environment $Environment -Request ([ordered]@{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = 'golden-' + [guid]::NewGuid().ToString('N')
        capability = $Capability
        params = $BoundWorkerParams
    })
    Assert-Condition ($Response.Json.schema -eq 'ue.trace-worker-response.v1') `
		"$Id Worker request returned no protocol response: $($Response.Stderr)"
    Assert-Condition (
		($Response.Json.ok -and $Response.ExitCode -eq 0) -or
		(-not $Response.Json.ok -and $Response.ExitCode -ne 0)) `
		"$Id Worker process status disagrees with its response envelope."

    if ($Response.Json.ok) {
        $Expected = [ordered]@{
            ok = $true
            query = $Response.Json.data.query
        }
    }
    else {
        Assert-Condition (-not [string]::IsNullOrWhiteSpace(
            [string] $Response.Json.error.code)) `
            "$Id Worker failure has no stable error code."
        $Expected = [ordered]@{
            ok = $false
            errorCode = [string] $Response.Json.error.code
        }
    }

    return [ordered]@{
        id = $Id
        workerRequest = [ordered]@{
            capability = $Capability
            params = $WorkerParams
        }
        coreRequest = [ordered]@{
            provider = $CoreProvider
            params = $CoreParams
        }
        expected = $Expected
    }
}

try {
    New-Item -ItemType Directory -Path @(
        $AllowedRoot,
        $StoreRoot,
        $ReportRoot) -Force | Out-Null

    $Environment = [ordered]@{
        UEAI_TRACE_STORE = $StoreRoot
        UEAI_TRACE_ROOTS = $AllowedRoot
    }
    $FixtureEnvelope = New-DiagnosticFixture $TracePath
    $ReceiptPath = [string] $FixtureEnvelope.receiptPath
    Assert-Condition (Test-Path -LiteralPath $ReceiptPath -PathType Leaf) `
        'Trace fixture receipt is missing.'
    $FixtureReceipt = Get-Content -LiteralPath $ReceiptPath -Raw |
        ConvertFrom-Json
    Assert-Condition (
        [string] $FixtureReceipt.managedEngineMarker -match
            '^UEAI_TRACE_ENGINE_VERSION=\d+\.\d+$') `
        'Trace fixture has no exact managed Engine marker.'

    $ImportResponse = Invoke-WorkerRequest -Environment $Environment -Request ([ordered]@{
        schema = 'ue.trace-worker-request.v1'
        action = 'execute'
        requestId = 'golden-import-' + [guid]::NewGuid().ToString('N')
        capability = 'production.trace.import'
        params = [ordered]@{
            tracePath = $TracePath
            copyMode = 'reference'
        }
    })
    Assert-Condition ($ImportResponse.ExitCode -eq 0 -and $ImportResponse.Json.ok) `
        "Trace fixture import failed: $($ImportResponse.Stderr)"
    $TraceId = [string] $ImportResponse.Json.data.traceId
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($TraceId)) `
        'Trace fixture import returned no traceId.'

    $Vectors = @()
    $TimingBase = [ordered]@{
        operation = 'events'
        filter = 'UEAITraceDiagnosticFixture'
        startTimeSeconds = 0.0
        endTimeSeconds = 60.0
        limit = 1
    }
    foreach ($Page in @(
        @{ Id = 'timing.events.page0'; Cursor = '0' },
        @{ Id = 'timing.events.page1'; Cursor = '1' },
        @{ Id = 'timing.events.maxCursor'; Cursor = '18446744073709551615' })) {
        $WorkerParams = Copy-Dictionary $TimingBase
        $WorkerParams.cursor = $Page.Cursor
        $CoreParams = Copy-Dictionary $WorkerParams
        $Vectors += New-GoldenVector $Page.Id `
            'production.trace.timing.query' $WorkerParams `
            'timing' $CoreParams $TraceId $Environment
    }

    $WorkerThreadParams = [ordered]@{
        operation = 'threads'
        startTimeSeconds = 0.0
        endTimeSeconds = 60.0
        cursor = '0'
        limit = 64
    }
    $CoreThreadParams = Copy-Dictionary $WorkerThreadParams
    $CoreThreadParams.operation = 'list'
    $Vectors += New-GoldenVector 'timing.threads.range' `
        'production.trace.timing.query' $WorkerThreadParams `
        'threads' $CoreThreadParams $TraceId $Environment

    foreach ($Definition in @(
        @{
            Id = 'counter.series.fixture'
            Capability = 'production.trace.counter.query'
            Provider = 'counter'
            Params = [ordered]@{
                operation = 'series'; filter = 'UEAI Trace Fixture Counter'
                startTimeSeconds = 0.0; endTimeSeconds = 60.0
                cursor = '0'; limit = 16
            }
        },
        @{
            Id = 'log.messages.fixture'
            Capability = 'production.trace.log.query'
            Provider = 'log'
            Params = [ordered]@{
                operation = 'messages'; filter = 'UEAI Trace diagnostic fixture'
                startTimeSeconds = 0.0; endTimeSeconds = 60.0
                cursor = '0'; limit = 16
            }
        },
        @{
            Id = 'bookmark.list.fixture'
            Capability = 'production.trace.bookmark.query'
            Provider = 'bookmark'
            Params = [ordered]@{
                operation = 'list'; filter = 'UEAI_TRACE_FIXTURE'
                startTimeSeconds = 0.0; endTimeSeconds = 60.0
                cursor = '0'; limit = 16
            }
        },
        @{
            Id = 'region.ranges.fixture'
            Capability = 'production.trace.region.query'
            Provider = 'region'
            Params = [ordered]@{
                operation = 'ranges'; filter = 'UEAI.Trace.DiagnosticFixture'
                startTimeSeconds = 0.0; endTimeSeconds = 60.0
                cursor = '0'; limit = 16
            }
        },
        @{
            Id = 'tasks.tasks.fixture'
            Capability = 'production.trace.tasks.query'
            Provider = 'tasks'
            Params = [ordered]@{
                operation = 'tasks'; filter = 'UEAITraceDiagnosticFixture'
                startTimeSeconds = 0.0; endTimeSeconds = 60.0
                cursor = '0'; limit = 16
            }
        },
        @{
            Id = 'io.events.fixture'
            Capability = 'production.trace.io.query'
            Provider = 'io'
            Params = [ordered]@{
                operation = 'events'; filter = 'io-fixture'
                startTimeSeconds = 0.0; endTimeSeconds = 60.0
                cursor = '0'; limit = 16
            }
        },
        @{
            Id = 'screenshot.list.missing'
            Capability = 'production.trace.screenshot.query'
            Provider = 'screenshot'
            Params = [ordered]@{
                operation = 'list'; startTimeSeconds = 0.0
                endTimeSeconds = 60.0; cursor = '0'; limit = 16
            }
        })) {
        $Vectors += New-GoldenVector $Definition.Id `
            $Definition.Capability $Definition.Params `
            $Definition.Provider $Definition.Params $TraceId $Environment
    }

    $ExpectedRoot = [ordered]@{
        schema = 'ue.trace-semantic-cross-runtime-golden.v1'
        fixtureSchema = [string] $FixtureReceipt.schema
        traceId = $TraceId
        vectors = @($Vectors)
    }
    [System.IO.File]::WriteAllText(
        $ExpectedPath,
        ($ExpectedRoot | ConvertTo-Json -Depth 40),
        [System.Text.UTF8Encoding]::new($false))

    $EditorArguments = @(
        "`"$Project`"",
        '-unattended',
        '-nop4',
        '-nosplash',
        '-NoSound',
        '-NullRHI',
        '-stdout',
        '-FullStdOutLogOutput',
        "-ExecCmds=`"Automation RunTests UE_AI_integration.Trace.Core.CrossRuntimeSemanticGolden`"",
        '-TestExit="Automation Test Queue Empty"',
        "-ReportExportPath=`"$ReportRoot`"",
        "-abslog=`"$LogPath`"",
        '-UEAITraceSemanticGolden',
        "-UEAITraceGoldenFixture=`"$TracePath`"",
        "-UEAITraceGoldenExpected=`"$ExpectedPath`"",
        "-UEAITraceGoldenActual=`"$ActualPath`""
    ) -join ' '

    $EditorStartInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $EditorStartInfo.FileName = $UnrealEditorCmd
    $EditorStartInfo.WorkingDirectory = [System.IO.Path]::GetDirectoryName($Project)
    $EditorStartInfo.Arguments = $EditorArguments
    $EditorStartInfo.UseShellExecute = $false
    $EditorStartInfo.CreateNoWindow = $true
    $EditorStartInfo.RedirectStandardOutput = $true
    $EditorStartInfo.RedirectStandardError = $true
    $EditorProcess = [System.Diagnostics.Process]::new()
    $EditorProcess.StartInfo = $EditorStartInfo
    if (-not $EditorProcess.Start()) {
        throw 'UnrealEditor-Cmd did not start for the Trace semantic golden.'
    }
    try {
        $EditorStdoutTask = $EditorProcess.StandardOutput.ReadToEndAsync()
        $EditorStderrTask = $EditorProcess.StandardError.ReadToEndAsync()
        if (-not $EditorProcess.WaitForExit($TimeoutSeconds * 1000)) {
            $EditorProcess.Kill()
            throw "Trace semantic Golden Automation timed out after $TimeoutSeconds seconds."
        }
        $EditorStdout = $EditorStdoutTask.Result
        $EditorStderr = $EditorStderrTask.Result
        Assert-Condition ($EditorProcess.ExitCode -eq 0) `
            "Trace semantic Golden Automation exited with $($EditorProcess.ExitCode). stderr: $EditorStderr"
    }
    finally {
        $EditorProcess.Dispose()
    }

    $ReportIndex = Join-Path $ReportRoot 'index.json'
    Assert-Condition (Test-Path -LiteralPath $ReportIndex -PathType Leaf) `
        "Automation report is missing. stdout: $EditorStdout"
    $Report = Get-Content -LiteralPath $ReportIndex -Raw | ConvertFrom-Json
    $GoldenTest = @($Report.tests | Where-Object {
        $_.fullTestPath -eq
            'UE_AI_integration.Trace.Core.CrossRuntimeSemanticGolden'
    }) | Select-Object -First 1
    Assert-Condition ($null -ne $GoldenTest -and $GoldenTest.state -eq 'Success') `
        'Editor-linked TraceAnalysisCore did not match the Worker golden vectors.'
    Assert-Condition (Test-Path -LiteralPath $ActualPath -PathType Leaf) `
        'Editor-linked TraceAnalysisCore produced no actual golden artifact.'

    $Succeeded = $true
    Write-Host 'Cross-runtime Trace semantic golden passed.'
    Write-Host "Worker expected: $ExpectedPath"
    Write-Host "Editor actual:   $ActualPath"
}
finally {
    if ($Succeeded -and -not $KeepArtifacts) {
        Remove-Item -LiteralPath $RunRoot -Recurse -Force
    }
    elseif (Test-Path -LiteralPath $RunRoot) {
        Write-Warning "Trace semantic golden artifacts retained at $RunRoot"
    }
}
