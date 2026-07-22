local construction = {}
local inspection = require("entity_inspection")

local direction_names = {
    [defines.direction.north] = "north",
    [defines.direction.northeast] = "northeast",
    [defines.direction.east] = "east",
    [defines.direction.southeast] = "southeast",
    [defines.direction.south] = "south",
    [defines.direction.southwest] = "southwest",
    [defines.direction.west] = "west",
    [defines.direction.northwest] = "northwest"
}

local function direction_name(direction)
    return direction_names[direction] or tostring(direction)
end

local function distance_to(player, entity)
    local dx = entity.position.x - player.position.x
    local dy = entity.position.y - player.position.y
    return math.sqrt(dx * dx + dy * dy), dx, dy
end

local function collect_build_requests(player, radius, requests, missing)
    local inventory = player.get_main_inventory()
    local ghosts = player.surface.find_entities_filtered {
        position = player.position,
        radius = radius,
        type = "entity-ghost",
        force = player.force
    }
    for _, ghost in ipairs(ghosts) do
        if ghost.valid then
            local prototype = prototypes.entity[ghost.ghost_name]
            local quality = ghost.quality and ghost.quality.name or "normal"
            local required_item = nil
            local required_count = 1
            local selected_item = nil
            local item_count = 0
            for _, item in ipairs(prototype and prototype.items_to_place_this or {}) do
                local count = inventory and inventory.get_item_count {
                    name = item.name,
                    quality = quality
                } or 0
                if not required_item then
                    required_item = item.name
                    required_count = item.count or 1
                    item_count = count
                end
                if not selected_item and count >= (item.count or 1) then
                    selected_item = item.name
                    required_count = item.count or 1
                    item_count = count
                end
            end

            local distance, dx, dy = distance_to(player, ghost)
            local available = selected_item ~= nil
            if not available and required_item then
                local key = required_item .. ":" .. quality
                local entry = missing[key] or {
                    name = required_item,
                    quality = quality,
                    count = 0
                }
                entry.count = entry.count + required_count
                missing[key] = entry
            end
            requests[#requests + 1] = {
                kind = "build",
                entity_name = ghost.ghost_name,
                ghost_type = ghost.ghost_type,
                position = {x = ghost.position.x, y = ghost.position.y},
                delta = {x = dx, y = dy},
                distance = distance,
                direction = ghost.direction,
                direction_name = direction_name(ghost.direction),
                quality = quality,
                item = selected_item or required_item,
                item_count = item_count,
                required_count = required_count,
                available = available,
                reachable = player.can_reach_entity(ghost)
            }
        end
    end
end

local function collect_deconstruction_requests(player, radius, requests)
    -- Ownership is intentionally unrestricted because marked trees and rocks belong to the neutral force.
    local marked = player.surface.find_entities_filtered {
        position = player.position,
        radius = radius,
        to_be_deconstructed = true
    }
    for _, entity in ipairs(marked) do
        if entity.valid and entity ~= player and entity.type ~= "entity-ghost" and
            entity.to_be_deconstructed(player.force) then
            local distance, dx, dy = distance_to(player, entity)
            requests[#requests + 1] = {
                kind = "deconstruct",
                entity_name = entity.name,
                entity_type = entity.type,
                position = {x = entity.position.x, y = entity.position.y},
                delta = {x = dx, y = dy},
                distance = distance,
                direction = entity.direction,
                direction_name = direction_name(entity.direction),
                minable = entity.minable,
                available = entity.minable,
                reachable = entity.minable and player.can_reach_entity(entity) or false
            }
        end
    end
end

