# Steam Frame controller input

This is an input-only extension for OpenVR and OpenXR. Rendering, existing
gamepad mappings, poses, haptics and other controller bindings are unchanged.
Native support follows the controllers, not the headset. OpenXR requests
`XR_VALVE_frame_controller_interaction` only when advertised. Its native profile
is `/interaction_profiles/valve/frame_controller_valve`; OpenVR uses
`frame_controller`.

## Lua API (backend SDK 2.41)

```lua
local vr = uevr.params.vr
local source = vr.get_right_joystick_source()
local kind = vr.get_controller_type(source) -- "frame", "touch", "index", etc.
local profile = vr.get_controller_profile(source) -- actual runtime identifier
local squeeze = vr.get_action_handle("/actions/default/in/Squeeze")
local axis = vr.get_action_axis(squeeze, source)
local grip = vr.get_action_state(vr.get_action_handle("/actions/default/in/Grip"), source)
local touch = vr.get_action_state(vr.get_action_handle("/actions/default/in/GripTouch"), source)
-- axis.active / axis.value (0..1), grip.active / grip.pressed, touch.active / touch.pressed
```

Use dot calls, like the existing `uevr.params.vr` function-pointer bindings.
`active` means the runtime has usable action data, not that a button is pressed.
Unavailable, unbound, invalid, or non-finite input is neutral: inactive and
zero/false. The analog grip and its end-stop click are separate from bumper and
trigger input. `TriggerAxis` exposes the analog trigger through the same API.
The normalized type is `unknown` before a valid observation, after invalidation,
or for an unrecognized controller. Read `get_controller_profile` for other raw
identifiers. OpenXR Touch emulation reports Touch. OpenVR reports the hardware
controller type even when using compatibility bindings: check action availability
before assuming native controls are bound. The bridge adapter requires an active
bumper binding before changing its legacy translation.

Sources follow UEVR's existing controller-swap setting. Each identifier describes
the physical controller behind that source. Identity is cached after input sync
and refreshed only after invalidation (retries are throttled); getters do not
scan devices or query runtime properties. Profile strings are copied to caller
storage. New C/C++ accessors are appended to the 2.40 API, and C++/Lua wrappers
check the backend version before reading the new tail. Older plugins remain
compatible; the updated LuaVR bridge also returns neutral input on older backends.

Lua source getters preserve OpenXR's valid zero-valued physical-left source as
opaque light userdata rather than `nil`, including when hands are swapped. C/C++
source values and all action/haptic consumers retain their existing ABI. A
missing OpenVR source or an unready zero source still returns `nil`. Older Lua
scripts passing `nil` as OpenXR's physical-left source remain compatible.

## Touch controls

New optional boolean actions are `TriggerTouch`, `GripTouch`, `BumperTouch`,
`JoystickTouch`, `DPad_UpTouch`, `DPad_RightTouch`, `DPad_DownTouch`,
`DPad_LeftTouch`, `StartButtonTouch` and `BackButtonTouch`. Prefix them with
`/actions/default/in/`. Face-button touches retain their existing actions.
Frame retains its native touch bindings. Touch/Quest compatibility profiles also
bind `TriggerTouch`, `JoystickTouch`, and the optional analog `TriggerAxis` and
`Squeeze` where the runtime accepts their components. OpenVR Touch additionally
binds the driver's advertised `GripTouch`, face touches and thumbrest touches.
OpenXR Touch/Touch Plus do not expose a separate grip-touch component: `GripTouch`
remains inactive there, not inferred from grip pressure. The actual driver and
active interaction profile determine availability, not the headset model.
Touch sensors and the newly exposed squeeze axis do not activate controller
gamepad-focus detection by themselves. Steam/system buttons are not appropriated.
This exposes the documented touch states, not arbitrary finger-joint tracking.

## Face buttons and existing Lua scripts

The native defaults send standard A/B/X/Y gamepad bits. UEVR's historical Lua
action aliases describe a different layout than the physical Touch labels:

| Physical Frame button (right controller) | Click action | Touch action |
| --- | --- | --- |
| A | AButtonRight | AButtonTouchRight |
| B | AButtonLeft | AButtonTouchLeft |
| X | BButtonRight | BButtonTouchRight |
| Y | BButtonLeft | BButtonTouchLeft |

