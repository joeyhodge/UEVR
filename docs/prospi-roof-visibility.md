# ProSpi stadium roof and background visibility

`VR_ProSpiShowStadiumRoofs=false` is the default. Enable **Always Show Stadium
Roof / Background** under VR -> Camera -> Game FOV. Match Game FOV, dolly, camera
assist and a particular rendering mode are not required. This is one toggle for
all ProSpi stadiums, not a Tokyo hash/address allowlist.

The flat Tokyo Dome A/B established that the game hides the actual roof mesh
components in behind-pitcher view and restores them behind the plate. The
SoloPlayMask actor remains hidden in both. This option does not change that
mask, sky postprocessing, exposure, LOD, camera coordinates, or projection.

A second same-process A/B found 98 components whose visibility changed from
false to true when switching behind pitcher to behind plate. Meshes, materials,
ownership and HiddenInGame stayed unchanged. This includes seven outfield-wall
pieces and structural stands, stairs, pillars, fences and rails, not only roofs.
The initial structural filter covered the roofs and walls but missed other
camera-hidden background pieces. A same-process Hokkaido Night comparison found
185 more camera-hidden components, including the exterior, glass back wall,
stands, shops, signs and transparent background layers. Its 10 never-visible
variants and Tokyo's 32 never-visible variants must remain untouched.

The guard now recognizes numbered/CamelCase structural roles such as
`wallGrazed02`, `rubberFence01` and `handrail05`. Additional background groups
(`p00_soto`, `p03_outfield`, `p07_alpha_outfield`, and static infield cheering
geometry) require evidence that the game actually shows that component. A
normally visible, unforced Draw sample or a successful original game show call
supplies this proof. Repeated hide calls, failed show calls and this guard's own
forced-visible readback do not supply it. Never-visible unknown variants stay
pending rather than being guessed visible.

If the option is first enabled while these extra pieces are already hidden,
briefly select behind the plate or Wide/Live with the option on, then return to
the desired camera. This lets the game identify its normal background for that
stadium. Proof is discarded on disable/travel and must be established for a new
component/mesh binding. No stadium names, hashes or camera coordinates are
hardcoded. Existing structural startup handling remains available without this
learning step.

## Scope and safety

- Only the exact ProSpi executable activates the guard.
- The Draw callback is matched against the active engine's reflected
  `GameViewport`. Either an already-normalized UObject pointer or the validated
  secondary viewport interface at `+0x28` is accepted. The latter also requires
  the game's vtable Draw slot to match the discovered Draw function. Current
  engine/viewport identities, classes, outer ownership and vtables remain
  validated; unrelated callbacks are never treated as UObjects or adjusted by
  guessing. This normalization is local to the guard, not shared Draw handling.
- Only live `StadiumStaticMeshComponent` objects owned by a current-world
  `StadiumActor` are eligible. Their component and StaticMesh asset names must
  identify a roof/ceiling, structural outfield role or an exported background
  group, and the asset package must be under `/Game/Stadiums/`. Outfield and
  background component/mesh stems must agree after removing only the standard
  FBX component suffix and SM_ asset prefix. Newly learned backgrounds also
  require the package leaf to match the actual mesh name.
- Hidden actors/components, masks, proxies, collision meshes, scoreboards,
  explicit LOD/extra/variant meshes, templates and
  pending-destruction objects are excluded. No roof is invented in open stadiums.
  Unknown inactive backgrounds are not authorized without game-visible proof.
- The existing UObject identity/liveness policy validates component, owner,
  mesh, package, level and world references. The actor's level must still be in
  the viewport world's current Levels array. A world change discards old targets
  without writes into the previous world.
- The native visibility setter is obtained only from the reflected
  `SceneComponent.SetVisibility` exec's direct calls. Unwind/module bounds,
  instruction decoding, the reflected visibility offset/mask, and the native
  flag-update/OnVisibilityChanged sequence must all agree. No fixed code RVA is
  used. Unsupported compiler/layout shapes fail closed; an update can therefore
  require revalidation, not an unsafe guess.
