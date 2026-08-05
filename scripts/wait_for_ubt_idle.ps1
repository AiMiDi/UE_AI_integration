[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $EngineRoot,

    [ValidateRange(1, 3600)]
    [int] $TimeoutSeconds = 600,

    [ValidateRange(0, 60)]
    [int] $StableSeconds = 10
)

$ErrorActionPreference = 'Stop'

$CanonicalEngineRoot = [IO.Path]::GetFullPath(
    (Resolve-Path -LiteralPath $EngineRoot -ErrorAction Stop).Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar)
$DotNetRoot = [IO.Path]::GetFullPath((
        Join-Path $CanonicalEngineRoot 'Engine\Binaries\ThirdParty\DotNet')).TrimEnd(
            [IO.Path]::DirectorySeparatorChar,
            [IO.Path]::AltDirectorySeparatorChar) +
    [IO.Path]::DirectorySeparatorChar
$Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$IdleSince = $null
$LastActiveSignature = ''

while ($true) {
    $Active = @(Get-CimInstance Win32_Process -ErrorAction Stop |
        Where-Object {
            $_.Name -ieq 'dotnet.exe' -and
            [string]$_.CommandLine -like '*UnrealBuildTool.dll*' -and
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath([string]$_.ExecutablePath).StartsWith(
                $DotNetRoot,
                [StringComparison]::OrdinalIgnoreCase)
        } |
        Sort-Object ProcessId)
    $Now = [DateTime]::UtcNow
    if ($Active.Count -eq 0) {
        if ($null -eq $IdleSince) {
            $IdleSince = $Now
            Write-Output "UBT is idle; verifying a $StableSeconds second stable window."
        }
        if (($Now - $IdleSince).TotalSeconds -ge $StableSeconds) {
            [ordered]@{
                schema = 'ue.release-ubt-idle.v1'
                engineRoot = $CanonicalEngineRoot
                stableSeconds = $StableSeconds
                observedAtUtc = $Now.ToString('o')
            } | ConvertTo-Json -Compress
            return
        }
    }
    else {
        $IdleSince = $null
        $Signature = (@($Active.ProcessId) -join ',')
        if ($Signature -ne $LastActiveSignature) {
            Write-Output "Waiting for UnrealBuildTool process(es): $Signature"
            $LastActiveSignature = $Signature
        }
    }
    if ($Now -ge $Deadline) {
        $ProcessSummary = @($Active | ForEach-Object {
                [ordered]@{
                    processId = $_.ProcessId
                    parentProcessId = $_.ParentProcessId
                    commandLine = [string]$_.CommandLine
                }
            })
        throw "Timed out waiting for an idle UnrealBuildTool lane: $($ProcessSummary | ConvertTo-Json -Compress)"
    }
    Start-Sleep -Seconds 1
}
