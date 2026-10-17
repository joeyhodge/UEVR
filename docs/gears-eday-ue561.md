# Gears E-Day UE5.6.1 compatibility

Scope: `GoWEDay-Steam.exe` and the observed `WinGDK/GoWEDay.exe`, file version
UE5.6.1, DX12. Other executable names, engine minors and games retain their
existing rendering paths. UObject
discovery itself is renderer-independent. This is a test integration, not yet
an in-game/HMD compatibility confirmation.

## Game Pass identity-only admission

The supplied GDK log identifies `FairlightConcept/Binaries/WinGDK/GoWEDay.exe`
on DX12. Its current Steam-only build bypassed this adapter, reached legacy
allocation replay after native-texture discovery failed, and ends during UI
texture creation. It contains no exception stack and does not prove the precise
failure, private layouts or file-version resource values.

The GDK addition admits only that case-insensitive basename with an immediate
`WinGDK` parent, while retaining the exact UE5.6.1 file-version requirement.
Missing/non-engine version resources are not accepted. This is eligibility to
attempt validated discovery, not a claim that Steam's layout is shared by GDK.
No runtime RVAs, guessed GDK names, layout alternatives, byte masks, virtual
slots, calling conventions, ownership or rendering defaults are changed.

Existing instruction, unwind, object/header, texture/device/extent, Slate ABI
and exact-generation eye-pair checks remain mandatory at their use sites.
Engine-owned allocation avoids replay; unsupported discovery preserves the
original draw/pair or declines the unsafe operation. GDK may still require its
own evidence-backed adapter if these contracts differ. Its EXE/PDB is unavailable
and the GDK path has not been runtime-tested. Steam contract fixtures and the
preserved Steam image can establish non-regression, not GDK compatibility.

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

## Native Fix renderer transaction

The UI repair is user-confirmed. Native Fix reached its first split render but
the legacy two-call transaction then failed in game RDG/shader-resource code.
The Gears-specific replacement uses the existing linked-family mechanism,
authorized by this game's own complete copy, array-copy, destructor, unwind and
view-metadata contracts rather than by a UE5.7 version assumption.

- Require a fresh input family with no existing renderer or owned upscalers.
- Use the game's deep family copy; retain independent Views/AllViews allocation
  and native reference-counted members. Use the base deleting destructor with
  deletion disabled. Gears uses zeroed, 16-byte-aligned UEVR-owned outer placement
  storage, retired by the matching UEVR owner after native member destruction.
  It does not depend on the generic GMalloc/name scanner. All member allocation
  and destruction still run through the validated game functions; no engine
  allocation is freed with UEVR's CRT, and no UEVR buffer is passed to engine Free.
- Keep the authored constructor eye identity, separate view states and shared
  exposure. Adapt the right singleton's pass/primary index only during rendering,
  with its Family backlink pointing at its own capture-target family.
- Submit both linked eye families through one original renderer call. Do not
  resubmit the same input family or decrement the scene's frame counter.
- Record shared interfaces before clone retirement, reject shared owned
  upscalers, and detach only borrowed ownership. Restore view metadata, backlinks,
  arrays and render targets before publishing the exact-generation capture.
- Derive the copy scan signature from the validated bytes, including the leading
  `0x40` REX prefix. The previous signature started one byte inside the function,
  so the correct unwind-boundary check rejected every result. End-to-end scans
  now require exactly one validated entry in the preserved game image.
- Keep both constructor rectangles packed for ordinary and failed-closed
  rendering. Only the linked transaction rebases the right rectangle into its
  single-eye target. This game's constructor proves constrained/unconstrained
  output offsets `+0x380/+0x390`; SWGR's different offsets are not reused. Compare
  live rectangles with exact-pair metadata and capture dimensions before writing,
  then restore on every exit. Failed restoration refuses rendering/publication.

For this validated transaction, preserving constructor identity is mandatory
regardless of the legacy Preserve Secondary Pass/Same Pass checkbox combination;
the physical right singleton is adapted within the renderer instead. Native Fix
remains opt-in. Unsupported contracts fail closed to the ordinary pair; a family
already consumed by another renderer is not submitted again. Other games keep
their existing paths. This transaction still requires a fresh HMD runtime test.

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
- Gears native family tests mutate every non-relocation copy/destruction byte,
  reject mismatched helpers/vtables/function boundaries and consumed families,
  and verify the constructor's pass/primary-index fields. The production
  validators also pass against the preserved executable's actual code and unwind
  table; no engine allocation/destruction address comes from a runtime JSON file.
- The production Kanan pattern parser finds the exact copy entry, while a
  regression fixture reproduces the old entry-plus-one match. Rectangle tests
  cover stale metadata, resolution mismatches, malformed/overlapping pairs,
  partial-write rollback, failed restoration and untouched neighboring bytes.

These are offline/runtime-code contract checks, not confirmation of Native Fix,
Ghost Fix, UObject browsing or the extracted HUD in the HMD after this follow-up.

## First controlled test

1. Let the flat game finish loading/shader work. Use DX12 and a clean profile;
   leave Extreme Compatibility, stereo emulation, MCP and Dumper7 off initially.
   Extreme Compatibility deliberately disables the render-target manager and
   is not an equivalent comparison with the ordinary stereo path.
2. Inject in ordinary Native with Native Fix off. Verify both scene eyes,
   head movement, menus/HUD, spectator and a level transition.
3. After confirming ordinary Native/UI, enable Native Fix in a stable scene. No
   checkbox workaround is required by the Gears linked-family adapter. Confirm
   the log reports its validated family functions and one linked renderer call;
   inspect both eyes and export fresh Hook Provenance/Native Frame Diagnostics.
   Check mode changes and a level transition before calling it runtime-confirmed.
4. Disable Native Fix before switching to Synced. Test Ghost Fix separately,
   including warm-up, movement and a transition. Do not combine mode changes
   with unrelated profile tweaks in the same comparison.

The provided user log is from a modified Synced/Extreme-Compatibility build,
not a clean current Native/Native Fix baseline. Static ABI checks cannot prove
the mechanism or elimination of its reported double image or shutdown crash.
