# UE6 Compatibility Phase 1

This phase is intentionally isolated to the `UE6Testing` branches in UESDK and UEVR. It establishes a source-validated UE6.0 compatibility profile without claiming packaged-game or HMD validation.

## Source snapshots

- UE6 target: `3e62f9ca59cb493c7e435569483c2a9dfd83af02`, `Engine/Build/Build.version` = 6.0.0.
- UE5 baseline: `dcc331c30ec045ba3ba7e25f36490a0b988649f5`, `Engine/Build/Build.version` = 5.8.1.
- UE6.1 and newer fail closed until a later source snapshot revalidates every ABI-sensitive path.

Run the source audit before updating the UE6 profile:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\ue6\Compare-UE6Snapshot.ps1 `
  -UE6Source "D:\UE6Analysis\20260802\UnrealEngine-ue6-main" `
  -BaselineSource "D:\UE6Analysis\UE5.8.1\UnrealEngine-5.8"
```

Critical drift returns a nonzero exit code. Support-file drift is reported as a warning so it can be reviewed before the snapshot manifest is intentionally updated.

## Implemented profile

- UESDK detects UE4, UE5, and UE6 from the embedded engine string with fixed-file metadata as a synchronous fallback. Every consumer receives the same version and source label.
- Exact UE6.0 has explicit FSceneView, CVar, XR, render-target-manager, viewport, UObject, FName, FField, and FProperty profiles.
- IXRTrackingSystem, IXRCamera, IHeadMountedDisplay, IStereoRendering, and render-target-manager ordering was revalidated against the UE6 snapshot rather than inherited from a broad `>= 5.8` test.
- The exact UE6.0 profile reuses only source-confirmed UE5.8-compatible runtime paths: scene-viewport target adoption, OpenXR live resize, UI-layer pose submission, and the D3D12 command-list root at `+0x28`.
- Fixed-layout helpers that are not source-safe on UE6, including fixed malloc-renderer creation and fixed RHI-texture slots, fail closed.

## UE6 Slate delta

UE6 inserts `ISlateViewport* WindowViewport` between `Window` and `ViewportInfo` in `FSlateDrawWindowPassInputs`. UEVR reads that exact UE6.0 layout, validates every pointer plus extent and UI scale, and normalizes it into the existing internal draw-input representation. If validation fails, the original Slate call runs unchanged.

Unknown UE6 minors bypass the Slate interception rather than being treated as an older UE layout.

## Deliberate exclusions

- No OpenXR SDK upgrade is included in Phase 1.
- No speculative UE6.1+ offsets or vtable aliases are enabled.
- Non-default UObject state-stream or remote-object configurations are not declared compatible.
- A successful Windows Release build is not treated as runtime, stereo, UI, or HMD proof.

## First packaged UE6 validation

1. Verify the log contains `[EngineVersion]` and `[UECompat]` with version `6.0.0`, a valid source, and `validated=true`.
2. Test DX12/OpenXR Native injection at a menu and in gameplay.
3. Confirm the FSceneView pair has valid player indices, stereo passes, view rectangles, and projections.
4. Confirm scene-viewport adoption reaches the requested per-eye dimensions without a resize loop.
5. Test Slate menu, HUD, ImGui, cursor, spectator UI, and level transitions for duplication or stale layers.
6. Test Synced Sequential only after Native is stable; keep DIBR and experimental methods opt-in.
7. Re-run the snapshot verifier before accepting any newer UE6 source archive.

## Branch procedure

Push UESDK `UE6Testing` first. Then update the UEVR UESDK submodule pointer, build UEVR from the exact worktree, and push only UEVR `UE6Testing`. Do not fan this phase out to production branches until a packaged UE6 runtime confirms the profile.