- Hooks/scans are not installed/run while the option has never been enabled.
  When enabled, background maintenance visits at most eight cached targets and
  discovery at most 256 objects per viewport Draw, sharing a 250 us check budget.
  There are five seconds between completed discovery sweeps and at most 256
  cached entries, including pending background candidates. Both complete
  captured stadiums fit this unchanged cap. At capacity, new structural targets
  may replace only unproven, never-forced pending entries; established proof and
  restoration obligations are never evicted. Once a target is authorized,
  camera-driven setter calls are handled synchronously, so a cut does not wait
  for the background sweep. The check budget does not bound an
  individual game setter or one-time reflected metadata discovery.
- Code-page `VirtualQuery` and instruction decoding run only during setter
  discovery. Names and reflected offsets are cached; the level-list buffers are
  reused. Cached targets retain current identity/liveness and world/level checks
  before mutation, without a duplicate eligibility pass in the Draw loop. This
  reduces CPU work without treating a once-readable UObject as permanently safe.
- The normal game visibility call still executes, including its original child
  propagation. Only validated, authorized geometry receives a separate no-propagation visible
  request. No cache iterator is retained across the native call, and readback
  reacquires the current identity's entry and revalidates the entire
  component/owner/level/mesh/package binding before acknowledging an override.
  Requests are shadowed so turning off can restore the latest game
  request, rather than the camera state that existed when the option was enabled.
- A different mesh/owner binding cannot learn from the previous binding's
  forced-visible value. It needs an independent game show or an unforced
  false/true transition. The UI distinguishes authorized targets from pending
  learning candidates; pending discovery is not reported as active coverage.
  A nonzero learning count can be expected for inactive variants that should
  never be made visible.
- Visibility state changes run on the game thread, never the render thread. No
  cache/hook lock is held across a game call. Non-stadium components are immediate
  pass-through. No UObject roots or render resources are acquired.

More visible stadium geometry can cost GPU time. This does not disable occlusion,
change the outdoor sky, or promise support for roof assets with unrecognized
names. Selection spans all stadiums using the validated stadium ownership and
asset conventions. Tokyo Dome and Hokkaido Night have complete captured runtime
A/B evidence; offline coverage is not runtime confirmation of the new override.

## Test before pushing

Enable in behind-the-plate or Wide/Live view first when testing the complete
background, then switch to behind the pitcher. Toggle on/off, then exercise fielding, pause cuts,
celebrations and replays. Verify roof and outfield walls stay visible when enabled, the normal
game behavior returns when disabled, and both eyes/spectator agree. Travel to
another dome and an open-air stadium; old-world targets must not persist. Repeat
with Native and Synced/Ghost, without changing camera-assist settings.

The offline suite covers captured setter bytes, changed/truncated layouts,
multi-stadium name selection, mask/variant exclusions, bounded cache sweeps,
eligibility, restoration and game-visible authorization. The captured fixtures
cover all 366 Tokyo components and 503 Hokkaido components: every one of their
98 and 185 camera-hidden changes is covered, including all seven Tokyo walls,
while their 32 and 10 never-visible variants remain unforced. The original
structural positives remain direct targets. Tests also cover unknown hidden
variants, failed show readback, repeated cuts, rebound forced-value rejection
and clearing proof for disable/travel.
It also covers the live/source native-bool metadata (`ByteMask=1`,
`FieldMask=0xff`), distinct from the component's packed visibility flag.
Initialization refusals log the specific validation stage once.
After failed discovery with no installed hook, an explicit off/on retries setup;
it does not repeat scans every frame or replace an existing visibility hook.
Viewport fixtures cover the captured secondary interface, direct pointers,
unrelated/unaligned/null pointers, overflow and explicit retry behavior.
Build/offline success means ready to test, not runtime confirmation.
