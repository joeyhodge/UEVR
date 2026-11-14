# Galactic Racer Panini tests

Both options default off, appear in Unreal -> Compatibility for SWGR UE5.7.4
DX12, and can be changed during a session. Enable only one at a time; selecting
both requests CVar restoration and leaves the component path inactive. Other games and rendering modes
are not modified. No alpha/FOV/world-scale/UI settings are changed.

```ini
VR_Compatibility_SWGRDisablePanini=true
VR_Compatibility_SWGRPaniniComponentFallback=false
```

The preferred test sets the game's boolean `Gfn.Panini.Enable` to zero through
the validated console interface. It preserves priority, checks readback, and
restores its saved value when disabled/VR becomes inactive if the object,
flags and value still match its ownership. Console/other external changes are
not fought. Unavailable or read-only variables fail closed. Retries are limited to eight,
two seconds apart; toggle off/on to retry. Shutdown does not call Unreal console
functions from an arbitrary destructor thread.

A read-only menu -> level trace confirmed that the game re-enables the same
variable at `SetByCode` priority. While this option remains enabled, that specific
reset can be re-armed at the variable's current priority, without raising it.
The object and non-priority flags must match; only constructor/code -> code
priority is accepted. Console priority, other priority changes, replaced objects
and read-only/unregistered variables retain the original fail-closed behavior.
There are at most three automatic re-arms before the path stops. Ten seconds of
successful two-second health polls replenishes this allowance for later level
changes; repeated resets do not. Each re-arm saves the game's latest enabled
value and flags as the restoration baseline. A rejected re-arm stops recovery
rather than retrying as an unowned initial write. Disabling the option never
re-acquires or restores over a value changed by the game or another owner.

UE5.7's console interface adds SetHelp ahead of GetFlags. This test decodes
the original slot-3 scalar flags getter rather than invoking an older SDK slot;
unknown accessors are rejected without executing them or changing SDK behavior.

The separate component test bypasses backend console lookup:

```ini
VR_Compatibility_SWGRDisablePanini=false
VR_Compatibility_SWGRPaniniComponentFallback=true
```

It requires exact original code and unwind sizes for the current game's Tick,
D/S writers and tagged setter. Unknown updates or existing detours reject it.
On the game thread only, the original component Tick sees its temporary-camera
suppression byte set. The original game code resets its Panini values; the
byte is immediately restored after Tick. Existing temporary-camera state and
other threads remain untouched. No component is disabled, retained, enumerated
or called speculatively. A dormant installed hook is retained across checkbox
changes rather than patched/unpatched every frame.

The current EXE and fresh CVar dump establish that this path exists; they do
not prove it causes the reported high-speed VR distortion. Test the same race
and speed with only one option changed, then test return to normal settings,
camera changes and level transitions. The preferred name-based CVar path is
less update-sensitive than the deliberately exact-build component fallback.
