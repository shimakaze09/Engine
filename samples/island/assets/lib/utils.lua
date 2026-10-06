-- assets/lib/utils.lua
--
-- UTILITY SCRIPT — not attached to any entity.
-- Load this from any entity script or global script with:
--   local utils = engine.require("assets/lib/utils.lua")
--
-- This file is a plain Lua module: it returns a table of helpers.
-- It has NO on_start / on_update hooks.
local M = {}

-- Clamp a value between lo and hi. Unlike math.clamp, which refuses
-- bounds out of order, this helper accepts them either way round.
function M.clamp(v, lo, hi)
    if lo > hi then
        lo, hi = hi, lo
    end
    return math.clamp(v, lo, hi)
end

-- Linear interpolation; the engine's math.lerp.
M.lerp = math.lerp

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
