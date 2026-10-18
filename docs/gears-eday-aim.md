# Gears E-Day aim initialization

The supplied 2026-10-18 log and minidump show a CPU null read in the game's
`UGameViewportClient::Draw`, immediately after enabling HMD/controller aim and
publishing UEVR's fake XR system. The failing query is not UObject attachment.

The dump's original 88-byte call sequence matches the saved runtime analysis
image. It checks `XRSystem`, obtains `GetHMDDevice`, passes stack storage in RDX,
calls HMD slot 28 and immediately copies the eight-byte result through RAX.
This is the debug-canvas `FIntPoint` query. Stock UE5.6 uses slot 27; UEVR's
unimplemented slot 28 returned null. The adjacent stock distortion method has a
different return type, so a global vtable shift or neighboring-slot alias would
be unsafe.

## Narrow correction

- Enter only the existing exact E-Day UE5.6.1 executable-identity gate. GDK
  identity permits validation, not reuse of Steam addresses or assumptions.
- Before exposing fake XR, inspect a bounded, unwind-owned original Draw body.
  Ignore the detoured entry. Validate the engine global, XR member, both HMD
  getter calls, null branches, hidden result storage and eight-byte consumer.
- Accept only one validated debug query at slot 27 or 28. Install the typed
  result handler in that slot; keep all stock SDK tables unchanged.
- Return initialized 1024x1024 storage, matching the engine default. Verify the
  receiver and writable eight-byte output range. No game-code NOP or crash
  recovery is added, and no per-frame scan, allocation or log is introduced.
- If proof fails, leave XR unpublished rather than guessing a slot. This can
  block aim initialization on an unsupported update; it is not runtime support
  for an untested GDK layout.

## Controller components

The plugin/Lua component-creation API uses reflected `AddComponentByClass`
and `FinishAddComponent`, not the HMD debug-canvas method. Motion-controller
component placement is handled separately by UObjectHook. Their creation and
registration can cause engine XR queries, so they belong in the runtime test,
but this dump does not identify an independent component-construction fault.
No component-creation or shared UObject behavior is changed by this repair.

## Verification and runtime test

`gears-eday-aim-tests` exercises the original instruction contract, original
null return, corrected hidden-output dispatch, eight-byte write bounds,
stock-slot preservation, ambiguity, incorrect owner/offset/slot/return ABI,
truncated memory and executable/range guards. Its optional `--draw` argument
checks the complete locally saved original Draw function without executing it.

A tester still needs to switch Game -> HMD -> Game -> each controller aim,
check actual aim orientation, create/register/attach left/right controller scene
components on the game thread, and test a level transition. Test with and
without Native Fix. A successful offline test/build is not confirmation of
these in-game behaviors or of GDK compatibility.
