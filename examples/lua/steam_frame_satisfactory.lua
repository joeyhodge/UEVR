-- Optional adapter for the Satisfactory UEVREnhancements bridge.
-- It does not install callbacks, change bindings, or modify the gamepad state.
local M = {}
local A, B, X, Y = 0x1000, 0x2000, 0x4000, 0x8000
local LB, RB, LS, RS, START, BACK = 0x0100, 0x0200, 0x0040, 0x0080, 0x0010, 0x0020

function M.buttons(vr, gamepad)
    local bits = gamepad.wButtons
    local function down(mask) return bits & mask ~= 0 end
    local left_frame, right_frame = false, false
    local left, right
    if vr.get_controller_type ~= nil and vr.get_action_state ~= nil then
        left, right = vr.get_left_joystick_source(), vr.get_right_joystick_source()
        local function native_frame(source)
            if vr.get_controller_type(source) ~= "frame" then return false end
            local bumper = vr.get_action_state(vr.get_action_handle("/actions/default/in/Bumper"), source)
            return bumper.active -- preserve legacy/emulated/custom-unbound fallback
        end
        left_frame, right_frame = native_frame(left), native_frame(right)
    end
    local function grip(source, frame, shoulder)
        if not frame then return down(shoulder) end
        local state = vr.get_action_state(vr.get_action_handle("/actions/default/in/Grip"), source)
        return state.active and state.pressed
    end
    local function frame_faces(source, frame)
        if not frame then return false end
        -- Availability follows the actual source, including swapped/mixed hands.
        local b = vr.get_action_state(vr.get_action_handle("/actions/default/in/AButtonLeft"), source)
        local x = vr.get_action_state(vr.get_action_handle("/actions/default/in/BButtonRight"), source)
        return b.active or x.active
    end
    local native_faces = frame_faces(left, left_frame) or frame_faces(right, right_frame)
    return {
        a = down(A), b = down(native_faces and B or X),
        x = down(native_faces and X or B), y = down(Y),
        left_stick = down(LS), right_stick = down(RS), left_grip = grip(left, left_frame, LB),
        right_grip = grip(right, right_frame, RB),
        start = down(START), back = down(BACK)
    }
end

function M.changed(current, previous)
    for key, value in pairs(current) do
        if previous[key] ~= value then return true end
    end
    return false
end

return M
