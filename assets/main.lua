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
local default_mesh_asset_id = nil

-- Helper: spawn a coloured physics box
local function spawn_box(x, y, z, r, g, b)
    local e = engine.spawn_entity()
    if e == nil then
        return nil
    end
    engine.set_position(e, x, y, z)
    engine.set_scale(e, 1.0, 1.0, 1.0)
    engine.add_rigid_body(e, 1.0)
    if default_mesh_asset_id ~= nil then
        engine.set_mesh(e, default_mesh_asset_id)
    end
    engine.set_albedo(e, r, g, b)
    engine.add_collider(e, 0.5, 0.5, 0.5)
    engine.set_acceleration(e, 0.0, -9.8, 0.0)
    return e
end

function M.on_start(self)
    engine.log("=== Scene Controller on_start ===")
    default_mesh_asset_id = engine.get_default_mesh_asset_id()

    -- Spawn the green player cube and attach a per-entity script to it.
    -- The script (assets/scripts/player.lua) handles keyboard movement.
    local player = spawn_box(0.0, 6.0, 0.0, 0.2, 0.8, 0.4)
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

    -- Timer: spawn two plain clones after 2 seconds
    engine.set_timeout(function()
        engine.log("timer: spawning clones")
        local c1 = spawn_box(-3.0, 9.0, 0.0, 0.9, 0.2, 0.2)
        if c1 ~= nil then
            engine.set_name(c1, "CloneRed")
        end
        local c2 = spawn_box(3.0, 9.0, 0.0, 0.2, 0.2, 0.9)
        if c2 ~= nil then
            engine.set_name(c2, "CloneBlue")
        end
    end, 2.0)

    -- Coroutine: drop waves of boxes from above
    engine.start_coroutine(function()
        engine.wait(4.0)
        engine.log("coroutine wave 1")
        for i = 0, 2 do
            spawn_box(-4.0 + i * 4.0, 12.0, -2.0, 0.9, 0.6, 0.1)
        end
        engine.wait(3.0)
        engine.log("coroutine wave 2")
        for i = 0, 3 do
            spawn_box(-6.0 + i * 4.0, 15.0, 2.0, 0.6, 0.1, 0.9)
        end
    end)

    engine.log("on_start done — " .. engine.get_entity_count() .. " entities")
end

function M.on_update(self, dt)
    -- Per-frame coordinator logic goes here.
    -- Each entity's own behaviour lives in its attached script.
end

return M
