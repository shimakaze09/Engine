-- assets/main.lua  (Scene Controller entity script)
--
-- Attached to the "Scene Controller" entity by the engine at startup.
-- The engine calls M.on_begin_play(self) once when Play is pressed and
-- M.on_tick(self, dt) every simulation step.
-- 'self' is an opaque, generation-checked entity handle.
--
-- This script sets up the scene: it spawns gameplay entities and attaches
-- their own scripts to them.  Per-entity BEHAVIOUR lives in separate scripts:
--   assets/scripts/player.lua  <- handles player movement
local M = {}

local g_scene_initialized = false

-- Detects scenes created by an older script version without saved state.
local function demo_scene_exists()
    return engine.find_entity_by_name("Player") ~= nil
end

-- Helper: spawn a coloured shape with gravity
local function spawn_dynamic(shape, x, y, z, r, g, b)
    local e = engine.spawn_shape(shape, x, y, z, r, g, b)
    if e == nil then
        return nil
    end
    if not engine.set_acceleration(e, 0.0, -9.8, 0.0) then
        engine.destroy_entity(e)
        return nil
    end
    return e
end

-- Helper: spawn a static shape (no gravity, no movement)
local function spawn_static(shape, x, y, z, r, g, b)
    local e = engine.spawn_shape(shape, x, y, z, r, g, b)
    if e == nil then
        return nil
    end
    if not engine.set_inverse_mass(e, 0.0) then
        engine.destroy_entity(e)
        return nil
    end
    return e
end

-- Saves the idempotence guard across a module hot reload.
function M.on_save_state(_self)
    return { scene_initialized = g_scene_initialized }
end

-- Restores saved state, with a world-state fallback for older script versions.
function M.on_reload(_self, state)
    g_scene_initialized =
        (type(state) == "table" and state.scene_initialized == true)
        or demo_scene_exists()
end

-- Destroys every entity from an incomplete scene setup.
local function rollback_setup(created)
    for i = #created, 1, -1 do
        engine.destroy_entity(created[i])
    end
    engine.log("scene setup failed; rolled back")
end

-- Creates the demo scene once when play begins.
function M.on_begin_play(_self)
    if g_scene_initialized or demo_scene_exists() then
        g_scene_initialized = true
        return
    end

    engine.log("=== Scene Controller on_begin_play ===")
    local created = {}

    -- Player: green cube the user controls with arrow keys + space.
    local player = spawn_dynamic("cube", 0.0, 3.0, 0.0, 0.2, 0.8, 0.4)
    if player == nil then
        rollback_setup(created)
        return
    end
    created[#created + 1] = player
    if not engine.set_friction(player, 0.9, 0.7)
        or not engine.set_restitution(player, 0.05)
        or not engine.add_script_component(
            player, "assets/scripts/player.lua")
        or not engine.set_name(player, "Player") then
        rollback_setup(created)
        return
    end

    -- A few static scenery shapes showing different collider types.
    local sphere = spawn_static("sphere", -2.0, 0.5, 2.0, 0.9, 0.5, 0.2)
    if sphere == nil then
        rollback_setup(created)
        return
    end
    created[#created + 1] = sphere
    if not engine.set_name(sphere, "Sphere Prop") then
        rollback_setup(created)
        return
    end

    local cylinder = spawn_static("cylinder", 2.0, 0.5, 2.0, 0.2, 0.6, 0.9)
    if cylinder == nil then
        rollback_setup(created)
        return
    end
    created[#created + 1] = cylinder
    if not engine.set_name(cylinder, "Cylinder Prop") then
        rollback_setup(created)
        return
    end

    local pyramid = spawn_static("pyramid", 0.0, 0.5, 3.5, 0.8, 0.3, 0.6)
    if pyramid == nil then
        rollback_setup(created)
        return
    end
    created[#created + 1] = pyramid
    if not engine.set_name(pyramid, "Pyramid Prop") then
        rollback_setup(created)
        return
    end

    -- One dynamic sphere to demonstrate physics interaction with the player.
    local ball = spawn_dynamic("sphere", 3.0, 2.0, 0.0, 0.95, 0.85, 0.2)
    if ball == nil then
        rollback_setup(created)
        return
    end
    created[#created + 1] = ball
    if not engine.set_friction(ball, 0.6, 0.4)
        or not engine.set_restitution(ball, 0.5)
        or not engine.set_name(ball, "Ball") then
        rollback_setup(created)
        return
    end

    g_scene_initialized = true
    engine.log("scene setup done - " .. engine.get_entity_count() .. " entities")
end

-- Runs per-frame scene coordinator logic.
function M.on_tick(_self, _dt)
    -- Per-frame coordinator logic goes here.
end

return M
