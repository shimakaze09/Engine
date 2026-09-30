-- assets/scripts/moving_platform.lua
--
-- Oscillates a platform along X between its authored position and
-- AMPLITUDE units further. Author its Rigid Body with Body Type Kinematic:
-- a kinematic body follows its velocity exactly, feels no gravity, never
-- sleeps and is never pushed back, and it is driven by velocity (not
-- teleported) so contact friction carries a rider and the player
-- controller can read its velocity to ride it.
-- Reference pattern: the engine caches ONE module table per script path and
-- calls it for every entity using the script, so per-entity state must live
-- in a table keyed by the entity handle (handles encode index, generation,
-- and world epoch, so a reused entity slot never aliases an old key) —
-- module-local state would be silently shared between instances.
local M = {}

-- Sweep sized so the far end stops just short of the goal islet's wall
-- (platform face 11.9 vs islet face 12.0): a kinematic body passes through
-- static geometry rather than stopping at it.
local AMPLITUDE = 2.0
local SPEED = 0.25
local TWO_PI = 2.0 * math.pi

local g_instances = {}

-- Remembers this platform's authored position as its oscillation origin.
function M.on_begin_play(self)
    local s = { base = nil, phase = 0.0 }
    local x, y, z = engine.get_position(self)
    if x ~= nil then
        s.base = { x = x, y = y, z = z }
    end
    g_instances[self] = s
end

-- Preserves this platform's oscillation state across a hot reload.
function M.on_save_state(self)
    return g_instances[self]
end

-- Restores this platform's oscillation state after a hot reload.
function M.on_reload(self, state)
    if type(state) == "table" then
        g_instances[self] = { base = state.base, phase = state.phase or 0.0 }
    end
end

-- Releases this platform's state so a reused entity slot starts fresh.
-- Fires on editor Stop, engine.new_scene, and engine.load_scene alike
-- (#198): the VM persists across a scene transition (Lua globals are the
-- cross-scene handoff channel) but `self` does not, so per-entity cleanup
-- belongs here rather than after the transition commits.
function M.on_end_play(self)
    g_instances[self] = nil
end

-- Sweep position for a phase value: 0..1..0 across one cycle.
local function sweep_x(base, phase)
    return base.x + AMPLITUDE * (0.5 - 0.5 * math.cos(phase * TWO_PI))
end

-- Drives the body's velocity toward the next sweep sample so contacts see
-- real platform motion.
function M.on_tick(self, dt)
    local s = g_instances[self]
    if s == nil or s.base == nil or dt <= 0.0 then
        return
    end
    s.phase = s.phase + dt * SPEED
    local x, y, z = engine.get_position(self)
    if x == nil then
        return
    end
    local target_x = sweep_x(s.base, s.phase)
    engine.set_velocity(self, (target_x - x) / dt, (s.base.y - y) / dt,
        (s.base.z - z) / dt)
end

return M
