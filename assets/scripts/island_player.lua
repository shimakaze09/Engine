-- assets/scripts/island_player.lua
--
-- Island Hopper player: WASD/arrow movement with a grounded space-bar jump,
-- animation state driven through speed/jump parameters (footstep events play
-- positional audio), jump/land sounds, water respawn, and a smoothed
-- third-person follow camera pushed through the camera manager.
-- Reference pattern: one module table serves every entity on this script,
-- so per-entity state (airborne flag, ground sample) is keyed by the entity
-- handle (generation-checked, so reused slots never alias); shared sound
-- handles and the single footstep event handler stay module-local and are
-- re-acquired after a hot reload.
local M = {}

local MOVE_SPEED = 4.5
local JUMP_VY = 6.5
local GROUND_CHECK_DISTANCE = 0.35
local SPAWN = { x = 0.0, y = 0.15, z = 5.0 }
local CAMERA_BACK = 6.5
local CAMERA_UP = 3.5

local g_instances = {}
local g_footstep = nil
local g_jump_sound = nil
local g_land_sound = nil
local g_handler_id = nil
local g_stale_handler_removed = false

-- Loads the effect sounds; safe to call again after a hot reload.
local function acquire_sounds()
    g_footstep = engine.load_sound("assets/sounds/footstep.wav")
    g_jump_sound = engine.load_sound("assets/sounds/jump.wav")
    g_land_sound = engine.load_sound("assets/sounds/land.wav")
end

-- Registers the shared footstep handler once per loaded module chunk; it
-- plays at the event entity's position, so one handler serves all players.
local function ensure_anim_handler()
    if g_handler_id ~= nil then
        return
    end
    g_handler_id = engine.on_anim_event_handler(function(entity, name)
        if name == "footstep" and entity ~= nil
            and g_footstep ~= nil and g_footstep ~= 0 then
            local x, y, z = engine.get_position(entity)
            if x ~= nil then
                engine.play_sound_at(g_footstep, x, y, z, 0.7)
            end
        end
    end)
end

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

    g_instances[self] = { airborne = false, last_ground = nil }
    acquire_sounds()
    ensure_anim_handler()

    local x, y, z = engine.get_position(self)
    if x ~= nil then
        engine.push_camera(self, x, y + CAMERA_UP, z + CAMERA_BACK,
            x, y + 1.0, z, 10.0, 4.0)
    end
end

-- Preserves this player's movement state (and the module's event-handler
-- registration id) across a hot reload.
function M.on_save_state(self)
    local s = g_instances[self]
    if s == nil then
        return nil
    end
    return {
        airborne = s.airborne,
        last_ground = s.last_ground,
        handler_id = g_handler_id,
    }
end

-- Restores this player's movement state after a hot reload; the previous
-- chunk's footstep handler is removed exactly once before the replacement
-- registers so events never double-fire.
function M.on_reload(self, state)
    if type(state) == "table" then
        g_instances[self] = {
            airborne = state.airborne == true,
            last_ground = state.last_ground,
        }
        if not g_stale_handler_removed and state.handler_id ~= nil then
            engine.remove_anim_event_handler(state.handler_id)
            g_stale_handler_removed = true
        end
    else
        g_instances[self] = { airborne = false, last_ground = nil }
    end
    acquire_sounds()
    ensure_anim_handler()
end

-- Releases this player's state so a reused entity slot starts fresh.
function M.on_end_play(self)
    g_instances[self] = nil
end

-- Movement, jump, animation parameters, and the follow camera per step.
function M.on_tick(self, dt)
    if not engine.is_alive(self) then
        return
    end
    local s = g_instances[self]
    if s == nil then
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
        s.airborne = true
    elseif grounded then
        if s.airborne then
            s.airborne = false
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
    -- toward an edge. Scripts run BEFORE the frame's physics steps, so the
    -- displacement observed now was simulated during the PREVIOUS tick's
    -- dt — each sample stores its dt and the next delta divides by it,
    -- keeping the inherited speed correct across catch-up frames.
    if grounded then
        local gx, _, gz = engine.get_position(ground)
        if gx ~= nil then
            if s.last_ground ~= nil and s.last_ground.id == ground
                and s.last_ground.dt > 0.0 then
                tx = tx + (gx - s.last_ground.x) / s.last_ground.dt
                tz = tz + (gz - s.last_ground.z) / s.last_ground.dt
            end
            s.last_ground = { id = ground, x = gx, z = gz, dt = dt }
        else
            s.last_ground = nil
        end
    else
        s.last_ground = nil
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
