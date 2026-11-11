# UE5.9.0 compatibility

## Status and scope

Developed on the paired UEVR/UESDK `59` branches from the `eBaseballfocused`
baseline, this profile is source-reviewed, binary-validated and HMD-tested in
a packaged UE5.9.0 Shipping demo using D3D12 and OpenXR. Native, Native Fix,
Synced Sequential, Ghost Fix with bounded separate-state bootstrap, UObject
Hook and aiming were confirmed working on 2026-11-09. This does not establish
compatibility with every future 5.9 build or custom engine fork.

The established branches retain their own UESDK pairing, game allowlists,
camera/crowd features, Lua interfaces and rendering-mode IDs. Non-DIBR branches
do not acquire DIBR or private eBaseball features through this update. There is
no global FSR, frame-generation or backbuffer workaround.

### Packaged Shipping runtime verification

The verified backend is `da51e73de36b6bb5d2dd11029e60ede6038e6be2`, paired with
UESDK `5852f690b72574a63b515a82149fc413f0294670`. The log confirms:

- Stage-2 discovery converged with Begin 4, PreRender 6, active 21 and the real
  frame counter at `0xA0`, following repeated live family/frame validation.
- Native Fix reached active only after a validated right-eye resource was
  submitted. Both constructor and native family ownership contracts passed.
- Ghost Fix's bounded bootstrap learned distinct eye histories, established
  stable ownership and remapped the right eye to its dedicated scene state.

The same backend also reached Native Fix active on the packaged Development
demo (`d59`, DX12), using its distinct native family copy/destructor contract.
Ordinary Native and Synced/Ghost worked in the Development DX11 run; Native
Fix produced a capture but could not enter the DX12-only linked renderer.
DebugGame, DIBR, mono and custom forks still require mode-specific runtime
validation.

The review uses these local, licensed archives (no Unreal source is included
in this repository):

| Archive | SHA256 |
| --- | --- |
| `UnrealEngine-5.8.4-release.zip` | `6CB978543AE72BBE4BDA70791F0A7B3FEE2F20BE85D477FD079B7F22FFE4E10D` |
| `UnrealEngine-dev-5.9.zip` | `DF455A9AEA465C2574DF92BB3FA4779327899E4D1B46FB095D466FA5024EBB18` |

The candidate declares 5.9.0, changelist/compatible changelist 0, branch `UE5`.
This is a development snapshot, not a claim about every future 5.9.0 build.
Archive-wide CRC/size inventory identifies 12,177 modified, 13,278 added and
3,957 removed paths. Targeted source/ABI review covers 116 critical files
(68 changed, 48 identical); the manifest records cryptographic hashes. This
inventory does not imply every changed engine/plugin file was manually reviewed.

### Installed binary cross-check

The installed 5.9.0 CL60351404 Shipping, Development and DebugGame executable/
PDB pairs were checked read-only. Each PE CodeView GUID/age matches its PDB.
`ue59-binary-contracts.json` records their SHA256 identities and selected RVAs.
These are stripped public-symbol PDBs: they do not supply private member/type
layouts. Public symbols plus original instructions/unwind records, not symbol
names alone, prove the tested contracts.

- Shipping `UGameViewportClient::Draw` is RVA `0x02aaf5e0`, matching the supplied
  symbol. The view constructor proves family/state, both rectangles and stereo
  members in all three images.
- The active extension callback is slot 21, not priority slot 20. A compiler
  fixture covers MSVC's grouping of both subscription overloads.
- XR call sites independently corroborate camera retrieval at `0x110` and
  camera rotation at `0x28`. UObject/UClass vtables retain PostInitProperties
  at slot 10. These do not authorize replaying it to allocate eye histories.
- FScene's IncrementFrameNumber call is `0x510`, not the older `0x4d8`.
  UE5.9 has separate renderer contracts; UE5.8's checks remain exact.
