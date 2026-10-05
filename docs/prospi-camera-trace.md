# ProSpi/eBaseball Camera Trace

## Purpose and scope

An opt-in, record-only recorder for the ProSpi executable family. It does not
change camera location, rotation, FOV, dolly, safety settings, rendering mode,
CVars, or camera_calibration.json. Native, Native Fix, Synced, Ghost, Mono and
DIBR continue using their existing camera logic. Other executables cannot start
the recorder.

Ball-follow means preserving a shot where the **game camera is already following
the ball**, e.g. a hit or foul. It never means aiming every pitcher/batter,
dugout, crowd, replay or establishing shot at the ball.

## First recording

1. Inject normally, with the existing working Learned Assist/settings unchanged.
2. After loading the stadium and letting injection settle, press **Ctrl+Alt+F9**. Alternatively
   enable **Record ProSpi Camera Cuts** in the Unreal/Match Game FOV ProSpi panel.
3. Record one stadium and play normally through pitching, hits/fouls, action shots,
   player close-ups, dugout/stands and replay cuts. Do not run Dumper7 or a broad
   UObject scan in the same run if avoidable; it can perturb timing.
4. Optionally press **Ctrl+Alt+F6** for a bad cut or **Ctrl+Alt+F7** for a good
   cut. Automatic suspect flags already retain fast cuts; manual marks are not
   required. A delayed mark identifies its current shot plus five seconds of
   preceding context, not a guaranteed earlier shot.
5. Press Ctrl+Alt+F9 to stop. Stop flush is asynchronous; wait until the UI status
   is Off before closing if possible. During recording files flush each second.

The UI shows written/dropped events, suspect samples and disk usage. Error or
storage-limit status affects only recording. Toggle off/on to start a new
session after an error. Keys can be changed/unbound in the panel and always
require Ctrl+Alt and the foreground game window. They are not consumed by UEVR.

Start inside the stadium, not at the main menu: the first recording wrote about
20 MB/minute. Stop between separate pause/pitching/ball-in-play comparisons if
convenient. The requested toggle saves normally, so turn it off before an
SDK-only session if recording is not wanted.

Pre-injection start is also supported with this profile setting:

```ini
VR_ProSpiCameraTrace=true
```

Default is false. Optional config-only stadium label (one stadium per session):

```ini
VR_ProSpiCameraTraceStadium=your-stadium-name
VR_ProSpiCameraTraceBadKey=117
VR_ProSpiCameraTraceGoodKey=118
VR_ProSpiCameraTraceToggleKey=120
```

Those decimal virtual keys are F6/F7/F9. Start/stop via hotkey is synchronized to
the displayed setting and saves with the normal UEVR config save operation.

## Files and measured evidence

Each session writes under `profile/camera_traces/session-*`:

- metadata.json: schema/build/dependency identity, selected camera settings,
  variant/RHI/runtime, stadium label and a read-only calibration snapshot.
- events.jsonl: original local input used by assist; learned candidates and
  normalized weights; raw/effective/base FOV, actual final focus/dolly, safety
  estimate/lift, play classification and segment/cut stabilizers.
- View events: stereo-hook input, neutral pose before headset movement and final
  per-eye pose; actual forward/right/up vectors, world scale and cache age.
  Poses use pitch/yaw/roll degrees and Unreal centimetres. View FOV fields are
  **hints from assist**, not measured projection FOV; projection events contain
  the actual column-major matrix returned to the engine.
- Post-tick observations: freshly read, reflection-validated PCM cache pose,
  timestamp and game-cut flag. The PCM has object-array identity/liveness checks;
  the writer and render callbacks never dereference a saved UObject.
- summary.json on normal stop: loss/probe failure counts and final status when
  enough space remains. Event/metadata/summary storage is capped at 512 MB.

Every view/projection embeds its coherent assist snapshot and camera transaction
number. This is diagnostic association, not a new engine frame counter or a
change to capture acceptance. World/PCM changes and unavailable camera invalidate
the epoch; late view tickets from an old world/session are discarded. Auxiliary
view family roles remain unclassified. `input_matches_assist` is a geometric
sanity check, not proof that a view belongs to the main scene.

Normal persisted rate is up to 30 Hz per event/eye channel; cut/marker/new suspect
bursts retain the preceding bounded five-second history and five seconds after.
Camera input enqueue is limited to 120 Hz except critical cut/suspect edges.
Queue and history have fixed limits; producers use try-lock and **drop evidence
rather than block rendering**. Drop counts must be considered when reviewing
coverage. Disk I/O and JSON serialization run on a background writer.

## Important limits

- The pre-tick cache can contain the previous assist's FOV write. That provenance
  is recorded; compare post-tick and stereo input rather than assuming every
  cached value is untouched game intent.
- Existing `BallFollow` classification is a camera-motion heuristic, not a
  validated ball-position source. Authoritative ball/velocity/subject telemetry
  is explicitly unavailable in this initial build. No mesh-name guess is used.
- The present safety floor is a configured estimate, **not stadium geometry**.
  Real field/dugout/stands collision has not been established. No diagnostic
  ProcessEvent collision queries, native hooks or guessed offsets are installed.
- Large lifts, conflicting learned neighbours, estimate/output discrepancies,
  abrupt focus changes and stale observations are **suspects**, not automatic
  declarations of an incorrect camera. Invalid/ambiguous evidence is never fed
  directly into calibration or camera corrections.
- Settings are snapshotted at start and per-frame effective decisions are
  recorded. Start a fresh session if changing the broader camera profile.

## Offline review

```powershell
python tools/analyze_prospi_camera_trace.py "C:\Users\josep\AppData\Roaming\UnrealVRMod\prospi-Win64-Shipping\camera_traces\session-..." --output "D:\Downloads from Edge\ProSpiCameraReview"
```

The tool streams the JSONL (including an interrupted final line), generates a
shot catalogue, camera-path CSV/SVG, prioritized suspect report and a separate
candidate_review.json. CSV timestamps are absolute steady-clock seconds; report
times are relative to the first event. The SVG is camera XY motion, not a
stadium/ball map. It refuses trace/profile ancestor output destinations and
never writes an importable calibration or auto-applies suggested values.

## Turning evidence into automatic assist

1. Review the first stadium's shot catalogue and measured neutral paths against
   raw game pose/dynamic FOV. Identify ambiguous nearby entries and excess lifts
   rather than silently overwriting the existing 53-entry calibration set.
2. Establish real game-authored camera/subject identity and a direct ball provider
   for ball-follow shots. Missing capability keeps the existing assist unchanged.
3. Build stadium-aware shot matching (pose, FOV, play/replay phase, subject and
   intended direction); derive framing/dolly from validated target distance and
   actual projection. Preserve the game-authored changing FOV, not a fixed zoom.
4. Measure local ground/dugout/stand geometry before replacing broad safety
   floors. Use measured camera-local versus world-Z displacement to avoid lifting
   ordinary close-ups excessively.
5. Shadow-evaluate proposed rules, test opt-in A/B with hysteresis at cuts, then
   expand to more stadiums. Retain existing assist on missing/ambiguous evidence.

This phase prepares recording and review; it does not claim that a fully
automated, ball/geometry-aware camera is already implemented.
