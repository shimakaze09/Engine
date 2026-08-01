-- assets/scripts/moving_platform.lua
--
-- Oscillates a heavy kinematic-style platform along X between its authored
-- position and AMPLITUDE units further. The platform is a near-infinite-mass
-- rigid body driven by velocity (not teleported), so contact friction can
-- carry a rider and the player controller can read its velocity to ride it.
local M = {}

-- Sweep sized so the far end stops just short of the goal islet's wall
-- (platform face 11.9 vs islet face 12.0) — the platform must never try
-- to push through static geometry or it stalls against it.
local AMPLITUDE = 2.0
local SPEED = 0.25
local TWO_PI = 2.0 * math.pi

local g_base = nil
local g_phase = 0.0

-- Remembers the authored position as the oscillation origin.
function M.on_begin_play(self)
    local x, y, z = engine.get_position(self)
    if x ~= nil then
        g_base = { x = x, y = y, z = z }
    end
end

-- Preserves the oscillation phase across a hot reload.
function M.on_save_state(_self)
    return { phase = g_phase, base = g_base }
end

-- Restores the oscillation phase after a hot reload.
function M.on_reload(_self, state)
    if type(state) == "table" then
        g_phase = state.phase or 0.0
        g_base = state.base
    end
end

-- Sweep position for a phase value: 0..1..0 across one cycle.
local function sweep_x(phase)
    return g_base.x + AMPLITUDE * (0.5 - 0.5 * math.cos(phase * TWO_PI))
end

-- Clamps a corrective velocity so a stalled platform catches up smoothly
-- instead of catapulting its rider.
local MAX_CORRECTIVE = 3.5
local function clamped(v)
    if v > MAX_CORRECTIVE then
        return MAX_CORRECTIVE
    elseif v < -MAX_CORRECTIVE then
        return -MAX_CORRECTIVE
    end
    return v
end

-- Drives the body's velocity toward the next sweep sample so contacts see
-- real platform motion; the platform is kept awake because the solver
-- would otherwise sleep it at the slow ends of the sweep and the script's
-- velocity writes would stall until a contact wakes it.
function M.on_tick(self, dt)
    if g_base == nil or dt <= 0.0 then
        return
    end
    g_phase = g_phase + dt * SPEED
    local x, y, z = engine.get_position(self)
    if x == nil then
        return
    end
    engine.wake_body(self)
    local target_x = sweep_x(g_phase)
    engine.set_velocity(self,
        clamped((target_x - x) / dt),
        clamped((g_base.y - y) / dt),
        clamped((g_base.z - z) / dt))
end

return M
