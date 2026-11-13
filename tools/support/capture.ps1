#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Snapshot,
    [ValidateRange(0, 2147483647)][int]$GameProcessId = 0,
    [string]$OutputRoot,
    [switch]$CpuWaits,
    [ValidateRange(5, 60)][int]$Seconds = 35,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'SupportCapture.psm1') -Force

$request = $null
if (-not $Snapshot -and -not $GameProcessId) {
    $root = Join-Path $env:APPDATA 'UnrealVRMod'
    $candidates = @(Get-ChildItem -LiteralPath $root -Directory | ForEach-Object {
        $file = Join-Path $_.FullName 'support_snapshot.json'
        if (Test-Path -LiteralPath $file -PathType Leaf) { Get-Item -LiteralPath $file }
    } | Sort-Object LastWriteTimeUtc -Descending)
    if (-not $candidates.Count) { throw 'Export Support Snapshot in VR > Health / Diagnostics first, or specify -GameProcessId.' }
    $Snapshot = $candidates[0].FullName
}
if ($Snapshot) {
    $request = Read-SupportRequest $Snapshot
    if ($GameProcessId -and $GameProcessId -ne $request.process.pid) { throw 'PID and snapshot disagree.' }
    $GameProcessId = [int]$request.process.pid
}
$observed = Get-InjectedBackend $GameProcessId
if ($request) { Assert-RequestProcess $request $observed }
if ($CpuWaits -and -not $ValidateOnly) {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'CPU wait capture needs an Administrator PowerShell terminal. This helper does not elevate itself.'
    }
}
if ($ValidateOnly) {
    Write-Host "Verified PID $GameProcessId, actual loaded DLL, and matching PDB $($observed.loaded.Guid)/$($observed.loaded.Age). No capture started."
    return
}
if (-not $OutputRoot) { $OutputRoot = Join-Path $env:LOCALAPPDATA 'UEVR\SupportCaptures' }
$OutputRoot = Get-LocalFullPath $OutputRoot
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssZ')
$unique = [Guid]::NewGuid().ToString('N')
$out = Join-Path $OutputRoot ("capture-$GameProcessId-$stamp-$unique")
New-Item -ItemType Directory -Path $out -Force | Out-Null
$profile = if ($request) { [string]$request.profile_dir } else { $null }
$record = [ordered]@{
    schema_version = 1; requested_seconds = $(if ($CpuWaits) { $Seconds } else { 0 }); cpu_waits = [bool]$CpuWaits
    pid = $observed.pid; process_start_filetime = $observed.started; executable = $observed.executable
    backend = $observed.pair.image; backend_sha256 = $observed.pair.image_sha256; pdb_sha256 = $observed.pair.pdb_sha256
    loaded_codeview = $observed.loaded; image_base = ('0x{0:x}' -f $observed.image_base)
    snapshot_build = $(if ($request) { $request.build } else { $null })
    bundle_started_utc = [DateTime]::UtcNow.ToString('o'); stopped_utc = $null
    trace_start_requested_utc = $null; trace_ready_utc = $null; trace_stop_requested_utc = $null; trace_saved_utc = $null
    start_exit = $null; stop_exit = $null; identity_rechecked = $false; game_exited = $false
    error = $null; secondary_errors = @(); before_evidence = @(); after_evidence = @()
    note = 'Read-only identity/evidence capture. No injection, game writes or settings changes. Saved exports are not assumed current. Only this unique WPR instance is stopped. Review paths/settings before sharing.'
}
$started = $false
$instance = "UEVRWaits_${GameProcessId}_$unique"
$wpr = Join-Path $env:SystemRoot 'System32\wpr.exe'
$trace = Join-Path $out 'cpu-waits.etl'
try {
    $symbols = Join-Path $out 'symbols'
    New-Item -ItemType Directory -Path $symbols | Out-Null
    Copy-Item -LiteralPath $observed.pair.image -Destination (Join-Path $symbols 'UEVRBackend.dll')
    Copy-Item -LiteralPath $observed.pair.pdb -Destination (Join-Path $symbols 'UEVRBackend.pdb')
    $copied = Get-BackendPair (Join-Path $symbols 'UEVRBackend.dll')
    if ($copied.image_sha256 -ne $observed.pair.image_sha256 -or $copied.pdb_sha256 -ne $observed.pair.pdb_sha256) {
        throw 'Symbol snapshot changed while copying; recording was not started.'
    }
    $record.before_evidence = @(Copy-ProfileEvidence $profile $out 'before-')
    if ($Snapshot) { Copy-Item -LiteralPath (Get-LocalFullPath $Snapshot) -Destination (Join-Path $out 'requested-snapshot.json') }
    Assert-SameProcess $observed (Get-InjectedBackend $GameProcessId)
    if ($CpuWaits) {
        $wprProfile = (Join-Path $PSScriptRoot 'UEVRWaits.wprp') + '!UEVRWaits'
        $record.trace_start_requested_utc = [DateTime]::UtcNow.ToString('o')
        & $wpr -start $wprProfile -filemode -instancename $instance 2>&1 |
            Tee-Object -FilePath (Join-Path $out 'wpr-start.log') | Out-Host
        $record.start_exit = $LASTEXITCODE
        if ($record.start_exit -ne 0) { throw 'WPR start failed. No other recording was cancelled.' }
        $started = $true
        $record.trace_ready_utc = [DateTime]::UtcNow.ToString('o')
        Write-Host "Recording $Seconds seconds. Leave settings unchanged; keep ImGui closed except for a needed manual marker. Do not run another trace concurrently."
        $timer = [Diagnostics.Stopwatch]::StartNew()
        while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
            $observed.game.Refresh()
            if ($observed.game.HasExited) { $record.game_exited = $true; break }
            Start-Sleep -Milliseconds 250
        }
    }
} catch {
    $record.error = $_.Exception.Message
} finally {
    if ($started) {
        try {
            $record.trace_stop_requested_utc = [DateTime]::UtcNow.ToString('o')
            & $wpr -stop $trace -instancename $instance 2>&1 |
                Tee-Object -FilePath (Join-Path $out 'wpr-stop.log') | Out-Host
            $record.stop_exit = $LASTEXITCODE
            $record.trace_saved_utc = [DateTime]::UtcNow.ToString('o')
        } catch {
            $message = "WPR save failed: $($_.Exception.Message)"
            if ($record.error) { $record.secondary_errors += $message } else { $record.error = $message }
        }
    }
    try {
        $record.after_evidence = @(Copy-ProfileEvidence $profile $out 'after-')
        $observed.game.Refresh()
        if ($observed.game.HasExited) { $record.game_exited = $true }
        else { Assert-SameProcess $observed (Get-InjectedBackend $GameProcessId); $record.identity_rechecked = $true }
    } catch {
        $message = "Final evidence/identity check failed: $($_.Exception.Message)"
        if ($record.error) { $record.secondary_errors += $message } else { $record.error = $message }
    }
    $record.stopped_utc = [DateTime]::UtcNow.ToString('o')
    $record | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $out 'capture.json') -Encoding utf8
}
if ($record.error) { throw "$($record.error) Evidence: $out" }
if ($CpuWaits -and ($record.stop_exit -ne 0 -or -not (Test-Path -LiteralPath $trace))) { throw "CPU trace was not saved. Inspect $out." }
Write-Host "Finished: $out"
Write-Host 'Export one more Support Snapshot after the run if the in-memory pacing history is needed; ETW does not automatically freeze or export it.'