- Shipping's proven family size/interface prefix is `0x1a8/0x168`.
  Development/DebugGame's debug-view-mode fields expand it to `0x1b8/0x178`; neither is
  substituted for the other. Complete copy/base-destructor contracts select
  separate bounded storage, retaining array and interface ownership checks.
- Constructor validation overlays only the hook's saved original entry in a
  local buffer. Renderer-body contracts exclude entry-hook bytes. Validation
  never temporarily unpatches a live function.

The optional `compatibility-policy-tests --ue59-image <exe> <symbol-manifest>`
integration test passed on all three images without executing engine code.
This is binary-contract validation, not in-game/HMD validation.

## Compatibility changes

### XR and stereo interfaces

- `IXRSystemIdentifier` gains a leading virtual destructor. Every inherited
  `IXRTrackingSystem` method shifts one slot. `GetSystemName` is now slot 1.
- `IXRCamera` shares that inherited destructor slot; its own declaration does
  not add a second one. Device ID is slot 2, implicit-position slot 3,
  `ApplyHMDRotation` slot 5 and `UpdatePlayerCamera` slot 6.
- Synthetic tracking/camera objects receive non-owning destructor callbacks,
  complete runtime-width `NAME_None` storage and a device-ID callback.
- `IHeadMountedDisplay`, `IStereoRendering` and `IStereoRenderTargetManager`
  retain their reviewed 5.8 interfaces. The unchanged interfaces are not shifted.
- Actual 5.9 identity takes priority over a legacy XR version override. An
  unknown 5.9 patch cannot install the older table as a fallback.

### Scene-view construction and native stereo

- `FSceneViewInitOptions` appends `ShaderPlatform`. Its existing family-relative
  PlayerIndex, stereo pass and world-scale prefix is unchanged (`+0x28`,
  `+0x78`, pass `+8`). UESDK has an explicit 5.9 initializer profile.
- The initializer is embedded in `FSceneView`: its growth can change later
  view fields. The stock non-editor view candidate is constrained/unconstrained
  rect `0x3a0/0x3b0`, pass/eye/primary `0xe80/0xe84/0xe88`. **These numbers are
  not permission to access a view.** A bounded constructor decoder must first
  prove byte-for-byte source/destination copies, then live pair/state/rect
  validation must succeed. SIMD/combined copies are supported by the proof.
- Constructor proof is cached against the function and discovered input layout,
  including failed proofs. There is no repeating image scan on every frame.
- Native Fix retains original engine-created views and temporal identities.
  It can use a linked-family transaction only when constructor, renderer,
  native copy/destructor, live eyes, target and generation contracts all pass.
  A different compiler/fork can legitimately decline this capability.
  Reviewed 5.9 DX11 can use the same renderer proof on standard Native OpenXR
  output. Present first validates the exact main viewport, generation-owned
  right capture, packed scene and double-wide XR images on the same device.
  The binding is reused until an identity changes or the swapchain is reset.
  Both color copies must validate before either half is written. Only a
  successful XR projection with two views of that released image marks the
  fix active. Split frames do not submit the ordinary packed depth texture.
  DX11 OpenVR, 2D, split-screen and legacy engine behavior are not extended.
- Both DX11 and DX12 5.9 paths are kept out of the old second-render fallback
  that could replay a truncated initializer or assume an old full-view size.
  Failed pre-render validation preserves the original packed eye render.
  If validation fails after a DX11 split was already issued, that incomplete
  frame is not submitted as ordinary packed stereo.
- There is no inferred `FViewInfo` stride, whole-view memcpy or blanket `+16`
  adjustment across unrelated renderer structures.

### View extensions, rendering and RHI

- A new deferred-translucency callback shifts the later extension tail. The
  reviewed early slots remain Setup 0, Begin 4 and PreRenderFamily 6; active is
  21, confirmed in both installed Shipping and Development vtables. MSVC groups
  both `SubscribeToPostProcessingPass` overloads before this method, including
  the deprecated overload; counting only source lines gives the wrong slot.
  The complete `FSceneViewFamily` declaration is identical to 5.8.4.
