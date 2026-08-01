-- assets/scripts/falling_rock.lua
--
-- The Island Hopper hazard: a rock held in the air until the player walks
-- underneath, then dropped with a warning alarm. After the drop it rests
-- wherever it lands (or falls out of the world); it only re-arms back onto
-- its perch once the player has moved well away, so the reset never
-- teleports in view and the trap cannot re-trigger in a loop.
local M = {}

local TRIGGER_RADIUS = 1.6
local RESET_BELOW_Y = -8.0
local REARM_DISTANCE = 7.0

local g_perch = nil
local g_state = "armed"
local g_alarm = nil

-- Remembers the perch position and loads the warning sound.
function M.on_begin_play(self)
    local x, y, z = engine.get_position(self)
    if x ~= nil then
        g_perch = { x = x, y = y, z = z }
    end
    g_alarm = engine.load_sound("assets/sounds/alarm.wav")
end

-- Preserves the hazard state across a hot reload.
function M.on_save_state(_self)
    return { state = g_state, perch = g_perch }
end

-- Restores the hazard state after a hot reload.
function M.on_reload(_self, state)
    if type(state) == "table" then
        g_state = state.state or "armed"
        g_perch = state.perch
    end
end

-- Squared horizontal distance between the player and a point; nil when no
-- player exists.
local function player_distance_sq(x, z)
    local player = engine.find_entity_by_name("Player")
    if player == nil then
        return nil, nil
    end
    local px, py, pz = engine.get_position(player)
    if px == nil then
        return nil, nil
    end
    local dx, dz = px - x, pz - z
    return dx * dx + dz * dz, py
end

-- Armed: drop when the player passes below. Dropped: wait for the rock to
-- settle or vanish, then re-arm only once the player is far away.
function M.on_tick(self, _dt)
    if g_perch == nil or not engine.is_alive(self) then
        return
    end

    local x, y, z = engine.get_position(self)
    if x == nil then
        return
    end

    if g_state == "armed" then
        local dist_sq, py = player_distance_sq(x, z)
        if dist_sq ~= nil and py ~= nil and py < y
            and dist_sq <= TRIGGER_RADIUS * TRIGGER_RADIUS then
            g_state = "dropped"
            if g_alarm ~= nil and g_alarm ~= 0 then
                engine.play_sound_at(g_alarm, x, y, z, 0.8)
            end
            engine.set_acceleration(self, 0.0, -9.8, 0.0)
        end
        return
    end

    local dist_sq = player_distance_sq(g_perch.x, g_perch.z)
    local out_of_world = y < RESET_BELOW_Y
    local player_far = dist_sq ~= nil
        and dist_sq > REARM_DISTANCE * REARM_DISTANCE
    if out_of_world and (player_far or dist_sq == nil) then
        g_state = "armed"
        engine.set_acceleration(self, 0.0, 0.0, 0.0)
        engine.set_velocity(self, 0.0, 0.0, 0.0)
        engine.set_position(self, g_perch.x, g_perch.y, g_perch.z)
    end
end

return M
