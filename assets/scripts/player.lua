-- assets/scripts/player.lua
--
-- Per-entity script for a player-controlled physics box.
-- Attach this to any entity with a ScriptComponent:
--   engine.add_script_component(entity, "assets/scripts/player.lua")
--
-- This script is a MODULE — it must return a table (M).
-- The engine calls M.on_begin_play(self) once on Play and
-- M.on_tick(self, dt) every simulation step.
-- 'self' is an opaque, generation-checked entity handle.
local M = {}

local MOVE_SPEED = 5.0
local JUMP_VY = 7.0
local GROUND_CHECK_DISTANCE = 0.65

-- Reports whether a downward ray reaches supporting geometry.
local function is_grounded(self, x, y, z)
    local hits = engine.raycast_all(
        x, y, z, 0.0, -1.0, 0.0, GROUND_CHECK_DISTANCE)
    for i = 1, #hits do
        local hit = hits[i]
        if hit.entity ~= self and hit.ny > 0.5 then
            return true
        end
    end
    return false
end

-- Applies the player's initial physics and material settings. Rotation is
-- locked (standard character-controller setup): a velocity-driven box would
-- otherwise trip over its own contact friction and tumble.
function M.on_begin_play(self)
    engine.log("Player on_begin_play, entity=" .. tostring(self))
    engine.set_restitution(self, 0.05)
    engine.set_friction(self, 0.9, 0.7)
    engine.set_lock_rotation(self, true)
    engine.set_roughness(self, 0.3)
    engine.set_metallic(self, 0.0)
end

-- Applies input-driven velocity once per simulation step.
function M.on_tick(self, _dt)
    if not engine.is_alive(self) then
        return
    end

    -- Respawn if the entity falls below y=-20
    local x, y, z = engine.get_position(self)
    if x == nil then
        return
    end
    if y < -20.0 then
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
    local move_x = 0.0
    local move_z = 0.0
    if engine.is_key_down(engine.KEY_LEFT) then
        move_x = move_x - 1.0
    end
    if engine.is_key_down(engine.KEY_RIGHT) then
        move_x = move_x + 1.0
    end
    if engine.is_key_down(engine.KEY_UP) then
        move_z = move_z - 1.0
    end
    if engine.is_key_down(engine.KEY_DOWN) then
        move_z = move_z + 1.0
    end

    local length_squared = move_x * move_x + move_z * move_z
    if length_squared > 1.0 then
        local inverse_length = 1.0 / math.sqrt(length_squared)
        move_x = move_x * inverse_length
        move_z = move_z * inverse_length
    end
    local tx = move_x * MOVE_SPEED
    local tz = move_z * MOVE_SPEED

    -- Space bar: jump only while supported by upward-facing geometry.
    if engine.is_key_pressed(engine.KEY_SPACE)
        and is_grounded(self, x, y, z) then
        vy = JUMP_VY
    end

    engine.set_velocity(self, tx, vy, tz)
end

return M
