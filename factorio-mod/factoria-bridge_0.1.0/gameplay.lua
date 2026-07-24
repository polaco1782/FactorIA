local gameplay = {}
local inspection = require("entity_inspection")

local directions = {
    north = defines.direction.north,
    east = defines.direction.east,
    south = defines.direction.south,
    west = defines.direction.west
}

local function entity_info(entity)
    local box = entity.bounding_box
    local info = {
        name = entity.name,
        type = entity.type,
        quality = entity.quality and entity.quality.name or "normal",
        position = {x = entity.position.x, y = entity.position.y},
        direction = entity.direction,
        direction_name = inspection.direction_name(entity.direction),
        unit_number = entity.unit_number,
        collision_box = {
            left_top = {x = box.left_top.x, y = box.left_top.y},
            right_bottom = {x = box.right_bottom.x, y = box.right_bottom.y}
        }
    }
    inspection.attach_connection_details(info, entity)
    return info
end

local function find_entity(player, x, y, predicate, description, require_reach)
    local position = {x = x, y = y}
    local found = player.surface.find_entities_filtered {position = position, radius = 0.75}
    table.sort(found, function(left, right)
        local ldx = left.position.x - position.x
        local ldy = left.position.y - position.y
        local rdx = right.position.x - position.x
        local rdy = right.position.y - position.y
        return ldx * ldx + ldy * ldy < rdx * rdx + rdy * rdy
    end)
    for _, candidate in ipairs(found) do
        if candidate ~= player and predicate(candidate) then
            if require_reach and not player.can_reach_entity(candidate) then
                error("Target entity is out of reach")
            end
            return candidate
        end
    end
    error("No " .. description .. " found at the requested position")
end

local function entity_inventory(entity, kind)
    if kind == "chest" and
        (entity.type == "container" or entity.type == "logistic-container") then
        return entity.get_inventory(defines.inventory.chest)
    elseif kind == "fuel" then
        return entity.get_fuel_inventory()
    elseif kind == "output" and
        (entity.type == "furnace" or entity.type == "assembling-machine") then
        return entity.get_inventory(defines.inventory.crafter_output)
    elseif kind == "input" then
        if entity.type == "lab" then
            return entity.get_inventory(defines.inventory.lab_input)
        elseif entity.type == "furnace" or entity.type == "assembling-machine" then
            return entity.get_inventory(defines.inventory.crafter_input)
        end
    end
    return nil
end

local function find_inventory_entity(player, arguments, require_reach)
    local kind = arguments.inventory
    if kind ~= "chest" and kind ~= "fuel" and kind ~= "input" and kind ~= "output" then
        error("inventory must be chest, fuel, input, or output")
    end
    local target = find_entity(
        player,
        arguments.x,
        arguments.y,
        function(candidate) return entity_inventory(candidate, kind) ~= nil end,
        "entity with a " .. kind .. " inventory",
        require_reach)
    return target, entity_inventory(target, kind)
end

