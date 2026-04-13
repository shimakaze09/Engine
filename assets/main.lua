-- assets/main.lua  (Scene Controller entity script)
--
-- Attached to the "Scene Controller" entity by the engine at startup.
-- The engine calls M.on_start(self) once when Play is pressed and
-- M.on_update(self, dt) every simulation step.
-- 'self' is the integer entity index of the Scene Controller.
--
-- This script sets up the scene: it spawns gameplay entities and attaches
-- their own scripts to them.  Per-entity BEHAVIOUR lives in separate scripts:
--   assets/scripts/player.lua  <- handles player movement
local M = {}

local g_scene_initialized = false

-- Helper: spawn a coloured shape with gravity
local function spawn_dynamic(shape, x, y, z, r, g, b)
    local e = engine.spawn_shape(shape, x, y, z, r, g, b)
    if e == nil then
        return nil
    end
    engine.set_acceleration(e, 0.0, -9.8, 0.0)
    return e
end

-- Helper: spawn a static shape (no gravity, no movement)
local function spawn_static(shape, x, y, z, r, g, b)
    local e = engine.spawn_shape(shape, x, y, z, r, g, b)
    if e == nil then
        return nil
    end
    engine.set_inverse_mass(e, 0.0)
    return e
end

function M.on_start(self)
    if g_scene_initialized then
        return
    end
    g_scene_initialized = true

    engine.log("=== Scene Controller on_start ===")

    -- Player: green cube the user controls with arrow keys + space.
    local player = spawn_dynamic("cube", 0.0, 3.0, 0.0, 0.2, 0.8, 0.4)
    if player ~= nil then
        engine.set_name(player, "Player")
        engine.set_friction(player, 0.9, 0.7)
        engine.set_restitution(player, 0.05)
        engine.add_script_component(player, "assets/scripts/player.lua")
    end

    -- A few static scenery shapes showing different collider types.
    local sphere = spawn_static("sphere", -2.0, 0.5, 2.0, 0.9, 0.5, 0.2)
    if sphere ~= nil then
        engine.set_name(sphere, "Sphere Prop")
    end

    local cylinder = spawn_static("cylinder", 2.0, 0.5, 2.0, 0.2, 0.6, 0.9)
    if cylinder ~= nil then
        engine.set_name(cylinder, "Cylinder Prop")
    end

    local pyramid = spawn_static("pyramid", 0.0, 0.5, 3.5, 0.8, 0.3, 0.6)
    if pyramid ~= nil then
        engine.set_name(pyramid, "Pyramid Prop")
    end

    -- One dynamic sphere to demonstrate physics interaction with the player.
    local ball = spawn_dynamic("sphere", 3.0, 2.0, 0.0, 0.95, 0.85, 0.2)
    if ball ~= nil then
        engine.set_name(ball, "Ball")
        engine.set_friction(ball, 0.6, 0.4)
        engine.set_restitution(ball, 0.5)
    end

    engine.log("on_start done - " .. engine.get_entity_count() .. " entities")
end

function M.on_update(self, dt)
    -- Per-frame coordinator logic goes here.
end

return M
