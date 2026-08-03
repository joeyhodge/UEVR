[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$UE6Source,

    [string]$BaselineSource,

    [string]$ManifestPath
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $PSScriptRoot 'ue6-snapshot.json'
}

function Resolve-SourceRoot {
    param([string]$Path, [string]$Label)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label source root does not exist: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-SnapshotVersion {
    param([string]$Root)

    $versionPath = Join-Path $Root 'Engine\Build\Build.version'
    if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) {
        throw "Build.version is missing: $versionPath"
    }

    return Get-Content -LiteralPath $versionPath -Raw | ConvertFrom-Json
}

function Test-ExpectedVersion {
    param(
        [object]$Actual,
        [object]$Expected,
        [string]$Label,
        [System.Collections.Generic.List[string]]$Failures
    )

    if ($Actual.MajorVersion -ne $Expected.major -or
        $Actual.MinorVersion -ne $Expected.minor -or
        $Actual.PatchVersion -ne $Expected.patch) {
        $Failures.Add(
            "$Label version is $($Actual.MajorVersion).$($Actual.MinorVersion).$($Actual.PatchVersion); expected $($Expected.major).$($Expected.minor).$($Expected.patch)")
    }
}

if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
    throw "Snapshot manifest does not exist: $ManifestPath"
}

$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$targetRoot = Resolve-SourceRoot -Path $UE6Source -Label 'UE6'
$baselineRoot = if ($BaselineSource) {
    Resolve-SourceRoot -Path $BaselineSource -Label 'Baseline'
} else {
    $null
}

$failures = [System.Collections.Generic.List[string]]::new()
$warnings = [System.Collections.Generic.List[string]]::new()
$results = [System.Collections.Generic.List[object]]::new()

Test-ExpectedVersion -Actual (Get-SnapshotVersion $targetRoot) -Expected $manifest.target.version -Label $manifest.target.name -Failures $failures
if ($baselineRoot) {
    Test-ExpectedVersion -Actual (Get-SnapshotVersion $baselineRoot) -Expected $manifest.baseline.version -Label $manifest.baseline.name -Failures $failures
}

foreach ($entry in $manifest.files) {
    $relativePath = $entry.path -replace '/', [IO.Path]::DirectorySeparatorChar
    $targetPath = Join-Path $targetRoot $relativePath
    $targetStatus = 'match'

    if (-not (Test-Path -LiteralPath $targetPath -PathType Leaf)) {
        $targetStatus = 'missing'
    } else {
        $targetHash = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash
        if ($targetHash -ne $entry.targetSha256) {
            $targetStatus = 'changed'
        }
    }

    $baselineStatus = 'not-checked'
    if ($baselineRoot) {
        $baselinePath = Join-Path $baselineRoot $relativePath
        if (-not (Test-Path -LiteralPath $baselinePath -PathType Leaf)) {
            $baselineStatus = 'missing'
        } else {
            $baselineHash = (Get-FileHash -LiteralPath $baselinePath -Algorithm SHA256).Hash
            $baselineStatus = if ($baselineHash -eq $entry.baselineSha256) { 'match' } else { 'changed' }
        }
    }

    $results.Add([pscustomobject]@{
        Severity = $entry.severity
        Area = $entry.area
        Target = $targetStatus
        Baseline = $baselineStatus
        Path = $entry.path
    })

    foreach ($snapshot in @(
        [pscustomobject]@{ Label = 'target'; Status = $targetStatus },
        [pscustomobject]@{ Label = 'baseline'; Status = $baselineStatus }
    )) {
        if ($snapshot.Status -in @('match', 'not-checked')) {
            continue
        }

        $message = "$($entry.area): $($snapshot.Label) $($snapshot.Status) ($($entry.path))"
        if ($entry.severity -eq 'critical') {
            $failures.Add($message)
        } else {
            $warnings.Add($message)
        }
    }
}

$results | Format-Table -AutoSize

if ($warnings.Count -gt 0) {
    Write-Warning ("Support-file drift detected:`n - " + ($warnings -join "`n - "))
}

if ($failures.Count -gt 0) {
    Write-Error ("UE6 snapshot validation failed:`n - " + ($failures -join "`n - "))
    exit 1
}

Write-Host "UE6 snapshot validation passed for $($manifest.target.name) ($($manifest.target.archiveCommit))."
if ($baselineRoot) {
    Write-Host "Baseline validation passed for $($manifest.baseline.name) ($($manifest.baseline.archiveCommit))."
}

exit 0
