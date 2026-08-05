[CmdletBinding()]
param(
    [ValidateSet('codex', 'claude', 'both')]
    [string]$Client = 'codex',
    [string]$CodexHome,
    [string]$ClaudeHome,
    [string]$TargetRoot,
    [switch]$Force,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$pluginRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceSkill = Join-Path $pluginRoot 'skills/ue-ai'
if (-not (Test-Path -LiteralPath (Join-Path $sourceSkill 'SKILL.md') -PathType Leaf)) {
    throw "UE AI entry Skill is missing from $sourceSkill"
}
if ($TargetRoot -and $Client -eq 'both') {
    throw '-TargetRoot can be used only with one selected client.'
}

function Resolve-ClientRoot {
    param([string]$Name)

    if ($TargetRoot) { return [IO.Path]::GetFullPath($TargetRoot) }
    $userProfile = [Environment]::GetFolderPath('UserProfile')
    if ($Name -eq 'codex') {
        if ($CodexHome) { return [IO.Path]::GetFullPath($CodexHome) }
        if ($env:CODEX_HOME) { return [IO.Path]::GetFullPath($env:CODEX_HOME) }
        return [IO.Path]::GetFullPath((Join-Path $userProfile '.codex'))
    }
    if ($ClaudeHome) { return [IO.Path]::GetFullPath($ClaudeHome) }
    if ($env:CLAUDE_CONFIG_DIR) { return [IO.Path]::GetFullPath($env:CLAUDE_CONFIG_DIR) }
    return [IO.Path]::GetFullPath((Join-Path $userProfile '.claude'))
}

function Get-SkillTreeDigest {
    param([string]$Root)

    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { return '' }
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    $entries = @(
        Get-ChildItem -LiteralPath $Root -Recurse -File -Force |
            Sort-Object FullName |
            ForEach-Object {
                $full = [IO.Path]::GetFullPath($_.FullName)
                if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
                    throw "Skill file escaped its root: $full"
                }
                [ordered]@{
                    path = $full.Substring($prefix.Length).Replace('\', '/')
                    sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
                }
            }
    )
    return ($entries | ConvertTo-Json -Depth 3 -Compress)
}

function Install-ForClient {
    param([string]$Name)

    $clientRoot = Resolve-ClientRoot $Name
    $skillsRoot = Join-Path $clientRoot 'skills'
    $destination = Join-Path $skillsRoot 'ue-ai'
    New-Item -ItemType Directory -Path $skillsRoot -Force | Out-Null

    $sourceDigest = Get-SkillTreeDigest $sourceSkill
    if ((Test-Path -LiteralPath $destination -PathType Container) -and
        (Get-SkillTreeDigest $destination) -eq $sourceDigest) {
        return [ordered]@{
            client = $Name
            status = 'unchanged'
            destination = $destination
            backup = $null
        }
    }
    if (Test-Path -LiteralPath $destination) {
        if (-not (Test-Path -LiteralPath $destination -PathType Container)) {
            throw "Entry Skill destination is not a directory: $destination"
        }
        if (-not $Force) {
            throw "Entry Skill already exists with different content: $destination. Re-run with -Force to preserve it as a backup and install this version."
        }
    }

    $stage = Join-Path $skillsRoot ('.ue-ai-stage-' + [Guid]::NewGuid().ToString('N'))
    $stageFull = [IO.Path]::GetFullPath($stage)
    $skillsPrefix = [IO.Path]::GetFullPath($skillsRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $stageFull.StartsWith($skillsPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Entry Skill staging escaped the selected client root: $stageFull"
    }

    $backup = $null
    try {
        New-Item -ItemType Directory -Path $stageFull | Out-Null
        Get-ChildItem -LiteralPath $sourceSkill -Force |
            Copy-Item -Destination $stageFull -Recurse -Force
        if ((Get-SkillTreeDigest $stageFull) -ne $sourceDigest) {
            throw 'Entry Skill staging hash verification failed.'
        }

        if (Test-Path -LiteralPath $destination -PathType Container) {
            $stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
            $backup = Join-Path $skillsRoot ("ue-ai.backup-$stamp")
            if (Test-Path -LiteralPath $backup) {
                $backup = Join-Path $skillsRoot ("ue-ai.backup-$stamp-" + [Guid]::NewGuid().ToString('N'))
            }
            Move-Item -LiteralPath $destination -Destination $backup
        }
        try {
            Move-Item -LiteralPath $stageFull -Destination $destination
        }
        catch {
            if ($backup -and -not (Test-Path -LiteralPath $destination) -and (Test-Path -LiteralPath $backup)) {
                Move-Item -LiteralPath $backup -Destination $destination
            }
            throw
        }
    }
    finally {
        if (Test-Path -LiteralPath $stageFull) {
            Remove-Item -LiteralPath $stageFull -Recurse -Force
        }
    }

    return [ordered]@{
        client = $Name
        status = 'installed'
        destination = $destination
        backup = $backup
    }
}

$clients = if ($Client -eq 'both') { @('codex', 'claude') } else { @($Client) }
$results = @($clients | ForEach-Object { Install-ForClient $_ })
$envelope = [ordered]@{
    ok = $true
    schema = 'ue.entry-skill-install.v1'
    skill = 'ue-ai'
    results = $results
}
if ($Json) {
    $envelope | ConvertTo-Json -Depth 5 -Compress
}
else {
    foreach ($result in $results) {
        Write-Output ("[{0}] UE AI entry Skill {1}: {2}" -f $result.client, $result.status, $result.destination)
        if ($result.backup) { Write-Output ("      previous copy preserved at: {0}" -f $result.backup) }
    }
}
