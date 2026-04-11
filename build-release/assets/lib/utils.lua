-- assets/lib/utils.lua
--
-- UTILITY SCRIPT — not attached to any entity.
-- Load this from any entity script or global script with:
--   local utils = engine.require("assets/lib/utils.lua")
--
-- This file is a plain Lua module: it returns a table of helpers.
-- It has NO on_start / on_update hooks.
local M = {}

-- Clamp a value between lo and hi.
function M.clamp(v, lo, hi)
    if v < lo then
        return lo
    end
    if v > hi then
        return hi
    end
    return v
end

-- Linear interpolation.
function M.lerp(a, b, t)
    return a + (b - a) * t
end

-- Sign of a number: returns -1, 0, or 1.
function M.sign(v)
    if v > 0 then
        return 1
    end
    if v < 0 then
        return -1
    end
    return 0
end

return M
