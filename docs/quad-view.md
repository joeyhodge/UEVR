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

With the default settings (wide 0.2, focus 0.25, density 1.0) the atlas costs
about 11% of the pixels of native stereo at 3840×3552. The panel prints the
atlas size and its share of native stereo pixels for the current headset.

## Requirements

- **OpenXR.** OpenVR receives only the wide views, so Quad View is not useful
  there.
- **D3D12 (preferred) or D3D11.** In both cases UEVR composes the atlas into a
  normal stereo image, so the runtime gets one layer per eye. The composed
  image supports edge blending and an optional sharp-area outline.
- **Eye tracking is optional.** When the runtime exposes
  `XR_EXT_eye_gaze_interaction` (for example Pimax Crystal), the sharp window
  follows the gaze. Without it the window stays centred, or where the offset
  sliders put it. Turn *Follow gaze* on and restart the game: the extension is
  requested when the runtime starts.

### Unreal Engine 4

UE4 games need *Allow on Unreal Engine 4 (experimental)*, and should use
D3D12. **UE4 + D3D11 is known to crash with four views** (D3D11 query error).
Use Native Stereo there. The UE4 path also waits for occlusion queries to
settle before it starts the four views.

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
| Width / Height | 0.25 | Share of the eye's field of view covered by the sharp window. |
| Pixel density | 1.0 | Sharp window resolution relative to the headset's native density. |
| Offset left/right, down/up | 0 | Moves the window for trackers that sit slightly off-centre. |
| Follow gaze | on | Use `XR_EXT_eye_gaze_interaction` to place the window. |
| Gaze smoothing | 8 ms | Low-pass time constant that averages eye-tracker noise. `0` follows the raw gaze. |
| Gaze dead zone | 0 (off) | Tangent units (~1.7°) the gaze must move before the window follows. |

The gaze filter averages the eye-tracker signal over the smoothing time (with a
median of three to reject single-frame spikes), so tracking noise is smoothed
without the window trailing a saccade. A dead zone can hold the window for small
movements, and the window recentres after 500 ms without valid gaze.

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
| Sharp area shape | Rounded rectangle | Rectangle, rounded rectangle or ellipse (D3D12 only; D3D11 uses a rectangle). |
| Show sharp area outline | off | Green outline for tuning the window. |
| Remove vignette | on | Sets `r.Tonemapper.Quality` while Quad View runs so each sharp view does not get its own dark edges. |
| Share exposure across the four views | on | Freezes `r.DefaultFeature.AutoExposure` and `r.EyeAdaptationQuality` so the wide and sharp views keep the same brightness. |
| Brightness | 1.0 | Multiplier for the composed image; only shown when exposure is shared. |
| Render sharp views first | off | Renders the sharp pair as views 0/1. Workaround for games whose Groom hair only draws in the first two views. |

### Debug

- **Show wide views only** hides the sharp views to inspect periphery quality.
- **Submit the sharp layer only** sends just the sharp windows.
- Log lines are prefixed with `[QuadView]` (D3D11 compose logs with
  `[QuadView][D3D11]`).

## Known limitations

- The feature is experimental and is not enabled by default.
- OpenVR shows only the wide views.
- UE4 support is opt-in; UE4 + D3D11 crashes with four views.
- D3D11 composes with a rectangular sharp area; the rounded and ellipse shapes
  need D3D12.
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
`QuadViewFocusHeight`, `QuadViewFocusDensity`, `QuadViewFollowGaze`,
`QuadViewGazeDeadZone`, `QuadViewGazeSmoothingMs`, `QuadViewWideLODScale`, `QuadViewSharedExposure`,
`QuadViewExposureBias`, `QuadViewEdgeBlend`, `QuadViewShowOutline`,
`QuadViewCompose`, `QuadViewFocusShape`, `QuadViewOffsetX`, `QuadViewOffsetY`,
`QuadViewWideMinPixels`, `QuadViewCullSmallActors`, `QuadViewCPUParticles`,
`QuadViewSharpFirst`, `QuadViewAllowUE4`, `QuadViewDisableVignette`,
`QuadViewDebugWideOnly`, `QuadViewDebugSharpOnly`.
