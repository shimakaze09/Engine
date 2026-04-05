local spawned = nil
local default_mesh_asset_id = nil

function on_start()
    engine.log("script started")
    default_mesh_asset_id = engine.get_default_mesh_asset_id()

    spawned = engine.spawn_entity()
    if spawned == nil then
        engine.log("spawn failed")
        return
    end

    engine.set_position(spawned, 0.0, 5.0, 0.0)
    engine.add_rigid_body(spawned, 1.0)
    if default_mesh_asset_id ~= nil then
        engine.set_mesh(spawned, default_mesh_asset_id)
    else
        engine.log("default mesh asset id unavailable")
    end
    engine.set_albedo(spawned, 0.2, 0.8, 0.4)
    engine.set_velocity(spawned, 0.0, 0.0, 0.0)
    engine.set_acceleration(spawned, 0.0, -9.8, 0.0)
    engine.add_collider(spawned, 0.5, 0.5, 0.5)
end

function on_update()
    if spawned == nil then
        return
    end

    if not engine.is_alive(spawned) then
        return
    end

    local x, y, z = engine.get_position(spawned)
    if y == nil then
        return
    end

    if y < -20.0 then
        engine.destroy_entity(spawned)
    end
end