Despite the `Left` suffix, Frame's B/Y actions are read from the actual right
controller source. Scripts using Touch aliases must choose the Frame aliases
when native Frame input is established. Neither legacy aliases nor correctly
mapped gamepad bits are globally swapped.

`examples/lua/steam_frame_satisfactory.lua` is an optional adapter for the older
Satisfactory UEVREnhancements bridge. It preserves legacy B/X and shoulder/grip
translation for non-Frame input, uses Frame's gamepad B/X without re-swapping,
and reads Frame grips independently. Its change detector includes grip-only
presses/releases, not just changes to XInput buttons. It installs no callbacks
and changes neither the gamepad state nor saved bindings.

```lua
local adapter = require("steam_frame_satisfactory") -- place beside the profile script
local previous = {}
-- Inside the existing XInput callback, replace only its button translation:
local buttons = adapter.buttons(uevr.params.vr, state.Gamepad)
if adapter.changed(buttons, previous) then
    previous = buttons
    uevr_bridge:UpdateButtonState(buttons.a, buttons.b, buttons.x, buttons.y,
        buttons.left_stick, buttons.left_grip, buttons.right_stick, buttons.right_grip, buttons.start)
end
```

The old game-side bridge accepts grip booleans, not a new analog-grip or touch
protocol. Those additional states are available to Lua; this patch does not
invent new game-side methods. Verify the tester's profile version before applying
the adapter to an independently customized bridge.

## Saved customizations and testing

OpenVR's `bindings_frame_controller.json` is automatically upgraded only when
its complete saved JSON matches the initial stock Frame defaults. Current stock
is not rewritten. Customized/partially edited, unreadable, malformed, duplicate-key,
deeply nested, oversized or linked files are preserved. Publication uses a complete
temporary file and same-directory rename, with the original bytes checked again
before replacement; it never truncates the saved Frame file. Custom OpenXR profiles
are unchanged. Custom files may leave new touch actions unbound (`active` is false);
bind those controls in the runtime or deliberately restore just the Frame default.
OpenVR's `bindings_oculus_touch.json` is upgraded only if the complete saved JSON
matches the stock old or current defaults. Customized, unreadable, malformed or
oversized Touch files are preserved; bind the extra actions manually or restore
just the Touch defaults deliberately. Existing digital/gamepad mappings, poses
and haptics are unchanged. Passive touches and the exposed grip axis do not activate
spoofing.

## Bounded gamepad recovery

With controller/pause intent and no selected XInput poll for two seconds, device
notification/window recovery uses 2/4/8/16/30-second retry delays, capped at 30
seconds. A selected XInput poll resets the retry schedule; retries do not stop
permanently. Recovery is suppressed unless OpenXR is ready and FOCUSED, or OpenVR
is ready and `IsInputAvailable()` is true. A restarting runtime never recovers the
window. Focus is rechecked immediately before window activation, outside the
action mutex. Unfocused recovery probes are limited to one per 250 ms.

If the engine has not ticked for over one second, XInput's action-sync fallback
shares one 8 ms minimum interval across indices/callbacks and obeys the same focus
gate. Recovery bookkeeping uses its own short critical section, not the mutex
held during VR driver action updates. Normal engine-tick action updates, physical gamepad processing, rendering
and runtime frame waits are not throttled. The separate OpenVR 30 ms restart
heuristic is unchanged. A fallback action sync skips a busy action mutex rather
than queuing more work behind the driver. These limits reduce redundant recovery
work, not diagnose the cause of a game's high CPU usage. No new configuration
setting is required.

Offline tests cover both generated runtime mappings, click/touch parity,
installed OpenVR driver components, neutral states, mixed/swapped controllers,
profile invalidation, guarded 2.40 API storage, production Lua registration,
the optional bridge adapter, exact stock migration, failed/locked-file publication,
retry backoff/focus, and concurrent stalled-engine fallback polling. Hardware
tests still need both runtimes, dashboard closed/open/closed, reconnect, B/X click
and touch, independent bumpers/grips, analog grip/trigger, reconnect, hand swap,
and an existing Touch controller/profile comparison.

Reference: <https://partner.steamgames.com/doc/steamhardware/steamframe/input>
Input focus: <https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSessionState.html>
and `IVRSystem::IsInputAvailable` in <https://github.com/ValveSoftware/openvr/blob/master/headers/openvr.h>.
