local inspection = {}

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

function inspection.direction_name(direction)
    return direction_names[direction] or tostring(direction)
end

function inspection.entity_reference(entity)
    if not entity or not entity.valid then return nil end
    return {
        name = entity.name,
        type = entity.type,
        position = {x = entity.position.x, y = entity.position.y},
        direction = inspection.direction_name(entity.direction),
        unit_number = entity.unit_number
    }
end

function inspection.optional_property(entity, name)
    local ok, value = pcall(function() return entity[name] end)
    if ok then return value end
    return nil
end

local function entity_references(entities)
    local result = {}
    for _, entity in pairs(entities or {}) do
        result[#result + 1] = inspection.entity_reference(entity)
    end
    return result
end

local function attach_item_connections(info, entity)
    if entity.type == "mining-drill" or entity.type == "inserter" then
        local drop_target = entity.drop_target
        info.item_output = {
            position = {x = entity.drop_position.x, y = entity.drop_position.y},
            connected = drop_target ~= nil,
            target = inspection.entity_reference(drop_target)
        }
    end
    if entity.type == "inserter" then
        local pickup_target = entity.pickup_target
        info.item_input = {
            position = {x = entity.pickup_position.x, y = entity.pickup_position.y},
            connected = pickup_target ~= nil,
            target = inspection.entity_reference(pickup_target)
        }
    end
    if entity.type == "mining-drill" then
        local area = entity.mining_area
        info.mining_area = {
            left_top = {x = area.left_top.x, y = area.left_top.y},
            right_bottom = {x = area.right_bottom.x, y = area.right_bottom.y}
        }
        info.mining_target = inspection.entity_reference(entity.mining_target)
    end
end

local function attach_belt_connections(info, entity)
    local belt_types = {
        ["transport-belt"] = true,
        ["underground-belt"] = true,
        splitter = true,
        loader = true,
        ["loader-1x1"] = true,
        ["linked-belt"] = true
    }
    if not belt_types[entity.type] then return end
    local neighbours = entity.belt_neighbours
    info.belt_connections = {
        inputs = entity_references(neighbours.inputs),
        outputs = entity_references(neighbours.outputs)
    }
    if entity.type == "underground-belt" then
        info.belt_connections.underground = inspection.entity_reference(
            inspection.optional_property(entity, "underground_belt_neighbour") or
                inspection.optional_property(entity, "neighbours"))
    elseif entity.type == "linked-belt" then
        info.belt_connections.linked = inspection.entity_reference(
            inspection.optional_property(entity, "linked_belt_neighbour") or
                inspection.optional_property(entity, "neighbours"))
    end
end

local function attach_fluid_details(info, entity)
    local fluidbox = entity.fluidbox
    if not fluidbox or #fluidbox == 0 then return end
    local boxes = {}
    local connection_count = 0
    local connected_count = 0
    local function add_prototype(target, prototype)
        target[#target + 1] = {
            production_type = prototype.production_type,
            filter = prototype.filter and prototype.filter.name or nil,
            minimum_temperature = prototype.minimum_temperature,
            maximum_temperature = prototype.maximum_temperature
        }
    end
    for index = 1, #fluidbox do
        local box = {index = index, connections = {}}
        local prototype = nil
        if entity.type == "entity-ghost" then
            -- Factorio cannot wrap the inner fluidbox prototype through LuaFluidBox.get_prototype.
            local ghost_prototype = entity.ghost_prototype
            prototype = ghost_prototype and ghost_prototype.fluidbox_prototypes[index] or nil
        else
            prototype = fluidbox.get_prototype(index)
        end
        local prototype_info = {}
        if prototype and prototype.object_name then
            add_prototype(prototype_info, prototype)
        elseif prototype then
            for _, entry in pairs(prototype) do add_prototype(prototype_info, entry) end
        end
        box.prototypes = prototype_info
        local fluid = fluidbox[index]
        if fluid then
            box.fluid = {name = fluid.name, amount = fluid.amount, temperature = fluid.temperature}
        end
        for _, connection in ipairs(fluidbox.get_pipe_connections(index)) do
            local target_owner = connection.target and connection.target.owner or nil
            local connection_info = {
                flow_direction = connection.flow_direction,
                connection_type = connection.connection_type,
                position = {x = connection.position.x, y = connection.position.y},
                target_position = {x = connection.target_position.x, y = connection.target_position.y},
                connected = target_owner ~= nil
            }
            if target_owner then
                connection_info.target = inspection.entity_reference(target_owner)
                connected_count = connected_count + 1
            end
            box.connections[#box.connections + 1] = connection_info
            connection_count = connection_count + 1
        end
        boxes[#boxes + 1] = box
    end
    info.fluidboxes = boxes
    info.fluid_connection_summary = {
        total = connection_count,
        connected = connected_count,
        open = connection_count - connected_count
    }
end

local function attach_electric_connections(info, entity)
    local prototype = entity.prototype
    if entity.type ~= "electric-pole" and not prototype.electric_energy_source_prototype then return end
    info.electric_connection = {
        network_id = entity.electric_network_id,
        connected_to_power_source = entity.is_connected_to_electric_network()
    }
    if entity.type == "electric-pole" then
        local radius = prototype.get_supply_area_distance(entity.quality.name)
        info.electric_connection.supply_radius = radius
        info.electric_connection.supply_area = {
            left_top = {x = entity.position.x - radius, y = entity.position.y - radius},
            right_bottom = {x = entity.position.x + radius, y = entity.position.y + radius}
        }
        info.electric_connection.maximum_wire_distance =
            prototype.get_max_wire_distance(entity.quality.name)
    end
end

local function attach_heat_connections(info, entity)
    local prototype = entity.prototype
    if not prototype.heat_buffer_prototype and not prototype.heat_energy_source_prototype then return end
    info.heat_connections = {neighbours = entity_references(entity.heat_neighbours)}
end

local function attach_wall_connections(info, entity)
    if entity.type ~= "wall" and entity.type ~= "gate" then return end
    local neighbours = inspection.optional_property(entity, "wall_neighbours") or
        inspection.optional_property(entity, "neighbours")
    if not neighbours then return end
    info.wall_connections = {
        north = inspection.entity_reference(neighbours.north),
        east = inspection.entity_reference(neighbours.east),
        south = inspection.entity_reference(neighbours.south),
        west = inspection.entity_reference(neighbours.west)
    }
end

function inspection.attach_connection_details(info, entity)
    local box = entity.bounding_box
    info.collision_box = {
        left_top = {x = box.left_top.x, y = box.left_top.y},
        right_bottom = {x = box.right_bottom.x, y = box.right_bottom.y}
    }
    attach_item_connections(info, entity)
    attach_belt_connections(info, entity)
    attach_fluid_details(info, entity)
    attach_electric_connections(info, entity)
    attach_heat_connections(info, entity)
    attach_wall_connections(info, entity)
end

local function entity_filter(player, arguments)
    local filter = {position = player.position, radius = arguments.radius}
    if arguments.names then filter.name = arguments.names
    elseif arguments.name then filter.name = arguments.name end
    if arguments.type then filter.type = arguments.type end
    return filter
end

function inspection.nearby_prototypes(player, arguments)
    local found = player.surface.find_entities_filtered(entity_filter(player, arguments))
    local seen = {}
    local entries = {}
    for _, entity in ipairs(found) do
        if entity ~= player and not seen[entity.name] then
            seen[entity.name] = true
            entries[#entries + 1] = {name = entity.name, type = entity.type}
        end
    end
    return {
        player_position = {x = player.position.x, y = player.position.y},
        prototypes = entries
    }
end

local function summarize(inventory, limit)
    if not inventory then return nil end
    local items = {}
    for _, item in pairs(inventory.get_contents()) do
        items[#items + 1] = {name = item.name, count = item.count}
    end
    table.sort(items, function(left, right) return left.count > right.count end)
    while #items > limit do table.remove(items) end
    return items
end

local function attach_inventory(info, name, inventory, limit)
    if not inventory then return end
    info.inventory = info.inventory or {}
    info.inventory[name] = {
        items = summarize(inventory, limit),
        empty = inventory.is_empty(),
        full = inventory.is_full()
    }
end

function inspection.get_nearby_entities(player, arguments)
    local found = player.surface.find_entities_filtered(entity_filter(player, arguments))
    table.sort(found, function(left, right)
        local ldx = left.position.x - player.position.x
        local ldy = left.position.y - player.position.y
        local rdx = right.position.x - player.position.x
        local rdy = right.position.y - player.position.y
        return ldx * ldx + ldy * ldy < rdx * rdx + rdy * rdy
    end)

    local counts = {}
    local total = 0
    local distinct = 0
    for _, entity in ipairs(found) do
        if entity ~= player then
            if not counts[entity.name] then
                counts[entity.name] = 0
                distinct = distinct + 1
            end
            counts[entity.name] = counts[entity.name] + 1
            total = total + 1
        end
    end
    local ordered = {}
    local duplicates = {}
    local seen = {}
    for _, entity in ipairs(found) do
        if entity ~= player then
            if not seen[entity.name] then
                seen[entity.name] = true
                ordered[#ordered + 1] = entity
            else
                duplicates[#duplicates + 1] = entity
            end
        end
    end
    for _, entity in ipairs(duplicates) do ordered[#ordered + 1] = entity end

    local offset = arguments.offset or 0
    local entities = {}
    for index = offset + 1, #ordered do
        if #entities >= 40 then break end
        local entity = ordered[index]
        local dx = entity.position.x - player.position.x
        local dy = entity.position.y - player.position.y
        local info = {
            name = entity.name,
            type = entity.type,
            force = entity.force and entity.force.name or nil,
            position = {x = entity.position.x, y = entity.position.y},
            delta = {x = dx, y = dy},
            distance = math.sqrt(dx * dx + dy * dy),
            direction = entity.direction,
            direction_name = inspection.direction_name(entity.direction),
            health = entity.health,
            minable = entity.minable,
            reachable = player.can_reach_entity(entity),
            prototype_count_in_radius = counts[entity.name]
        }
        if entity.status then
            for name, value in pairs(defines.entity_status) do
                if value == entity.status then
                    info.status = name
                    break
                end
            end
        end
        if entity.type == "resource" then
            info.amount = entity.amount
        elseif entity.type == "container" or entity.type == "logistic-container" then
            attach_inventory(info, "chest", entity.get_inventory(defines.inventory.chest), 6)
        end
        attach_inventory(info, "fuel", entity.get_fuel_inventory(), 4)
        if entity.type == "lab" then
            attach_inventory(info, "input", entity.get_inventory(defines.inventory.lab_input), 6)
        elseif entity.type == "furnace" or entity.type == "assembling-machine" then
            attach_inventory(info, "input", entity.get_inventory(defines.inventory.crafter_input), 6)
            attach_inventory(info, "output", entity.get_inventory(defines.inventory.crafter_output), 4)
            info.crafting = entity.is_crafting()
            local recipe = entity.get_recipe()
            info.recipe = recipe and recipe.name or nil
            if info.crafting then info.wait_tool = "wait_for_machine_output" end
        end
        inspection.attach_connection_details(info, entity)
        entities[#entities + 1] = info
    end
    return {
        radius = arguments.radius,
        player_position = {x = player.position.x, y = player.position.y},
        coordinate_hint = "positive x is east; positive y is south",
        regex = arguments.regex,
        ordering = "nearest representative of each distinct prototype first, then nearest duplicate instances",
        offset = offset,
        total_entities = total,
        distinct_prototypes = distinct,
        truncated = offset + #entities < total,
        next_offset = offset + #entities < total and offset + #entities or nil,
        entities = entities
    }
end

return inspection
