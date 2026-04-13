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

local MOVE_SPEED = 5.0
local JUMP_VY = 7.0

function M.on_start(self)
    engine.log("Player on_start, entity=" .. self)
    engine.set_restitution(self, 0.05)
    engine.set_friction(self, 0.9, 0.7)
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
        engine.set_position(self, 0.0, 3.0, 0.0)
        engine.set_velocity(self, 0.0, 0.0, 0.0)
        engine.log("Player respawned")
        return
    end

    -- Read current velocity (preserve vertical component from physics).
    local vx, vy, vz = engine.get_velocity(self)
    if vx == nil then
        return
    end

    -- Arrow keys: directly set horizontal velocity for responsive control.
    local tx = 0.0
    local tz = 0.0
    if engine.is_key_down(engine.KEY_LEFT) then
        tx = -MOVE_SPEED
    elseif engine.is_key_down(engine.KEY_RIGHT) then
        tx = MOVE_SPEED
    end
    if engine.is_key_down(engine.KEY_UP) then
        tz = -MOVE_SPEED
    elseif engine.is_key_down(engine.KEY_DOWN) then
        tz = MOVE_SPEED
    end

    -- Space bar: jump (only when nearly stationary vertically)
    if engine.is_key_pressed(engine.KEY_SPACE) then
        if math.abs(vy) < 0.5 then
            vy = JUMP_VY
        end
    end

    engine.set_velocity(self, tx, vy, tz)
end

return M