- 5.9 uses source-slot observation plus repeated validated family/frame
  correlation. It cannot fall through to the unrestricted counter heuristic.
  A mismatched active slot leaves callbacks inactive rather than guessing.
- The first packaged Shipping test exposed a stage-2 admission check still
  requiring the older active slot 20 despite observing 21 in stage 1. Both
  stages now select 21 for reviewed 5.9, leaving the legacy profiles on 20.
  Repeated family ownership/frame proofs remain mandatory before hooks install;
  no synthetic frame increments or pacing changes are used. Regression tests
  cover the production gate wiring as well as its complete eligibility matrix.
- Cache key `ue59_view_extension_discovery_v1` separates this topology from
  older discovery. Schema, types, ranges, module ownership and exact topology
  are checked before reuse. Invalid/stale caches are discarded.
- Rendering/RHI work is split into new source files, with new low-latency RDG
  hints, uniform-block commands and DynamicRHI virtuals. Those additions do
  not justify shifting arbitrary RHI slots. Command ownership remains checked.
- The legacy `RHICreateTexture2D` slot-178 helper refuses 5.9. Engine-owned
  viewport/capture resources are required instead of calling an obsolete ABI.
- Native, Synced/AFR, Ghost Fix, DIBR, mono and split-screen keep separate
  ownership/cadence checks. No mode is globally substituted for another.
- Reviewed 5.9 LocalPlayer histories are allocated lazily by actual
  CalcSceneViewInitOptions calls. Native Fix and Synced/Ghost therefore do not
  replay PostInitProperties, including callbacks left installed after a mode
  change. Synced/Ghost retains remap-only by default. Its optional separate-state
  bootstrap requires main Draw and a ready view-count hook; the existing stable-
  scene, two-frame pulse, cooldown and three-attempt limits remain intact.
  It never fabricates a ViewState or reports active before an owned right-eye
  remap. Native/Fix, mono, DIBR and split-screen cannot enable this remap.

### Slate, UI, textures and Canvas

- Slate draw inputs insert a 40-byte screenshot snapshot after the first four
  pointers. Cursor, scene rect and UI scale move to `0x48/0x50/0x60`.
  A separate 5.9 prefix reader normalizes them for existing consumers. It
  never dereferences screenshot arrays or uses a prefix size as an array stride.
- Outer/inner Slate call signatures retain the reviewed contract. UI target
  capture still requires function, ownership, resource, extent and generation
  evidence; a version match alone cannot publish a texture.
- Reviewed 5.9 shares the modern render-target-manager path, completed-Draw
  scene adoption, dedicated UI capability routing, consumer-fenced conversion
  ring and spectator-copy lifetime handling. Legacy game-specific routes are
  not automatically enabled for a 5.9 build of the same executable name.
- `FTextureResource` adds pending-reference helpers while explicitly preserving
  its layout; render-target-2D resource creation source remains unchanged.
  Resource readiness can still be deferred, not assumed from UObject creation.
- `FCanvas` gains cached shader-platform state; UEVR does not copy its full
  object or infer a new HUD layout from that addition.
- The Unreal Canvas authoring/editor workspace is not a replacement for game
  UMG/Slate and introduces no UEVR runtime dependency.

### Reflection and input

- Console interfaces are byte-identical to 5.8.4. An explicit 5.9 selection
  chooses the validated modern interface, not the legacy default. Unknown
  5.9 patches have no callable console fallback slots.
- The stock UObject header, FName variants and FField contracts remain
  discoverable. The optional remote-handle build moves RemoteId into
  `UObjectBase`: that header is explicitly refused, not treated as stock.
- Stock 5.9 must pass the 8/12-byte FName header witnesses and existing object
  array index/stride/liveness checks. Garbage/unreachable masks are selected
  explicitly; this does not hardcode a new FUObjectItem stride.
