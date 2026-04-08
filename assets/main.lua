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

-- Helper: spawn a coloured cube with physics
local function spawn_cube(x, y, z, r, g, b)
    local e = engine.spawn_shape("cube", x, y, z, r, g, b)
    if e == nil then
        return nil
    end
    engine.set_acceleration(e, 0.0, -9.8, 0.0)
    return e
end

-- Helper: spawn a sphere
local function spawn_sphere(x, y, z, r, g, b)
    local e = engine.spawn_shape("sphere", x, y, z, r, g, b)
    if e == nil then
        return nil
    end
    engine.set_acceleration(e, 0.0, -9.8, 0.0)
    return e
end

-- Helper: spawn a cylinder
local function spawn_cylinder(x, y, z, r, g, b)
    local e = engine.spawn_shape("cylinder", x, y, z, r, g, b)
    if e == nil then
        return nil
    end
    engine.set_acceleration(e, 0.0, -9.8, 0.0)
    return e
end

-- Helper: spawn a pyramid
local function spawn_pyramid(x, y, z, r, g, b)
    local e = engine.spawn_shape("pyramid", x, y, z, r, g, b)
    if e == nil then
        return nil
    end
    engine.set_acceleration(e, 0.0, -9.8, 0.0)
    return e
end

function M.on_start(self)
    engine.log("=== Scene Controller on_start ===")

    -- Spawn the green player cube and attach a per-entity script to it.
    local player = spawn_cube(0.0, 6.0, 0.0, 0.2, 0.8, 0.4)
    if player ~= nil then
        engine.set_name(player, "Player")
        engine.add_script_component(player, "assets/scripts/player.lua")
    end

    -- A point light for colour variation (directional sun already exists)
    local pt = engine.spawn_entity()
    if pt ~= nil then
        engine.set_name(pt, "PointLight")
        engine.set_position(pt, 0.0, 6.0, 3.0)
        engine.add_light(pt, "point")
        engine.set_light_color(pt, 0.4, 0.6, 1.0)
        engine.set_light_intensity(pt, 2.0)
    end

    -- Collision handler: log the first few bounces
    local bounces = 0
    engine.on_collision_handler(function(a, b)
        bounces = bounces + 1
        if bounces <= 5 then
            engine.log("bounce #" .. bounces)
        end
    end)

    -- Timer: spawn two clones after 2 seconds
    engine.set_timeout(function()
        engine.log("timer: spawning clones")
        local c1 = spawn_cube(-3.0, 9.0, 0.0, 0.9, 0.2, 0.2)
        if c1 ~= nil then
            engine.set_name(c1, "CloneRed")
        end
        local c2 = spawn_sphere(3.0, 9.0, 0.0, 0.2, 0.2, 0.9)
        if c2 ~= nil then
            engine.set_name(c2, "CloneSphere")
        end
    end, 2.0)

    -- Coroutine: drop waves of shapes from above
    engine.start_coroutine(function()
        engine.wait(4.0)
        engine.log("coroutine wave 1")
        spawn_cube(-4.0, 12.0, -2.0, 0.9, 0.6, 0.1)
        spawn_sphere(0.0, 12.0, -2.0, 0.5, 0.9, 0.3)
        spawn_cube(4.0, 12.0, -2.0, 0.9, 0.6, 0.1)
        engine.wait(3.0)
        engine.log("coroutine wave 2")
        spawn_cylinder(-6.0, 15.0, 2.0, 0.6, 0.1, 0.9)
        spawn_pyramid(0.0, 15.0, 2.0, 1.0, 0.8, 0.0)
        spawn_cylinder(6.0, 15.0, 2.0, 0.6, 0.1, 0.9)
    end)

    engine.log("on_start done — " .. engine.get_entity_count() .. " entities")
end

function M.on_update(self, dt)
    -- Per-frame coordinator logic goes here.
end

return M
