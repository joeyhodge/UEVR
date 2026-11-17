# Quad View on UE4 and UE5

Quad View renders four Unreal views per frame instead of two. UE4 and UE5
expose the engine's stereo API in different ways, and the feature needed a
separate set of fixes for each. This page records the problems found while
bringing the feature up on both engines, what was changed to fix them, and what
is still unsupported. Player-facing requirements and settings are in
[quad-view.md](quad-view.md); game-specific Native Stereo and Native Fix
compatibility is covered by the other pages in `docs/`.

## At a glance

| Area | UE5 | UE4 |
| --- | --- | --- |
| View identity | The stereo hooks receive the view index 0–3 (`INDEX_NONE` for a mono pass). | The hooks receive the eye pass, not the view index. UEVR reconstructs the view index from call order. |
| Primary view | View 0 is primary; views 1–3 are secondaries. | Pass 1 is primary; passes 2–4 are secondaries. |
| View states | Four states, allocated by `LocalPlayer` from `GetDesiredNumberOfViews` during `PostInitProperties`. | Same; UE4.25/4.26 may need a one-off `PostInitProperties` replay for their legacy `ViewStates` array. |
| View math | Double precision (Large World Coordinates). A missed heuristic is corrected for 5.0–5.3 and Windrose on UE5.6+. | Float vectors and matrices, through the existing UEVR paths. |
| Opt-in | None. | *Allow on Unreal Engine 4 (experimental)*, off by default. |
| Queries | Untouched. | `r.AllowOcclusionQueries` and `r.GPUStatsEnabled` are turned off while Quad View runs; the four views start after 60 settled frames. |
| Graphics API | D3D12 preferred, D3D11 compose supported. | D3D12; UE4 + D3D11 is a known crash with four views. |
| Depth submission | Off. The atlas is not the stereo layout the runtime would reproject with. | Off, same reason. |

Only the OpenXR runtime receives the composed stereo image or the optional
sharp-window overlay. OpenVR still receives only the wide views.

## How the four views reach Unreal

The portable slot table is in `src/mods/vr/QuadView.hpp`:

| Slot | View |
| --- | --- |
| 0 | left wide |
| 1 | right wide |
| 2 | left sharp |
| 3 | right sharp |

`slot_for_view()` maps a zero-based view index to the slot and also accepts
one-based indices for callers that already have them. Quad View passes
zero-based indices on both engines; UE4 recovers them from call order.
`QuadViewSharpFirst` swaps the two pairs so the sharp views are rendered first.
Every view of a frame uses one head-pose sample. The atlas layout and both
sharp windows are captured with that sample and stored with the frame; the
game thread (view rects and projections) and the render thread (compose and
submit) look them up by frame number. That is what keeps the sharp image and
its window together when a pose or gaze update, or a slider change, lands
between render and submit (see issue 21).

## Issues and fixes

### 1. UE4 rendered only two of the four views

**Symptom.** The game showed the wide pair at low resolution, or the sharp pair
when *Render sharp views first* was on, but never all four.

**Cause.** Unreal sizes `LocalPlayer::ViewStates` once, in
`PostInitProperties`, from `GetDesiredNumberOfViews`. Quad View originally
declared its four views only after the UE4 settle gate opened. By then the
array had been built with two entries, so Unreal only rendered the first pair.

**Fix.** Return 4 from `GetDesiredNumberOfViews` whenever Quad View is
configured and no conflicting mode is active, including while the UE4 settle
gate is still closed. The gate now delays only the rendering, not the
declaration. UE5 already relied on the same early declaration. For UE4.25/4.26,
whose legacy `ViewStates` array can already exist with fewer than four
validated states, UEVR replays `PostInitProperties` once to resize it; an
ambiguous or invalid array fails closed. Fixed in `68084a2` (UE4) on top of
`6762b0d` (UE5).

### 2. The right eye and the sharp views shared one rectangle

**Symptom.** One eye showed the sharp view on black, the other showed no sharp
view, or the sharp pair landed in the wrong eye.

**Cause.** UE4 passes the stereo pass, not the view index, to `AdjustViewRect`,
the view-offset hook and the projection hook. The engine's own mapping did not
give the four views unique values, so the right-eye view and both sharp views
were indistinguishable.

**Fix.** The engine builds the four views of a frame in request order, so the
position of each hook call identifies the view (see the next issue). The scene
view also receives the pass of its eye. UE5 keeps its view indices. The first
attempt (`04833a9`) gave the views passes 1–4; the final mapping landed in
`68084a2`.

