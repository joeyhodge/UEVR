# SWGR command-list discovery

Scope: `SWGR-Win64-Shipping.exe`, UE5.7.4, actual D3D12 renderer. Other
games, engine versions and RHIs retain their existing command-list paths.

## Report and cause

A reported bad injection logs `New FRHICommandBase detected`, followed by
`vtable is not module-owned` rejection, without a successful RHI hijack.
The old Slate scan can accept the allocator cursor at offset zero, cache that
offset for the session, and repeatedly fall back to render-thread poses.
This is consistent with the reported persistent world-follow/snap-back behavior;
the log alone cannot prove every visible tracking artifact has this cause.

Matching UE5.7.4 source and both inspected SWGR binaries establish that
`FRHICommandListBase` starts with `FMemStackBase`, with `Root` at `+0x28`
and `CommandLink` at `+0x30`. The updated constructor moved; its validated
instructions and field layout did not. No constructor RVA is stored at runtime.

## Repair

- Discover a unique constructor during hook setup; require its unwind boundary
  and the instructions that zero Root and assign CommandLink to its address.
- Bypass the allocator scan and its cached offset for this scope only.
- Check the first/last command, null tail link and stable queue header before
  passing Root through the existing command-vtable safety guard.
- Empty or rejected queues use the existing pose fallback for that frame and
  retry the proven fields next frame. No allocator-memory substitute is used.
- Do not change eye scheduling, frame compensation, XR waits, rendering modes,
  UI placement, CVars, or other games' custom command-list handling.

The updated image also moves five image-relative SDK table/counter operands
used by the optional Bink seek-budget validator. Accept the complete original
or complete updated decoder contract, never a mixture. No mask is broadened;
the option's default, seek behavior and decode-completion checks are unchanged.

## Verification

Offline tests cover constructor mutations, invalid/truncated memory, a plausible
allocator cursor, empty and changing queues, invalid tail/Next pointers, and
recovery on later frames. Both Bink decoder variants retain byte-mutation tests.
Read-only PE checks must also accept the actual original and updated code,
including linked Native Fix, Slate, Bink and texture-accessor contracts.

For runtime confirmation, repeat injections without changing settings. Check
for the SWGR validated-Root log and actual `Successfully hijacked command list`
execution, then test head motion, menus and a level transition. Keep collision
right-eye VFX and unrelated hangs as separate investigations unless new
evidence connects them. Build/test success is not HMD confirmation.
