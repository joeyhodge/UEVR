# Edith Finch legacy aiming

Finch uses UE4.10.1, but has no Windows version resource and its native branch
name does not match the generic version recognizer. Its legacy HMD camera
callback also receives a camera manager and POV, not the quaternion/position
outputs used by newer XR camera callbacks. Selecting a version table alone is
not a safe fix.

The Finch adapter hooks the verified native `ProcessViewRotation` method with
its original four-argument signature. It calls the game first, then reuses the
existing HMD/controller aim calculation only for the current, reciprocal
local-player/controller/camera binding. It does not publish a fake HMD, write
camera-manager/POV storage, patch a following camera call, or perform a second
control-rotation update during the engine tick.

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

## Verification

The existing compatibility-policy suite includes original-code fixtures,
changed-code rejection, ASLR, ownership/retirement, mode gating, and bounded
setup tests. `--finch-image <RVA-mapped-analysis-image>` additionally checks
the contracts against the saved original executable capture.

In-game verification is still required: HMD aim, each controller hand,
controller loss/reconnection, Game-mode restoration, scripted cameras and
level changes, with and without Native Fix. A successful build is not evidence
that these runtime transitions have passed.
