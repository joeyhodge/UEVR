# Dispatch world-locked movies (experimental)

This opt-in adapter addresses the AdHocMedia story movie behind Dispatch's
already-working dedicated menus. It does not convert video to stereoscopic 3D.
The original game draw and desktop presentation remain intact; OpenXR receives
one opaque, world-locked movie quad visible to both eyes, followed by the normal
dedicated game UI and framework layers.

## Test settings

- Runtime: OpenXR; rendering method: Native. Native Fix may remain enabled.
- Compatibility: enable `Dispatch World-Locked Movies (Experimental)`.
- Leave `2D Screen` off. The old Dispatch Auto2D writer is removed, including
  its media-player polling and widget fallback. Manual 2D is never overwritten.
- Movie placement uses UI distance, height/size and X/Y offsets, but always
  stage space, even if UI Follow View is enabled. Menu layer settings are not
  changed. The full decoded aspect ratio is preserved, not cropped per eye.
- Synced/AFR, Mono, DIBR, stereo emulation and OpenVR are excluded. An unsupported
  executable or source leaves the original renderer in control.

## Ownership and cost

Two validated native hooks observe the media publisher and its post-process
callback. Enabling in an existing movie/world uses one bootstrap pass, at most
1,024 object slots per game tick, ending when the exact world subsystem is
found or the captured array bound is reached. It never enables the global
UObject hooks, periodically scans widgets or invokes media-player getters.
Subsequent media switches come from native publisher notifications.

Shutdown disables the native entry patches before renderer teardown. Original
game calls do not hold a hook mutex across Present; immutable trampolines are
retained through process/CRT teardown rather than freed beneath an in-flight call.

The source is matched to the subsystem, world, media source/player, MediaTexture weak index/serial
and extension vtable. Destruction flags are rejected. The renderer publishes a
leased native resource with an epoch and bounded callback freshness. Pausing
the decoder does not invalidate a still-rendered movie; `IsPlaying` is not a
condition. Source/world/mode changes invalidate the epoch. DX12 GPU references
remain owned by each destination image until its copy fence retires.
Movie XR image waits are nonblocking. A timeout retains the same acquisition
for the next attempt; it never acquires a second image or releases before a
successful wait. Movie cleanup polls normally and uses only bounded GPU drains
during teardown. An unproven acquisition/retirement retains the complete context
instead of inheriting the general copy path's unbounded waits.

Movie swapchains are independent of scene, Slate and ImGui swapchains. They are
reused at unchanged dimensions; a replacement is committed only after complete
creation/enumeration and safe retirement of the previous context. Allocation
failures are cached for the current media epoch. An unproven DX12 submission
retains its image, source and command context rather than freeing GPU references.

Only after a current copy and successful image release is an opaque BOTH-eye
stage-space movie quad eligible to replace that frame's movie-bearing projection
layer. Normal game/framework UI stays above it. No game render target is cleared,
no final-backbuffer screenshot is used, and no persistent rendering mode changes.

## Reviewed binary contract

Dispatch-Win64-Shipping.exe, UE4.27.2, PE timestamp 1769654770, image size
0x489d000; SHA-256 observed during analysis:
`af9e2f7a877f795fa6db7a66b0ef0e73339b7c87b278146bf45cb7457233a24e`.
The adapter verifies PE identity and native instruction/vtable contracts; it
does not hash the whole executable during gameplay. A future game update needs
fresh validation rather than speculative offsets.

| Native function | RVA | Evidence |
| --- | --- | --- |
| ActiveMediaObjects publisher | 0xd17b10 | Copies 0x60 bytes; updates extension at subsystem +0xa0 |
| Movie post-process callback | 0xd15220 | Five-argument member/sret ABI, 24-byte screen-pass result |
| D3D11 native resource getter | 0xc1c8e0 | Texture pointer at RHI +0x90; no virtual probing |
| D3D12 PresentInternal | 0x14650c0 | Called after viewport flush and execute-task wait |
| CResourceState::GetSubresourceState | 0x1454a00 | Uniform validity bit or inline/allocated per-mip state |
| Command-context manager accessor | 0x1407370 | Default manager on owning device +0x38 |
| Manager calibration accessor | 0x14071f0 | Default D3D12 queue at manager +0x28 |
| D3D12 texture native getter | 0x1454210 | FD3D12Resource at RHI +0xc0, native resource +0x20 |

DX12 requires the exact post-flush native Present scope, matching COM swapchain
identity, matching resource/viewport device, default graphics queue and a single
GPU node. Only committed BGRA textures are supported; placed heaps require an
additional heap lease and are rejected. The current mip-zero state is read from
the validated resource state layout at that boundary and must be shader-read.
It is checked again after acquiring the XR image. The copy transitions/restores
only mip zero, never assumes that the remaining media mips share its state, and
uses the same queue as the game and OpenXR graphics binding.

## Verification

Offline checks cover executable rejection, complete mode gating, weak identity,
stale epochs, paused republishing, aspect preservation and DX12 state decoding.
WARP DX11/DX12 tests copy spatially distinct pixels from three-mip media sources
to single-mip outputs, including alpha-zero pixels and every border. DX12 debug
validation checks mip-zero restoration while other mips have different states.
Mocked XR checks cover opaque BOTH-eye stage-space layer eligibility, ownership
transfer, failed creation/enumeration and rollback.

Build/test success is not HMD confirmation. Test DX11 and DX12 independently:
playing, pause/menu, resume, movie-to-movie and movie-to-nonmovie transitions,
main menu/load, manual 2D, feature disable/re-enable, recenter, ImGui open/closed
and shutdown. Verify no head-locked duplicate remains behind the movie, both eyes
see the same video, menus stay interactive and the desktop remains unchanged.