### 3. UE4 could not identify the views through `GetViewPassForIndex`

**Symptom.** The stereo hooks received an eye pass, not a unique view value, so
the four views could not be told apart from it.

**Cause.** This UE4 `GetViewPassForIndex` implementation only knows about the
two eye passes (left for view 0, right for the rest), and its view-pass hook is
not present in this vtable. The slot after `GetDesiredNumberOfViews` is not that
function on UE4.

**Fix.** Map the position of the call to the view index. Each stereo hook
keeps its own `quadview::CallOrder` counter, reset at the start of every
viewport draw (see issue 22). Do not install the `GetViewPassForIndex` hook on engines without
double precision; it is only valid on the UE5-side vtable. Fixed in `68084a2`
and `e348c91`.

### 4. The wide view bled into the sharp views

**Symptom.** Sharp views carried temporal history and ghosting from the wide
image; TAA/TSR history looked shared.

**Cause.** UE4 gave every pass-2 view the same `FSceneViewState`. The wide
right view and both sharp views shared one temporal history.

**Fix.** On UE4, hand each of the four views one of the four states Unreal
allocated, and set the stereo pass of the eye that view belongs to. UE4.25/4.26
publishes the four validated states from the legacy array before the views are
built. If the states cannot be validated, the fix is not applied rather than
guessing. Fixed in `68084a2`.

### 5. UE4 crashed on its first four-view frame

**Symptom.** A fatal D3D11 query error (`DXGI_ERROR_INVALID_CALL`, inside
`D3D11Query`) on the first four-view frame or shortly after injection.

**History.** UE4 was first enabled in `9125d78`; its first four-view frame
crashed, which is why the path is now behind an explicit opt-in.

**Cause.** UE4's D3D11 renderer still had occlusion queries and GPU timing
queries in flight while the view count and view states changed.

**Fix.** Three layers:

1. UE4 Quad View is opt-in, so a game cannot enter the path by accident
   (`7860d39`).
2. While Quad View is active on UE4, UEVR sets `r.AllowOcclusionQueries=0` and
   `r.GPUStatsEnabled=0`, and restores the game's values when Quad View is
   turned off (`8a30e1b`, `b4cc500`).
3. The value changes and the switch to four views are separated. UEVR waits
   60 frames after the query cvars are applied before `is_quad_view_active()`
   starts the four views (`3f5f00b`). This is the `QUAD_VIEW_UE4_SETTLE_FRAMES`
   gate.

### 6. UE4 + D3D11 still crashes with four views

**Symptom.** Even with occlusion queries and GPU stats off, the UE4.26 D3D11
bring-up title still died in `D3D11Query` with four views. Native Stereo was
stable in the same build.

**Decision.** The UE4 + D3D11 combination remains parked. The UI warns that
UE4 with D3D11 is known to crash and recommends Native Stereo; documentation
recommends D3D12 for UE4. This is a known limitation, not a hidden workaround.
Recorded in `9f45aad`.

### 7. A mono pass was projected as the right sharp view

**Symptom.** A view with `INDEX_NONE` (`-1`) received the sharp projection, so
a mono or full pass could show a sharp-window image.

**Fix.** `quad_view_index_from()` treats negative indices as `-1`; a view that
is not one of the four is left as a mono pass. The *Show wide views only*
debug toggle was added at the same time to inspect the periphery without the
sharp views. Fixed in `3542cc3`.

### 8. D3D11 sent two stacked projection layers

**Symptom.** On Pimax the wide layer kept showing through the sharp layer, and
the sharp window never became a clean replacement. The headset received a wide
pair underneath a sharp pair.

**Cause.** D3D12 had a compositor that turned the atlas into plain stereo;
D3D11 still submitted the wide rects and the sharp rects as two layers.

**Fix.** Compose the atlas on the D3D11 side too: stretch each wide view over
its eye, paste the sharp view where the focus window lies, then use the normal
single-layer submit. A sharp-only debug toggle was added next to the existing
wide-only toggle. Fixed in `d4add86`.

### 9. D3D11 could not render into the swapchain image

**Symptom.** The compose path failed or skipped, and the headset fell back to
the stacked-layer path.

**Cause.** Swapchain images cannot be used as render targets here, and the
formats can be typeless or sRGB.

