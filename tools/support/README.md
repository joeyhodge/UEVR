# Verified UEVR support capture

These tools read the selected process's loaded backend identity, its on-disk
DLL/PDB, and optional already-saved UEVR evidence. They do not inject, modify
game memory, change settings, enable diagnostics, or download/execute symbols.
PowerShell 7 x64 and the matching backend PDB are required. WPR is only used
when `-CpuWaits` is explicitly supplied; that operation needs Administrator.

## Normal support bundle

1. In UEVR, open **VR > Health / Diagnostics** and select **Refresh Health**.
   Resolved-CVar readback runs on the following game tick; refresh completion
   and sample ages remain visible in the JSON. No missing CVar is scanned.
2. If pacing evidence is needed, enable **Record Frame Pacing**, close ImGui,
   reproduce the issue, stop recording, and **Export Support Snapshot**.
   Stopping retains history. Re-enabling starts a new epoch. Opening ImGui or
   using **Mark Current Frame** can affect pacing; account for that in analysis.
3. Run `support/capture.cmd` beside the injector. In a source checkout use
   `tools/support/capture.cmd`. It selects the newest exported support snapshot
   and verifies that its process/start time/backend are still current.
4. Review the resulting `%LOCALAPPDATA%\UEVR\SupportCaptures` folder before
   sharing. Full paths, settings, logs and process/module addresses may appear.

For an explicit snapshot or a process without the new health export:

```powershell
pwsh -NoProfile -File .\support\capture.ps1 -Snapshot 'C:\path\support_snapshot.json' -ValidateOnly
pwsh -NoProfile -File .\support\capture.ps1 -GameProcessId 1234
```

The PID-only form cannot infer the correct profile or a compiled commit ID;
it still verifies the loaded image's CodeView identity against the actual
DLL/PDB and retains their exact SHA-256 hashes. No obsolete commit allowlist
needs updating. Multiple live games should use an explicit snapshot or PID.

## Optional CPU/wait trace

From an Administrator PowerShell 7 terminal:

```powershell
.\support\capture.ps1 -Snapshot 'C:\path\support_snapshot.json' -CpuWaits -Seconds 35
```

This retains verified symbols before starting, then records sampled CPU stacks,
context switches and ready-thread stacks for 5-60 seconds. Only its unique WPR
instance is stopped, including on errors. It never cancels another recording or
elevates itself. If start fails, investigate the reported error rather than
manually cancelling unrelated traces. WPR save can take longer than recording.

Keep the scene, rendering method, resolution, upscaler, refresh rate, Sync stage
and compatibility settings unchanged. Keep instrumentation identical between
comparisons; prefer Hitch Diagnostics and Camera Trace off unless needed.
Do not run a GPU capture concurrently. Symbol hashing/copying occurs before
the measured interval; bundle and WPR interval timestamps are separate.

After the trace, stop/export the pacing recorder if it was enabled. The helper
does not remotely freeze it or assume that an older on-disk export is fresh.
Re-running without `-CpuWaits` makes a new bundle containing that newer export.
Saved config files may not represent unsaved live settings. Evidence manifests
include file timestamps and explicitly disclose this limitation.

## Identity and failure handling

The helper reads PE CodeView data from the actual loaded module and compares
GUID/age, PE stamp and image size with the file on disk. The PDB information
stream must match. Hashes are taken while holding read-only file leases; a
rebuild must not replace the pair halfway through validation. Symbols copied
into a bundle are validated again. The process/start time, module base/extent,
loaded identity and file hashes are checked before and after recording.

A changed PID/run, replaced image, mismatched/missing PDB, ambiguous image
identity, unsupported/corrupt file or access denial stops verification. This is
not permission to bypass protections or load a DLL for inspection. The backend
and PDB are never executed by this tool. Test-only native loading is restricted
to the dedicated harmless `support-capture-fixture.dll` built by CMake.

Files are only copied from a small allowlist beneath the local UnrealVRMod
profile root. Arbitrary/UNC/device paths nominated by snapshot contents are not
accepted. Existing game profiles are never overwritten. Each bundle has a new
unique output directory; captures and symbols can use substantial disk space.
