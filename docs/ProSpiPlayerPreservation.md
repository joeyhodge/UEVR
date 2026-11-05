# ProSpi player preservation

The existing default-off Balanced and Heavy settings are retained. This change
does not alter the crowd/world-cull hook, camera matching, zoom, dolly, FOV,
gameplay, ball follow, replay, celebration or interview framing.

## Verified native paths

The player visibility handoff is in the game's native player updater, not a
general Unreal object/primitive visibility scan. Its model handle is an integer:
zero is valid and UINT64_MAX is the invalid dummy observed in the live roster.

| Edition | Image size | PE timestamp | Handoff RVA | Enabled | Bounds | Transform object | Visibility byte |
| --- | --- | --- | --- | --- | --- | --- | --- |
| eBaseball | 0x2e02a000 | 1790730967 | 0x5851c98 | +0x6e0 | +0x7e0 | +0x388 | R12B |
| ProSpi 2025 | 0x2beeb000 | 1773341438 | 0x5600809 | +0x6d8 | +0x990 | +0x380 | R15B |

Both editions are checked independently against their enabled-flag branch,
bounds setup, native bounds-pointer setter, matrix-copy producer and renderer
handoff. Unwind ownership must also cover the handoff. Identity alone is not
permission to assume the other edition's layout. Unknown builds retain the
previous signature-based path where it validates; otherwise no hook is installed.

The native culling object contains a row-vector affine transform at +0x110.
The same local AABB supplied by the game's updater is transformed to a conservative
world sphere. The independently checked camera/component bridge converts native
coordinates to UE coordinates as X/Z/Y * 10. Invalid/non-finite/oversized bounds
and changing transforms do not authorize restoration.

## Runtime behavior

- Balanced uses a fixed 256-entry, four-way associative identity/history cache.
  Current HMD relevance does not require a promotion slot or prior broadcast
  visibility, so cache collisions cannot impose a player-count limit.
- The observer reads the final corrected per-eye pose and copies the existing
  projection results. It does not evaluate camera matching/smoothing again or
  change any camera/projection output. A conservative projection union, body
  bounds and head-motion margin cover both eyes and asymmetric/canted headsets.
- Only current camera-generation packets at most 250 ms old authorize HMD
  relevance. During a cut's initial missing packet, only genuine game-visible
  history can authorize restoration (3 seconds in-shot, 500 ms across one cut).
  A forced bit never teaches this history.
- Heavy remains an explicit all-enabled fallback. It does not require a frustum
  packet and does not run the HMD observer. Native state identity is read at the
  actual callback instead of relying on the old eight-state pointer-probe cache.
- Both paths preserve PLAYEROFF. State/vtable/model/transform identity is checked
  again before changing only the proved visibility register's low byte. No game
  function, property write, object-list walk or allocation is performed by this
  native callback.
- The engine tick publishes active-world identity using class-identity-scoped
  reflected field offsets, with no per-tick negative property discovery. Native
  callbacks remain on the game's original call thread, with thread-local caches
  and no engine/UObject calls; they need not be assumed to run on the engine tick
  thread. Off, 2D, inactive HMD, mode changes and world transitions revoke the
  guard/history authority. View readers use nonblocking lock attempts.

The read-only live roster fixture contains 48 native states: 40 enabled real
models, seven disabled states and one enabled invalid-handle dummy. It is not
proof that intentionally disabled players should be enabled.

## Stadium scope

The stadium guard reuses successful eligibility validation only between learning
and forcing within the same operation. Original calls, reentrancy, maintenance
and restoration still retain their existing ownership/liveness checks. Its scan
budgets, geometry selection and visibility-proof requirements are unchanged.

Unknown hidden-on-entry background variants are deliberately not authorized from
names, mere ownership or LastRenderTime. A timestamp alone cannot establish the
mesh binding/active variant that produced it, and an earlier override could train
it. Existing structural handling and genuine game-visible learning stay in place.

## Verification and testing

`compatibility-policy-tests` checks the captured native code for both editions,
every witness-byte mutation/truncation, model zero/-1, PLAYEROFF, reuse, cache
collisions/pressure, the live roster, signed coordinate conversion, frustum edges,
asymmetry, stale packets and cut generations. The existing stadium and camera
fixture suites remain required.

Build success is not HMD confirmation. Test Balanced with unchanged camera
settings in normal pitching/batting, field/bench head turns, a crowded celebration,
an interview and a stadium/level transition. Verify intentional off states stay
off. Heavy remains available for a controlled comparison, not a promised FPS fix:
drawing extra models still costs CPU/GPU work. The isolated CPU workload does not
measure engine draw cost or end-to-end frame rate.