**Fix.** Compose into an offscreen texture with a render-target view and a
shader-resource view, then `CopyResource` it into the destination. Typeless
formats get a concrete view format. When the swapchain is sRGB, the atlas is
sampled through an sRGB view as well, so decoding and re-encoding cancel and
the composed bytes match the old direct copy instead of coming out brighter.
D3D12 uses plain UNORM/FLOAT views to preserve the same bytes. Fixed in
`d4add86` and `eb29844`.

### 10. D3D11 had a hard-edged sharp window

**Symptom.** The sharp rectangle was cut into the wide image with no fade, and
the optional outline was missing.

**Fix.** A compose pixel shader fades the sharp view into the wide image over
the *Edge blend* share, using the same blend range as the D3D12 compositor, and
draws the same 3-pixel green outline. Fixed in `eb29844`. The two composes
now share one shader (issue 23), so D3D11 also has the rounded and ellipse
shapes that D3D12 gained in `5fe8b7e`.

### 11. Shared exposure made the D3D11 image darker

**Symptom.** With *Share exposure across the four views* on, the D3D11
composed image was darker than the game's own adaptation.

**Cause.** Sharing exposure freezes `r.DefaultFeature.AutoExposure` and
`r.EyeAdaptationQuality`, so the engine no longer chooses the game's normal
brightness.

**Fix.** The pass mapping already makes the first view (view 0 on UE5, pass 1
on UE4) the only primary, so Unreal shares one eye-adaptation history across
the four views. The *Share exposure* option freezes that history with the two
cvars above, so the sharp window and the wide periphery cannot drift apart.
The D3D11 compose applies a brightness multiplier (`QuadViewExposureBias`,
0.5–2.0) to the whole composed image while shared exposure is on. The two cvars
are restored when the option or Quad View is turned off. Fixed in `eb29844`.

### 12. Each sharp view had its own dark edges

**Symptom.** The sharp window had a vignette around all four sides because it
is its own Unreal view.

**Fix.** While Quad View is active and *Remove vignette* is on, drop
`r.Tonemapper.Quality` below 2 (to 1), then restore the game's value when the
option or Quad View is turned off. Fixed in `d2506e5`.

### 13. Sharp content was rendered for one window and placed with another

**Symptom.** The sharp window flickered or shifted by the gaze delta on the
UE4 path. The content stayed sharp, but it was offset from the wide image for a
frame or two.

**Cause.** The sharp projection runs in `AdjustViewRect` /
`CalculateStereoProjectionMatrix`; the compose runs later in the present. On
UE4 those can see different pose samples. A global per-frame latch could also
be overwritten by the next frame's `AdjustViewRect` before the compose ran.

**Fix.** First a per-frame latch, then the frame's submit focus, fixed in
`396b8e9`, `eb29844` and `d84f567`. The submit focus still let the game thread
read the render thread's frame; issue 21 replaced both with a per-frame
snapshot.

### 14. Groom hair only drew in the first two views

**Symptom.** Games whose Groom hair runs only for the first two views lost hair
in the later views.

**Fix.** *Render sharp views first* renders the sharp pair as views 0/1. The
first view of each eye in render order now owns the pose sample and the
once-per-eye side effects, so the option works without a second pose path. The
left sharp view then drives exposure and shadows, which can look different;
the UI says so. Fixed in `8a30e1b`.

### 15. Foveated-rendering plugins fought Quad View

**Symptom.** A plugin such as Cheeky Foveated DLSS competed for the views and
render targets.

**Fix.** When `config.txt` selects Quad View, the plugin loader skips DLLs
whose file name contains `foveat` and lists them in the plugin errors with how
to get them back. Fixed in `7860d39`.

### 16. The wide-view LOD field moved between engine versions

**Symptom.** The wide views could not be biased to simpler LODs, or the wrong
field was changed.

**Cause.** `FSceneViewInitOptions::LODDistanceFactor` moves between engine
versions.

**Fix.** Find it at runtime from the fields around it — `CursorPos`,
`LODDistanceFactor` and `OverrideFarClippingPlaneDistance` — require a unique
match, and only trust the offset after it has been stable for 60 views. If the
layout changes under it, the feature switches itself off. Nanite meshes ignore
the bias; their detail follows the wide views' resolution instead. Fixed in
`b82e53d`.

### 17. Actor culling crashed during level loads

**Symptom.** A crash in `UEVRBackend` during a level load with *Also cull small
actors* on.

**Cause.** The actor list and bounds were tracked across a world teardown, and
the object scan used names on half-loaded objects.

