-- assets/scripts/island_hopper.lua
--
-- Island Hopper template controller. Preloads the bundled prop meshes the
-- scene references, runs the collect-a-thon loop (spinning coin pickups,
-- the bonus gem, the goal check), plays the pickup/win/splash sounds, and
-- saves the best completion time through engine.save_data.
local M = {}

local COIN_COUNT = 8
local PICKUP_RADIUS = 0.9
local GOAL_RADIUS = 1.6

local PRELOAD_MESHES = {
    "assets/props/tree_trunk.mesh", "assets/props/tree_canopy.mesh",
    "assets/props/rock_large.mesh", "assets/props/rock_small.mesh",
    "assets/props/crate.mesh", "assets/props/barrel.mesh",
    "assets/props/bush.mesh", "assets/props/mushroom.mesh",
    "assets/props/signpost.mesh", "assets/props/plank.mesh",
    "assets/props/coin.mesh", "assets/props/gem.mesh",
    "assets/props/platform_square.mesh", "assets/props/platform_round.mesh",
    "assets/props/flag_pole.mesh", "assets/props/flag_banner.mesh",
}

local g_time = 0.0
local g_coins = 0
local g_gem_taken = false
local g_won = false
local g_splashed = false
local g_spin = 0.0
local g_pickup_sound = nil
local g_win_sound = nil
local g_splash_sound = nil

-- Preserves progress across a script hot reload.
function M.on_save_state(_self)
    return { time = g_time, coins = g_coins, gem = g_gem_taken, won = g_won }
end

-- Restores progress after a script hot reload.
function M.on_reload(_self, state)
    if type(state) == "table" then
        g_time = state.time or 0.0
        g_coins = state.coins or 0
        g_gem_taken = state.gem == true
        g_won = state.won == true
    end
end

-- Requests the bundled meshes, loads the effect sounds, and starts the
-- ambient loop and the run timer.
function M.on_begin_play(_self)
    for i = 1, #PRELOAD_MESHES do
        engine.load_asset_async(PRELOAD_MESHES[i], 2)
    end

    g_pickup_sound = engine.load_sound("assets/sounds/pickup.wav")
    g_win_sound = engine.load_sound("assets/sounds/win.wav")
    g_splash_sound = engine.load_sound("assets/sounds/splash.wav")

    engine.set_bus_volume("music", 0.30)
    engine.play_music("assets/sounds/waves.wav", 1.0, true)

    local saved = engine.load_data()
    if type(saved) == "table" and saved.best_time ~= nil then
        engine.log(string.format("Island Hopper - best time %.1fs. Collect %d coins, then reach the flag!",
            saved.best_time, COIN_COUNT))
    else
        engine.log(string.format("Island Hopper - collect %d coins, then reach the flag!",
            COIN_COUNT))
    end
end

-- Plays a positional one-shot when the sound loaded.
local function play_at(sound, x, y, z, gain)
    if sound ~= nil and sound ~= 0 then
        engine.play_sound_at(sound, x, y, z, gain)
    end
end

-- Collects one named pickup when the player is close enough; returns true
-- when the entity was taken.
local function try_pickup(name, px, py, pz, sound)
    local e = engine.find_entity_by_name(name)
    if e == nil then
        return false
    end
    local x, y, z = engine.get_position(e)
    if x == nil then
        return false
    end
    local dx, dy, dz = x - px, y - py, z - pz
    if dx * dx + dy * dy + dz * dz <= PICKUP_RADIUS * PICKUP_RADIUS then
        play_at(sound, x, y, z, 0.9)
        engine.destroy_entity(e)
        return true
    end
    return false
end

-- Spins and bobs one named pickup for readability at a distance.
local function animate_pickup(name, angle)
    local e = engine.find_entity_by_name(name)
    if e ~= nil then
        engine.set_rotation(e, 0.0, math.sin(angle * 0.5), 0.0,
            math.cos(angle * 0.5))
    end
end

-- Announces the win once: jingle, best-time bookkeeping, saved slot.
local function finish_run()
    g_won = true
    local px, py, pz = engine.get_position(engine.find_entity_by_name("Player"))
    play_at(g_win_sound, px or 0.0, py or 0.0, pz or 0.0, 1.0)

    local total = g_time
    local saved = engine.load_data()
    local best = nil
    if type(saved) == "table" then
        best = saved.best_time
    end
    if best == nil or total < best then
        engine.save_data({ best_time = total })
        engine.log(string.format("YOU WIN! %.1fs - NEW BEST TIME!", total))
    else
        engine.log(string.format("YOU WIN! %.1fs (best %.1fs)", total, best))
    end
    if g_gem_taken then
        engine.log("...and you found the bonus gem!")
    end
end

-- Drives pickups, the splash-out call, and the goal check every step.
function M.on_tick(_self, dt)
    if g_won then
        return
    end
    g_time = g_time + dt
    g_spin = g_spin + dt * 2.5

    local player = engine.find_entity_by_name("Player")
    if player == nil then
        return
    end
    local px, py, pz = engine.get_position(player)
    if px == nil then
        return
    end

    for i = 1, COIN_COUNT do
        local name = "Coin" .. i
        animate_pickup(name, g_spin)
        if try_pickup(name, px, py, pz, g_pickup_sound) then
            g_coins = g_coins + 1
            engine.log(string.format("Coin %d/%d", g_coins, COIN_COUNT))
        end
    end
    animate_pickup("Gem", g_spin * 1.4)
    if not g_gem_taken
        and try_pickup("Gem", px, py, pz, g_pickup_sound) then
        g_gem_taken = true
        engine.log("Bonus gem collected!")
    end

    if py < -1.0 and not g_splashed then
        g_splashed = true
        play_at(g_splash_sound, px, py, pz, 0.9)
    elseif py > 0.0 then
        g_splashed = false
    end

    if g_coins >= COIN_COUNT then
        local goal = engine.find_entity_by_name("Goal")
        if goal ~= nil then
            local gx, gy, gz = engine.get_position(goal)
            if gx ~= nil then
                local dx, dy, dz = gx - px, gy - py, gz - pz
                if dx * dx + dy * dy + dz * dz <= GOAL_RADIUS * GOAL_RADIUS then
                    finish_run()
                end
            end
        end
    end
end

return M
