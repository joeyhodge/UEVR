# ProSpi stadium roof and structural outfield visibility

`VR_ProSpiShowStadiumRoofs=false` is the default. Enable **Always Show Stadium
Roof / Outfield Structure** under VR -> Camera -> Game FOV. Match Game FOV, dolly, camera
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
The guard selects 36 roof/structural components from that capture, including all
seven walls. It deliberately does not unhide every outfield asset: 32 unused
display/ad variants stay hidden, and unrelated decoration keeps game behavior.

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
  identify a roof/ceiling or a structural outfield role, and the asset package must
  be under `/Game/Stadiums/`. Outfield component/mesh stems must also agree after
  removing only the standard FBX component suffix and SM_ asset prefix.
- Hidden actors/components, masks, proxies, collision meshes, scoreboards,
  alternate ads, LOD/extra/variant meshes, templates and
  pending-destruction objects are excluded. No roof is invented in open stadiums.
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
  cached targets. Camera-driven setter calls are handled synchronously, so a cut
  does not wait for the background sweep. The check budget does not bound an
  individual game setter or one-time reflected metadata discovery.
- Code-page `VirtualQuery` and instruction decoding run only during setter
  discovery. Names and reflected offsets are cached; the level-list buffers are
  reused. Cached targets retain current identity/liveness and world/level checks
  before mutation, without a duplicate eligibility pass in the Draw loop. This
  reduces CPU work without treating a once-readable UObject as permanently safe.
- The normal game visibility call still executes, including its original child
  propagation. Only a validated roof receives a separate no-propagation visible
  request. No cache iterator is retained across the native call, and readback
  reacquires the current identity's entry. Requests are shadowed so turning off can restore the latest game
  request, rather than the camera state that existed when the option was enabled.
- Visibility state changes run on the game thread, never the render thread. No
  cache/hook lock is held across a game call. Non-stadium components are immediate
  pass-through. No UObject roots or render resources are acquired.

More visible stadium geometry can cost GPU time. This does not disable occlusion,
change the outdoor sky, or promise support for roof assets with unrecognized
names. Selection spans all stadiums using the validated stadium ownership and
asset conventions; only Tokyo Dome has the captured runtime A/B so far.

## Test before pushing

Toggle on/off behind the pitcher and plate, then exercise fielding, pause cuts,
celebrations and replays. Verify roof and outfield walls stay visible when enabled, the normal
game behavior returns when disabled, and both eyes/spectator agree. Travel to
another dome and an open-air stadium; old-world targets must not persist. Repeat
with Native and Synced/Ghost, without changing camera-assist settings.

The offline suite covers captured setter bytes, changed/truncated layouts,
multi-stadium name selection, mask/variant exclusions, bounded cache sweeps,
eligibility and restoration. The captured fixture covers all 366 runtime stadium
components, the 98 visibility changes, seven walls and 32 unused hidden variants.
It also covers the live/source native-bool metadata (`ByteMask=1`,
`FieldMask=0xff`), distinct from the component's packed visibility flag.
Initialization refusals log the specific validation stage once.
After failed discovery with no installed hook, an explicit off/on retries setup;
it does not repeat scans every frame or replace an existing visibility hook.
Viewport fixtures cover the captured secondary interface, direct pointers,
unrelated/unaligned/null pointers, overflow and explicit retry behavior.
Build/offline success means ready to test, not runtime confirmation.
