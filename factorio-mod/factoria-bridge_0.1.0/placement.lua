local placement = {}
local inspection = require("entity_inspection")

local directions = {
    north = defines.direction.north,
    east = defines.direction.east,
    south = defines.direction.south,
    west = defines.direction.west
}

local cardinal_directions = {
    {name = "north", value = defines.direction.north},
    {name = "east", value = defines.direction.east},
    {name = "south", value = defines.direction.south},
    {name = "west", value = defines.direction.west}
}

local function find_target(player, position)
    local found = player.surface.find_entities_filtered {position = position, radius = 0.75}
    table.sort(found, function(left, right)
        local ldx = left.position.x - position.x
        local ldy = left.position.y - position.y
        local rdx = right.position.x - position.x
        local rdy = right.position.y - position.y
        return ldx * ldx + ldy * ldy < rdx * rdx + rdy * rdy
    end)
    for _, candidate in ipairs(found) do
        if candidate ~= player and candidate.valid and candidate.force == player.force and
            candidate.type ~= "resource" and candidate.type ~= "item-entity" and
            candidate.type ~= "character" then
            return candidate
        end
    end
    error("No connectable entity found at the requested position")
end

local function item_place_result(item_name)
    local item = prototypes.item[item_name]
    if not item then error("Unknown item prototype: " .. tostring(item_name)) end
    if not item.place_result then
        error("Item cannot be placed as an entity: " .. tostring(item_name))
    end
    return item.place_result
end

local function position_info(position)
    if not position then return nil end
    return {x = position.x, y = position.y}
end

local function contains(entities, expected)
    for _, entity in pairs(entities or {}) do
        if entity == expected then return true end
    end
    return false
end

local function find_connection(candidate, target, connection_kind)
    local optional = inspection.optional_property
    if connection_kind == "item-from-target" then
        if optional(target, "drop_target") == candidate then
            return {
                kind = "item",
                flow = "target-to-new",
                endpoint_owner = "target",
                endpoint = position_info(optional(target, "drop_position"))
            }
        elseif optional(candidate, "pickup_target") == target then
            return {
                kind = "item",
                flow = "target-to-new",
                endpoint_owner = "new",
                endpoint = position_info(optional(candidate, "pickup_position"))
            }
        end
    elseif connection_kind == "item-to-target" then
        if optional(candidate, "drop_target") == target then
            return {
                kind = "item",
                flow = "new-to-target",
                endpoint_owner = "new",
                endpoint = position_info(optional(candidate, "drop_position"))
            }
        elseif optional(target, "pickup_target") == candidate then
            return {
                kind = "item",
                flow = "new-to-target",
                endpoint_owner = "target",
                endpoint = position_info(optional(target, "pickup_position"))
            }
        end
    elseif connection_kind == "fluid" then
        local fluidbox = candidate.fluidbox
        if not fluidbox then return nil end
        for index = 1, #fluidbox do
            for connection_index, pipe in ipairs(fluidbox.get_pipe_connections(index)) do
                local owner = pipe.target and pipe.target.owner or nil
                if owner == target then
                    return {
                        kind = "fluid",
                        fluidbox_index = index,
                        connection_index = connection_index,
                        position = position_info(pipe.position),
                        target_position = position_info(pipe.target_position),
                        flow_direction = pipe.flow_direction
                    }
                end
            end
        end
    elseif connection_kind == "belt-from-target" or connection_kind == "belt-to-target" then
        local candidate_neighbours = optional(candidate, "belt_neighbours")
        local target_neighbours = optional(target, "belt_neighbours")
        if connection_kind == "belt-from-target" and
            ((candidate_neighbours and contains(candidate_neighbours.inputs, target)) or
                (target_neighbours and contains(target_neighbours.outputs, candidate))) then
            return {kind = "belt", flow = "target-to-new"}
        elseif connection_kind == "belt-to-target" and
            ((candidate_neighbours and contains(candidate_neighbours.outputs, target)) or
                (target_neighbours and contains(target_neighbours.inputs, candidate))) then
            return {kind = "belt", flow = "new-to-target"}
        end
        local underground = optional(candidate, "underground_belt_neighbour") or
            optional(candidate, "neighbours")
        if underground == target then
            local candidate_type = optional(candidate, "belt_to_ground_type")
            local target_type = optional(target, "belt_to_ground_type")
            if connection_kind == "belt-from-target" and target_type == "input" and
                candidate_type == "output" then
                return {kind = "underground-belt", flow = "target-to-new"}
            elseif connection_kind == "belt-to-target" and candidate_type == "input" and
                target_type == "output" then
                return {kind = "underground-belt", flow = "new-to-target"}
            end
        end
    elseif connection_kind == "electric" then
        local candidate_network = optional(candidate, "electric_network_id")
        local target_network = optional(target, "electric_network_id")
        if candidate_network and candidate_network == target_network then
            return {kind = "electric", network_id = candidate_network}
        end
    elseif connection_kind == "heat" then
        if contains(optional(candidate, "heat_neighbours"), target) or
            contains(optional(target, "heat_neighbours"), candidate) then
            return {kind = "heat"}
        end
    elseif connection_kind == "wall" then
        local neighbours = optional(candidate, "wall_neighbours") or optional(candidate, "neighbours")
        if neighbours and (neighbours.north == target or neighbours.east == target or
            neighbours.south == target or neighbours.west == target) then
            return {kind = "wall"}
        end
    end
    return nil
