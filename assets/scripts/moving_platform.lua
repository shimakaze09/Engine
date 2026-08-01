-- assets/scripts/moving_platform.lua
--
-- Oscillates a static-collider platform along X between its authored
-- position and AMPLITUDE units further, carrying the Island Hopper's
-- final hop toward the goal islet.
local M = {}

local AMPLITUDE = 2.4
local SPEED = 0.7

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

-- Advances the sweep one fixed step.
function M.on_tick(self, dt)
    if g_base == nil then
        return
    end
    g_phase = g_phase + dt * SPEED
    local sweep = 0.5 - 0.5 * math.cos(g_phase * 2.0 * math.pi * 0.5)
    engine.set_position(self, g_base.x + AMPLITUDE * sweep, g_base.y, g_base.z)
end

return M
