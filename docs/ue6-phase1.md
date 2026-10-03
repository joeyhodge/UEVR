# UE6 Compatibility: October 2026 Refresh

This work is isolated to `UE6Testing` in UESDK and UEVR. It merges the current
`eBaseballfocused` baseline, including Mono/DIBR, then adds the source-reviewed
UE6.0 profile. Source review, Windows compilation, and offline fixtures are not
packaged-game, timing, stereo, UI, or HMD validation.

## Snapshots and scope

- Current UE6 main: `6e797985ecc380099fca961d292c46a8bd6b3f91`, 6.0.0.
- Previous UE6 review: `3e62f9ca59cb493c7e435569483c2a9dfd83af02`, 6.0.0.
- Original UE5.8.1 comparison: `dcc331c30ec045ba3ba7e25f36490a0b988649f5`.
- Stable inputs: UEVR `cf3a21d1898e38dd318871b2eeba50d1479ac9a9` and
  UESDK `f208d804e280fec4136466f6e47825ca916ae245`.
- Stock Win64 ABI only. UE6.1+ fails closed. Non-default state-stream,
  remote-object, HDR Slate extraction, and licensee layouts are not
  claimed compatible.

Both development snapshots report 6.0.0 despite significant ABI changes.
Version detection alone cannot prove compatibility with a later archive or
licensee build. The manifest records the reviewed source hashes; runtime
discovery additionally validates the supported helper/resource layouts.
Logs explicitly mark the profile experimental and packaged-runtime unverified.

## Source-backed changes

| Area | October finding and handling |
| --- | --- |
| UObject lifecycle | Three destruction virtuals move into UObjectBase. Validate PostInitProperties slot 13, or 12 for the monolithic constinit layout; never fall back to UE5 slot 10. Preserve dynamically discovered ProcessEvent and retained-object serial/membership checks. |
| Object liveness | Garbage is bit 21, Unreachable bit 28, and live RefCounted bit 29. UE6 must not mistake RefCounted for legacy PendingKill. FUObjectArray item/chunk layout remains compatible with the validated modern path. |
| FName / reflection | FName storage is unchanged from August. Keep owned 8/12-byte names, borrowed Lua bindings, and structural FField/property discovery; do not import a guessed full UE6 property layout. |
| SceneView / Family | Family Scene moves after MaxLuminance; retain immutable structural discovery. InitOptions' early player/stereo fields remain compatible; the added BufferUsage tail is not copied or synthesized. Double-precision view math is explicit. |
| View extension / XR | A removed deprecated extension virtual changes later ordering; preserve instruction-validated active-function discovery. IXRTrackingSystem, IXRCamera, HMD, StereoRendering and StereoRTManager ordering remains source-compatible with the reviewed UE5.8 aliases. |
| Viewport | GetDebugCanvas is slot 53: one new cursor virtual precedes it, while two enqueue methods follow it. GetSizeXY remains slot 5. Both helpers require bounded read-only instruction proof before invocation. UE5.4 direct resource presets are not used for UE6. |
| Slate | FHDRMetaData expands DrawWindow inputs; see the dedicated adapter details below. The UE5 AddSlateDrawElementsPass fallback is disabled because UE6 inserts FinalVertices. |
| Native texture | Prove FRHITexture::GetDesc's LEA accessor at source-bounded slots 2-4, then GetNativeResource at its relative +3 slot. Validate RHI descriptor, typed DX11/DX12 resource, device, extent, format, flags, and unchanged descriptor before exposing a borrowed native pointer. No guessed global offsets are published. |
| CVar | Modern virtual ordering is unchanged, but concrete object storage and manager internals changed. UE6 uses validated virtual interfaces, not raw legacy storage. Set context reserves enough space for flags plus an 8/12-byte NAME_None. No automatic priority escalation. |
| RHI / rendering | FMemStackBase still occupies 0x28 before the command root. Preserve the reviewed modern pipeline; no new healthy-frame waits, cadence adjustment, PSO recreation, or residency policy is introduced. Fixed malloc-renderer/CreateTexture2D helpers remain unavailable. |

## UE6 Slate adapter

The Win64 input prefix has five pointers, FHDRMetaData at +0x30, cursor at
+0x1b0, scene rect at +0x1b8, and scale at +0x1c8. DrawWindow returns a
24-byte output through RDX, with GraphBuilder in R8 and inputs in R9. Neither
the output storage nor GraphBuilder is an RHI command list.

The adapter normalizes a validated **SDR** prefix into UEVR's internal input
representation. Original engine inputs, metadata, tail, and output storage
remain untouched. Dedicated UI dimensions come from the validated viewport
extent, not a hard-coded 1920x1080 target. Invalid, incomplete, unsupported HDR,
or unknown inputs preserve the original draw and do not enable interception.
A scoped guard prevents an unsupported nested draw from inheriting a redirect.

Native texture validation does not retain engine objects or solve their GPU
lifetime. Consumers still validate device/descriptor identity and acquire COM
ownership before retaining resources; existing retirement paths remain in use.

## Offline verification

- Standalone UESDK builds with its pinned Kananlib dependency.
- Readable-page version fixtures cover chunk boundaries, guard/no-access holes,
  overflow, embedded branch/version disagreement, and unknown UE6 minors.
- Independent MSVC models verify render-target overload grouping, point sret,
  and all four optional FRHI base-vtable combinations.
- Actual DX11/DX12 WARP textures exercise typed native validation and reject
  inconsistent extents, formats, flags, and invalid descriptor calls.
- UEVR fixtures cover UE4/UE5 policy preservation, liveness masks, bounded
  read-only helpers, partial-register rejection, Slate SDR/HDR metadata, and
  descriptor bounds. Snapshot fixtures reject same-version critical drift.

## Next development snapshot

Retain both source archives. Before changing the profile, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ue6\Compare-UE6Snapshot.ps1 `
  -UE6Source "D:\UE6Analysis\20261003\UnrealEngine-ue6-main" `
  -BaselineSource "D:\UE6Analysis\20260802\UnrealEngine-ue6-main"
```

Critical drift returns nonzero even if Build.version still says 6.0.0.
Support-file drift is a warning, not automatic approval. For each new archive:
review changed consumers and configuration-dependent ABIs, update guards and
negative tests, then deliberately update hashes and the reviewed source label.
Do not simply regenerate the manifest to silence drift. Do not commit licensed
engine source or extracted source diffs.

## First packaged UE6 validation

1. Verify EngineVersion and UECompat logs show 6.0.0, the reviewed source,
   experimental=true, and packaged_runtime_verified=false.
2. Start DX12/OpenXR Native at menu and gameplay with diagnostic plugins off.
3. Check Draw observation, valid SceneView pairs/projections, native texture
   identity, and requested scene extent without resize loops.
4. Exercise menu, HUD, cursor, ImGui, spectator, and repeated level transitions.
5. Validate CVars by readback; test UObject/Lua traversal, GC, attachments, and
   map travel. Do not count successful discovery as successful setter execution.
6. Test Synced/Ghost, Native Fix, then opt-in Mono/DIBR; test DX11 separately.
7. Keep the stable baseline available until these runtime checks pass.

Push UESDK `UE6Testing` first, pin that exact commit in UEVR, verify the full
build and offline suites, then push only UEVR `UE6Testing`. Stable production
branches and the local `57workuevr` artifacts are not replaced by this refresh.
