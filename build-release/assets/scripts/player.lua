-- assets/scripts/player.lua
--
-- Per-entity script for a player-controlled physics box.
-- Attach this to any entity with a ScriptComponent:
--   engine.add_script_component(entity, "assets/scripts/player.lua")
--
-- This script is a MODULE — it must return a table (M).
-- The engine calls M.on_start(self) once on Play and
-- M.on_update(self, dt) every simulation step.
-- 'self' is the entity's integer ID.
local M = {}

function M.on_start(self)
    engine.log("Player on_start, entity=" .. self)
    engine.set_restitution(self, 0.6)
    engine.set_roughness(self, 0.3)
    engine.set_metallic(self, 0.0)
end

function M.on_update(self, dt)
    if not engine.is_alive(self) then
        return
    end

    -- Respawn if the entity falls below y=-20
    local x, y, z = engine.get_position(self)
    if y ~= nil and y < -20.0 then
        engine.set_position(self, 0.0, 6.0, 0.0)
        engine.set_velocity(self, 0.0, 0.0, 0.0)
        engine.log("Player respawned")
        return
    end

    -- Arrow keys: push the player horizontally
    if engine.is_key_down(engine.KEY_LEFT) then
        engine.set_additional_acceleration(self, -8.0, 0.0, 0.0)
    elseif engine.is_key_down(engine.KEY_RIGHT) then
        engine.set_additional_acceleration(self, 8.0, 0.0, 0.0)
    elseif engine.is_key_down(engine.KEY_UP) then
        engine.set_additional_acceleration(self, 0.0, 0.0, -8.0)
    elseif engine.is_key_down(engine.KEY_DOWN) then
        engine.set_additional_acceleration(self, 0.0, 0.0, 8.0)
    end

    -- Space bar: jump (only when nearly stationary vertically)
    if engine.is_key_pressed(engine.KEY_SPACE) then
        local vx, vy, vz = engine.get_velocity(self)
        if vy ~= nil and math.abs(vy) < 0.5 then
            engine.set_velocity(self, vx, 8.0, vz)
        end
    end
end

return M
