# Subnautica 2 beta viewport texture handoff

This adapter applies only to `Subnautica2-Win64-Test.exe`, UE5.7, D3D12.
The Shipping and WinGDK Shipping identities and their existing Subnautica
compatibility paths are unchanged.

The matching developer PDB identifies `FSceneViewport::InitRHI`,
`FRHICommandListBase::CreateTextureInitializer`, and
`FRHITextureInitializer::Finalize`. The initializer and Finalize have separate
hidden output references. The viewport's original allocation output is still
empty immediately after Finalize; its caller subsequently moves the returned
texture into the viewport's owning reference. Reading the earlier reference
leaves UEVR's scene target empty even though the engine created the texture.

The existing bounded initializer/finalize discovery now records Finalize's
own output location for this beta. It accepts two unambiguous stack-relative
LEA argument definitions and no intervening calls, branches, writes, register
clobbers or stack changes. The post-call resolver uses the current RSP/RBP,
checks alignment, arithmetic, user-address bounds and initializer overlap.
No EXE addresses or output displacements are hardcoded at runtime, and RAX is
not used as an ownership guess. Changed setup shapes remain unmodified.

The proven current creator output takes precedence over the earlier allocation
reference: that reference can be empty on first allocation or still hold the
previous buffer during reallocation. Before a
virtual call, the candidate must have a module-owned vtable and match the
PDB/source-verified UE5.7 RHI descriptor shape. The native resource must then
belong to the active device, have a valid render-target-only color shape and
the current eye/packed extent, and differ from the dedicated UI texture.
The engine continues to own and assign its RHI reference; the adapter does not
alter that reference or use the desktop backbuffer as a scene substitute.

The beta's PDB also matches the existing UE5.7 Slate input/output layouts.
No general UI ABI change is made here. The first run failed D3D12 setup before
any scene copy or submit, so correct UI composition must be checked again once
that earlier failure is removed. No settings, rendering method or CVars are
changed. A future engine/layout change needs a new validation, not an address
bump or a blanket extension of the Shipping fixes.
