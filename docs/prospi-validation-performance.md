# ProSpi validation cost follow-up

The November 6 CPU/wait trace used the confirmed 22e8a9a9 backend, Native Fix,
Balanced player preservation on, Heavy off and Hitch Diagnostics off. It has no
reported lost buffers/events. The direct player-preservation callback is small
in that capture; restoring players can still add native animation/render work.

Repeated Windows memory checks in Slate vtable and stereo scene-state validation
account for substantially more sampled CPU than the player cache. The stadium
guard also reaches the checked UObject reader, but its scope and original
visibility/lifetime checks are unchanged by this patch.

Two redundant pieces of work are removed:

- The existing vtable rule still requires a readable twelve-slot table and six
  executable entries. Probing stops after six successes, or once the remaining
  slots cannot reach six. All 4096 stable success/failure combinations return
  the original result. Valid tables usually need six executable-range queries
  instead of twelve; no pointer or memory-region validity is cached.
- Scene-state validation no longer asks whether the state pointer is readable
  twice consecutively. Its existing safe_read_value still validates the state
  on every call. Vtable readability, a nonnull first function, executable memory
  and loaded-module ownership remain mandatory.

There are no changes to player selection, culling, stadium policy, camera
framing, view-state ownership, rendering/submission cadence or settings. The
sampled trace is evidence of CPU cost, not proof these changes remove every
hit/replay stall or guarantee thirty frames per second. HMD testing remains
necessary. This follow-up is isolated to the eBaseballfocused workstream.