- CDO precreation can now be disabled. Null CDOs are legitimate. Offset
  discovery uses three distinct class/CDO witnesses, at most three attempts
  spaced five seconds apart, then returns unavailable without guessing an
  offset or forcing CDO construction. Proven slots are checked before reading.
- New input-mode suppression rules and PlayerController virtuals are noted;
  no hardcoded PlayerController shift or forced input-policy override is added.
  Head/controller aim and reflected calls still need a live acceptance test.
- Lua/plugin API signatures are unchanged. Existing scripts require no migration.

## Offline checks and build

The source audit is repeatable without an Unreal build:

```powershell
python tools/audit_ue59_sources.py `
  --baseline 'D:\Downloads from Edge\UnrealEngine-5.8.4-release.zip' `
  --candidate 'D:\Downloads from Edge\UnrealEngine-dev-5.9.zip' `
  --output 'D:\b\ue59-source-audit.json'
```

A changed archive/hash fails the reviewed-manifest check. Do not update that
manifest or admit a new patch without reviewing the changed contracts.

Use VS2022 for UEVR; a full Unreal build is neither required nor performed.
The primary backend build uses the established v143 14.38 toolset and Windows
SDK 10.0.22621. UESDK now declares CMake 3.25, matching the `FetchContent SYSTEM`
feature already present in its generated build. The TOML source preserves those
SYSTEM flags when regenerated with its pinned cmkr 0.2.26; the parent keeps
cmkr 0.2.14. No dependency revision is silently upgraded.

```powershell
cmake -S . -B build-59 -G 'Visual Studio 17 2022' -A x64 `
  -DCMAKE_BUILD_TYPE=Release -DUEVR_BUILD_COMPATIBILITY_TESTS=ON `
  -DUESDK_BUILD_DISCOVERY_TESTS=ON
cmake --build build-59 --config Release --parallel 6
ctest --test-dir build-59 -C Release --output-on-failure --no-tests=error
```

The audit passes 19 source-contract checks. Tests cover numeric/string identity
conflicts and future-version rejection, real compiler inherited-destructor and
overload-ordering ABI, unchanged old slots, constructor-code
positive/negative/truncation fixtures, eye transaction restoration/canaries,
opaque screenshot-aware Slate decoding, malformed/stale extension caches and
bounded CDO retry. Shipping and Development family storage have independent
cleanup/canary/alias tests; every unmasked ownership byte is mutation-tested.
The established rendering, camera, Lua, UObject and
diagnostic suites remain enabled. CI runs SDK discovery tests as well.

## Runtime acceptance for additional builds and modes

1. Record the exact installed 5.9 executable/modules, matching PDB GUID/age,
   engine changelist, RHI, backend/SDK commits, mode and options. An editor PDB
   alone does not validate a non-editor shipping layout.
2. Confirm native version, object/FName/FField/array witnesses, ProcessEvent,
   PostInitProperties, LocalPlayer states, viewport Draw and fake XR tables.
3. Confirm both main eye views, passes, states, projections, rects and final
   source textures. Verify actual stereo/IPD in the HMD, not just desktop output.
4. Test Native first without optional overrides; then Native Fix (including
   its fail-closed path), Synced/AFR, Ghost Fix, DIBR and mono individually.
   For the DX11 Native Fix change, repeat on Development and Shipping with
   OpenXR. Verify the generation/destination proof, selected/copied frame
   serials and successful two-eye projection, then toggle Fix off and on.
5. Check UI/HUD, ImGui open/closed, spectator output, resolution changes,
   loading/level transitions, head and both controller aim modes, stale-object
   transitions and shutdown. Repeat injection to expose lifecycle races.
6. Compare working older UE games and run each branch's regression suite.
   Keep unknown layouts fail-closed; a successful Shipping demo must not
   authorize unrelated compiler, platform, engine-patch or rendering-mode ABIs.
