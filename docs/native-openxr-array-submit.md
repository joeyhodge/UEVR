# Experimental Native OpenXR texture-array submission

The existing Texture-Array Submit and Async OpenXR Wait/Pre-Acquire options stay
default-off. They require D3D12, OpenXR, Native Stereo and Native Fix, with Same
Stereo Pass off. Existing title exclusions still apply. There is no UE-version
allowlist and no game profile or CVar is changed automatically.

## Allocation and fallback

Eligibility uses Framework's actual renderer, not VR's cached first-frame
renderer flag. The two-slice swapchain can be created during initialization or
added after a mid-game enable. Adding it does not recreate or discard the
working double-wide, UI or depth swapchains. A compatible allocation is retained
for reuse when the option is disabled and enabled again.

Allocation is transactional: images, device identity, dimensions, format family
and copy contexts must validate before readiness is published. An allocation
failure leaves ordinary submission intact. Attempts are capped at three per
request/session/extent, with backoff. Off/on explicitly re-arms an exhausted
request; a real session or extent change gets its own budget. A failed array
never requests a repeating full-swapchain rebuild.

Normal resolution/session rebuilds still own GPU retirement and destruction.
An incompatible existing array is not replaced opportunistically while in use.

## Copy and async readiness

Both eye copy boxes must cover the destination extent and belong to compatible
single-sample color format families. BGRA typeless and SRGB resources are
compatible; unrelated formats or guessed crop/scale conversions are not.
Sources stay retained until the copy context's GPU work retires.
An unproven recorded GPU submission blocks reuse of that array generation. Its
sources, destinations and command allocators are retained through teardown
rather than destroyed without a retirement proof; ordinary submission remains
the fallback. A process-lifetime retention can therefore occur after a GPU or
fence failure, rather than risking use-after-free.

The array is selected for projection only after the current OpenXR frame's
copy was recorded, enqueued and released successfully. A stale or failed copy
uses the existing double-wide path instead. Allocated is not the same as copied:
the menu distinguishes allocation readiness from a validated array submission.
The current array path submits color only; it does not reinterpret the ordinary
packed depth swapchain as array depth. Double-wide depth behavior is unchanged.

Async wait/pre-acquire requires a ready array and a validated array copy. It
does not run just because both boxes are checked. The worker rechecks readiness
before acquisition, and an unused acquisition is released on disable/fallback.
This avoids reporting normal fallback while an unwanted async wait still runs.
An image-wait timeout retains that acquisition without copying or releasing it.
Later attempts wait on the same image; a successful wait is required before any
write or release. The render path uses a zero-duration image wait and can keep
the double-wide fallback while the image is unavailable. Disabling the option
continues nonblocking cleanup until an unused image can legally be released.

## Verification

The native-openxr-array-tests target covers all scope-gate combinations, bounded
retry/re-arm/session/extent behavior, current-frame selection and invalid copy
descriptors. A WARP readback checks distinct pixels in both array slices,
including typeless-to-SRGB compatible copies, with D3D12 debug-layer checks when
available. Both CI workflows explicitly build the registered test executable.

Real OpenXR/HMD testing is still needed: enable while injected, disable/re-enable,
start with the option saved on, test scene/resolution changes, then test async
separately. These options do not guarantee an FPS improvement and keep the
double-wide allocation for safe fallback.
