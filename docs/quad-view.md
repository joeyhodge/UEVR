# Quad View (experimental)

Quad View is a fourth Rendering Method. Instead of two full-resolution eye
images, Unreal renders four views per frame:

| View     | What it covers                                      | Resolution                        |
| -------- | --------------------------------------------------- | --------------------------------- |
| Wide L/R | the whole per-eye field of view                     | `Quad View Wide Scale` of native  |
| Sharp L/R| a small window around where you look                | `Focus Width/Height` × density    |

UEVR places all four in one atlas render target, then either submits the wide
views plus the sharp windows as two OpenXR projection layers, or composes the
atlas into one normal stereo image. The point is to spend pixels where the eye
can resolve them: the periphery is cheap and low resolution, while the sharp
window keeps text and detail crisp.

With the default settings (wide 0.2, focus 0.35, density 1.0) the atlas costs
about 16% of the pixels of native stereo at 3840×3552. The panel prints the
atlas size and its share of native stereo pixels for the current headset.

The engine-specific workarounds and the issues that needed them are recorded
in [Quad View on UE4 and UE5](quad-view-ue4-ue5.md).

## Requirements

- **OpenXR.** On OpenVR Quad View does not start: the game renders plain
  Native Stereo and the panel says so.
- **D3D12 (preferred) or D3D11.** In both cases UEVR composes the atlas into a
  normal stereo image, so the runtime gets one layer per eye. Both use the
  same compose shader: bicubic wide views, sharpening, edge blending, the
  sharp-area shapes and an optional outline. On D3D12 the compose draws
  straight into the headset's swapchain image.
- **Eye tracking is optional.** When the runtime exposes
  `XR_EXT_eye_gaze_interaction` (for example Pimax Crystal), the sharp window
  follows the gaze. Without it the window stays centred, or where the offset
  sliders put it. Turn *Follow gaze* on and restart the game: the extension is
  requested when the runtime starts.

### Unreal Engine 4

UE4 games need *Allow on Unreal Engine 4 (experimental)*, and should use
D3D12. **UE4 + D3D11 can crash with four views** (D3D11 query error); two
UE4.26 D3D11 games have since run with it, so it is worth a try.

While Quad View runs on UE4 the occlusion queries are turned off as a crash
workaround. That also turns occlusion culling off, so Unreal draws hidden
objects in all four views. *UE4: keep occlusion culling*
(`QuadViewUE4KeepOcclusion`, off by default) leaves the queries on: faster in
dense scenes, but turn it off again if the game crashes.

UE4 + D3D11 was first seen crashing with four views (D3D11 query error).
Use Native Stereo there.

The UE4 path uses float view math, declares four views before Unreal allocates
`LocalPlayer::ViewStates`, gives each view its own stereo pass (1–4) and view
state, and maps views by call order because UE4's `GetViewPassForIndex` cannot
tell the four views apart. While Quad View runs on UE4, occlusion queries and
GPU stats are turned off and the four views start only after 60 settled frames.
These workarounds, and the reason each one exists, are in
[Quad View on UE4 and UE5](quad-view-ue4-ue5.md).

## How to enable

1. Use the OpenXR runtime.
2. In the UEVR menu, set **Rendering Method** to **Quad View (Experimental)**.
3. Start with D3D12.
4. Adjust the sharp window and the wide views; watch the atlas line in the
   panel to see the pixel cost.
5. Optional: enable *Follow gaze* under *Sharp views* and restart the game.

Quad View turns itself off while Extreme Compatibility Mode, Native Stereo
Fix, SceneView compatibility, Splitscreen compatibility or stereo emulation is
active, and it does not use the depth buffer (the atlas layout is not the
stereo layout the runtime would reproject with).

## Settings

### Sharp views (where you look)

| Setting | Default | Description |
| --- | --- | --- |
| Width / Height with eye tracking | 0.35 | Share of the eye's field of view covered by the sharp window while it follows the gaze. |
| Width / Height without eye tracking | 0.5 / 0.45 | Used when the runtime started without eye tracking (or *Follow gaze* is off). The window cannot follow the eyes, so it is larger. Chosen once per session, so a blink never resizes the views. |
| Pixel density | 1.0 | Sharp window resolution relative to the headset's native density. |
| Offset left/right, down/up | 0 | Moves the window for trackers that sit slightly off-centre. |
| Follow gaze | on | Use `XR_EXT_eye_gaze_interaction` to place the window. |
| Gaze smoothing | 0.15 ms | Low-pass time constant for the gaze. The default is close to raw (a median of three still rejects spikes); raise it to a few ms for a noisy tracker. `0` follows the raw gaze. |
| Gaze dead zone | 0 (off) | Tangent units (~1.7°) the gaze must move before the window follows. |