**Fix.** Track the world; when it changes, drop every candidate, reset the scan
cursors and wait about 300 frames before scanning again. Do not cull without a
world or controller. The player controller is resolved in VR's engine tick and
passed in, and the object scan no longer calls `get_full_name()`; class default
objects have empty bounds and are never hidden. Fixed in `00eb1f5` and
`b77e859`; the experimental wide-view culling itself was added in `ec0fd8a`.

### 18. The gaze window was noisy, slow or jumped

**Symptom.** With eye tracking on, the sharp window hopped over the wide image
on tracker noise, trailed a saccade, or flickered when the render and compose
disagreed.

**Fix.** The final filter is a median of the last three samples (one-frame
spikes never pass) followed by a low-pass whose time constant is exposed as
*Gaze smoothing* (`QuadViewGazeSmoothingMs`, default 8 ms, 0–100 ms). It uses
the frame clock as `dt`, so the feel does not change with frame rate. The dead
zone is optional and off by default (`QuadViewGazeDeadZone`). When the gaze
is lost the window holds for 500 ms, then eases to the centre over 150 ms
instead of jumping (issue 24). `QuadViewOffsetX/Y` shifts the
window for a tracker that sits slightly off-centre, applied wherever the window
is placed (`a9ed075`). The window is stored with each frame, so the sharp
projection, compose and overlay all use the same focus (issue 21). A once-per-second
`[QuadView][Gaze]` line logs the raw and filtered gaze, the window centres and
the age of the tracker sample at the predicted display time, which separates
UEVR's smoothing from headset latency.

The intermediate attempts are worth knowing: the old dead-zone snap made the
window hop; a speed-dependent low-pass stabilised it but felt slow; a
0.02-tangent-per-update cap fixed jumps but added lag and was removed once the
compose was frame-matched. The final design keeps the latency user-controlled
instead of hiding it in the filter. Fixed across `b465889`, `396b8e9`,
`dcea47b`, `e6cb150`, `662bda6`, `e61b7d0` and `b3c523a`.

### 19. Eye tracking changed OpenXR startup for every game

**Symptom.** `XR_EXT_EYE_GAZE_INTERACTION` was requested even when Quad View
was not selected, changing OpenXR startup for unrelated rendering methods.

**Fix.** Request the extension only when Rendering Method is Quad View and
*Follow gaze* is on. Otherwise OpenXR starts exactly as upstream. The setting
is read at runtime startup, so toggling gaze needs a game restart; the panel
says so. Fixed in `50f3eae`.

### 20. Build, shader and CI issues found along the way

- `std::min`/`std::max` needed parentheses against `Windows.h` macros
  (`363f250`).
- The gaze helper had to be defined inside the `runtimes` namespace
  (`08ceab8`).
- Building with the local DXC regenerated two existing alpha-luminance shader
  bytecode includes with no source change. They were restored so the change
  set only adds the new compose shaders (`61360dd`).
- The D3D12 compositor's sharp-only checkbox was wired only to the D3D11
  compose; the D3D12 compositor now skips the wide draws and clears the reused
  output so the sharp views are shown on black (`9c336c0`).
- The new D3D12 compose shaders are compiled into includes and the CMakeLists
  is regenerated for them (`835aa6a`).
- `quad-view-tests` was registered with CTest but not built by CI, so the test
  showed as *Not Run*. The target is now built with the other offline tests
  (`ecbab71`).

### 21. The sharp view could be rendered and composed from different frames

**Symptom.** None reported once smoothing was added; found in review. The
same "sharp content offset from the wide image" as issue 13, whenever the
render thread runs a frame behind the game thread.

**Cause.** The sharp-view projection runs on the game thread for frame N, but
it read the focus of the render thread's current frame, which can still be
N-1. The compose then used frame N's focus for an image rendered with N-1's.
The layout was also recomputed from the live sliders at every call site.

**Fix.** `update_quad_view_gaze` stores the layout and both sharp windows in
the frame's pipeline state (`quadview::FrameSnapshot`). The game thread reads
the snapshot of the frame whose pose it is using (`get_quad_view_frame_game`),
the compose and submit read the frame they present
(`get_quad_view_frame_render`). A slot that no longer belongs to the frame is
ignored. The latch is gone; live values are only a startup fallback.

### 22. UE4 view counting could shift or never reset

**Symptom.** None reported; found in review. Would show as the sharp view
jumping, or as Quad View falling back to plain views, on some UE4 games.

**Cause.** The call-order counters reset when `g_frame_count` changed, but
`g_frame_count` only advances on the view-extension path. Without it the
counters never reset after the first frame. A stereo-pass call earlier in the
frame would also shift the four views by one.

