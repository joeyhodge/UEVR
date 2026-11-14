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
The paired recorder validates both eyes before emitting any commands. It
transitions each distinct source and the array once, copies slices 0 and 1,
then restores the original source states and the array's RENDER_TARGET state.
Aliased packed sources share one barrier; contradictory states for that same
resource are rejected. Ordinary double-wide copy helpers are not changed.

Array command reuse polls retirement instead of waiting on a CPU fence. An
unretired slot keeps its allocator, source references and acquired image owned;
that frame uses the ordinary fallback. Failed releases are retried by polling,
not by flushing all copy contexts or waiting indefinitely. Once disabled,
cleanup also polls old source references until they can be retired. Actual
swapchain destruction can drain boundedly; it never frees an unproven generation.

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
does not run just because both boxes are checked. Each request pins its runtime,
session generation, wait sequence, display time and array generation. Eligibility
is captured under the frame lock. Pre-acquire uses that same runtime and rejects
stale tickets after frame, session, resolution or option changes; its opportunistic
lock attempts never invert frame/copy lock ordering.
Session-state events and resolution-change frame closure share that frame lock,
so they cannot inspect or retire a half-published worker frame.

Queued work is cancelled when leaving the scope. An already entered xrWaitFrame
cannot be cancelled with a stop token: it drains normally, and cancellation
suppresses its later pre-acquire. Mono transitions and runtime replacement wait
for that ownership to retire. Shutdown explicitly joins the worker before its
owner, condition variable, mutex or copy component is destroyed. Runtime
replacement defers rather than joining an active wait while holding render locks.
Framework shutdown drains before renderer teardown and permanently closes the
worker to late-present requests; the VR destructor repeats that drain defensively.
Worker exceptions disable async pacing until OpenXR is reinitialized. Their
array generation is retained if acquisition/retirement may be unproven, without
disabling ordinary submission or faulting a replacement generation.

An unused acquisition is released on disable/fallback only when safe.
An image-wait timeout retains that acquisition without copying or releasing it.
Later attempts wait on the same image; a successful wait is required before any
write or release. The render path uses a zero-duration image wait and can keep
the double-wide fallback while the image is unavailable. Disabling the option
continues nonblocking cleanup until an unused image can legally be released.
The successful SESSION_LOSS_PENDING result preserves wait/acquire/release
bookkeeping; TIMEOUT_EXPIRED still never grants access despite being positive.

## Verification

The native-openxr-array-tests target covers all scope-gate combinations, bounded
retry/re-arm/session/extent behavior, current-frame selection and invalid copy
descriptors. It also tests blocked-wait shutdown, pinned request lifetime,
cancellation/restart, concurrent publishers/draining, exceptions, stale frame
tickets, pending fences and deferred XR release ownership. The same paired
recorder used in production is exercised with both aliased packed and separate
eye sources. WARP readbacks check distinct pixels in both array slices and
typeless-to-SRGB copies, with D3D12 debug-layer checks when available. The test
prints whether that layer was enabled. `--worker-only` runs the non-GPU lifecycle
tests for repeated stress checks. Both CI workflows already build this test target.

Real OpenXR/HMD testing is still needed: enable while injected, disable/re-enable,
start with the option saved on, test scene/resolution changes, then test async
separately. These options do not guarantee an FPS improvement and keep the
double-wide allocation for safe fallback.

## OpenXR contracts

- [xrWaitFrame](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitFrame.html):
  serialize waits and retain a coherent display time for every frame.
- [xrWaitSwapchainImage](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitSwapchainImage.html):
  a timeout retains the same acquisition; it is not write/release permission.
- [D3D12 image state](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_D3D12_enable-swapchain-image-state.html):
  restore color images to RENDER_TARGET on the session's bound queue.
- [xrDestroySwapchain](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrDestroySwapchain.html):
  submitted GPU commands must complete before destruction, even though ordinary
  image release does not require a CPU fence wait.
