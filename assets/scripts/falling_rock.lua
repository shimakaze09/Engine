-- assets/scripts/falling_rock.lua
--
-- The Island Hopper hazard: a rock held in the air until the player walks
-- underneath, then dropped with a warning alarm. After the drop it rests
-- wherever it lands (or falls out of the world); it only re-arms back onto
-- its perch once the player has moved well away, so the reset never
-- teleports in view and the trap cannot re-trigger in a loop.
-- Reference pattern: one module table serves every entity on this script,
-- so per-entity state is keyed by the entity handle (generation-checked, so
-- reused slots never alias) while the shared alarm sound stays module-local
-- and is re-acquired after a hot reload.
local M = {}

local TRIGGER_RADIUS = 1.6
local RESET_BELOW_Y = -8.0
local REARM_DISTANCE = 7.0

local g_instances = {}
local g_alarm = nil

-- Loads the warning sound; safe to call again after a hot reload.
local function acquire_sounds()
    g_alarm = engine.load_sound("assets/sounds/alarm.wav")
end

-- Remembers this rock's perch position and loads the warning sound.
function M.on_begin_play(self)
    local s = { perch = nil, state = "armed" }
    local x, y, z = engine.get_position(self)
    if x ~= nil then
        s.perch = { x = x, y = y, z = z }
    end
    g_instances[self] = s
    acquire_sounds()
end

-- Preserves this rock's hazard state across a hot reload.
function M.on_save_state(self)
    return g_instances[self]
end

-- Restores this rock's hazard state after a hot reload.
function M.on_reload(self, state)
    if type(state) == "table" then
        g_instances[self] = {
            perch = state.perch,
            state = state.state or "armed",
        }
    end
    acquire_sounds()
end

-- Releases this rock's state so a reused entity slot starts fresh.
function M.on_end_play(self)
    g_instances[self] = nil
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
    local s = g_instances[self]
    if s == nil or s.perch == nil or not engine.is_alive(self) then
        return
    end

    local x, y, z = engine.get_position(self)
    if x == nil then
        return
    end

    if s.state == "armed" then
        local dist_sq, py = player_distance_sq(x, z)
        if dist_sq ~= nil and py ~= nil and py < y
            and dist_sq <= TRIGGER_RADIUS * TRIGGER_RADIUS then
            s.state = "dropped"
            if g_alarm ~= nil and g_alarm ~= 0 then
                engine.play_sound_at(g_alarm, x, y, z, 0.8)
            end
            engine.set_acceleration(self, 0.0, -9.8, 0.0)
        end
        return
    end

    local dist_sq = player_distance_sq(s.perch.x, s.perch.z)
    local out_of_world = y < RESET_BELOW_Y
    local player_far = dist_sq ~= nil
        and dist_sq > REARM_DISTANCE * REARM_DISTANCE
    if out_of_world and (player_far or dist_sq == nil) then
        s.state = "armed"
        engine.set_acceleration(self, 0.0, 0.0, 0.0)
        engine.set_velocity(self, 0.0, 0.0, 0.0)
        engine.set_position(self, s.perch.x, s.perch.y, s.perch.z)
    end
end

return M
