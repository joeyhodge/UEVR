# Health, frame pacing and immediate-operation validation

## Scope

This work adds observations and removes duplicate immediate-operation checks.
It does not change rendering methods, camera/player/stadium rules, CVars,
Same Pass eligibility, native view states, swapchains, GPU commands, frame
prediction, async scheduling, wait ownership or submit/retirement policy.
AFW and the installed injector package are separate workstreams.

## Session-only pacing recorder

**VR > Health / Diagnostics > Record Frame Pacing** is off on every launch,
independent of saved profiles. No expensive hitch sampler is enabled with it.
The fixed 4,096-event ring records CPU intervals for UEVR present/post-present,
DX11/DX12 frame work, selected DX12 UI/scene-copy and spectator recording scopes,
OpenXR wait/begin/end and OpenVR WaitGetPoses. A manual marker is available.

Producers make one gate attempt; contention drops a sample instead of waiting
or spinning. They allocate no memory, format no JSON, write no logs/files and
read no additional engine resources. The disabled path reads an enable token
and performs no diagnostic clock/thread queries. Snapshot/formatting work is
explicitly UI/export work. An export can itself stall the menu; do it outside
the interval being measured. Exports hold the event gate only while copying
plain values. Ring overwrite, lifetime drop count and busy snapshots are
explicit, not fabricated zero-cost frames.

Stopping retains the current epoch. Re-enabling starts a new logical epoch;
late spans from an old epoch cannot append to a new recording. A publication
that already entered its gate may complete while the enable token changes;
new-epoch readers cannot see the old payload. Concurrent snapshot and overwrite
accesses are serialized by the same try-only gate, not a raced-data seqlock.

Wait events annotate actual thread ID and the captured operation: normal,
wait-only worker, array/pre-acquire worker, or a normal consumer fulfilling a
reserved wait. No displayed "active" flag is used to guess the producer.
An immutable observation-only runtime ID separates replacement runtimes even
when their session-generation or frame counters restart. Rendering never reads
that diagnostic ID and no raw runtime pointer is retained in the ring.
Wait-only decision events distinguish busy frame/assignment locks, startup,
reinitialization, invalid submission identity, an already-begun/synchronized
frame, reservation availability and the existing conservative pose/slot guard.
That guard and the two-phase pose admission protocol are unchanged.

The pose/slot reason is deliberately called `next_pose_or_retained_slot_evidence`:
the current guard is conservative and a populated recycled ring slot is not
proof of a fresh next-frame pose. This recorder does not authorize relaxing it.

Frame domains are explicit: an engine frame argument, runtime-internal frame,
UEVR render frame or next-frame candidate is not silently relabelled as another
clock. Unknown frames/results/display times remain null. The observed wait
sequence is the runtime's clock at the observation; it does not prove which
wait generated an older pipelined submitted image. Actual API display times
are recorded. QPC/steady-clock bracketing in health exports supports ETW
correlation. Event sequence is publication order, not start/end-time order.
The wait API's end timestamp/result is fixed before later clock publication,
logging or unwinding, so that bookkeeping cannot inflate its measured interval.

**These are CPU intervals, including time blocked inside an API. They are not
GPU execution times or motion-to-photon latency.** Nested scopes overlap and
must not be added together as independent costs. GPU timestamps are intentionally
not added to this first pass: they need their own delayed query/readback and
retirement tests, without adding fence waits or modifying game command lists.
An OpenXR runtime can throttle xrWaitFrame intentionally; a long wait or a
fallback alone does not identify the source of a hitch or establish causation.

## Read-only health view

The health view consolidates backend/SDK build IDs and tracked-dirty flags,
requested/effective mode, RHI/runtime, Native Fix/Ghost Fix status, automatic UI
capability, pre-5.7 observation status, wait completion counters, selected
conflicting requests and existing resolved-CVar observations. It never toggles
options automatically or promises image correctness from readable resources.

Refresh is explicit and requests existing resolved-CVar readback on the next
game tick. Cached JSON is not rebuilt every frame; sample ages, missing values
and contended snapshots remain unavailable rather than becoming zero/false.
Ghost/UI status readers are try-only; cached CVar and UI-pose export readers
also use nonblocking attempts. Existing engine sampling and write policy are
unchanged. The view calls no version scanner. File-resource version is labelled
metadata, not engine ABI authority; detailed engine/layout evidence remains in
the existing Hook Provenance export.

**Export Support Snapshot** atomically replaces only the diagnostic JSON, with
process ID/start identity, loaded-backend path, build identity, health and pacing
history. It neither enables other diagnostics nor modifies profile settings.
See [support capture instructions](../tools/support/README.md) for exact-image
DLL/PDB verification and optional self-owned CPU/wait recording. Build packaging
includes those helpers automatically beside the backend.

## Validation changes

- All Native Fix eye-pair checks may share a successful scene-state type proof
  inside the existing locked read-only pair operation. Every live state pointer,
  current vtable and first slot is still read. Different/mutated/unreadable
  identities reject or need a new proof. The batch is destroyed at operation
  exit and cannot authorize later packets or survive a native call.
- A Slate boolean-slot call reads the object table once, performs the same full
  twelve-slot/six-executable-entry type test, reads/checks the requested slot
  once and validates its executable address immediately before the original
  guarded call. Redundant object/slot-range queries are removed; derived-address
  overflow now rejects. No virtual slot is guessed and no new engine call is
  made. Custom-game adapters and the original SEH boundary remain intact.

There is no persistent pointer cache, skipped ownership predicate, relaxed
descriptor/generation rule or version-only authorization. Stable predicate and
mutation tests prove the local rule, not immunity to arbitrary concurrent
destruction or a universal FPS improvement. HMD testing is still required.

## Verification

The existing diagnostic test target covers epoch/overwrite/drop/busy behavior,
concurrent writers/readers/re-arm, owner restoration, complete-once spans,
null/unknown fields, health findings and every stable slot predicate combination.
Existing eye-pair tests exhaust shared/distinct state-type combinations and
mutation/capacity/nesting cases. The support-capture suite builds a dedicated
harmless fixture and tests real file/PDB/mapped-image identity, malformed files,
PID/run changes, profile isolation and script ownership. It is a dependency of
the already-built diagnostic target, so CI cannot register a missing executable.

First compare the new build with recording OFF: Native/Fix, Synced/Ghost, Mono,
menus/loading, focus/resolution changes and exit. Then enable pacing separately,
reproduce a representative scene and export it. A matching OFF/ON wait-only test
must keep Same Pass on, arrays/pre-acquire off, and all other settings identical.
Offline builds/tests do not establish headset rendering or an FPS gain.

References reviewed:
- [OpenXR Tutorial, Windows/D3D12](https://openxr-tutorial.com/windows/d3d12/):
  educational graphics scaffolding, not production lifetime policy.
- [Khronos xrWaitFrame](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitFrame.html):
  externally serialized waits and coherent display-time ownership.
- [Microsoft D3D12 timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing):
  GPU timestamp queries are a separate clock/readback contract.