function construction.get_requests(player, arguments)
    local radius = arguments.radius
    local kind = arguments.kind or "all"
    local offset = arguments.offset or 0
    local available_only = arguments.available_only == true
    if type(radius) ~= "number" or radius < 1 or radius > 8192 then
        error("Construction request radius must be between 1 and 8192")
    end
    if kind ~= "all" and kind ~= "build" and kind ~= "deconstruct" then
        error("Unknown construction request kind: " .. tostring(kind))
    end
    if type(offset) ~= "number" or offset < 0 or offset % 1 ~= 0 then
        error("Construction request offset must be a non-negative integer")
    end

    local requests = {}
    local missing = {}
    if kind == "all" or kind == "build" then
        collect_build_requests(player, radius, requests, missing)
    end
    if kind == "all" or kind == "deconstruct" then
        collect_deconstruction_requests(player, radius, requests)
    end
    table.sort(requests, function(left, right)
        if left.distance ~= right.distance then return left.distance < right.distance end
        if left.kind ~= right.kind then return left.kind < right.kind end
        if left.entity_name ~= right.entity_name then return left.entity_name < right.entity_name end
        if left.position.x ~= right.position.x then return left.position.x < right.position.x end
        return left.position.y < right.position.y
    end)

    local total_all = #requests
    local available_count = 0
    local filtered = {}
    for _, request in ipairs(requests) do
        if request.available then
            available_count = available_count + 1
        end
        if not available_only or request.available then
            filtered[#filtered + 1] = request
        end
    end

    local page = {}
    for index = offset + 1, #filtered do
        if #page >= 40 then break end
        page[#page + 1] = filtered[index]
    end
    local missing_items = {}
    for _, entry in pairs(missing) do
        missing_items[#missing_items + 1] = entry
    end
    table.sort(missing_items, function(left, right)
        if left.name ~= right.name then return left.name < right.name end
        return left.quality < right.quality
    end)

    local next_offset = nil
    if offset + #page < #filtered then
        next_offset = offset + #page
    end
    return {
        radius = radius,
        kind = kind,
        available_only = available_only,
        player_position = {x = player.position.x, y = player.position.y},
        coordinate_hint = "positive x is east; positive y is south",
        offset = offset,
        total_requests = #filtered,
        total_requests_before_filter = total_all,
        available_count = available_count,
        truncated = next_offset ~= nil,
        next_offset = next_offset,
        missing_items = missing_items,
        requests = page
    }
end

function construction.build_ghost(player, request)
    local position = request.position
    local ghosts = player.surface.find_entities_filtered {
        position = position,
        radius = 0.25,
        type = "entity-ghost",
        ghost_name = request.entity_name
    }
    local ghost = ghosts[1]
    if not ghost or not ghost.valid then
        return {fulfilled = false, missing = true, reason = "ghost_missing"}
    end
    if not player.can_reach_entity(ghost) then
        return {
            fulfilled = false,
            out_of_reach = true,
            reason = "ghost_out_of_reach",
            position = position
        }
    end

    local inventory = player.get_main_inventory()
    if not inventory then
        error("Controlled character has no main inventory")
    end
    local item_name = request.item
    local quality = ghost.quality and ghost.quality.name or "normal"
    local prototype = prototypes.entity[ghost.ghost_name]
    local required_count = nil
    for _, item in ipairs(prototype and prototype.items_to_place_this or {}) do
        if item.name == item_name then
            required_count = item.count or 1
            break
        end
    end
    if not required_count then
        return {
            fulfilled = false,
            reason = "item_does_not_build_ghost",
            item = item_name,
            entity_name = ghost.ghost_name
        }
    end

    local available = inventory.get_item_count {name = item_name, quality = quality}
    if available < required_count then
        return {
            fulfilled = false,
            reason = "missing_item",
            item = item_name,
            quality = quality,
            required_count = required_count,
            item_count = available
        }
    end
    local removed = inventory.remove {name = item_name, count = required_count, quality = quality}
    if removed ~= required_count then
        if removed > 0 then
            inventory.insert {name = item_name, count = removed, quality = quality}
        end
        return {
            fulfilled = false,
            reason = "item_could_not_be_reserved",
            item = item_name,
            quality = quality
        }
    end

    local revived, collided, entity, proxy = pcall(function()
        return ghost.revive {raise_revive = true, overflow = inventory}
    end)
    if not revived or not entity then
        inventory.insert {name = item_name, count = removed, quality = quality}
        return {
            fulfilled = false,
            reason = "ghost_could_not_be_revived",
            error = not revived and tostring(collided) or nil,
            item = item_name,
            quality = quality
        }
    end

    local entity_details = {
        name = entity.name,
        type = entity.type,
        quality = entity.quality.name,
        position = {x = entity.position.x, y = entity.position.y},
        direction = entity.direction,
        direction_name = direction_name(entity.direction),
        unit_number = entity.unit_number
    }
    inspection.attach_connection_details(entity_details, entity)
    return {
        fulfilled = true,
        item = item_name,
        item_quality = quality,
        consumed_count = removed,
        remaining_item_count = inventory.get_item_count {name = item_name, quality = quality},
        collided_items = collided or {},
        item_request_proxy_created = proxy ~= nil,
        entity = entity_details
    }
end

return construction
