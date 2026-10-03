$ErrorActionPreference = 'Stop'
$temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$root = Join-Path $temp ('UEVR-UE6Snapshot-' + [Guid]::NewGuid().ToString('N'))
$checker = Join-Path $PSScriptRoot '..\scripts\ue6\Compare-UE6Snapshot.ps1'
$powershell = (Get-Process -Id $PID).Path

function Write-Fixture([string]$Path, [string]$Value) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    [IO.File]::WriteAllText($Path, $Value)
}

function Fixture-Hash([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($Path))).Replace('-', '') }
    finally { $sha.Dispose() }
}

function Check([string]$Name, [int]$Expected) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $powershell
    $start.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -UE6Source "{1}" -BaselineSource "{2}" -ManifestPath "{3}"' -f $checker, $target, $baseline, $manifestPath
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($start)
    $output = $process.StandardOutput.ReadToEnd()
    $errors = $process.StandardError.ReadToEnd()
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    $process.Dispose()
    if ($exitCode -ne $Expected) { throw "$Name returned $exitCode, expected $Expected`n$output`n$errors" }
}

try {
    $target = Join-Path $root 'target'
    $baseline = Join-Path $root 'baseline'
    $manifestPath = Join-Path $root 'manifest.json'
    $version = '{"MajorVersion":6,"MinorVersion":0,"PatchVersion":0}'
    foreach ($directory in @($target, $baseline)) {
        Write-Fixture (Join-Path $directory 'Engine\Build\Build.version') $version
        Write-Fixture (Join-Path $directory 'Engine\critical.txt') 'reviewed-critical'
        Write-Fixture (Join-Path $directory 'Engine\support.txt') 'reviewed-support'
    }
    $files = foreach ($path in @('Engine/Build/Build.version', 'Engine/critical.txt', 'Engine/support.txt')) {
        @{
            severity = $(if ($path -like '*support*') { 'support' } else { 'critical' })
            area = 'synthetic-fixture'
            path = $path
            targetSha256 = Fixture-Hash (Join-Path $target $path)
            baselineSha256 = Fixture-Hash (Join-Path $baseline $path)
        }
    }
    @{
        schemaVersion = 1
        target = @{ name = 'synthetic target'; archiveCommit = 'synthetic'; version = @{ major = 6; minor = 0; patch = 0 } }
        baseline = @{ name = 'synthetic baseline'; archiveCommit = 'synthetic'; version = @{ major = 6; minor = 0; patch = 0 } }
        files = @($files)
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Check 'matching snapshots' 0
    Write-Fixture (Join-Path $target 'Engine\support.txt') 'changed-support'
    Check 'support drift is warning only' 0
    Write-Fixture (Join-Path $target 'Engine\critical.txt') 'changed-critical'
    Check 'critical drift with unchanged engine version' 1
    Remove-Item -LiteralPath (Join-Path $target 'Engine\critical.txt')
    Check 'missing critical file' 1
    Write-Fixture (Join-Path $target 'Engine\critical.txt') 'reviewed-critical'
    Write-Fixture (Join-Path $baseline 'Engine\critical.txt') 'changed-baseline'
    Check 'baseline drift' 1
    Write-Fixture (Join-Path $baseline 'Engine\critical.txt') 'reviewed-critical'
    Write-Fixture (Join-Path $target 'Engine\Build\Build.version') '{"MajorVersion":6,"MinorVersion":1,"PatchVersion":0}'
    Check 'unreviewed version' 1
    Write-Host 'UE6 snapshot drift fixtures passed.'
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    if (-not $resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -or
        -not (Split-Path -Leaf $resolved).StartsWith('UEVR-UE6Snapshot-')) { throw 'Unsafe fixture cleanup path' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