end

function placement.find_connection(player, arguments)
    local target = find_target(player, {x = arguments.target_x, y = arguments.target_y})
    local place_result = item_place_result(arguments.item)
    local direction_options = {
        {name = "north", value = defines.direction.north, swap = false},
        {name = "east", value = defines.direction.east, swap = true},
        {name = "south", value = defines.direction.south, swap = false},
        {name = "west", value = defines.direction.west, swap = true}
    }
    if not place_result.supports_direction then direction_options = {direction_options[1]} end

    local candidates = {}
    for _, direction in ipairs(direction_options) do
        local width = direction.swap and place_result.tile_height or place_result.tile_width
        local height = direction.swap and place_result.tile_width or place_result.tile_height
        local x_offset = width % 2 == 0 and 0 or 0.5
        local y_offset = height % 2 == 0 and 0 or 0.5
        local minimum_x = math.ceil(target.position.x - arguments.search_radius - x_offset)
        local maximum_x = math.floor(target.position.x + arguments.search_radius - x_offset)
        local minimum_y = math.ceil(target.position.y - arguments.search_radius - y_offset)
        local maximum_y = math.floor(target.position.y + arguments.search_radius - y_offset)
        for grid_x = minimum_x, maximum_x do
            for grid_y = minimum_y, maximum_y do
                local x = grid_x + x_offset
                local y = grid_y + y_offset
                local target_dx = x - target.position.x
                local target_dy = y - target.position.y
                if target_dx * target_dx + target_dy * target_dy <=
                    arguments.search_radius * arguments.search_radius then
                    local player_dx = x - player.position.x
                    local player_dy = y - player.position.y
                    candidates[#candidates + 1] = {
                        x = x,
                        y = y,
                        direction = direction.name,
                        direction_value = direction.value,
                        distance_from_player = math.sqrt(player_dx * player_dx + player_dy * player_dy),
                        distance_from_target = math.sqrt(target_dx * target_dx + target_dy * target_dy)
                    }
                end
            end
        end
    end
    table.sort(candidates, function(left, right)
        if left.distance_from_target == right.distance_from_target then
            return left.distance_from_player < right.distance_from_player
        end
        return left.distance_from_target < right.distance_from_target
    end)

    local inventory = player.get_main_inventory()
    local source = inventory and inventory.find_item_stack(arguments.item)
    local quality = source and source.quality.name or "normal"
    local options = {}
    for _, candidate in ipairs(candidates) do
        if #options >= 12 then break end
        local build = {
            name = place_result.name,
            position = {x = candidate.x, y = candidate.y},
            direction = candidate.direction_value,
            force = player.force,
            quality = quality,
            create_build_effect_smoke = false
        }
        if player.surface.can_place_entity {
            name = build.name,
            position = build.position,
            direction = build.direction,
            force = build.force,
            build_check_type = defines.build_check_type.manual
        } then
            local created, preview = pcall(function() return player.surface.create_entity(build) end)
            if created and preview then
                local inspected, option = pcall(function()
                    pcall(function() preview.update_connections() end)
                    pcall(function() target.update_connections() end)
                    local matched = find_connection(preview, target, arguments.connection)
                    if not matched then return nil end
                    local preview_info = {
                        name = preview.name,
                        type = preview.type,
                        position = {x = preview.position.x, y = preview.position.y},
                        direction_name = inspection.direction_name(preview.direction)
                    }
                    inspection.attach_connection_details(preview_info, preview)
                    return {
                        x = candidate.x,
                        y = candidate.y,
                        direction = candidate.direction,
                        distance_from_player = candidate.distance_from_player,
                        distance_from_target = candidate.distance_from_target,
                        connection = matched,
                        preview = preview_info
                    }
                end)
                preview.destroy()
                pcall(function() target.update_connections() end)
                if not inspected then error(option) end
                if option then options[#options + 1] = option end
            end
        end
    end
    table.sort(options, function(left, right)
        return left.distance_from_player < right.distance_from_player
    end)
    local result = {
        found = #options > 0,
        item = arguments.item,
        entity_name = place_result.name,
        item_count = inventory and inventory.get_item_count(arguments.item) or 0,
        connection = arguments.connection,
        search_radius = arguments.search_radius,
        target = inspection.entity_reference(target),
        options = options,
        preview_entities_removed = true
    }
    if #options > 0 then
        local recommended = options[1]
        result.recommended = recommended
        result.next_tool = {
            name = "walk_to_for_placement",
            arguments = {
                item = arguments.item,
                x = recommended.x,
                y = recommended.y,
                direction = recommended.direction
            }
        }
        result.verification =
            "After placement, require the reported connection target or network to match this target."
    else
        result.reason = "no_valid_connected_placement_in_search_radius"
    end
    return result
end

function placement.plan_walk(player, arguments)
    local place_result = item_place_result(arguments.item)
    local direction = directions[arguments.direction]
    if not direction then error("Invalid placement direction") end
    local requested = {x = arguments.x, y = arguments.y}
    local build = {name = place_result.name, position = requested, direction = direction}
    local inventory = player.get_main_inventory()
    local result = {
        item = arguments.item,
        entity_name = place_result.name,
        requested_position = requested,
        requested_direction = arguments.direction,
        current_position = {x = player.position.x, y = player.position.y},
        build_distance = player.build_distance,
        item_count = inventory and inventory.get_item_count(arguments.item) or 0,
        target_valid_before_move = player.surface.can_place_entity {
            name = place_result.name,
            position = requested,
            direction = direction,
            force = player.force,
            build_check_type = defines.build_check_type.manual
        },
        placement_ready = player.can_place_entity(build)
    }
    if result.placement_ready then
        result.candidates = {}
        return result
    end

    local box = place_result.collision_box
    local extent = math.max(
        math.abs(box.left_top.x),
        math.abs(box.left_top.y),
        math.abs(box.right_bottom.x),
        math.abs(box.right_bottom.y))
    local inner = extent + 1.25
    local outer = extent + math.max(2, math.min(4, player.build_distance - 0.5))
    if outer < inner then outer = inner end
    local candidates = {}
    local seen = {}
    for ring = inner, outer, 0.75 do
        for index = 0, 15 do
            local angle = index * math.pi / 8
            local anchor = {
                x = requested.x + math.cos(angle) * ring,
                y = requested.y + math.sin(angle) * ring
            }
            local candidate = player.surface.find_non_colliding_position(player.name, anchor, 0.7, 0.2)
            if candidate then
                local key = math.floor(candidate.x * 4 + 0.5) .. ":" ..
                    math.floor(candidate.y * 4 + 0.5)
                local tx = candidate.x - requested.x
                local ty = candidate.y - requested.y
                local edge_distance = math.max(0, math.sqrt(tx * tx + ty * ty) - extent)
                if not seen[key] and edge_distance <= player.build_distance - 0.25 then
                    seen[key] = true
                    local px = candidate.x - player.position.x
                    local py = candidate.y - player.position.y
                    candidates[#candidates + 1] = {
                        x = candidate.x,
                        y = candidate.y,
                        distance_from_player = math.sqrt(px * px + py * py),
                        edge_distance = edge_distance
                    }
                end
            end
        end
    end
    table.sort(candidates, function(left, right)
        return left.distance_from_player < right.distance_from_player
    end)
    while #candidates > 24 do table.remove(candidates) end
    result.candidates = candidates
    return result
end

function placement.verify_walk(player, arguments)
    local place_result = item_place_result(arguments.item)
    local direction = directions[arguments.direction]
    if not direction then error("Invalid placement direction") end
    local requested = {x = arguments.x, y = arguments.y}
    local result = {
        current_position = {x = player.position.x, y = player.position.y},
        build_distance = player.build_distance,
        target_valid = player.surface.can_place_entity {
            name = place_result.name,
            position = requested,
            direction = direction,
            force = player.force,
            build_check_type = defines.build_check_type.manual
        },
        placement_ready = player.can_place_entity {
            name = place_result.name,
            position = requested,
            direction = direction
        }
    }
    if result.target_valid then return result end

    local alternatives = {}
    local seen = {}
    local function test_position(x, y, distance)
        local key = x .. ":" .. y
        for _, candidate in ipairs(cardinal_directions) do
            local direction_key = key .. ":" .. candidate.name
            if not seen[direction_key] and player.surface.can_place_entity {
                name = place_result.name,
                position = {x = x, y = y},
                direction = candidate.value,
                force = player.force,
                build_check_type = defines.build_check_type.manual
            } then
                seen[direction_key] = true
                alternatives[#alternatives + 1] = {
                    x = x,
                    y = y,
                    direction = candidate.name,
                    distance_from_requested = distance
                }
            end
        end
    end
    for ring = 1, 8 do
        for offset = -ring, ring do
            test_position(requested.x + offset, requested.y - ring, ring)
            test_position(requested.x + offset, requested.y + ring, ring)
        end
        for offset = -ring + 1, ring - 1 do
            test_position(requested.x - ring, requested.y + offset, ring)
            test_position(requested.x + ring, requested.y + offset, ring)
        end
        if #alternatives >= 12 then break end
    end
    while #alternatives > 12 do table.remove(alternatives) end
    result.valid_alternatives = alternatives
    local blockers = {}
    local nearby = player.surface.find_entities_filtered {position = requested, radius = 6}
    for _, entity in ipairs(nearby) do
        if entity ~= player and #blockers < 12 then
            blockers[#blockers + 1] = {
                name = entity.name,
                type = entity.type,
                position = {x = entity.position.x, y = entity.position.y}
            }
        end
    end
    result.nearby_entities = blockers
    local tile = player.surface.get_tile(requested)
    result.target_tile = {
        name = tile.name,
        position = {x = tile.position.x, y = tile.position.y}
    }
    return result
end

return placement
