# Gears E-Day UE5.6.1 compatibility

Initial scope: `GoWEDay-Steam.exe`, file version UE5.6.1, DX12. Other executable
names, engine minors and games retain their existing rendering paths. UObject
discovery itself is renderer-independent. This is a test integration, not yet
an in-game/HMD compatibility confirmation.

## Verified contracts

- The GC-settings initializer loads an encoded heap singleton. Discovery derives
  its global and XOR key from the load/decode/store instructions, requires the
  same global on load and store, and retains existing array/item/object checks.
  No process-specific heap address, module RVA or XOR key is a runtime prerequisite.
- The completed game-thread viewport Draw exposes the scene reference at +8.
  Its render-thread accessor uses +0x2d8; the previous +0x2e0 fallback selects a
  Slate wrapper. The new observer runs only after Draw and validates the original
  getter, including UEVR's own optional AHUD pointer-hook case.
- The first `BufferedRT` helper constructs a descriptor, not a texture. Replaying
  it with texture-reference storage writes descriptor flags (9) as a bogus
  pointer. Allocation remains engine-owned: no allocation replay/midhooks or
  retained stack references. The completed-Draw observer publishes the result;
  dedicated Slate UI is created separately.
- FD3D12Texture's native getter is slot 6, not slot 5. Slot 5 requires hidden
  descriptor output storage. The adapter validates the descriptor/native getter
  code and mirrors only its direct resource read; it does not invoke speculative
  virtuals or the getter's linked-texture fallback.
- Slate DrawWindow uses RDX for hidden output storage, R8 for FRDGBuilder and R9
  for pass inputs. Its real command list is the validated builder +0xc0 reference.
  RegisterExternalTexture's argument transport is checked independently.
- FSceneRenderer and FSceneView both reference TranslucentSortPolicy. The former
  was incorrectly cached as the view constructor. This title now requires the
  actual init-option family/state/player/pass copies, including family +0x158
  and state +0x160. Cached candidates receive the same validation; discovery
  requires one unique CVar-anchored unwind owner, not the largest refcount.
- The live protected image reserved the usable nearby allocation range. The
  short RIP-relative destructor and Slate E8 callsite could not use a private
  far trampoline. Use the validated raw-texture RegisterExternalTexture entry
  and FUObjectArray::FreeUObjectIndex entry instead. They support far relocation
  without borrowed image padding or protection overrides.
- UObject retirement is observed before FreeUObjectIndex clears the object slot.
  Validate the array/header layout first, publish the disabled hook and array
  identity before enabling, match the object's index/serial registration before
  removal, and always forward the original call. Creation tracking uses the
  existing incremental array fallback; no speculative AddObject ABI is hooked.

Relocation fields and the derived XOR key may change without breaking these
contracts. Changed member offsets, instructions or calling conventions are
rejected rather than guessed. This reduces update sensitivity; it is not a
promise that arbitrary future game updates remain compatible.

## Rendering boundaries

The scene observer retains device, extent and repeated completed-Draw checks.
Only a validated RHI texture seeds capture/UI resource discovery. Native Fix
keeps its generation, owner, device, BGRA eye-size and compositor-consumption
checks. Ghost Fix retains its existing eye-pair/state validation. Neither fix
is forced active, and no frame-ID handoff from the modified user build is ported.

The UI path uses UEVR's rooted dedicated target. It preserves original Slate
arguments and return storage, services workers with the real RHI command list,
and does not promote pooled scene/swapchain outputs into the UI target. Invalid
contracts preserve the original draw. Existing rendering-method IDs, controller
bindings, profiles and defaults are unchanged.

The function-entry UI adapter redirects only the exact SlateOutputTexture name
on the active DrawWindow thread. It preserves builder/name/flags and the return,
uses the existing validated resource/extent/device path, and does not claim a
working UI redirect when hook installation fails. Hook Provenance includes the
new entry hook. Other callers, spectator resources and offscreen names pass through.

The register scanner runs after DrawWindow has already been hooked. For this
title it starts at the independently validated +0x25 original-code boundary,
requiring the stolen prologue to end before that boundary. It does not decode
the absolute-jump pointer literal at the patched entry. The bounded scan requires
one exact name/argument/callee contract before the first return; changed,
truncated, non-executable and ambiguous bodies preserve the original UI path.
Scanners for other titles are unchanged.

## Offline follow-up verification

- All five new byte contracts match read-only live captures. Chained unwind
  metadata independently identifies the real constructor and GC retirement entry.
- Captured constructor instructions execute in Unicorn with the expected
  family/state/player/pass copies and unchanged inputs. The captured free-index
  function clears precisely the expected array item and object's internal index.
- An own-process-only test reserves the rel32 range without committing 4 GiB of
  RAM. It reproduces failure of the old short destructor, then installs, exercises
  and removes all three new private far trampolines, checking original-call
  forwarding, arguments, returns, retirement ordering and restored code bytes.
- Negative instruction fixtures reject changed field offsets, register/ownership
  operations, truncated code and the incorrect renderer-constructor candidate.
- Slate discovery is also exercised with an actual far DrawWindow hook already
  enabled in the test process. A separate read-only check runs the production
  scanner against the preserved game image, then against an offline copy with
  a far entry stub; both identify the same output registration function.

These are offline/runtime-code contract checks, not confirmation of Native Fix,
Ghost Fix, UObject browsing or the extracted HUD in the HMD after this follow-up.

## First controlled test

1. Let the flat game finish loading/shader work. Use DX12 and a clean profile;
   leave Extreme Compatibility, stereo emulation, MCP and Dumper7 off initially.
   Extreme Compatibility deliberately disables the render-target manager and
   is not an equivalent comparison with the ordinary stereo path.
2. Inject in ordinary Native with Native Fix off. Verify both scene eyes,
   head movement, menus/HUD, spectator and a level transition.
3. The previous test reached the first split render but crashed in a game shader
   resource binding with Preserve Secondary Pass enabled. That failure is not
   repaired or runtime-confirmed by the Slate scanner change. After checking
   UI with Native Fix off, the next controlled comparison is Preserve Secondary
   Pass off with the existing Same Pass option left on; do not combine it with
   unrelated changes. Inspect both eyes and export fresh diagnostics if stable.
4. Disable Native Fix before switching to Synced. Test Ghost Fix separately,
   including warm-up, movement and a transition. Do not combine mode changes
   with unrelated profile tweaks in the same comparison.

The provided user log is from a modified Synced/Extreme-Compatibility build,
not a clean current Native/Native Fix baseline. Static ABI checks cannot prove
the mechanism or elimination of its reported double image or shutdown crash.
