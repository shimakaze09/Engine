-- assets/scripts/island_player.lua
--
-- Island Hopper player: WASD/arrow movement with a grounded space-bar jump,
-- animation state driven through speed/jump parameters (footstep events play
-- positional audio), jump/land sounds, water respawn, and a smoothed
-- third-person follow camera pushed through the camera manager.
local M = {}

local MOVE_SPEED = 4.5
local JUMP_VY = 6.5
local GROUND_CHECK_DISTANCE = 0.35
local SPAWN = { x = 0.0, y = 0.15, z = 5.0 }
local CAMERA_BACK = 6.5
local CAMERA_UP = 3.5

local g_footstep = nil
local g_jump_sound = nil
local g_land_sound = nil
local g_airborne = false
local g_last_ground = nil

-- Returns the supporting entity under the capsule base, or nil when
-- airborne (the handle lets the controller ride moving ground).
local function ground_entity(self, x, y, z)
    local hits = engine.raycast_all(
        x, y + 0.25, z, 0.0, -1.0, 0.0, GROUND_CHECK_DISTANCE + 0.25)
    for i = 1, #hits do
        local hit = hits[i]
        if hit.entity ~= self and hit.ny > 0.5 then
            return hit.entity
        end
    end
    return nil
end

-- Locks rotation, hooks footstep events to positional audio, and frames
-- the follow camera.
function M.on_begin_play(self)
    engine.set_restitution(self, 0.02)
    engine.set_friction(self, 0.9, 0.7)
    engine.set_lock_rotation(self, true)

    g_footstep = engine.load_sound("assets/sounds/footstep.wav")
    g_jump_sound = engine.load_sound("assets/sounds/jump.wav")
    g_land_sound = engine.load_sound("assets/sounds/land.wav")
    engine.on_anim_event_handler(function(entity, name)
        if name == "footstep" and entity ~= nil
            and g_footstep ~= nil and g_footstep ~= 0 then
            local x, y, z = engine.get_position(entity)
            if x ~= nil then
                engine.play_sound_at(g_footstep, x, y, z, 0.7)
            end
        end
    end)

    local x, y, z = engine.get_position(self)
    if x ~= nil then
        engine.push_camera(self, x, y + CAMERA_UP, z + CAMERA_BACK,
            x, y + 1.0, z, 10.0, 4.0)
    end
end

-- Movement, jump, animation parameters, and the follow camera per step.
function M.on_tick(self, dt)
    if not engine.is_alive(self) then
        return
    end

    -- The avatar must never sleep: the solver would otherwise idle the
    -- body after a few still seconds and velocity writes stop applying
    -- until a contact wakes it — input would go dead.
    engine.wake_body(self)

    local x, y, z = engine.get_position(self)
    if x == nil then
        return
    end
    if y < -6.0 then
        engine.set_position(self, SPAWN.x, SPAWN.y, SPAWN.z)
        engine.set_velocity(self, 0.0, 0.0, 0.0)
        return
    end

    local vx, vy, vz = engine.get_velocity(self)
    if vx == nil then
        return
    end

    local move_x = 0.0
    local move_z = 0.0
    if engine.is_key_down(engine.KEY_LEFT) or engine.is_key_down(engine.KEY_A) then
        move_x = move_x - 1.0
    end
    if engine.is_key_down(engine.KEY_RIGHT) or engine.is_key_down(engine.KEY_D) then
        move_x = move_x + 1.0
    end
    if engine.is_key_down(engine.KEY_UP) or engine.is_key_down(engine.KEY_W) then
        move_z = move_z - 1.0
    end
    if engine.is_key_down(engine.KEY_DOWN) or engine.is_key_down(engine.KEY_S) then
        move_z = move_z + 1.0
    end

    local length_squared = move_x * move_x + move_z * move_z
    if length_squared > 1.0 then
        local inverse_length = 1.0 / math.sqrt(length_squared)
        move_x = move_x * inverse_length
        move_z = move_z * inverse_length
    end
    local tx = move_x * MOVE_SPEED
    local tz = move_z * MOVE_SPEED

    local ground = ground_entity(self, x, y, z)
    local grounded = ground ~= nil
    if engine.is_key_pressed(engine.KEY_SPACE) and grounded then
        vy = JUMP_VY
        engine.set_anim_param(self, "jump", 1.0)
        if g_jump_sound ~= nil and g_jump_sound ~= 0 then
            engine.play_sound_at(g_jump_sound, x, y, z, 0.6)
        end
        g_airborne = true
    elseif grounded then
        if g_airborne then
            g_airborne = false
            if g_land_sound ~= nil and g_land_sound ~= 0 then
                engine.play_sound_at(g_land_sound, x, y, z, 0.5)
            end
        end
        engine.set_anim_param(self, "jump", 0.0)
    end

    -- Riding: the controller hard-sets velocity, so moving ground must be
    -- carried explicitly. The carry uses the ground's ACTUAL displacement
    -- (not its commanded velocity, which can overshoot when the ground's
    -- own corrective is clamped) so the rider tracks it without drifting
    -- toward an edge.
    if grounded then
        local gx, _, gz = engine.get_position(ground)
        if gx ~= nil then
            if g_last_ground ~= nil and g_last_ground.id == ground
                and dt > 0.0 then
                tx = tx + (gx - g_last_ground.x) / dt
                tz = tz + (gz - g_last_ground.z) / dt
            end
            g_last_ground = { id = ground, x = gx, z = gz }
        else
            g_last_ground = nil
        end
    else
        g_last_ground = nil
    end

    engine.set_velocity(self, tx, vy, tz)

    -- The walk animation tracks the player's own input, not inherited
    -- platform velocity, so riders idle while standing still.
    local input_speed = math.sqrt(move_x * move_x + move_z * move_z)
    engine.set_anim_param(self, "speed", input_speed)

    engine.push_camera(self, x, y + CAMERA_UP, z + CAMERA_BACK,
        x, y + 1.0, z, 10.0, 4.0)
end

return M
