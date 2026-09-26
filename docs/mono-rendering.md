# Mono rendering (experimental)

This opt-in feature is available on `eBaseballfocused`, `uevr-dibr-optin-safe`, and
`UEVRMono+DIBR+UEVR`. It does not change the default
rendering method. Select **Mono (Experimental)** in the injector or in
**VR / Unreal / Rendering Method**. Its saved `VR_RenderingMethod` ID is **5**;
Native, Synced, AFR and the two DIBR IDs remain 0 through 4.

## What it does

- Renders one centered, head-tracked engine view and copies that current image
  into both OpenXR eye regions every frame. It does not alternate eyes, reuse
  the previous eye, or synthesize depth like DIBR.
- Uses a symmetric union projection with separately validated per-eye crops.
  Both submitted views describe the same centered rendered pose. Calibrated
  eye rotations are transformed into that common optical basis, not discarded.
  Submitted FOVs match the actual integer-pixel crops, including rounded edges.
- Retains UEVR's double-wide engine allocation to avoid a new engine render
  target layout/lifetime contract. Only the left region supplies scene pixels.
  This reduces scene-view work but does not halve allocation size or guarantee
  an FPS improvement.
- Leaves UI extraction and spectator routing in their existing paths. Native
  Fix, Ghost Fix/bootstrap, DIBR and scene-depth submission are ineffective in
  Mono; their saved preferences are not erased.

There is no binocular scene depth. Compositor UI may still have separate depth.
Image quality, comfort and performance require testing in each game/headset.

## Supported path and rejection behavior

Implemented for **DX11 or DX12 with OpenXR** and a validated single-view main
family/scene source. Eye calibration/cant is supported when both frusta fit a
finite forward-facing common perspective; horizon-crossing rays fail closed.
The engine renders in the left optical orientation at the midpoint of the eyes.
OpenXR maps the submitted common-pose crops to each physical display (as allowed
by [XrCompositionLayerProjectionView](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerProjectionView.html)).
OpenVR is not implemented. Extreme Compatibility, split-screen, 2D Screen Mode,
SceneView compatibility, stereo emulation, and the restricted NASCAR rendering
paths are not eligible.

The scene copy requires a matching, single-sample, single-mip, non-array BGRA8
source/destination on the same device. Unexpected formats, missing hooks,
ambiguous/auxiliary views and stale frame identities fail closed rather than
submitting an unrelated or previous eye. No new engine-layout offsets or UI
scanner exceptions are introduced.

A rejected request leaves the previous rendering method running. If a required
capability disappears while Mono is already active, submission stops with an
in-game status; select a supported method to exit through the same retirement
transaction. An unsupported saved mode ID is preserved and explicitly reported,
not silently converted to another mode.
While awaiting a valid scene, a ready Mono backbuffer is still cleared to avoid
desktop feedback/flicker. No new clear work is queued during resource retirement.

## Startup and live switching

ImGui, config reload and Lua/API requests all use the same transition:

1. Stop new component copies and wait for an in-flight Native Fix OpenXR worker
   to finish, if present.
2. Poll existing DX12 consumer fences, or an immediate-context DX11 event query.
   A changed/cancelled request cannot reuse an earlier retirement acknowledgement.
3. Commit the method and generation together at a game-frame boundary.
4. Rebuild component resources and projection history, clear old parity/capture
   state, and wait for a fresh matching main-view pose, projection and scene.

Starting with Mono selected uses this same transaction; it does not require
manually selecting another renderer first. Live transitions do not intentionally
require reinjection. They can remain waiting while gameplay/rendering is stopped
or required evidence is unavailable. A paused game may need to resume to finish
the switch. DX11 retains source ownership through image reuse and retires copies
before rebuilding; DX12 retains each source through its image's command fence.

## Verification and first HMD test

Offline checks cover stable IDs, old-mode policy outcomes, unknown profile values,
startup/live/config transitions, rapid cancellation, concurrent method/generation
publication, geometry/crops and stale-frame rejection. Geometry fixtures include
the captured WMR 0.4-degree inter-eye calibration, yaw/pitch/roll, asymmetric FOV,
quaternion signs, stage/view-space agreement, pixel rounding and horizon rejection.
DX11 and DX12 WARP tests
execute the actual copy helpers and read back both output regions across resize,
including invalid-source rejection. DX12 also tests state restoration and checks
the debug message queue when the debug layer is available. Frontend ID tests run
for both DIBR and non-DIBR option lists.

These checks are **not** HMD/game validation. Before treating the mode as stable:

1. Record a working Native/Synced baseline in a DX11 game and a DX12 game.
2. Switch into Mono during gameplay; check current identical scene content,
   tracking, head turns, UI, spectator, controller attachments and recentering.
3. Switch back to Native, Synced/Ghost and DIBR where supported; confirm their
   saved settings and rendering recover. Repeat several times, including quick
   cancelled requests and config reload.
4. Test injection with Mono saved, menu/pause/inventory, level travel, resolution
   changes and session loss/re-entry. Repeat with One Frame Lag on and off.
5. Export Hook Provenance/diagnostics if a transition remains waiting. The support
   bundle includes requested/effective mode, generation and Mono status. Compare
   CPU/GPU timings at identical resolution/settings before judging performance.

## Attribution

The feature concept and single-centered-view integration are adapted from
[Noniv's UEVR Mono proposal, praydog/UEVR PR #442](https://github.com/praydog/UEVR/pull/442)
(reviewed revision `566ffc4fb52ffdd73f00c0b49a869470e8b3d9e3`). This fork adds separate
DX11/DX12 copy/lifetime paths, stable ID 5 alongside DIBR, guarded transitions,
frame/geometry validation, frontend integration and offline tests rather than
applying the upstream patch wholesale. Existing project licensing applies.