**Fix.** The counters reset at the start of every `UGameViewportClient::Draw`
(the engine frame is the fallback without a draw hook). The three copies of
the index resolution are one helper, `resolve_quad_view_index`, and the
counter is `quadview::CallOrder`, unit tested.

### 23. The D3D11 and D3D12 composes drifted apart

**Symptom.** Different edge fades (linear on D3D11, smoothstep on D3D12),
shapes only on D3D12, and a runtime-compiled D3D11 shader.

**Fix.** One shader, `quad_view_compose.hlsli`, compiled by DXC for D3D12 and
by FXC (`ps_5_0`) for D3D11. It also upscales the wide views bicubically
(`QuadViewWideBicubic`), sharpens the sharp views (`QuadViewSharpen`) and
clamps every tap to the sprite's own atlas rect so filtering never pulls in
the neighbouring view. On D3D12 the compose draws straight into the swapchain
image, saving a full-size copy per frame.

### 24. Losing the gaze made the window jump to the centre

**Symptom.** A long blink or a tracking dropout made the sharp window hop to
the centre in one frame.

**Fix.** The window holds for 500 ms, then eases to the centre over 150 ms.
Gaze that returns during the ease starts again from where the eyes are.
Without eye tracking the window now has its own, larger size
(`QuadViewFixedWidth/Height`, 0.5 x 0.45), chosen once per session.

### 25. Brightness only worked on D3D11, and was applied without shared exposure

**Fix.** The D3D12 compose shader takes the multiplier as a root constant
(`b1`, root parameter 2); D3D12Component builds the compose SpriteBatch with
the shader's own root signature. Both paths apply it only while shared
exposure is on, which is when the panel shows it (`fc5eeb1`).

## Choosing the UE4 or UE5 path

The path is selected by `FFakeStereoRenderingHook::has_double_precision()`.
UE5 uses double-precision view math (Large World Coordinates); UE4 uses float.
The detection reads the hooked view-offset/projection functions and is logged
unconditionally:

```
Double precision (LWC) view math: YES
```

Two UE5 exceptions matter because a wrong verdict silently corrupts every
vector or matrix written back into the engine:

- UE5.0–5.3 thunks can hide the double-precision instructions. UEVR forces the
  double path for those versions (`FFakeStereoRenderingHook::hook`).
- A known UE5.6 game (Windrose) needed the same fallback.

`GetViewPassForIndex` is only hooked when double precision is detected. On UE4
the slot after `GetDesiredNumberOfViews` is not that function, so hooking it
there would patch the wrong code.

UE5.0.3 also passes a negative view index into `GetViewPassForIndex`; the hook
returns `eSSP_FULL` for it, which is what later UE5 versions do themselves.

## What is still unsupported

- **UE4 + D3D11 with four views.** It still crashes in `D3D11Query`; use
  D3D12, or use Native Stereo on D3D11. See *Next steps* below for how to
  narrow it down.
- **UE4 support is experimental and thinly verified.** The bring-up title was
  UE4.26 on D3D11, where the four-view path failed. Treat UE4 + D3D12 as the
  intended path and validate per game.
- **OpenVR.** It receives only the wide views; the sharp window and the
  composed image are OpenXR features.
- **Depth.** Quad View does not submit depth because the atlas layout is not
  the stereo layout the runtime would reproject with.
- **Nanite.** Nanite meshes ignore the wide-view LOD scale.
- **Foveated plugins.** They are skipped when Quad View is selected; switch
  rendering method and restart to use them.
- **Conflicting modes.** Quad View turns itself off while Extreme
  Compatibility Mode, SceneView compatibility, Splitscreen compatibility,
  stereo emulation, AFR or 2D screen is active.
- **Games that need Native Stereo Fix.** The fix only runs with the Native
  Stereo method, so it is never active with Quad View; see *Next steps*.
- **CPU particles.** `r.EmitterSpawnRateScale` is a whole-image setting, so
  lowering it also thins particles inside the sharp window.
- **Shared exposure.** It freezes the engine's auto-exposure and eye
  adaptation. Both composes apply the *Brightness* multiplier to compensate
  (D3D12 through a root constant, since `fc5eeb1`).
- **Sharp-first order.** The left sharp view drives exposure and shadows, so
  lighting can differ from the default order.
- **Per-view post-processing.** The wide views run the same post-processing
  as the sharp ones. Unreal applies post-process settings after the
  FSceneView constructor UEVR hooks, so a per-view trim (ambient occlusion,
  reflections, motion blur in the periphery) would need version-specific
  FSceneView offsets; cvars would change all four views.