The gaze filter averages the eye-tracker signal over the smoothing time (with a
median of three to reject single-frame spikes), so tracking noise is smoothed
without the window trailing a saccade. A dead zone can hold the window for small
movements. When the gaze is lost (a blink, a dropout) the window holds its
place for 500 ms, then eases back to the centre over 150 ms instead of jumping.

### Wide views (periphery cost)

| Setting | Default | Description |
| --- | --- | --- |
| Resolution | 0.2 | Wide view size as a share of the headset's per-eye resolution. |
| LOD scale | 0.5 | Unreal LOD bias for the wide views. Nanite ignores it. |
| Cull objects smaller than | 0 (off) | Sets `foliage.MinimumScreenSize` for the wide views; the sharp area keeps small objects. |
| Also cull small actors | off | Experimental: hides small actors in the wide views through `HiddenActors`. |
| CPU particles | 1.0 | `r.EmitterSpawnRateScale` for Cascade particles. It is a whole-image setting, so it also thins particles in the sharp window. |

### Blending

| Setting | Default | Description |
| --- | --- | --- |
| Compose into one image | on | Compose the atlas into one stereo image and send the normal single-layer submit. |
| Edge blend | 0.2 | Share of the sharp window that fades into the wide view. |
| Sharp area shape | Rounded rectangle | Rectangle, rounded rectangle or ellipse. |
| Sharpen the sharp views | 0.5 | Contrast-adaptive sharpening of the sharp window, 0 to 1. `0` turns it off. |
| Smooth wide-view upscaling | on | Bicubic (Catmull-Rom) upscaling of the wide views. They are stretched about five times at the default resolution, and bilinear filtering shimmers. |
| Show sharp area outline | off | Green outline for tuning the window. |
| Remove vignette | on | Sets `r.Tonemapper.Quality` while Quad View runs so each sharp view does not get its own dark edges. |
| Share exposure across the four views | on | Freezes `r.DefaultFeature.AutoExposure` and `r.EyeAdaptationQuality` so the wide and sharp views keep the same brightness. |
| Brightness | 1.0 | Multiplier for the composed image (D3D11 and D3D12); shown and applied only when exposure is shared. |
| Render sharp views first | off | Renders the sharp pair as views 0/1. Workaround for games whose Groom hair only draws in the first two views. |

### Debug

- **Show wide views only** hides the sharp views to inspect periphery quality.
- **Submit the sharp layer only** sends just the sharp windows.
- Log lines are prefixed with `[QuadView]` (D3D11 compose logs with
  `[QuadView][D3D11]`).

## Known limitations

- The feature is experimental and is not enabled by default.
- OpenVR is not supported; it falls back to Native Stereo.
- UE4 support is opt-in and should use D3D12; UE4 + D3D11 crashes with four
  views. See [Quad View on UE4 and UE5](quad-view-ue4-ue5.md) for the full
  engine-specific list.
- Foveated rendering plugins (for example Cheeky Foveated DLSS) fight Quad View
  over the views and render targets. When Quad View is the selected Rendering
  Method, UEVR skips plugins whose file name contains `foveat`.
- Games whose post-process effects only run for the first two views may need
  *Render sharp views first*.
- Nanite meshes ignore the wide-view LOD scale; their detail follows the wide
  views' resolution instead.

## Configuration keys

The settings are stored in `config.txt` like other UEVR options:

`VR_RenderingMethod=3`, `QuadViewWideScale`, `QuadViewFocusWidth`,
`QuadViewFocusHeight`, `QuadViewFixedWidth`, `QuadViewFixedHeight`,
`QuadViewFocusDensity`, `QuadViewFollowGaze`,
`QuadViewGazeDeadZone`, `QuadViewGazeSmoothingMs`, `QuadViewWideLODScale`, `QuadViewSharedExposure`,
`QuadViewExposureBias`, `QuadViewEdgeBlend`, `QuadViewShowOutline`,
`QuadViewCompose`, `QuadViewFocusShape`, `QuadViewOffsetX`, `QuadViewOffsetY`,
`QuadViewWideMinPixels`, `QuadViewCullSmallActors`, `QuadViewCPUParticles`,
`QuadViewSharpFirst`, `QuadViewAllowUE4`, `QuadViewUE4KeepOcclusion`, `QuadViewDisableVignette`,
`QuadViewDebugWideOnly`, `QuadViewDebugSharpOnly`, `QuadViewSharpen`,
`QuadViewWideBicubic`.
