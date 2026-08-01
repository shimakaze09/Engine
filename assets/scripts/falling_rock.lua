-- assets/scripts/falling_rock.lua
--
-- The Island Hopper hazard: a rock held in the air until the player walks
-- underneath, then dropped (with a warning alarm); it resets to its perch
-- after falling out of the world.
local M = {}

local TRIGGER_RADIUS = 1.6
local RESET_BELOW_Y = -8.0

local g_perch = nil
local g_falling = false
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
    return { falling = g_falling, perch = g_perch }
end

-- Restores the hazard state after a hot reload.
function M.on_reload(_self, state)
    if type(state) == "table" then
        g_falling = state.falling == true
        g_perch = state.perch
    end
end

-- Watches for the player passing below; drops, then resets after the fall.
function M.on_tick(self, _dt)
    if g_perch == nil or not engine.is_alive(self) then
        return
    end

    local x, y, z = engine.get_position(self)
    if x == nil then
        return
    end

    if g_falling then
        if y < RESET_BELOW_Y then
            g_falling = false
            engine.set_acceleration(self, 0.0, 0.0, 0.0)
            engine.set_velocity(self, 0.0, 0.0, 0.0)
            engine.set_position(self, g_perch.x, g_perch.y, g_perch.z)
        end
        return
    end

    local player = engine.find_entity_by_name("Player")
    if player == nil then
        return
    end
    local px, py, pz = engine.get_position(player)
    if px == nil or py >= y then
        return
    end
    local dx, dz = px - x, pz - z
    if dx * dx + dz * dz <= TRIGGER_RADIUS * TRIGGER_RADIUS then
        g_falling = true
        if g_alarm ~= nil and g_alarm ~= 0 then
            engine.play_sound_at(g_alarm, x, y, z, 0.8)
        end
        engine.set_acceleration(self, 0.0, -9.8, 0.0)
    end
end

return M
