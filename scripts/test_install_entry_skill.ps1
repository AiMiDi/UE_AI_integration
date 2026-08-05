[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Installer,
    [Parameter(Mandatory = $true)]
    [string]$SourceRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-Condition {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Read-JsonOutput {
    param([object[]]$Output)
    return ($Output | Out-String | ConvertFrom-Json)
}

function Get-TreeDigest {
    param([string]$Root)
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    return @(
        Get-ChildItem -LiteralPath $Root -Recurse -File -Force |
            Sort-Object FullName |
            ForEach-Object {
                $full = [IO.Path]::GetFullPath($_.FullName)
                [ordered]@{
                    path = $full.Substring($prefix.Length).Replace('\', '/')
                    sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
                }
            }
    ) | ConvertTo-Json -Depth 3 -Compress
}

$installerPath = [IO.Path]::GetFullPath($Installer)
$sourceSkill = Join-Path ([IO.Path]::GetFullPath($SourceRoot)) 'skills/ue-ai'
Assert-Condition (Test-Path -LiteralPath $installerPath -PathType Leaf) "Entry Skill installer is missing: $installerPath"
Assert-Condition (Test-Path -LiteralPath (Join-Path $sourceSkill 'SKILL.md') -PathType Leaf) "Entry Skill source is missing: $sourceSkill"

$tempRoot = [IO.Path]::GetFullPath((Join-Path -Path ([IO.Path]::GetTempPath()) -ChildPath ('ueai-entry-skill-test-' + [Guid]::NewGuid().ToString('N'))))
$tempPrefix = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
Assert-Condition ($tempRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) 'Entry Skill test root escaped the temporary directory.'

try {
    $first = Read-JsonOutput @(& $installerPath -Client codex -TargetRoot $tempRoot -Json)
    Assert-Condition ($first.ok -eq $true) 'First entry Skill install failed.'
    Assert-Condition ($first.results[0].status -eq 'installed') 'First entry Skill install must report installed.'
    $destination = Join-Path $tempRoot 'skills/ue-ai'
    Assert-Condition ((Get-TreeDigest $destination) -eq (Get-TreeDigest $sourceSkill)) 'Installed entry Skill differs from its source.'

    $second = Read-JsonOutput @(& $installerPath -Client codex -TargetRoot $tempRoot -Json)
    Assert-Condition ($second.results[0].status -eq 'unchanged') 'Repeated entry Skill install must be idempotent.'

    [IO.File]::AppendAllText((Join-Path $destination 'SKILL.md'), "`nlocal override`n", [Text.UTF8Encoding]::new($false))
    $blocked = $false
    try { & $installerPath -Client codex -TargetRoot $tempRoot -Json | Out-Null }
    catch { $blocked = $true }
    Assert-Condition $blocked 'A differing local entry Skill must not be replaced without -Force.'

    $forced = Read-JsonOutput @(& $installerPath -Client codex -TargetRoot $tempRoot -Force -Json)
    Assert-Condition ($forced.results[0].status -eq 'installed') 'Forced entry Skill install failed.'
    Assert-Condition ([bool]$forced.results[0].backup) 'Forced entry Skill install must preserve a backup.'
    Assert-Condition (Test-Path -LiteralPath ([string]$forced.results[0].backup) -PathType Container) 'Entry Skill backup is missing.'
    Assert-Condition ((Get-TreeDigest $destination) -eq (Get-TreeDigest $sourceSkill)) 'Forced entry Skill install did not activate the source tree.'

    [ordered]@{
        ok = $true
        schema = 'ue.entry-skill-install-test.v1'
        installed = $true
        idempotent = $true
        conflictBlocked = $true
        backupVerified = $true
    } | ConvertTo-Json -Compress
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Assert-Condition ($tempRoot.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase)) 'Unsafe entry Skill test cleanup target.'
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
