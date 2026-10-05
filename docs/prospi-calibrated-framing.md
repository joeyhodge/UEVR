# ProSpi calibrated framing experiment

This is an opt-in extension of Learned Assist, not a replacement for the current
modes or a completed ball/geometry-aware camera autopilot. Other games are not
eligible. Historical mode IDs 0/1/2 and the default Assist mode are unchanged.

## Evidence

The Tokyo Dome recording `session-11867238994400` found consistent learned focus
distances of 9,060-10,307 cm for the bad behind-pitcher views. The historical
`OutfieldHomeFovCeiling` reduced them all to 7,600 cm. That produces only about
7,200 cm of forward camera advance and supports the reported zoomed-out result.

Good-labelled pause cuts overlap these same rigs. Good labels are soft feedback,
not ideal coordinates or permission to change all similar shots. The new mode
can also change those overlapping cuts; compare them again during testing.

The fresh reflected SDK provides camera-controller wrappers and a camera-type
interface, but no verified current gameplay phase, ball target/velocity or
stadium collision provider. Generated SDK method bodies are stubs. Older
Dumper7 wrappers for the replay getter have incompatible return signatures and
are not invoked. No new ProcessEvent call or native hook is introduced.

The October 4 fresh Dumper7 v2 ID map passes complete section/bounds validation.
Its camera/replay exec RVAs agree with the live UFunction native pointers. Twelve
bounded camera/replay code samples match the disk EXE byte-for-byte, despite the
nonstandard sections, so a runtime-analysis EXE is not required for this work.
The old game PDBs do not match the current CodeView identity and are not used.
The fresh map still describes the replay getter as void while the code writes
a boolean result; use the mapping as address evidence, not a proven callable
signature. The framing experiment does not call any of these functions.

## Enable

In Unreal / Match Game FOV / Auto Camera Sequencer, select:

`Calibrated Framing (Experimental)`

Keep Match Game FOV, Dolly, Auto Camera Sequencer, Camera Safety Guard and its
Outfield Low Rule enabled. Leave Broad Cinematic Camera Assist (Legacy) off, as
in the first recording; its separate floors/latches keep their original path.
Use the existing saved pose calibrations. Existing FOV controls, game-authored
rotation, HMD projection and exact saved overrides keep their previous behavior.
No calibration file or profile setting is automatically rewritten.

The corresponding existing mode setting is:

```ini
VR_MatchGameFOVProSpiAutoCameraSequencerMode=3
```

Restore `Learned Assist` (mode 2) for an immediate comparison/fallback.

## Acceptance and fallback

The center-field behavior only bypasses the generic outfield focus ceiling for a central,
telephoto, home-facing center-field rig with two or three matching pose entries.
Entries must be finite, nearby, from the same supported preset family, at least
0.8 confidence and agree within a 25% focus spread. The blended focus must match
the current learned decision, stay below 15,000 cm and exceed the old ceiling
by no more than 40%. These are conservative trust bounds, not measured subject
distance or stadium dimensions.

The predicted forward advance must also remain in the bounded central framing
envelope after allowing for right/up offsets and maximum safety lift, and the
configured floor must be achievable within that lift budget. This validation
uses the same observed default FOV as the final dolly calculation. The existing
downstream floor/lift/cap logic remains active and unchanged.

Missing, weak, conflicting or unsafe support uses the historical Learned
Assist ceiling. Behind-plate, dugout, stand, unknown, under-stand and other risky
rigs receive no new outfield ceiling bypass. Exact overrides retain priority. Fallback
reasons are shown in ImGui; the trace records the status and proposed focus/dolly.
An active legacy cinematic option or segment latch also keeps the old path,
so an accepted decision cannot then be overwritten by those later policies.
The center-field policy is stateless, with no saved targets or world pointers
to carry over between cuts, mode changes or level loads.

This mode never creates a ball-follow target or changes camera yaw/pitch to
chase a ball. It follows the authored camera's movement. Existing `BallFollow`
labels are heuristic and are not used as proof of a real ball position.

## Low baseline / celebration refinement

The second Tokyo Dome recording, `session-35746876825400`, confirmed three
separate problems in the historical policy. A nearby 220 cm saved focus was
raised to 1,250 cm until FOV crossed 18 degrees. A continuous pan lost its
preserved dolly at a 110-degree yaw boundary and collapsed to zero. The height
estimate subtracted forward motion even when the actual upward-pitched camera
advanced upwards, producing a matching neutral view about 483 cm above the
field. These are recorded discontinuities, not a verified celebration subject.

