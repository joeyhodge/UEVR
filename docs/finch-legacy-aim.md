# Edith Finch legacy aiming

Finch uses UE4.10.1, but has no Windows version resource and its native branch
name does not match the generic version recognizer. Its legacy HMD camera
callback also receives a camera manager and POV, not the quaternion/position
outputs used by newer XR camera callbacks. Selecting a version table alone is
not a safe fix.

Finch's movement-camera tick later overwrites `ControlRotation` from its camera
component. An early `ProcessViewRotation` change therefore does not establish
that the game camera accepted the aim. Recentring at that early point can remove
head motion that never reached the camera.

The adapter hooks the verified native camera handoff instead. Its five-argument
ABI includes the hidden rotator result, frame delta, requested world rotation
and existing face-pawn flag. The original helper normalizes the selected
rotation, updates the controller, and returns it to the movement-camera caller
which applies that same orientation to its camera component. Only this exact
consumer callsite and a reciprocal live local-player/controller/camera binding
are accepted.

Pose planning is side-effect-free. Recenter and automatic decoupled-pitch state
are committed only after the native return is validated. There is no fake HMD,
camera-manager/POV storage write, following-call NOP, early rotation mutation,
or second engine-tick control-rotation writer. If the game stops using the
handoff, the existing timeout also restores only pitch state owned by the
Finch adapter.

## Guards

- Exact executable identity, PE metadata, native 4.10.1 changelist, original
  code witnesses, and matching camera/controller vtable methods are required.
- Reflected ownership fields must match the native callsites. Objects and
  their classes must still belong to the live object array on each aim call.
- Game aim, inactive HMD, unavailable controllers, invalid poses, conflicting
  camera control, and unknown builds preserve the game's rotation.
- Setup is limited to three attempts, at least one second apart. Returning to
  Game aim permits a retry if initial metadata discovery was too early.
- The hook is prepared disabled and published before activation. Rendering,
  Native Fix, UI, eye rectangles, gamma, and other games are unchanged.

The SDK also recognizes this validated build's legacy `Hand`/`PlayerIndex`
motion-controller properties instead of expecting a modern `MotionSource`.
This is not automatic creation of VR hands or game-native grabbing support.

## Journal Camera

Finch's pause journal is world-space geometry, viewed by a separate camera
pointing down at the book. Flattening that camera's pitch places the book
sideways or below the HMD. The menu exception preserves its authored rotation;
it does not change the working gameplay aim planner or control-rotation hook.

Bounded game-thread preparation verifies the existing exact-build contract,
reflected `MeshTextMenu.MenuCamera` ownership, native controller/camera fields
and the menu instance in the streamed Menu level. The menu actor, camera, level
and class identities retain any available object-array serials. Current classes,
local ownership and the selected camera reference are rechecked even when UE4
has not allocated weak serials. Eye callbacks do not
scan objects, convert names, call menu functions or modify game objects.

Only the validated journal camera and player-to-journal blends bypass pitch
flattening. Other scripted cameras, stale/unknown state and normal gameplay
keep the existing path. This applies when Decoupled Pitch is active, including
manual use in Game aim and automatic use in HMD/controller aim. No saved option
is changed. The UI pitch-compensation cache is neutral only for this menu view.

## Verification

The existing compatibility-policy suite includes original-code fixtures,
changed-code rejection, ASLR, ownership/retirement, mode gating, and bounded
setup tests. `--finch-image <RVA-mapped-analysis-image>` additionally checks
the contracts against the saved original executable capture.

In-game verification is still required: HMD aim, each controller hand,
controller loss/reconnection, Game-mode restoration, scripted cameras and
level changes, with and without Native Fix. A successful build is not evidence
that these runtime transitions have passed.
