-- assets/main.lua  (Scene Controller entity script)
--
-- The project's scene-level module, attached to the "Scene Controller"
-- entity of the startup scene. It starts empty, like a new Unity project:
-- add gameplay here, or attach scripts to the entities you create.
-- 'self' is an opaque, generation-checked entity handle.
local M = {}

-- Called once when Play is pressed (or when this entity is created
-- during play). Spawn entities and attach their scripts here.
function M.on_begin_play(_self)
end

-- Called once per fixed step with the fixed delta; input read here is
-- seen once per step, so gameplay that reacts to input belongs here.
function M.on_fixed_tick(_self, _dt)
end

-- Called once per rendered frame that advanced simulation; dt sums every
-- fixed step the frame took.
function M.on_tick(_self, _dt)
end

return M