## Next steps

### Games that need Native Stereo Fix

`VR::is_native_stereo_fix_enabled()` requires Rendering Method = Native Stereo,
so with Quad View selected the fix never runs, whatever its checkbox says. The
fix is about fifty title- and version-guarded paths in
`FFakeStereoRenderingHook.cpp` (same stereo pass, preserved secondary pass,
texture-array submit, per-title render-target adoption) built around exactly
two views. Making Quad View work on top of it means auditing each path for a
view count of four, per title, with the game running; it cannot be done
blind without risking the titles the fix exists for. Start with one title
that needs the fix, enable Quad View, and follow the first broken view in
the log. The UEVR-Quad-Views-Mono project (two views from one mono camera)
shows the infrastructure can host foveated views.

### UE4 + D3D11 crash

Occlusion queries and GPU stats are already off and the four views start
after a 60-frame settle, yet `D3D11Query` still fails. The remaining query
users in UE4's D3D11 RHI are the frame-timing queries of the GPU profiler,
the event queries used for frame pacing (`r.OneFrameThreadLag`,
`r.FinishCurrentFrame`) and per-view-state queries that a view state
allocated for two views may still hold. To narrow it down with the game:

1. Try `r.OneFrameThreadLag 0` and `r.HZBOcclusion 0` before switching to Quad
   View.
2. Run with `-d3ddebug` to get the D3D11 debug layer's message for the bad
   `GetData` call (which query type, which resource).
3. Check whether the crash follows the view-state swap: with *Render sharp
   views first* the swap order changes.

### OpenVR

The compose does not depend on the runtime: OpenVR could receive the composed
image too, but OpenVR has no eye gaze, so it would only ever be fixed
foveation.

## Verification

The portable parts are covered by `tests/QuadViewTests.cpp` and run by
`quad-view-tests` in CI: slot mapping (zero- and one-based), layout fitting,
focus-window clamping and offsets, gaze filtering (noise averaging, saccade
response, dead zone, spike rejection, recentring), the off-axis projection,
compose rectangles and alpha, focus shapes, LOD offset detection and pixel
share. The rendering paths themselves are checked in the test build; the log
shows the selected path and output state:

| Log line | What it tells you |
| --- | --- |
| `Double precision (LWC) view math: YES/NO` | Which engine path Quad View will use. |
| `[QuadView] AdjustViewRect: atlas …` | The atlas and the four view rectangles for this headset. |
| `[QuadView] UE4: occlusion queries settled, starting the four views` | The 60-frame UE4 settle gate has opened. |
| `[QuadView][D3D11] compose target created …` / `edge blend shader ready` | The D3D11 single-layer compose is ready. |
| `[QuadView][D3D12] Compositor ready: …` | The D3D12 compositor is ready; otherwise it falls back to the overlay layer. |
| `[QuadView][Gaze] mask=… age=…` | Raw/filtered gaze, window centres and tracker sample age. |

These fixes are not a substitute for per-game testing. In particular, UE4
titles still need the opt-in box and should be tried on D3D12.

## Source map

| File | Responsibility |
| --- | --- |
| `src/mods/vr/FFakeStereoRenderingHook.cpp` | View count, pass/index mapping, call order, view states, projection, UE4 query workarounds, LOD. |
| `src/mods/vr/QuadView.hpp` | Portable slot/layout/gaze/projection/compose math and the LOD offset finder. |
| `src/mods/vr/D3D11Component.cpp` | D3D11 offscreen compose and sRGB handling. |
| `src/mods/vr/d3d12/QuadViewCompositor.cpp` | D3D12 atlas compositor, drawing straight into the swapchain image (copy fallback). |
| `src/mods/vr/shaders/quad_view_compose.hlsli` | The compose pixel logic shared by D3D11 and D3D12: bicubic wide views, sharpening, shapes, edge blend, outline. |
| `src/mods/vr/runtimes/OpenXR.cpp` | Gaze sampling, the per-frame focus state, wide/sharp layer submission. |
| `src/mods/VR.cpp` | Settings, cvars, UE4 settle gate, panel. |
| `src/mods/PluginLoader.cpp` | Foveated-plugin skip. |
| `src/mods/vr/QuadViewCulling.cpp` | Experimental wide-view actor culling. |
| `tests/QuadViewTests.cpp` | Offline coverage for the portable parts. |
