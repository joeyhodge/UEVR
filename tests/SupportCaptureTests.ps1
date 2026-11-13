#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceRoot, [Parameter(Mandatory)][string]$Fixture,
    [Parameter(Mandatory)][string]$CaptureHost)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $SourceRoot 'tools\support\SupportCapture.psm1') -Force
function Assert([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert $rejected $Message
}
function Put32([byte[]]$Bytes, [int]$Offset, [uint32]$Value) {
    [Array]::Copy([BitConverter]::GetBytes($Value), 0, $Bytes, $Offset, 4)
}
$scratch = Join-Path ([IO.Path]::GetTempPath()) ('uevr-support-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $scratch | Out-Null
$originalAppData = $env:APPDATA
$image = Join-Path $scratch 'UEVRBackend.dll'
$pdb = Join-Path $scratch 'UEVRBackend.pdb'
$handle = [IntPtr]::Zero
$captureProcess = $null
try {
    Copy-Item -LiteralPath $Fixture -Destination $image
    Copy-Item -LiteralPath ([IO.Path]::ChangeExtension($Fixture, '.pdb')) -Destination $pdb
    $pair = Get-BackendPair $image
    Assert ($pair.identity.Guid -match '^[a-f0-9-]{36}$' -and $pair.identity.Age -gt 0) 'Real fixture DLL/PDB identity did not verify.'
    $tokens = $null; $errors = $null
    foreach ($file in @('capture.ps1','SupportCapture.psm1')) {
        [Management.Automation.Language.Parser]::ParseFile((Join-Path $SourceRoot "tools\support\$file"), [ref]$tokens, [ref]$errors) | Out-Null
        Assert ($errors.Count -eq 0) "PowerShell parse errors in $file"
    }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UEVRSupportFixtureLoader {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll")]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
    # Only load the dedicated harmless fixture, never a UEVR backend/game image.
    Assert ([IO.Path]::GetFileName($Fixture) -ceq 'support-capture-fixture.dll') 'Only the dedicated test fixture may be loaded.'
    $handle = [UEVRSupportFixtureLoader]::LoadLibraryW($Fixture)
    Assert ($handle -ne [IntPtr]::Zero) 'Harmless fixture could not load.'
    $module = @((Get-Process -Id $PID).Modules | Where-Object { $_.BaseAddress -eq $handle })
    Assert ($module.Count -eq 1) 'Loaded fixture module identity is ambiguous.'
    $loaded = [UEVR.Support.Identity]::FromProcess($PID, $handle.ToInt64(), $module[0].ModuleMemorySize)
    Assert ([UEVR.Support.Identity]::Same($loaded, $pair.identity)) 'Mapped-image RVA and disk raw-file identity differ.'
    Assert ($loaded.ImageSize -eq $pair.identity.ImageSize -and $loaded.Timestamp -eq $pair.identity.Timestamp) 'Loaded image extent/stamp differ.'

    $bytes = [IO.File]::ReadAllBytes($image)
    $codeview = -1
    for ($i = 0; $i -le $bytes.Length - 24; ++$i) {
        if ([BitConverter]::ToUInt32($bytes, $i) -eq 0x53445352) { $codeview = $i; break }
    }
    Assert ($codeview -ge 0) 'Fixture CodeView bytes are absent.'
    $changed = [byte[]]$bytes.Clone()
    Put32 $changed ($codeview + 20) ($pair.identity.Age + 1)
    [IO.File]::WriteAllBytes($image, $changed)
    Reject { Get-BackendPair $image } 'Wrong PDB age was accepted.'
    Assert (-not [UEVR.Support.Identity]::Same([UEVR.Support.Identity]::FromImage($image), $loaded)) 'Changed disk image borrowed loaded identity.'
    $changed = [byte[]]$bytes.Clone(); $changed[$codeview + 4] = $changed[$codeview + 4] -bxor 0x80
    [IO.File]::WriteAllBytes($image, $changed)
    Reject { Get-BackendPair $image } 'Wrong PDB GUID was accepted.'
    foreach ($length in @(0, 2, 63)) {
        [IO.File]::WriteAllBytes($image, $bytes[0..([Math]::Max(0, $length - 1))])
        Reject { [UEVR.Support.Identity]::FromImage($image) } 'Truncated PE header was accepted.'
    }
    $changed = [byte[]]$bytes.Clone(); Put32 $changed 60 ([uint32]::MaxValue)
    [IO.File]::WriteAllBytes($image, $changed)
    Reject { [UEVR.Support.Identity]::FromImage($image) } 'Overflowed PE header was accepted.'
    [IO.File]::WriteAllBytes($image, $bytes)
    $pdbBytes = [IO.File]::ReadAllBytes($pdb)
    foreach ($mutation in @(@(32, 3), @(40, [uint32]::MaxValue), @(44, [uint32]::MaxValue), @(52, [uint32]::MaxValue))) {
        $changedPdb = [byte[]]$pdbBytes.Clone()
        Put32 $changedPdb $mutation[0] $mutation[1]
        [IO.File]::WriteAllBytes($pdb, $changedPdb)
        Reject { [UEVR.Support.Identity]::FromPdb($pdb) } 'Malformed MSF superblock was accepted.'
    }
    [IO.File]::WriteAllBytes($pdb, $pdbBytes)
    $writer = [IO.File]::Open($pdb, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
    try { Reject { Get-BackendPair $image } 'An actively-written symbol file was accepted as a stable snapshot.' }
    finally { $writer.Dispose() }
    $pair = Get-BackendPair $image
    Reject { Get-LocalFullPath '\\example.invalid\share\not-a-local-file' } 'UNC path was accepted.'
    Reject { Get-LocalFullPath '\\?\C:\not-a-normal-drive-path' } 'Device path was accepted.'

    function Observed {
        return [pscustomobject]@{ pid = 42; started = '123'; executable = 'C:\game\game.exe'; image_base = 4096; image_size = 32768
            pair = [pscustomobject]@{ image = 'C:\uevr\UEVRBackend.dll'; image_sha256 = 'a'; pdb_sha256 = 'b' }; loaded = $loaded }
    }
    $before = Observed
    Assert-SameProcess $before (Observed)
    foreach ($field in @('pid','started','executable','image_base','image_size')) {
        $after = Observed
        if ($field -in 'pid','image_base','image_size') { $after.$field = 999 } else { $after.$field = 'changed' }
        Reject { Assert-SameProcess $before $after } "Changed $field was accepted."
    }
    foreach ($field in @('image','image_sha256','pdb_sha256')) {
        $after = Observed; $after.pair.$field = 'changed'
        Reject { Assert-SameProcess $before $after } "Changed pair $field was accepted."
    }
    $request = [pscustomobject]@{ schema_version = 1; diagnostic_only = $true
        process = [pscustomobject]@{ pid = 42; start_filetime = '123'; executable = 'C:\game\game.exe'; backend_path = 'C:\uevr\UEVRBackend.dll' } }
    Assert-RequestProcess $request $before
    $request.process.start_filetime = 'old-run'
    Reject { Assert-RequestProcess $request $before } 'Reused PID with an old snapshot was accepted.'
    $request.process.start_filetime = '123'
    $requestFile = Join-Path $scratch 'support_snapshot.json'
    $request | ConvertTo-Json | Set-Content -LiteralPath $requestFile
    Read-SupportRequest $requestFile | Out-Null
    $request.schema_version = 999
    $request | ConvertTo-Json | Set-Content -LiteralPath $requestFile
    Reject { Read-SupportRequest $requestFile } 'Unknown snapshot schema was accepted.'

    $env:APPDATA = Join-Path $scratch 'appdata'
    $profile = Join-Path $env:APPDATA 'UnrealVRMod\fixture'
    $destination = Join-Path $scratch 'evidence'
    New-Item -ItemType Directory -Path $profile,$destination -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $profile 'config.txt') -Value 'VR_NativeOpenXRAsyncWaitOnly=true'
    Set-Content -LiteralPath (Join-Path $profile 'not-requested.txt') -Value 'must not copy'
    $hash = (Get-FileHash -LiteralPath (Join-Path $profile 'config.txt')).Hash
    $files = @(Copy-ProfileEvidence $profile $destination 'before-')
    Assert ($files.Count -eq 1 -and $files[0].freshness -like 'Saved file only*') 'Saved evidence was incorrectly described as live/fresh.'
    Assert ((Get-FileHash -LiteralPath (Join-Path $profile 'config.txt')).Hash -eq $hash) 'Profile settings changed during capture.'
    Assert (-not (Test-Path -LiteralPath (Join-Path $destination 'before-not-requested.txt'))) 'Unrelated profile file was copied.'
    Reject { Copy-ProfileEvidence $scratch $destination 'bad-' } 'Snapshot nominated files outside the profile root.'
    $launcher = Get-Content -LiteralPath (Join-Path $SourceRoot 'tools\support\capture.ps1') -Raw
    Assert ($launcher -notmatch '(?i)wpr[^\r\n]*-cancel|WriteProcessMemory|Set-ItemProperty|git\s+(reset|checkout)') 'Capture has a destructive/global operation.'
    Assert ($launcher.Contains('-instancename $instance') -and $launcher.Contains('finally {')) 'Trace stop does not retain instance/finally ownership.'
    Assert ([IO.Path]::GetFileName($CaptureHost) -ceq 'support-capture-host.exe') 'Only the dedicated test host may start.'
    Assert ((Get-FileHash -LiteralPath $image).Hash -eq (Get-FileHash -LiteralPath $Fixture).Hash) 'Test host may only load an unchanged fixture copy.'
    $captureProcess = Start-Process -FilePath $CaptureHost -ArgumentList ('"' + $image + '"') -WindowStyle Hidden -PassThru
    $ready = $false
    for ($attempt = 0; $attempt -lt 100; ++$attempt) {
        try { $observedChild = Get-InjectedBackend $captureProcess.Id; $ready = $true; break } catch { Start-Sleep -Milliseconds 50 }
    }
    Assert $ready 'Dedicated test host did not publish its harmless fixture.'
    $captureOutput = Join-Path $scratch 'real-capture'
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -NonInteractive -File (Join-Path $SourceRoot 'tools\support\capture.ps1') `
        -GameProcessId $captureProcess.Id -OutputRoot $captureOutput -ValidateOnly
    Assert ($LASTEXITCODE -eq 0 -and -not (Test-Path -LiteralPath $captureOutput)) 'ValidateOnly started a capture or failed live image validation.'
    & (Join-Path $PSHOME 'pwsh.exe') -NoProfile -NonInteractive -File (Join-Path $SourceRoot 'tools\support\capture.ps1') `
        -GameProcessId $captureProcess.Id -OutputRoot $captureOutput
    Assert ($LASTEXITCODE -eq 0) 'End-to-end bundle capture failed.'
    $bundle = @(Get-ChildItem -LiteralPath $captureOutput -Directory)
    Assert ($bundle.Count -eq 1) 'Bundle output is not unique.'
    $manifest = Get-Content -LiteralPath (Join-Path $bundle[0].FullName 'capture.json') -Raw | ConvertFrom-Json
    Assert ($manifest.identity_rechecked -and -not $manifest.cpu_waits -and $null -eq $manifest.error -and
        $manifest.loaded_codeview.Guid -eq $pair.identity.Guid -and $manifest.pid -eq $captureProcess.Id) 'Bundle lost actual loaded identity/ownership metadata.'
    $copiedPair = Get-BackendPair (Join-Path $bundle[0].FullName 'symbols\UEVRBackend.dll')
    Assert ($copiedPair.image_sha256 -eq $observedChild.pair.image_sha256 -and
        $copiedPair.pdb_sha256 -eq $observedChild.pair.pdb_sha256) 'Bundle symbols do not match the real selected image.'
    Write-Host 'Support capture identity, malformed-image, PID/run, profile and ownership tests passed.'
} finally {
    if ($captureProcess -and -not $captureProcess.HasExited) { Stop-Process -Id $captureProcess.Id -ErrorAction SilentlyContinue }
    $env:APPDATA = $originalAppData
    if ($handle -ne [IntPtr]::Zero) { [UEVRSupportFixtureLoader]::FreeLibrary($handle) | Out-Null }
    # Flat files explicitly created by this test are retained in its unique temp
    # directory for diagnosis. No recursive deletion or user profile mutation.
}
