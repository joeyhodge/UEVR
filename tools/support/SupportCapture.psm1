Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not ('UEVR.Support.Identity' -as [type])) {
    Add-Type -LiteralPath (Join-Path $PSScriptRoot 'Identity.cs')
}

function Get-LocalFullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'A local path is required.' }
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^[A-Za-z]:[\\/]') { throw 'Only local drive paths are accepted.' }
    return $full
}

function Get-BackendPair([string]$Backend) {
    $image = Get-LocalFullPath $Backend
    $pdb = [IO.Path]::ChangeExtension($image, '.pdb')
    if (-not (Test-Path -LiteralPath $image -PathType Leaf) -or -not (Test-Path -LiteralPath $pdb -PathType Leaf)) {
        throw 'Keep the actual loaded UEVRBackend.dll and its matching PDB together before capturing.'
    }
    $imageLease = $null; $pdbLease = $null
    try {
        # Keep identity and hashes on the same files; a concurrent rebuild must
        # fail validation rather than replace them halfway through this operation.
        $imageLease = [IO.File]::Open($image, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $pdbLease = [IO.File]::Open($pdb, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $debug = [UEVR.Support.Identity]::FromImage($image)
        $symbols = [UEVR.Support.Identity]::FromPdb($pdb)
        if (-not [UEVR.Support.Identity]::Same($debug, $symbols)) { throw 'DLL/PDB CodeView GUID and age do not match.' }
        return [pscustomobject]@{
            image = $image; pdb = $pdb; identity = $debug
            image_sha256 = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash.ToLowerInvariant()
            pdb_sha256 = (Get-FileHash -LiteralPath $pdb -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    } finally {
        if ($pdbLease) { $pdbLease.Dispose() }
        if ($imageLease) { $imageLease.Dispose() }
    }
}

function Get-InjectedBackend([int]$GameProcessId) {
    if (-not [Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell 7.' }
    $game = Get-Process -Id $GameProcessId -ErrorAction Stop
    $modules = @($game.Modules | Where-Object { $_.ModuleName -ieq 'UEVRBackend.dll' })
    if ($modules.Count -ne 1) { throw 'The selected process must have exactly one loaded UEVRBackend.dll.' }
    $module = $modules[0]
    $pair = Get-BackendPair $module.FileName
    $loaded = [UEVR.Support.Identity]::FromProcess($game.Id, $module.BaseAddress.ToInt64(), $module.ModuleMemorySize)
    if (-not [UEVR.Support.Identity]::Same($loaded, $pair.identity) -or
        $loaded.Timestamp -ne $pair.identity.Timestamp -or $loaded.ImageSize -ne $pair.identity.ImageSize) {
        throw 'The DLL on disk is not the image loaded in this game. Nothing was recorded.'
    }
    return [pscustomobject]@{
        game = $game; pair = $pair; loaded = $loaded
        pid = $game.Id; started = $game.StartTime.ToUniversalTime().ToFileTimeUtc().ToString()
        executable = $game.Path; image_base = $module.BaseAddress.ToInt64(); image_size = $module.ModuleMemorySize
    }
}

function Assert-SameProcess($Before, $After) {
    if ($Before.pid -ne $After.pid -or $Before.started -ne $After.started -or
        $Before.executable -ine $After.executable -or $Before.image_base -ne $After.image_base -or
        $Before.image_size -ne $After.image_size -or $Before.pair.image -ine $After.pair.image -or
        $Before.pair.image_sha256 -ne $After.pair.image_sha256 -or
        $Before.pair.pdb_sha256 -ne $After.pair.pdb_sha256 -or
        -not [UEVR.Support.Identity]::Same($Before.loaded, $After.loaded)) {
        throw 'The process or loaded backend changed during capture; do not compare this as an unchanged-build run.'
    }
}

function Read-SupportRequest([string]$Snapshot) {
    $path = Get-LocalFullPath $Snapshot
    $file = Get-Item -LiteralPath $path
    if ($file.Length -gt 8MB) { throw 'The support snapshot exceeds the bounded schema size.' }
    $request = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($request.schema_version -ne 1 -or $request.diagnostic_only -ne $true -or
        $request.process.pid -le 0 -or [string]::IsNullOrEmpty($request.process.start_filetime)) {
        throw 'Expected a UEVR read-only support snapshot. Export a fresh snapshot in VR > Health / Diagnostics.'
    }
    return $request
}

function Assert-RequestProcess($Request, $Observed) {
    if ($Request.process.pid -ne $Observed.pid -or $Request.process.start_filetime -ne $Observed.started -or
        $Request.process.executable -ine $Observed.executable -or $Request.process.backend_path -ine $Observed.pair.image) {
        throw 'The snapshot belongs to a different process/run/backend. Export a fresh snapshot; no commit allowlist needs editing.'
    }
}

function Copy-ProfileEvidence([string]$Profile, [string]$Destination, [string]$Prefix) {
    if (-not $Profile) { return @() }
    $profilePath = Get-LocalFullPath $Profile
    $allowedRoot = (Get-LocalFullPath (Join-Path $env:APPDATA 'UnrealVRMod')).TrimEnd('\') + '\'
    if (-not $profilePath.StartsWith($allowedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'A snapshot may only nominate a profile under the local UnrealVRMod directory.'
    }
    $rows = @()
    foreach ($name in @('log.txt', 'config.txt', 'support_snapshot.json', 'hook_provenance.json', 'native_frame_diagnostics.json')) {
        $source = Join-Path $profilePath $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
        $file = Get-Item -LiteralPath $source
        $target = Join-Path $Destination ($Prefix + $name)
        Copy-Item -LiteralPath $source -Destination $target
        $rows += [pscustomobject]@{ name = $name; saved_utc = $file.LastWriteTimeUtc.ToString('o');
            sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant();
            freshness = 'Saved file only; not necessarily current live settings or a fresh diagnostic export.' }
    }
    return $rows
}

Export-ModuleMember -Function Get-LocalFullPath,Get-BackendPair,Get-InjectedBackend,Assert-SameProcess,Read-SupportRequest,Assert-RequestProcess,Copy-ProfileEvidence
