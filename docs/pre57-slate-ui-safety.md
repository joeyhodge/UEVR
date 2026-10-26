# Pre-5.7 Slate UI discovery and resource safety

This initial implementation targets UE5.5/5.6 DX12. Existing title adapters keep
priority. UE5.4's immediate-command-list adapter, UE5.7/5.8, rendering methods,
alpha policy and input bindings are unchanged.

## UI resource checks

The pre-5.7 Slate validation, retained-target maintenance and dedicated-UI
presentation paths interpret small pointer getters instead of executing guessed
virtual slots. The interpreter only reads members, models a private stack and
follows bounded null branches. It rejects object writes, hidden-output methods,
calls, loops, unsupported instructions and incomplete code. An uninitialized
linked-resource path is a retry/rejection, not permission to call another virtual.

A native-module vtable alone does not identify ID3D12Resource. The read-only path
queries that interface before GetDesc, checks the active device, and rejects
ambiguous native resources. UI promotion additionally requires the trusted
extent, a supported single-sample/single-slice BGRA target and a distinct native
scene resource. Two different RHI wrappers for the same scene resource do not
qualify. Unsupported HDR/array/MSAA targets need their own conversion contract.
The same validation runs at the registration write boundary, before changing
RDX, so an alias introduced after promotion cannot redirect Slate into the scene.

Presentation keeps one validated native COM reference for the current callback
and does not fall back to the historical SDK getter when UI validation rejects
a target. This does not introduce a new GPU copy or change existing adapters'
submission/retirement contracts. Non-UI discovery callers keep their old path.
Rejected UI is also excluded from spectator, alpha and clear passes; stale
contexts remain retained rather than being freed while a copy may be in flight.

Glory also cancels a superseded synthetic-UI request after a validated viewport
UI promotion, using the existing guarded cancellation path. Its saved working
run showed a late allocation failure clearing that already-promoted target.
Resetting the in-flight generation rejects late completions without destroying
the active UI reference or changing the other games' producer selection.

## Optional observation

Compatibility checkbox: `Observe UE5.5/5.6 Slate UI (Experimental)`.

Config key, off by default:

```ini
VR_Compatibility_Pre57SlateUIObservation=true
```

This option is diagnostic only. It does not redirect UI, create a render target,
set CVars, change alpha, or select a rendering mode. Leave it off for an initial
working-game comparison. Existing adapters reuse their existing callback; they
never receive a second observation hook at the same callsite.

For an unknown game, discovery requires complete unwind-owned caller and callee
ranges, restored original hook-entry bytes, one exact output-name transaction and
a complete recognized raw-registration code contract. It uses no per-EXE RVAs or
JSON input. A different compiler, pooled/hidden-return ABI, inlined variant,
missing owner or ambiguous candidate remains unsupported; no nearest-call guess
is installed. Scanning happens once outside the Slate draw callback. A missing
DrawWindow does not consume that attempt. Toggle off/on to retry discovery.

Runtime observations are limited to one per second and contain values only.
They never retain an unknown engine pointer for a future call or publish a GPU
resource. Hook Provenance reports discovery, rejection, texture/device/scene
relations and missing ownership proofs. "Distinct engine texture" is only a
candidate, not a Ready state. A known scene alias requires a separately validated
owned-target adapter.

The batch-selection helper requires a compiled full element stride and a pinned
main window/viewport identity, examines every bounded input, and rejects duplicate
matches. It is not wired to guess unknown batch layouts or promote the first
window. Existing title-specific batched adapters are unchanged.

## Limits and testing

This is not universal automatic dedicated UI. Unknown-game redirection still
needs positive window, pooled-role/content, command-queue and GPU-retirement
contracts. A retained FRHITexture alone is not sufficient evidence. No new route
may be activated just because a texture is readable or has the expected size.

Tests cover original shipping Glory code, synthetic MSVC/Clang getter patterns,
hook/owner/ABI mutations, descriptor/device/native-alias rejection, batch
selection and observation invalidation on scene/window/device/mode/extent changes.
The hardware lifecycle suite also checks real WARP resources, devices, queues
and DX11 objects against the ID3D12Resource query.

Before propagation, compare a working pre-5.7 title (initially Glory): menus,
in-game HUD, pause, HMD/spectator, ImGui open/closed, level changes and resolution
changes. Offline tests and successful builds are not HMD confirmation.
# Glory status logging

Glory's unchanged trusted-extent, accepted-target, output and routing messages
are suppressed per thread and diagnostic source. Changed numeric snapshots remain
eligible for the existing rate-limited INFO messages. Allocation, promotion,
failure and lifecycle diagnostics are unchanged. Other games keep their previous
logging behavior. These value-only snapshots never skip resource validation or
authorize reuse of a cached pointer.