local function research_queue(force)
    local queue = {}
    local queued = {}
    for index, technology in ipairs(force.research_queue) do
        queue[#queue + 1] = {position = index, name = technology.name, level = technology.level}
        queued[technology.name] = true
    end
    return queue, queued
end

local function research_lab_status(force)
    local summary = {
        total_labs = 0,
        connected_labs = 0,
        powered_labs = 0,
        unpowered_labs = 0,
        low_power_labs = 0,
        working_labs = 0,
        blocked_labs = {}
    }

    for _, surface in pairs(game.surfaces) do
        for _, lab in ipairs(surface.find_entities_filtered {force = force, type = "lab"}) do
            local connected = lab.is_connected_to_electric_network()
            local status = inspection.status_name(lab) or "unknown"
            local no_power = not connected or status == "no_power"
            local low_power = status == "low_power"
            summary.total_labs = summary.total_labs + 1
            if connected then summary.connected_labs = summary.connected_labs + 1 end
            if no_power then
                summary.unpowered_labs = summary.unpowered_labs + 1
            elseif low_power then
                summary.low_power_labs = summary.low_power_labs + 1
            else
                summary.powered_labs = summary.powered_labs + 1
            end
            if status == "working" then summary.working_labs = summary.working_labs + 1 end

            if status ~= "working" and #summary.blocked_labs < 6 then
                summary.blocked_labs[#summary.blocked_labs + 1] = {
                    name = lab.name,
                    surface = surface.name,
                    position = {x = lab.position.x, y = lab.position.y},
                    status = status,
                    connected_to_electric_network = connected
                }
            end
        end
    end

    -- A powered lab alone is not enough: Factorio must report a lab as working before waiting can advance research.
    summary.ready_for_research_wait = summary.working_labs > 0
    return summary
end

local function technology_info(technology, use_dedicated_character)
    local prerequisites = {}
    local missing = {}
    for name, prerequisite in pairs(technology.prerequisites) do
        prerequisites[#prerequisites + 1] = name
        if not prerequisite.researched then
            missing[#missing + 1] = name
        end
    end
    table.sort(prerequisites)
    table.sort(missing)
    local ingredients = {}
    for _, ingredient in ipairs(technology.research_unit_ingredients) do
        ingredients[#ingredients + 1] = {name = ingredient.name, amount = ingredient.amount}
    end
    table.sort(ingredients, function(left, right) return left.name < right.name end)
    local unlocks = {}
    for _, effect in ipairs(technology.prototype.effects) do
        if effect.type == "unlock-recipe" then unlocks[#unlocks + 1] = effect.recipe end
    end
    table.sort(unlocks)

    local source_trigger = technology.prototype.research_trigger
    local trigger = nil
    if source_trigger then
        trigger = {type = source_trigger.type}
        if source_trigger.type == "craft-item" then
            trigger.item = source_trigger.item
            trigger.count = source_trigger.count
        elseif source_trigger.type == "mine-entity" then
            trigger.entity = source_trigger.entity
        elseif source_trigger.type == "craft-fluid" then
            trigger.fluid = source_trigger.fluid
            trigger.amount = source_trigger.amount
        elseif source_trigger.type == "build-entity" then
            trigger.entity = source_trigger.entity
        elseif source_trigger.type == "send-item-to-orbit" then
            trigger.item = source_trigger.item
        elseif source_trigger.type == "capture-spawner" then
            trigger.entity = source_trigger.entity
        end
    end
    local available = technology.enabled and not technology.researched and #missing == 0
    local info = {
        name = technology.name,
        level = technology.level,
        enabled = technology.enabled,
        researched = technology.researched,
        available = available,
        startable = available and trigger == nil,
        prerequisites = prerequisites,
        missing_prerequisites = missing,
        ingredients = ingredients,
        research_unit_count = technology.research_unit_count,
        research_unit_energy_ticks = technology.research_unit_energy,
        research_unit_time_seconds = technology.research_unit_energy / 60,
        unlocks_recipes = unlocks,
        research_mode = trigger and "trigger" or "lab",
        research_trigger = trigger
    }
    if trigger then
        info.research_trigger_progress = remote.call(
            "factoria_bridge",
            "research_trigger_progress",
            use_dedicated_character == true,
            technology.name)
    end
    return info
end

function gameplay.get_game_state(player, _, use_dedicated_character)
    local bridge = remote.interfaces.factoria_bridge
    local connected = player.player
    return {
        tick = game.tick,
        player = {
            name = connected and connected.name or "FactorIA AI",
            dedicated = use_dedicated_character == true,
            position = {x = player.position.x, y = player.position.y},
            health = player.health,
            max_health = player.max_health,
            walking = player.walking_state.walking,
            direction = player.walking_state.direction,
            mining = player.mining_state.mining,
            selected = player.selected and player.selected.name or nil,
            cursor_item = player.cursor_stack and player.cursor_stack.valid_for_read and
                player.cursor_stack.name or nil
        },
        crafting_queue_size = player.crafting_queue_size,
        rockets_launched = player.force.rockets_launched,
        items_launched = player.force.items_launched,
        rocket_launch_confirmed = player.force.rockets_launched > 0,
        environment = {
            surface = player.surface.name,
            daytime = player.surface.daytime,
            darkness = player.surface.darkness,
            pollution = player.surface.get_pollution(player.position)
        },
        bridge_mod_available = bridge ~= nil,
        bridge_runtime_available = bridge ~= nil and bridge.runtime_info ~= nil and bridge.start_action ~= nil,
        bridge_research_trigger_actions_available = bridge ~= nil and
            bridge.record_research_trigger_action ~= nil and bridge.research_trigger_progress ~= nil
    }
end

function gameplay.get_inventory(player)
    local inventory = player.get_main_inventory()
    local items = {}
    if inventory then
        for _, item in pairs(inventory.get_contents()) do
            items[#items + 1] = {
                name = item.name,
                count = item.count,
                quality = tostring(item.quality)
            }
        end
    end
    table.sort(items, function(left, right) return left.name < right.name end)
    return {items = items}
end

local function recipe_product_matches(recipe, item_name)
    for _, product in ipairs(recipe.products) do
        if product.type == "item" and product.name == item_name then return true end
    end
    return false
end

local function recipe_info(player, recipe, craftable_count)
    local inventory = player.get_main_inventory()
    local ingredients = {}
    for _, ingredient in ipairs(recipe.ingredients) do
        local available = ingredient.type == "item" and inventory and
            inventory.get_item_count(ingredient.name) or nil
        ingredients[#ingredients + 1] = {
            name = ingredient.name,
            type = ingredient.type,
            amount = ingredient.amount,
            available_count = available,
            missing_count = available and math.max(0, ingredient.amount - available) or nil
        }
    end
    table.sort(ingredients, function(left, right) return left.name < right.name end)

    local products = {}
    for _, product in ipairs(recipe.products) do
        products[#products + 1] = {
            name = product.name,
            type = product.type,
            amount = product.amount,
            amount_min = product.amount_min,
            amount_max = product.amount_max,
            probability = product.probability
        }
    end
    table.sort(products, function(left, right) return left.name < right.name end)

    return {
        name = recipe.name,
        enabled = recipe.enabled,
        craftable_count = craftable_count,
        category = recipe.category,
        additional_categories = recipe.additional_categories,
        energy_seconds = recipe.energy,
        ingredients = ingredients,
        products = products
    }
end

function gameplay.get_craftable_recipes(player, arguments)
    if arguments.recipe and arguments.item then
        error("get_craftable_recipes accepts either recipe or item, not both")
    end
    if arguments.recipe and not player.force.recipes[arguments.recipe] then
        error("Unknown recipe: " .. tostring(arguments.recipe))
    end
    if arguments.item and not prototypes.item[arguments.item] then
        error("Unknown item prototype: " .. tostring(arguments.item))
    end

    local targeted = arguments.recipe ~= nil or arguments.item ~= nil
    local recipes = {}
    for name, recipe in pairs(player.force.recipes) do
        local matches = name ~= "recipe-unknown" and
            (not arguments.recipe or name == arguments.recipe) and
            (not arguments.item or recipe_product_matches(recipe, arguments.item))
        if matches then
            local count = player.get_craftable_count(recipe)
            if targeted then
                recipes[#recipes + 1] = recipe_info(player, recipe, count)
            elseif recipe.enabled and count > 0 then
                recipes[#recipes + 1] = {name = name, craftable_count = count}
            end
        end
    end
    table.sort(recipes, function(left, right) return left.name < right.name end)
    local total = #recipes
    while #recipes > 100 do table.remove(recipes) end
    return {
        recipes = recipes,
        total_count = total,
        truncated = total > #recipes,
        filter = targeted and {recipe = arguments.recipe, item = arguments.item} or nil
    }
end

function gameplay.get_research_status(player, arguments, use_dedicated_character)
    local force = player.force
    local queue, queued = research_queue(force)
    if arguments.technology then
        local technology = force.technologies[arguments.technology]
        if not technology then error("Unknown technology: " .. tostring(arguments.technology)) end
        local info = technology_info(technology, use_dedicated_character)
        info.queued = queued[technology.name] or false
        local labs = nil
        if force.current_research == technology then
            info.progress = force.research_progress
            labs = research_lab_status(force)
        end
        return {
            research_enabled = force.research_enabled,
            technology = info,
            queue = queue,
            lab_status = labs,
            research_wait_ready = labs and labs.ready_for_research_wait or false
        }
    end

    local available = {}
    local triggered = {}
    for _, technology in pairs(force.technologies) do
        local info = technology_info(technology, use_dedicated_character)
        if info.startable and not queued[technology.name] then
            available[#available + 1] = info
        elseif info.available and info.research_trigger then
            triggered[#triggered + 1] = info
        end
    end
    table.sort(available, function(left, right) return left.name < right.name end)
    table.sort(triggered, function(left, right) return left.name < right.name end)
    local total = #available
    while #available > 80 do table.remove(available) end
    local trigger_total = #triggered
    while #triggered > 80 do table.remove(triggered) end
    local current = force.current_research and
        technology_info(force.current_research, use_dedicated_character) or nil
    if current then current.progress = force.research_progress end
    local labs = current and research_lab_status(force) or nil
    return {
        research_enabled = force.research_enabled,
        current = current,
        queue = queue,
        lab_status = labs,
        research_wait_ready = labs and labs.ready_for_research_wait or false,
        available = available,
        total_available = total,
        truncated = total > #available,
        triggered_milestones = triggered,
        total_triggered_milestones = trigger_total,
        triggered_milestones_truncated = trigger_total > #triggered
    }
end

function gameplay.start_research(player, arguments, use_dedicated_character)
    local force = player.force
    if not force.research_enabled then
        error("Research is disabled for this force")
    end
    local technology = force.technologies[arguments.technology]
    if not technology then
        error("Unknown technology: " .. tostring(arguments.technology))
    end
    local info = technology_info(technology, use_dedicated_character)
    local queue, queued = research_queue(force)
    if technology.researched then
        return {accepted = false, reason = "already_researched", technology = info, queue = queue}
    elseif not technology.enabled then
        return {accepted = false, reason = "technology_disabled", technology = info, queue = queue}
    elseif #info.missing_prerequisites > 0 then
        return {accepted = false, reason = "missing_prerequisites", technology = info, queue = queue}
    elseif technology.prototype.research_trigger then
        return {accepted = false, reason = "trigger_action_required", technology = info, queue = queue}
    end

    local already_queued = queued[technology.name] or false
    if not already_queued then
        local updated_queue = {}
        for _, queued_technology in ipairs(force.research_queue) do
            updated_queue[#updated_queue + 1] = queued_technology
        end
        updated_queue[#updated_queue + 1] = technology
        force.research_queue = updated_queue
    end
    queue, queued = research_queue(force)
    local accepted = queued[technology.name] == true
    local labs = force.current_research == technology and research_lab_status(force) or nil
    return {
        accepted = accepted,
        already_queued = already_queued,
        reason = accepted and (already_queued and "already_queued" or "queued") or
            "rejected_by_factorio",
        technology = info,
        queue = queue,
        current_research = force.current_research and force.current_research.name or nil,
        research_progress = force.research_progress,
        lab_status = labs,
        research_wait_ready = labs and labs.ready_for_research_wait or false
    }
end

function gameplay.find_resource_patches(player, arguments)
    local resource = arguments.resource
    local filter = {position = player.position, radius = arguments.radius, type = "resource"}
    if resource ~= "any" then filter.name = resource end
    local found = player.surface.find_entities_filtered(filter)
    local cell_size = 32
    local grouped = {}
    for _, entity in pairs(found) do
        local cell_x = math.floor(entity.position.x / cell_size)
        local cell_y = math.floor(entity.position.y / cell_size)
        local key = entity.name .. ":" .. cell_x .. ":" .. cell_y
        local patch = grouped[key]
        local dx = entity.position.x - player.position.x
        local dy = entity.position.y - player.position.y
        local distance = math.sqrt(dx * dx + dy * dy)
        if not patch then
            patch = {
                resource = entity.name,
                count = 0,
                total_amount = 0,
                sum_x = 0,
                sum_y = 0,
                nearest_distance = distance,
                nearest_position = {x = entity.position.x, y = entity.position.y},
                bounds = {
                    left = entity.position.x,
                    right = entity.position.x,
                    top = entity.position.y,
                    bottom = entity.position.y
                }
            }
            grouped[key] = patch
        end
        patch.count = patch.count + 1
        patch.total_amount = patch.total_amount + (entity.amount or 0)
        patch.sum_x = patch.sum_x + entity.position.x
        patch.sum_y = patch.sum_y + entity.position.y
        patch.bounds.left = math.min(patch.bounds.left, entity.position.x)
        patch.bounds.right = math.max(patch.bounds.right, entity.position.x)
        patch.bounds.top = math.min(patch.bounds.top, entity.position.y)
        patch.bounds.bottom = math.max(patch.bounds.bottom, entity.position.y)
        if distance < patch.nearest_distance then
            patch.nearest_distance = distance
            patch.nearest_position = {x = entity.position.x, y = entity.position.y}
        end
    end

    local patches = {}
    for _, patch in pairs(grouped) do
        patch.center = {x = patch.sum_x / patch.count, y = patch.sum_y / patch.count}
        patch.sum_x = nil
        patch.sum_y = nil
        patch.delta = {
            x = patch.nearest_position.x - player.position.x,
            y = patch.nearest_position.y - player.position.y
        }
        patches[#patches + 1] = patch
    end
    table.sort(patches, function(left, right)
        return left.nearest_distance < right.nearest_distance
    end)
    while #patches > 24 do table.remove(patches) end
    return {
        requested_resource = resource,
        radius = arguments.radius,
        player_position = {x = player.position.x, y = player.position.y},
        coordinate_hint = "positive x is east; positive y is south",
        grouping = "32-tile survey cells; adjacent results can belong to one physical patch",
        patches = patches
    }
end

function gameplay.find_water(player, arguments)
    local radius = arguments.radius
    local water_names = {}
    for name, prototype in pairs(prototypes.tile) do
        if prototype.fluid and prototype.fluid.name == "water" then
            water_names[#water_names + 1] = name
        end
    end
    table.sort(water_names)
    local result = {
        radius = radius,
        player_position = {x = player.position.x, y = player.position.y},
        coordinate_hint = "positive x is east; positive y is south",
        terrain_scope = "generated terrain only",
        water_tile_prototypes = water_names
    }
    if #water_names == 0 then
        result.found = false
        result.reason = "No tile prototype produces water for an offshore pump"
        return result
    end

    local function contains_water(search_radius)
        return player.surface.count_tiles_filtered {
            position = player.position,
            radius = search_radius,
            name = water_names,
            limit = 1
        } > 0
    end

    local lower = 0
    local upper = math.min(32, radius)
    while upper < radius and not contains_water(upper) do
        lower = upper
        upper = math.min(radius, upper * 2)
    end
    if not contains_water(upper) then
        result.found = false
        result.reason = "No water was found in generated terrain within the requested radius; " ..
            "explore or search a larger radius"
        return result
    end
    for _ = 1, 14 do
        if upper - lower <= 0.25 then break end
        local middle = (lower + upper) / 2
        if contains_water(middle) then upper = middle else lower = middle end
    end

    local tiles = player.surface.find_tiles_filtered {
        position = player.position,
        radius = math.min(radius, upper + 2),
        name = water_names,
        limit = 256
    }
    table.sort(tiles, function(left, right)
        local ldx = left.position.x + 0.5 - player.position.x
        local ldy = left.position.y + 0.5 - player.position.y
        local rdx = right.position.x + 0.5 - player.position.x
        local rdy = right.position.y + 0.5 - player.position.y
        return ldx * ldx + ldy * ldy < rdx * rdx + rdy * rdy
    end)
    local offsets = {
        {x = 0, y = -1, direction = "north"},
        {x = 1, y = 0, direction = "east"},
        {x = 0, y = 1, direction = "south"},
        {x = -1, y = 0, direction = "west"}
    }
    local water_from_shore = {
        north = "south",
        east = "west",
        south = "north",
        west = "east"
    }
    local pump_directions = {
        {name = "north", value = defines.direction.north},
        {name = "east", value = defines.direction.east},
        {name = "south", value = defines.direction.south},
        {name = "west", value = defines.direction.west}
    }
    local shorelines = {}
    local seen = {}
    for _, tile in ipairs(tiles) do
        if #shorelines >= 16 then break end
        local water = {x = tile.position.x + 0.5, y = tile.position.y + 0.5}
        for _, offset in ipairs(offsets) do
            local land = {x = water.x + offset.x, y = water.y + offset.y}
            local land_tile = player.surface.get_tile(land)
            if not land_tile.prototype.fluid and not land_tile.collides_with("player") then
                local key = land.x .. ":" .. land.y
                if not seen[key] then
                    seen[key] = true
                    local walk = player.surface.find_non_colliding_position(player.name, land, 0.49, 0.1)
                    if walk then
                        local dx = walk.x - player.position.x
                        local dy = walk.y - player.position.y
                        local shoreline = {
                            water_tile = tile.name,
                            fluid = tile.prototype.fluid.name,
                            water_position = water,
                            shore_position = {x = land.x, y = land.y},
                            walk_position = {x = walk.x, y = walk.y},
                            water_from_shore = water_from_shore[offset.direction],
                            distance = math.sqrt(dx * dx + dy * dy)
                        }
                        if prototypes.entity["offshore-pump"] then
                            for _, pump_direction in ipairs(pump_directions) do
                                if player.surface.can_place_entity {
                                    name = "offshore-pump",
                                    position = land,
                                    direction = pump_direction.value,
                                    force = player.force,
                                    build_check_type = defines.build_check_type.manual
                                } then
                                    shoreline.offshore_pump_placement = {
                                        item = "offshore-pump",
                                        x = land.x,
                                        y = land.y,
                                        direction = pump_direction.name
                                    }
                                    break
                                end
                            end
                        end
                        shorelines[#shorelines + 1] = shoreline
                    end
                end
            end
        end
    end
    table.sort(shorelines, function(left, right) return left.distance < right.distance end)
    result.found = #tiles > 0
    result.nearest_water_distance = upper
    result.shorelines = shorelines
    if #shorelines == 0 then
        result.reason = "Water was found, but no adjacent walkable shoreline was found in the nearest sample"
    end
    return result
end

function gameplay.take_screenshot(player, arguments)
    game.take_screenshot {
        surface = player.surface,
        position = player.position,
        resolution = {x = arguments.width, y = arguments.height},
        zoom = arguments.zoom,
        path = arguments.path,
        show_gui = false,
        show_entity_info = true,
        force_render = true
    }
    return {requested = true}
end

function gameplay.stop_walking(player)
    player.walking_state = {walking = false, direction = defines.direction.north}
    return {
        stopped = true,
        position = {x = player.position.x, y = player.position.y}
    }
end

function gameplay.place_entity(player, arguments, use_dedicated_character)
    local direction = directions[arguments.direction]
    if not direction then
        error("direction must be north, east, south, or west")
    end
    local inventory = player.get_main_inventory()
    if not inventory then
        error("Controlled character has no main inventory")
    end
    local source = inventory.find_item_stack(arguments.item)
    if not source then
        error("Player does not have item: " .. tostring(arguments.item))
    end
    local place_result = source.prototype.place_result
    if not place_result then
        error("Item cannot be placed as an entity: " .. tostring(arguments.item))
    end

    local requested = {x = arguments.x, y = arguments.y}
    local target_valid = player.surface.can_place_entity {
        name = place_result.name,
        position = requested,
        direction = direction,
        force = player.force,
        build_check_type = defines.build_check_type.manual
    }
    local accepted = player.can_place_entity {
        name = place_result.name,
        position = requested,
        direction = direction
    }
    local entity = nil
    if accepted then
        local quality = source.quality.name
        local removed = inventory.remove {name = arguments.item, count = 1, quality = quality}
        if removed == 1 then
            entity = player.surface.create_entity {
                name = place_result.name,
                position = requested,
                direction = direction,
                force = player.force,
                quality = quality,
                raise_built = true
            }
            if not entity then
                inventory.insert {name = arguments.item, count = 1, quality = quality}
            end
        end
    end

    local result = {
        placed = entity ~= nil,
        build_accepted = accepted,
        target_valid = target_valid,
        item = arguments.item,
        requested_position = requested,
        requested_direction = arguments.direction,
        player_position = {x = player.position.x, y = player.position.y},
        build_distance = player.build_distance,
        remaining_item_count = inventory.get_item_count(arguments.item)
    }
    if entity then
        result.entity = entity_info(entity)
        local updates = remote.call(
            "factoria_bridge",
            "record_research_trigger_action",
            use_dedicated_character,
            "build-entity",
            entity.name,
            entity.quality.name,
            1)
        result.research_trigger_tracking = {available = true, updates = updates}
    elseif not accepted and target_valid then
        result.reason = "out_of_build_reach"
        result.suggested_tool = "walk_to_for_placement"
    elseif not accepted then
        result.reason = "invalid_build_target_or_direction"
    else
        result.reason = "Factorio accepted the build but the placed entity could not be verified"
    end
    return result
end

function gameplay.set_assembler_recipe(player, arguments)
    local target = find_entity(
        player,
        arguments.x,
        arguments.y,
        function(candidate) return candidate.type == "assembling-machine" end,
        "assembling machine",
        true)
    local recipe = player.force.recipes[arguments.recipe]
    if not recipe then error("Unknown recipe: " .. tostring(arguments.recipe)) end
    if not recipe.enabled then error("Recipe is not enabled: " .. tostring(arguments.recipe)) end

    local previous = target.get_recipe()
    local removed = target.set_recipe(arguments.recipe)
    local destination = player.get_main_inventory()
    local returned = {}
    local spilled = {}
    for _, item in ipairs(removed) do
        local inserted = destination and destination.insert(item) or 0
        if inserted > 0 then
            returned[#returned + 1] = {name = item.name, quality = item.quality, count = inserted}
        end
        local remainder = item.count - inserted
        if remainder > 0 then
            target.surface.spill_item_stack {
                position = target.position,
                stack = {name = item.name, quality = item.quality, count = remainder},
                enable_looted = true,
                force = player.force
            }
            spilled[#spilled + 1] = {name = item.name, quality = item.quality, count = remainder}
        end
    end
    local selected = target.get_recipe()
    return {
        entity = entity_info(target),
        previous_recipe = previous and previous.name or nil,
        recipe = selected and selected.name or nil,
        recipe_set = selected ~= nil and selected.name == arguments.recipe,
        returned_to_player = returned,
        spilled_beside_machine = spilled
    }
end

function gameplay.insert_item(player, arguments)
    local target, destination = find_inventory_entity(player, arguments, true)
    local item_prototype = prototypes.item[arguments.item]
    if not item_prototype then
        error("Unknown item prototype: " .. tostring(arguments.item))
    end
    local source = player.get_main_inventory()
    if not source then error("Controlled character has no main inventory") end
    local available = source.get_item_count(arguments.item)
    if available == 0 then error("Player does not have item: " .. tostring(arguments.item)) end
    local insertable = destination.get_insertable_count(arguments.item)
    local requested = math.min(arguments.count, available, insertable)
    local removed = requested > 0 and source.remove {name = arguments.item, count = requested} or 0
    local inserted = removed > 0 and destination.insert {name = arguments.item, count = removed} or 0
    if inserted < removed then source.insert {name = arguments.item, count = removed - inserted} end
    local result = {
        entity = entity_info(target),
        inventory = arguments.inventory,
        item = arguments.item,
        requested_count = arguments.count,
        inserted_count = inserted,
        accepted = inserted > 0,
        available_before = available,
        insertable_before = insertable,
        player_remaining = source.get_item_count(arguments.item),
        machine_item_count = destination.get_item_count(arguments.item),
        complete = inserted == arguments.count
    }
    if inserted == 0 and arguments.inventory == "fuel" and item_prototype.fuel_value == 0 then
        result.reason = "item_is_not_fuel"
        result.required = "a fuel item accepted by this burner, such as coal"
    elseif inserted == 0 and insertable == 0 then
        result.reason = "item_not_accepted_by_target_inventory"
    elseif inserted < arguments.count and available < arguments.count then
        result.reason = "insufficient_player_items"
    elseif inserted < arguments.count then
        result.reason = "target_inventory_capacity_limited"
    end
    if arguments.inventory == "fuel" then
        result.item_fuel_value = item_prototype.fuel_value
        result.item_fuel_category = item_prototype.fuel_category
    end
    return result
end

function gameplay.take_item(player, arguments)
    local target, source = find_inventory_entity(player, arguments, true)
    if not prototypes.item[arguments.item] then
        error("Unknown item prototype: " .. tostring(arguments.item))
    end
    local destination = player.get_main_inventory()
    if not destination then error("Controlled character has no main inventory") end
    local available = source.get_item_count(arguments.item)
    local requested = math.min(arguments.count, available, destination.get_insertable_count(arguments.item))
    local removed = requested > 0 and source.remove {name = arguments.item, count = requested} or 0
    local inserted = removed > 0 and destination.insert {name = arguments.item, count = removed} or 0
    if inserted < removed then source.insert {name = arguments.item, count = removed - inserted} end
    return {
        entity = entity_info(target),
        inventory = arguments.inventory,
        item = arguments.item,
        requested_count = arguments.count,
        taken_count = inserted,
        available_before = available,
        machine_remaining = source.get_item_count(arguments.item),
        complete = inserted == arguments.count
    }
end

function gameplay.machine_output(player, arguments)
    local target = find_entity(
        player,
        arguments.x,
        arguments.y,
        function(candidate)
            return candidate.type == "furnace" or candidate.type == "assembling-machine"
        end,
        "furnace or crafting machine",
        false)
    if not prototypes.item[arguments.item] then
        error("Unknown item prototype: " .. tostring(arguments.item))
    end
    local output = target.get_inventory(defines.inventory.crafter_output)
    if not output then error("Target machine has no output inventory") end
    local input = target.get_inventory(defines.inventory.crafter_input)
    local fuel = target.get_inventory(defines.inventory.fuel)
    return {
        entity = entity_info(target),
        crafting = target.is_crafting(),
        output_count = output.get_item_count(arguments.item),
        input_empty = input == nil or input.is_empty(),
        output_full = output.is_full(),
        fuel_empty = fuel and fuel.is_empty() or nil
    }
end

function gameplay.transfer_to_container(player, arguments)
    local source = player.get_main_inventory()
    if not source then error("Controlled character has no main inventory") end
    local position = {x = arguments.x, y = arguments.y}
    local found = player.surface.find_entities_filtered {
        position = position,
        radius = 1,
        type = {"container", "logistic-container"}
    }
    table.sort(found, function(left, right)
        local ldx = left.position.x - position.x
        local ldy = left.position.y - position.y
        local rdx = right.position.x - position.x
        local rdy = right.position.y - position.y
        return ldx * ldx + ldy * ldy < rdx * rdx + rdy * rdy
    end)
    local target = found[1]
    if not target then error("No container found at the requested position") end
    if not player.can_reach_entity(target) then error("Container is out of reach") end
    local destination = target.get_inventory(defines.inventory.chest)
    if not destination then error("Target entity has no chest inventory") end
    local destination_limit = #destination
    if destination.supports_bar() then
        destination_limit = math.min(destination_limit, destination.get_bar() - 1)
    end

    local moved_by_item = {}
    local total = 0
    for source_index = 1, #source do
        local stack = source[source_index]
        if stack.valid_for_read then
            local item_name = stack.name
            local quality = tostring(stack.quality)
            local before = stack.count
            for destination_index = 1, destination_limit do
                destination[destination_index].transfer_stack(stack)
                if not stack.valid_for_read then break end
            end
            local after = stack.valid_for_read and stack.count or 0
            local transferred = before - after
            if transferred > 0 then
                local key = item_name .. ":" .. quality
                local moved = moved_by_item[key] or {name = item_name, count = 0, quality = quality}
                moved.count = moved.count + transferred
                moved_by_item[key] = moved
                total = total + transferred
            end
        end
    end
    local moved = {}
    for _, item in pairs(moved_by_item) do moved[#moved + 1] = item end
    table.sort(moved, function(left, right) return left.name < right.name end)
    local remaining = {}
    for _, item in pairs(source.get_contents()) do
        remaining[#remaining + 1] = {
            name = item.name,
            count = item.count,
            quality = tostring(item.quality)
        }
    end
    table.sort(remaining, function(left, right) return left.name < right.name end)
    return {
        container = entity_info(target),
        moved = moved,
        moved_total = total,
        complete = source.is_empty(),
        remaining = remaining
    }
end

return gameplay