Mode 3 now gives a bounded low baseline rig a separate path:

- Nearby short-focus matches (at most three, at least 0.828 confidence, within
  500 cm and agreeing within 25%) can retain their original 10-1,250 cm focus
  instead of the conflicting generic baseline floor. A valid current camera
  and live view-target identity are required before committing the focus.
- Focus transitions are limited to 600 cm/second. A lost validated match can
  retain the current focus for at most 250 ms; it does not retain an old subject.
  Real cut flags, significant pose jumps, world/camera/target identity changes,
  target index/serial reuse, rendering-mode changes, gaps over 250 ms, invalid
  data and leaving the supported path reset continuity. FOV itself is not
  smoothed, and quantized pose IDs are not treated as authoritative cuts.
- Height uses the signed forward/up basis used by the stereo path. It avoids
  unnecessary lift on upward shots, preserves nonzero forward dolly across
  small preset/yaw boundary changes, and caps downward advance if the configured
  floor cannot be reached within the existing lift budget. Two centimetres of
  clearance cover the tiny supported roll contribution; no floor is lowered.

This requires the Field Floor rule, full dolly-cap strength (1), baseline rule
off, legacy cinematic assist off, actual FOV clamping/cut stabilization off,
and no generic preset auto-apply. Stand/outfield bands are excluded using the
configured thresholds, not guessed stadium geometry. Large offsets, roll over
0.01 degrees, decoupled pitch, unsupported rigs or excessive floor/lift budgets
keep the historical path. Exact overrides retain focus priority; safety still
uses its validated geometry when applicable.

The runtime identity probe is independent of recording, uses validated reflected
properties and object liveness, and never invokes a new native/ProcessEvent call.
It does not hard-code game addresses or automatically rewrite saved calibration.
The geometric envelope was tested against Tokyo Dome evidence; it is not proof
of a different stadium's collision, subjects or ideal focus.

## Test sequence

1. Use the same Tokyo Dome setup, existing calibration and other settings.
2. Test the behind-pitcher low/offset/high pitching views with mode 3, then mode 2.
3. Check paused cinematic cycles, especially the overlapping center-field cuts.
4. Recheck behind-plate pitching, hits/fouls and live ball-in-play shots.
5. Record inside the stadium, mark good/bad if possible, then stop recording.
6. Replay a home-run celebration on both sides of the field, including transitions
   into and out of the dugout. Compare mode 3 with mode 2 without changing other
   settings. Opposite-side focus is never inferred by mirroring a saved entry.

Offline fixtures cover 45 first/middle/last observations from bad pitching,
overlapping soft-good cinema, behind-plate and moving gameplay segments.
Guard tests cover old-mode no-ops, capability gates, exact overrides, non-finite
inputs, unsupported/weak/conflicting matches, overshoot and insufficient height.
An optional **offline-only** shadow review uses the original trace snapshot:

```powershell
prospi-camera-framing-tests.exe --shadow "path/to/session" "external/review.json"
```

Shadow output is not runtime validation and cannot be loaded as calibration.
It keeps good/bad marks separate from numeric acceptance. No observed label is
automatically promoted into a camera rule.

The celebration suite adds 427 complete camera observations with matching
neutral stereo views from the two adjacent celebration cuts. It independently
reconstructs the quaternion basis and tests continuity resets, frame-rate
independence, signed lift/cap budgets, invalid evidence and old-mode no-ops.
The separate offline review command is:

```powershell
prospi-celebration-framing-tests.exe --shadow "path/to/session" "external/review.json"
```

Both original recordings were shadow-reviewed. The latest recording's 3,220
accepted center-field samples remain unchanged by the celebration refinement.
The earlier soft-good low-rig shot changes height by about 2 cm for floor
clearance; labels alone are not evidence that any new camera is visually ideal.

## Remaining work

Unsupported low/dugout and high-lift cinematic cases remain on the old path.
Subject-aware aim/focus and collision-aware refinement still need reliable
subjects or stadium geometry and another recorded comparison. Do not globally
lower stand/dugout floors based only on suspect flags: several soft-good cuts
hit the same lift limits. A passing offline review is ready for an HMD test,
not confirmation that every celebration now has the ideal target.
