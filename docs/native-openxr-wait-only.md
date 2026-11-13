# Native OpenXR asynchronous wait only

This separate per-game experiment is **off by default**. In VR > Unreal, expand
**Native OpenXR Wait Only**. Its config key is `VR_NativeOpenXRAsyncWaitOnly`.
Loading a profile without this key or resetting defaults leaves it off.

## Scope

- D3D12, OpenXR, exact Native Stereo and Very Late synchronization only.
- Native Fix and Same Stereo Pass may remain enabled. The option never changes
  either, renderer view identities, shadows, image copies or swapchains.
- Leave both Texture-Array Submit and Async Wait/Pre-Acquire off. The existing
  pre-acquire selection remains authoritative, including its normal fallback.
- Synced/AFR, Mono/DIBR, stereo emulation, split-screen, 2D screen, compatibility
  modes and pending mode transitions do not enable wait-only. Dune, legacy
  STALKER 2 and NASCAR retain their title-specific pacing.

The normal double-wide path is used. This is **not** Same Pass-compatible array
submission and does not loosen the array mode's existing Same Pass restriction.

## Frame and lifetime ownership

One persistent, owned worker handles a captured immutable operation and runtime.
Only the array operation can acquire an image. Wait-only reserves the next wait
under the runtime frame lock and uses the existing synchronized frame publisher.
It refuses to advance the clock underneath already-published next-frame poses.

A delayed or cancelled queued worker cannot let the next pose use an old clock:
the normal consumer can fulfill that reservation under the same runtime lock.
The stale worker then skips its wait. This necessary synchronization may remove
the performance benefit in some games; it is not bypassed to improve a graph.

Turning the checkbox off cancels future work, not a running `xrWaitFrame`.
Completed waits remain published for normal begin/end consumption. Session
events, resolution reconfiguration and teardown invalidate stale reservations;
the worker is joined before its runtime and callback owner are destroyed.

The status and worker/normal-consumer completion counters distinguish real worker
progress from eligibility, queued work and fallback. Support diagnostics export
these counters. No per-frame log spam or additional expensive diagnostics are
enabled. An exceptional worker failure requires OpenXR reinitialization; an
ordinary rejected runtime wait can be re-armed by toggling off/on after recovery.

## Validation and limits

Offline tests exercise the production eligibility, dispatcher, handoff and
owned worker, including cancellation, competing normal consumers, stale runtime
generations, reset, retry and changing request operations. The existing WARP
array-copy and retirement tests remain enabled. These tests do not substitute
for an actual game/HMD or prove a frame-time improvement.

First compare OFF/ON with Native Fix ON, Same Pass ON and arrays/pre-acquire OFF,
keeping resolution, runtime, upscaler, refresh rate and sync stage unchanged.
Check head-motion stability, both-eye shadows, menus, loading, focus changes,
resolution changes and repeated mode entry/exit. Afterwards test ordinary Native
with Native Fix OFF. Measure critical-path timing and HMD delivery, not just
aggregate RHI wait time. A GPU-bound scene may see no improvement.

No private engine offsets, external plugin, CVar overrides, extra frame buffering
or changes to existing rendering-method IDs are introduced.

References:
- [Khronos frame submission guide](https://github.com/KhronosGroup/OpenXR-Guide/blob/main/chapters/frame_submission.md)
- [xrWaitFrame](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitFrame.html)
