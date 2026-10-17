# Star Wars Zero Company UE5.6.1 compatibility

## Revision 199049 (2026-10-17)

The supplied `SWZeroCompany.exe` reports engine file version 5.6.1.0 and
game build 199049. Its matching native PDB has GUID
`28FDBAB2-F3FD-10A9-20AC-B2EF0623B9F0`, age 1. The EXE SHA256 is
`aa3ff2e0759c90bae58054f2a68398e5b705fdfd05659242404d8c3602c6f36e`.
The supplied PDB has public symbols but no private type records; layout checks
therefore use actual instructions and matching engine source, not inferred PDB
member offsets.

All three previous Slate-address selectors reject this executable. A separate
immutable entry in `SWZeroCompanyBinary.hpp` restores the verified addresses for:

- Slate DrawWindow and its RegisterExternalTexture entry hook;
- the viewport's force-separate-target accessor and desktop-copy clamp;
- InitializeHMDDevice's inlined stereo construction and both interface tables;
- the two real FRHITexture vtables and their slot-5 native-resource getter;
- the exact custom hologram caller and Nanite dispatch.

The previous entries, call ABIs, scene conversion, resource ownership, input,
Native Fix scheduling and Synced/Ghost behavior remain unchanged. A relocated
address is never sufficient by itself: the existing byte/table validations still
apply. Unknown or ambiguous revisions are not admitted.

## Hologram coverage

For revision 199049 only, the incomplete-raster guard also applies while Native
Fix is enabled. Installation additionally verifies Nanite's actual loads of
`VisibleClustersSWHW` at +0x38 and `ShadingMask` at +0x60. The direct call belongs
to RenderHologramCaptureInner according to its chained Windows unwind record.

The callback is restricted to that exact caller, D3D12, an active HMD and Native
stereo. It skips only the existing unreadable/incomplete-input failure cases;
complete inputs call the original dispatch unchanged. It does not synthesize
missing resources, change shader compilation, wait for the GPU or suppress
D3D12 error reporting. Other callers, rendering methods and older revisions keep
their established behavior.

## Runtime verification

Build and offline validation are not confirmation of in-game hologram behavior.
Test ordinary Native first, including hologram menus and combat actions, then
repeat with Native Fix. Check both eyes, dedicated UI and spectator output, and
include a level transition. Record fresh log/provenance/native diagnostics; if a
D3D12 command-list failure remains, retain the matching crash dump rather than
assuming this guard covers every possible GPU fault.
