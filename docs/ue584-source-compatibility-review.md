# UE5.8.4 source compatibility review

Reviewed the supplied UE5.8.3 and UE5.8.4 release source archives on 2026-10-24.
This review does not substitute for a game's runtime ABI/resource validation.

## Contracts retained

The archive comparison found 209 added/removed/modified files. A focused set of
108 UEVR-facing source/header files was compared byte-for-byte and by SHA-256;
all 108 are identical between these source releases. This includes the reviewed:

- Scene view/init/family, viewport, local-player and stereo/XR interfaces.
- View-state, temporal processing and exposure interfaces used by stereo modes.
- RHI texture/command-list, RDG builder/resource and pooled-target contracts.
- Slate renderer/input/output and UI resource paths.
- UObject array/item/GC flags, names, reflection and console interfaces.

No new offsets, virtual slots, texture-create ABI, view counts or projection
rules are introduced by the UEVR/UESDK compatibility update.

## Changes considered

- D3D12 enhanced-barrier synchronization/layout fixes, state/vertex/pixel-format
  helper refactoring and shader/root-signature tooling changes. UEVR does not
  call the changed tooling helpers; no engine barrier override is added.
- Renderer fixes in instance-culling batching, Lumen atlas extraction, ray-tracing
  debug/MSAA and virtual-texture updates. These do not change the reviewed view,
  RDG or pooled-resource interfaces. Game/upscaler behavior still needs testing.
- Streaming/file-cache, shader-job cancellation, platform crash reporting and
  NVAPI updates do not warrant speculative changes to UEVR's hooks.
- Bink sound initialization changes from 44.1 kHz to 48 kHz; the reviewed movie
  rendering interfaces are unchanged. No game-specific movie patch is generalized.

## Narrow compatibility update

- Extend the reviewed Slate source-version gate through UE5.8.4, including its
  existing strict pooled-wrapper fallback. Keep all call-ABI, extent, device,
  ownership, stability and generation validation.
- Extend the source-reviewed UObject liveness policy through UE5.8.4. Its
  Garbage/Unreachable flag contract is unchanged; do not reinterpret item layout.
- Include UE5.8.4 in Hook Provenance's reviewed-source versions.
- Test positive 5.8.4 selection and negative 5.8.5/future/other-engine cases.

UE5.8.5 and later are not automatically admitted to these reviewed-source gates.
Existing game-specific adapters, rendering modes, settings and AFW are unchanged.
The proposed generic pre-5.7 dedicated UI scanner is a separate, deferred task.

## Verification limits

Source review and offline build/tests establish compatibility readiness, not a
successful run in every UE5.8.4 title or custom engine fork. A representative
UE5.8.4 game should still be tested for injection, Native/Native Fix, Synced/Ghost,
UI/HUD/spectator and resize/map/mode transitions before claiming runtime coverage.
