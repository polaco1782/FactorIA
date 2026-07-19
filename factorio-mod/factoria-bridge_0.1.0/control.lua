local interface_name = "factoria_bridge"

local function ensure_storage()
    storage.path_results = storage.path_results or {}
end

script.on_init(ensure_storage)
script.on_configuration_changed(ensure_storage)

script.on_event(defines.events.on_script_path_request_finished, function(event)
    ensure_storage()
    local points = nil
    if event.path then
        points = {}
        for _, waypoint in ipairs(event.path) do
            points[#points + 1] = {
                position = {
                    x = waypoint.position.x,
                    y = waypoint.position.y
                },
                needs_destroy_to_reach = waypoint.needs_destroy_to_reach
            }
        end
    end
    storage.path_results[event.id] = {
        found = points ~= nil,
        path = points,
        try_again_later = event.try_again_later
    }
end)

remote.add_interface(interface_name, {
    request_path = function(player_index, target_x, target_y, radius)
        ensure_storage()
        local player = game.get_player(player_index)
        if not player or not player.valid then
            error("FactorIA player is not available")
        end
        local character = player.character
        if not character or not character.valid then
            error("FactorIA player has no character")
        end
        return player.surface.request_path {
            bounding_box = character.prototype.collision_box,
            collision_mask = character.prototype.collision_mask,
            start = player.position,
            goal = {x = target_x, y = target_y},
            force = player.force,
            radius = radius,
            pathfind_flags = {
                allow_destroy_friendly_entities = false,
                allow_paths_through_own_entities = false,
                cache = false
            },
            can_open_gates = true,
            entity_to_ignore = character
        }
    end,

    take_path_result = function(path_id)
        ensure_storage()
        local result = storage.path_results[path_id]
        if result then
            storage.path_results[path_id] = nil
        end
        return result
    end
})
