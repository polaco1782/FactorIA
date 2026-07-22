local action = {}
local construction = require("construction")
local placement = require("placement")

local directions = {
    north = defines.direction.north,
    northeast = defines.direction.northeast,
    east = defines.direction.east,
    southeast = defines.direction.southeast,
    south = defines.direction.south,
    southwest = defines.direction.southwest,
    west = defines.direction.west,
    northwest = defines.direction.northwest
}

local function control_character(use_dedicated_character)
    local interface = remote.interfaces.factoria_bridge
    if not interface or not interface.get_control_character then
        error("The updated FactorIA Bridge mod is required to select a control character")
    end
    return remote.call("factoria_bridge", "get_control_character", use_dedicated_character == true)
end

local function stop_walking(player)
    if player and player.valid then
        player.walking_state = {walking = false, direction = defines.direction.north}
    end
end

local function stop_mining(player)
    if player and player.valid then
        player.mining_state = {mining = false}
    end
end

local function position_of(player)
    return {x = player.position.x, y = player.position.y}
end

local function inventory_contents(player)
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

local function distance_to(player, target)
    local dx = target.x - player.position.x
    local dy = target.y - player.position.y
    return math.sqrt(dx * dx + dy * dy), dx, dy
end

local function squared_distance(first, second)
    local dx = first.x - second.x
    local dy = first.y - second.y
    return dx * dx + dy * dy
end

-- Prefer a reachable minable entity, while keeping selection deterministic when several
-- resource tiles share the same prototype name.
local function find_mining_target(player, name)
    local entities = player.surface.find_entities_filtered {
        position = player.position,
        radius = 10,
        name = name
    }
    local best = nil
    local best_reachable = false
    local best_distance = math.huge
    for _, entity in ipairs(entities) do
        if entity.valid and entity.minable then
            local reachable = player.can_reach_entity(entity)
            local distance = squared_distance(player.position, entity.position)
            local earlier_position = best and distance == best_distance and
                (entity.position.x < best.position.x or
                    (entity.position.x == best.position.x and entity.position.y < best.position.y))
            if not best or (reachable and not best_reachable) or
                (reachable == best_reachable and (distance < best_distance or earlier_position)) then
                best = entity
                best_reachable = reachable
                best_distance = distance
            end
        end
    end
    return best, best_reachable
end

local function find_marked_mining_target(player, name, position)
    -- Match neutral trees and rocks too; the per-force marker check below remains authoritative.
    local entities = player.surface.find_entities_filtered {
        position = position,
        radius = 0.25,
        name = name,
        to_be_deconstructed = true
    }
    for _, entity in ipairs(entities) do
        if entity.valid and entity.minable and entity.to_be_deconstructed(player.force) then
            return entity, player.can_reach_entity(entity)
        end
    end
    return nil, false
end

local function begin_mining_target(state, entity)
    state.entity = entity
    state.entity_name = entity.name
    state.entity_position = {x = entity.position.x, y = entity.position.y}
    state.initial_amount = entity.type == "resource" and entity.amount or nil
    state.entity_mined_count = 0
    state.discrete_entity_counted = false

    -- Use the resolved entity position instead of a model-provided point between tiles.
    state.player.update_selected_entity(state.entity_position)
    state.player.selected = entity
    state.player.mining_state = {mining = true, position = state.entity_position}
end

local function walking_direction(dx, dy)
    local absolute_x = math.abs(dx)
    local absolute_y = math.abs(dy)
    if absolute_x < absolute_y * 0.4 then
        return dy < 0 and defines.direction.north or defines.direction.south
    elseif absolute_y < absolute_x * 0.4 then
        return dx < 0 and defines.direction.west or defines.direction.east
    elseif dx >= 0 and dy < 0 then
        return defines.direction.northeast
    elseif dx >= 0 and dy >= 0 then
        return defines.direction.southeast
    elseif dx < 0 and dy >= 0 then
        return defines.direction.southwest
    end
    return defines.direction.northwest
end

local function completed_walk_result(state, timed_out, blocked)
    stop_walking(state.player)
    local distance = distance_to(state.player, state.target)
    return {
        reached = distance <= state.stopping_distance,
        timed_out = timed_out or false,
        blocked = blocked or false,
        distance = distance,
        position = position_of(state.player),
        waypoint_count = #state.waypoints
    }
end

local function request_path(player, target, stopping_distance)
    return player.surface.request_path {
        bounding_box = player.prototype.collision_box,
        collision_mask = player.prototype.collision_mask,
        start = player.position,
        goal = target,
        force = player.force,
        radius = math.max(0.1, stopping_distance - 0.4),
        pathfind_flags = {
            allow_destroy_friendly_entities = false,
            allow_paths_through_own_entities = false,
            cache = false
        },
        can_open_gates = true,
        entity_to_ignore = player
    }
end

local function begin_path(state, target, stopping_distance)
    state.target = {x = target.x, y = target.y}
    state.stopping_distance = stopping_distance
    state.path_request_id = request_path(state.player, state.target, stopping_distance)
    state.phase = "waiting_path"
end

local function begin_walk_from_path(state, path_result)
    if not path_result.found then
        return false
    end
    local waypoints = {}
    for _, waypoint in ipairs(path_result.path or {}) do
        waypoints[#waypoints + 1] = {
            x = waypoint.position.x,
            y = waypoint.position.y
        }
    end
    if #waypoints == 0 then
        waypoints[1] = {x = state.target.x, y = state.target.y}
    end
    state.walk = {
        player = state.player,
        waypoints = waypoints,
        waypoint_index = 1,
        target = state.target,
        stopping_distance = state.stopping_distance,
        deadline_tick = state.deadline_tick,
        last_progress_tick = game.tick,
        best_distance = math.huge
    }
    state.phase = "walking"
    return true
end

local function construction_kind_for_task(task_kind)
    if task_kind == "build-ghosts" then
        return "build"
    elseif task_kind == "remove-markers" then
        return "deconstruct"
    end
    return nil
end

local function construction_request_label(kind)
    return kind == "build" and "entity ghosts" or "deconstruction-marked entities"
end

local function remaining_task_requests(player, radius, kind)
    return construction.get_requests(player, {
        radius = radius,
        kind = kind,
        offset = 0,
        available_only = false
    })
end

local function pending_task_requests_result(kind, request_count)
    local label = construction_request_label(kind)
    return {
        task_terminal = false,
        task_completed = false,
        returned_to_issuer = false,
        reason = kind == "build" and "construction_requests_remain" or
            "deconstruction_requests_remain",
        remaining_requests = request_count,
        instruction = "Service the remaining " .. label .. " before returning to the task issuer."
    }
end

local function completed_task_message(kind)
    return "Nearby " .. construction_request_label(kind) ..
        " are clear and the agent returned to the issuing player."
end

function action.start(arguments)
    local player = control_character(arguments.use_dedicated_character)
    local kind = arguments.kind

    if kind == "walk_direction" then
        local direction = directions[arguments.direction]
        if not direction then
            error("Invalid walking direction")
        end
        return {
            kind = kind,
            player = player,
            direction = direction,
            deadline_tick = game.tick + math.max(1, math.ceil(arguments.duration_seconds * 60))
        }
    elseif kind == "walk_path" then
        if type(arguments.waypoints) ~= "table" or #arguments.waypoints == 0 then
            error("Walking requires at least one waypoint")
        end
        local state = {
            kind = kind,
            player = player,
            waypoints = arguments.waypoints,
            waypoint_index = 1,
            target = arguments.target,
            stopping_distance = arguments.stopping_distance,
            deadline_tick = game.tick + math.max(1, math.ceil(arguments.maximum_duration_seconds * 60)),
            last_progress_tick = game.tick,
            best_distance = math.huge
        }
        return state
    elseif kind == "walk_to" then
        local target = arguments.target
        if type(target) ~= "table" or type(target.x) ~= "number" or type(target.y) ~= "number" then
            error("walk_to requires an exact target position")
        end
        local state = {
            kind = kind,
            player = player,
            deadline_tick = game.tick + math.max(1, math.ceil(arguments.maximum_duration_seconds * 60))
        }
        begin_path(state, target, arguments.stopping_distance)
        return state
    elseif kind == "build_ghosts" then
        local state = {
            kind = kind,
            player = player,
            use_dedicated_character = arguments.use_dedicated_character == true,
            radius = arguments.radius,
            requested_count = arguments.count,
            completed_count = 0,
            attempts = {},
            phase = "scan",
            deadline_tick = game.tick + math.max(1, math.ceil(arguments.maximum_duration_seconds * 60)),
            maximum_duration_seconds = arguments.maximum_duration_seconds
        }
        return state
    elseif kind == "deconstruct_marked" then
        return {
            kind = kind,
            player = player,
            use_dedicated_character = arguments.use_dedicated_character == true,
            radius = arguments.radius,
            requested_count = arguments.count,
            completed_count = 0,
            attempts = {},
            phase = "scan",
            deadline_tick = game.tick + math.max(1, math.ceil(arguments.maximum_duration_seconds * 60)),
            maximum_duration_seconds = arguments.maximum_duration_seconds
        }
    elseif kind == "walk_to_for_placement" then
        local request = {
            item = arguments.item,
            x = arguments.x,
            y = arguments.y,
            direction = arguments.direction
        }
        local plan = placement.plan_walk(player, request)
        plan.next_tool = {name = "place_entity", arguments = request}
        local candidates = plan.candidates
        plan.candidates = nil
        if plan.placement_ready then
            plan.reached_build_position = true
            plan.reason = "already_ready_for_place_entity"
            return nil, plan
        elseif #candidates == 0 then
            plan.reached_build_position = false
            plan.reason = "no_collision_free_build_position"
            return nil, plan
        end
        local state = {
            kind = kind,
            player = player,
            request = request,
            plan = plan,
            candidates = candidates,
            candidate_index = 1,
            deadline_tick = game.tick + math.max(1, math.ceil(arguments.maximum_duration_seconds * 60))
        }
        begin_path(state, candidates[1], 0.5)
        return state
    elseif kind == "return_to_task_issuer" then
        local target = remote.call("factoria_bridge", "agent_task_return_target", arguments.task_id)
        if not target.available then
            return nil, {
                task_terminal = true,
                task_completed = false,
                returned_to_issuer = false,
                reason = target.reason or "issuer_unavailable",
                message = "The in-game task ended because its issuing player is unavailable.",
                target = target
            }
        end

        local request_kind = construction_kind_for_task(target.task_kind)
        if not request_kind then
            return nil, {
                task_terminal = true,
                task_completed = false,
                returned_to_issuer = false,
                reason = "unsupported_task_kind",
                message = "The in-game task has an unsupported task kind.",
                target = target
            }
        end
        local requests = remaining_task_requests(player, arguments.radius, request_kind)
        if requests.total_requests_before_filter > 0 then
            return nil, pending_task_requests_result(
                request_kind,
                requests.total_requests_before_filter)
        end
        if not target.same_surface then
            return nil, {
                task_terminal = true,
                task_completed = false,
                returned_to_issuer = false,
                reason = "issuer_on_another_surface",
                message = "The task requests are clear, but the issuing player is on another surface.",
                target = target
            }
        elseif target.distance_to_agent <= 4 then
            return nil, {
                task_terminal = true,
                task_completed = true,
                returned_to_issuer = true,
                reason = "returned_to_issuer",
                message = completed_task_message(request_kind),
                target = target
            }
        end

        local state = {
            kind = kind,
            player = player,
            task_id = arguments.task_id,
            radius = arguments.radius,
            request_kind = request_kind,
            deadline_tick = game.tick + math.max(1, math.ceil(arguments.maximum_duration_seconds * 60))
        }
        begin_path(state, target.position, 3)
        return state
    elseif kind == "craft" then
        local recipe = player.force.recipes[arguments.recipe]
        if not recipe then error("Unknown recipe: " .. tostring(arguments.recipe)) end
        if not recipe.enabled then error("Recipe is not enabled: " .. tostring(arguments.recipe)) end
        local inventory = player.get_main_inventory()
        if not inventory then error("Controlled character has no main inventory") end
        local queue_before = player.crafting_queue_size
        if queue_before > 0 then
            return nil, {
                requested = arguments.count,
                queued = 0,
                completed = false,
                crafting_queue_size = queue_before,
                reason = "crafting_queue_not_empty"
            }
        end

        local products = {}
        local seen = {}
        for _, product in ipairs(recipe.products) do
            if product.type == "item" and not seen[product.name] then
                seen[product.name] = true
                products[#products + 1] = {
                    name = product.name,
                    initial_count = inventory.get_item_count(product.name)
                }
            end
        end
        local queued = player.begin_crafting {
            count = arguments.count,
            recipe = arguments.recipe,
            silent = true
        }
        if queued == 0 then
            return nil, {
                recipe = arguments.recipe,
                requested = arguments.count,
                queued = 0,
                crafting_queue_size = player.crafting_queue_size,
                recipe_energy_seconds = recipe.energy,
                products = products,
                reason = "recipe_could_not_be_queued"
            }
        end
        return {
            kind = kind,
            player = player,
            use_dedicated_character = arguments.use_dedicated_character == true,
            recipe = arguments.recipe,
            requested = arguments.count,
            queued = queued,
            products = products,
            recipe_energy_seconds = recipe.energy,
            deadline_tick = game.tick + 10 * 60 * 60
        }
    elseif kind == "mine" or kind == "mine_marked" then
        local requested_name = arguments.name
        local requested_count = arguments.count
        if type(requested_name) ~= "string" or #requested_name == 0 then
            error("Mining requires an entity prototype name")
        end
        if type(requested_count) ~= "number" or requested_count < 1 or requested_count % 1 ~= 0 then
            error("Mining count must be a positive integer")
        end
        local deconstruction_request = kind == "mine_marked"
        local entity, reachable
        if deconstruction_request then
            local position = {x = arguments.target_x, y = arguments.target_y}
            if type(position.x) ~= "number" or type(position.y) ~= "number" then
                error("Marked mining requires an exact target position")
            end
            entity, reachable = find_marked_mining_target(player, requested_name, position)
        else
            entity, reachable = find_mining_target(player, requested_name)
        end
        if not entity then
            stop_mining(player)
            return nil, {
                mined = false,
                missing = true,
                entity_present = false,
                requested_name = requested_name,
                deconstruction_request = deconstruction_request
            }
        end
        -- Batch counts apply to resource amounts; a discrete entity is mined only once.
        local effective_count = entity.type == "resource" and requested_count or 1
        if not reachable then
            stop_mining(player)
            return nil, {
                mined = false,
                out_of_reach = true,
                entity_present = true,
                requested_name = requested_name,
                deconstruction_request = deconstruction_request,
                entity = {name = entity.name, position = entity.position}
            }
        end

        local state = {
            kind = "mine",
            player = player,
            use_dedicated_character = arguments.use_dedicated_character == true,
            requested_name = requested_name,
            requested_count = requested_count,
            effective_count = effective_count,
            resource_target = entity.type == "resource",
            deconstruction_request = deconstruction_request,
            mined_count = 0,
            deadline_tick = game.tick + math.max(1, math.ceil(arguments.maximum_duration_seconds * 60))
        }
        begin_mining_target(state, entity)
        return state
    end

    error("Unknown FactorIA player action kind: " .. tostring(kind))
end

local function tick_directional_walk(state)
    if not state.player.valid then
        return true, {reached = false, player_unavailable = true}
    end
    if game.tick >= state.deadline_tick then
        stop_walking(state.player)
        return true, {reached = true, position = position_of(state.player)}
    end
    state.player.walking_state = {walking = true, direction = state.direction}
    return false
end

local function tick_path_walk(state)
    if not state.player.valid then
        return true, {reached = false, player_unavailable = true}
    end
    if game.tick >= state.deadline_tick then
        return true, completed_walk_result(state, true, false)
    end

    local waypoint = state.waypoints[state.waypoint_index]
    local distance, dx, dy = distance_to(state.player, waypoint)
    local final_waypoint = state.waypoint_index == #state.waypoints
    local waypoint_distance = final_waypoint and 0.35 or 0.65
    if distance <= waypoint_distance then
        state.waypoint_index = state.waypoint_index + 1
        state.best_distance = math.huge
        state.last_progress_tick = game.tick
        if state.waypoint_index > #state.waypoints then
            return true, completed_walk_result(state, false, false)
        end
        waypoint = state.waypoints[state.waypoint_index]
        distance, dx, dy = distance_to(state.player, waypoint)
    end

    if distance < state.best_distance - 0.15 then
        state.best_distance = distance
        state.last_progress_tick = game.tick
    elseif game.tick - state.last_progress_tick >= 120 then
        local result = completed_walk_result(state, false, true)
        result.distance_to_waypoint = distance
        return true, result
    end

    state.player.walking_state = {walking = true, direction = walking_direction(dx, dy)}
    return false
end

local function tick_navigation(state)
    if state.phase == "waiting_path" then
        local path_result = storage.path_results[state.path_request_id]
        if not path_result then
            if game.tick >= state.deadline_tick then
                stop_walking(state.player)
                return true, {
                    reached = false,
                    pathfinder_timed_out = true,
                    navigation = "factorio_pathfinder"
                }
            end
            return false
        end
        storage.path_results[state.path_request_id] = nil
        if not begin_walk_from_path(state, path_result) then
            stop_walking(state.player)
            return true, {
                reached = false,
                unreachable = not path_result.try_again_later,
                pathfinder_busy = path_result.try_again_later or false,
                navigation = "factorio_pathfinder"
            }
        end
    end

    local done, result = tick_path_walk(state.walk)
    if not done then
        return false
    end
    result.navigation = "factorio_pathfinder"
    result.pathfinder_available = true
    result.walking_control = "factorio_bridge_runtime"
    return true, result
end

local function construction_result(state, reason)
    local latest = {total_requests = 0, available_count = 0, missing_items = {}}
    if state.player.valid then
        latest = construction.get_requests(state.player, {
            radius = state.radius,
            kind = "build",
            offset = 0,
            available_only = false
        })
    end
    return {
        kind = "build",
        requested_count = state.requested_count,
        completed_count = state.completed_count,
        target_met = state.completed_count >= state.requested_count,
        timed_out = reason == "timed_out",
        reason = reason,
        maximum_duration_seconds = state.maximum_duration_seconds,
        attempts = state.attempts,
        remaining_requests = latest.total_requests,
        remaining_available = latest.available_count,
        missing_items = latest.missing_items
    }
end

local function record_build_trigger(state, action_result)
    if not action_result.fulfilled or not action_result.entity then
        return
    end
    local entity = action_result.entity
    local updates = remote.call(
        "factoria_bridge",
        "record_research_trigger_action",
        state.use_dedicated_character,
        "build-entity",
        entity.name,
        entity.quality or "normal",
        1)
    action_result.research_trigger_tracking = {available = true, updates = updates}
end

local function accept_build_attempt(state, request, action_result, navigation)
    record_build_trigger(state, action_result)
    local attempt = {request = request, action = action_result}
    if navigation then
        attempt.navigation = navigation
    end
    state.attempts[#state.attempts + 1] = attempt
    if not action_result.fulfilled then
        return true, construction_result(state, action_result.reason or "request_failed")
    end

    state.completed_count = state.completed_count + 1
    state.current_request = nil
    state.phase = "scan"
    if state.completed_count >= state.requested_count then
        return true, construction_result(state, "requested_count_met")
    end
    return false
end

local function tick_build_ghosts(state)
    if not state.player.valid then
        stop_walking(state.player)
        return true, construction_result(state, "player_unavailable")
    elseif game.tick >= state.deadline_tick then
        stop_walking(state.player)
        return true, construction_result(state, "timed_out")
    end

    if state.phase == "scan" then
        local observation = construction.get_requests(state.player, {
            radius = state.radius,
            kind = "build",
            offset = 0,
            available_only = true
        })
        local request = observation.requests[1]
        if not request then
            local reason = observation.total_requests_before_filter == 0 and
                "no_requests" or "no_available_requests"
            return true, construction_result(state, reason)
        end

        local action_result = construction.build_ghost(state.player, request)
        if action_result.out_of_reach then
            state.current_request = request
            state.initial_action_result = action_result
            begin_path(state, request.position, 2)
            return false
        end
        return accept_build_attempt(state, request, action_result)
    end

    local navigation_done, navigation = tick_navigation(state)
    if not navigation_done then
        return false
    elseif not navigation.reached then
        state.attempts[#state.attempts + 1] = {
            request = state.current_request,
            navigation = navigation,
            action = state.initial_action_result
        }
        return true, construction_result(state, "unreachable_request")
    end

    local request = state.current_request
    local action_result = construction.build_ghost(state.player, request)
    return accept_build_attempt(state, request, action_result, navigation)
end

local function tick_return_to_task_issuer(state)
    local navigation_done, navigation = tick_navigation(state)
    if not navigation_done then
        return false
    end
    if not navigation.reached then
        return true, {
            task_terminal = false,
            task_completed = false,
            returned_to_issuer = false,
            reason = "issuer_not_reached",
            instruction = "Invoke return_to_task_issuer again after reviewing the navigation result.",
            navigation = navigation
        }
    end

    local target = remote.call("factoria_bridge", "agent_task_return_target", state.task_id)
    if not target.available or not target.same_surface then
        return true, {
            task_terminal = true,
            task_completed = false,
            returned_to_issuer = false,
            reason = target.reason or "issuer_became_unavailable",
            message = "The issuing player became unavailable while the agent was returning.",
            target = target,
            navigation = navigation
        }
    end
    local requests = remaining_task_requests(state.player, state.radius, state.request_kind)
    if requests.total_requests_before_filter > 0 then
        local result = pending_task_requests_result(
            state.request_kind,
            requests.total_requests_before_filter)
        result.navigation = navigation
        return true, result
    end
    local returned = target.distance_to_agent <= 4
    return true, {
        task_terminal = returned,
        task_completed = returned,
        returned_to_issuer = returned,
        reason = returned and "returned_to_issuer" or "issuer_moved_during_return",
        message = returned and completed_task_message(state.request_kind) or nil,
        instruction = not returned and "The issuing player moved; invoke return_to_task_issuer again." or nil,
        target = target,
        navigation = navigation
    }
end

local function tick_walk_to_for_placement(state)
    if not state.player.valid then
        state.plan.reached_build_position = false
        state.plan.reason = "player_unavailable"
        return true, state.plan
    end
    local navigation_done, navigation = tick_navigation(state)
    if not navigation_done then return false end
    state.plan.movement = navigation
    local candidate = state.candidates[state.candidate_index]
    if navigation.reached then
        local verification = placement.verify_walk(state.player, state.request)
        state.plan.current_position = verification.current_position
        state.plan.build_distance = verification.build_distance
        state.plan.target_valid = verification.target_valid
        state.plan.placement_ready = verification.placement_ready
        state.plan.approach_position = {x = candidate.x, y = candidate.y}
        state.plan.reached_build_position = verification.placement_ready
        if verification.placement_ready then
            state.plan.reason = "ready_for_place_entity"
            return true, state.plan
        elseif not verification.target_valid then
            state.plan.reason = "invalid_build_target_or_direction"
            state.plan.valid_alternatives = verification.valid_alternatives
            state.plan.nearby_entities = verification.nearby_entities
            state.plan.target_tile = verification.target_tile
            return true, state.plan
        end
    end

    state.candidate_index = state.candidate_index + 1
    candidate = state.candidates[state.candidate_index]
    if candidate and game.tick < state.deadline_tick then
        begin_path(state, candidate, 0.5)
        return false
    end
    state.plan.placement_ready = false
    state.plan.reached_build_position = false
    state.plan.reason = "no_pathable_position_within_build_reach"
    return true, state.plan
end

local function update_mined_count(state)
    if state.initial_amount then
        local remaining = state.entity.valid and state.entity.amount or 0
        local entity_count = math.max(0, state.initial_amount - remaining)
        if entity_count > state.entity_mined_count then
            state.mined_count = state.mined_count + entity_count - state.entity_mined_count
            state.entity_mined_count = entity_count
        end
    elseif not state.entity.valid and not state.discrete_entity_counted then
        state.mined_count = state.mined_count + 1
        state.discrete_entity_counted = true
    end
    return state.mined_count
end

local function with_mining_counts(state, result, count)
    result.mined_count = count or update_mined_count(state)
    result.requested_count = state.requested_count
    result.effective_count = state.effective_count
    result.count_adjusted = state.requested_count ~= state.effective_count
    result.requested_name = state.requested_name
    result.deconstruction_request = state.deconstruction_request or false
    return result
end

local function tick_mining(state)
    local count = update_mined_count(state)
    if not state.player.valid then
        stop_mining(state.player)
        return true, with_mining_counts(state, {
            mined = count > 0,
            target_met = count >= state.effective_count,
            player_unavailable = true,
            entity_present = state.entity.valid
        }, count)
    elseif count >= state.effective_count then
        stop_mining(state.player)
        return true, with_mining_counts(state, {
            mined = true,
            target_met = true,
            entity_present = state.entity.valid,
            resource_remaining = state.entity.valid and state.initial_amount and state.entity.amount or nil,
            entity = state.entity.valid and {name = state.entity_name, position = state.entity_position} or nil
        }, count)
    elseif not state.entity.valid and state.resource_target then
        local next_entity, reachable = find_mining_target(state.player, state.requested_name)
        if next_entity and reachable then
            begin_mining_target(state, next_entity)
            return false
        end
        stop_mining(state.player)
        return true, with_mining_counts(state, {
            mined = count > 0,
            target_met = false,
            entity_present = next_entity ~= nil,
            depleted = true,
            missing = next_entity == nil,
            out_of_reach = next_entity ~= nil and not reachable,
            entity = next_entity and {
                name = next_entity.name,
                position = {x = next_entity.position.x, y = next_entity.position.y}
            } or nil
        }, count)
    elseif not state.entity.valid then
        stop_mining(state.player)
        return true, with_mining_counts(state, {
            mined = count > 0,
            target_met = count >= state.effective_count,
            entity_present = false,
            depleted = false,
            missing = false
        }, count)
    elseif not state.player.can_reach_entity(state.entity) then
        stop_mining(state.player)
        return true, with_mining_counts(state, {
            mined = count > 0,
            target_met = false,
            out_of_reach = true,
            entity_present = true,
            entity = {name = state.entity_name, position = state.entity_position}
        }, count)
    elseif game.tick >= state.deadline_tick then
        local progress = state.player.character_mining_progress
        stop_mining(state.player)
        return true, with_mining_counts(state, {
            mined = count > 0,
            target_met = false,
            timed_out = true,
            entity_present = true,
            progress = progress,
            entity = {name = state.entity_name, position = state.entity_position}
        }, count)
    end

    state.player.update_selected_entity(state.entity_position)
    state.player.selected = state.entity
    state.player.mining_state = {mining = true, position = state.entity_position}
    return false
end

local function finish_mining(state, result)
    if result.mined and not state.trigger_recorded then
        state.trigger_recorded = true
        result.inventory = inventory_contents(state.player)
        local updates = remote.call(
            "factoria_bridge",
            "record_research_trigger_action",
            state.use_dedicated_character,
            "mine-entity",
            state.requested_name,
            "normal",
            result.mined_count or 1)
        result.research_trigger_tracking = {available = true, updates = updates}
    end
    return result
end

local function deconstruction_result(state, reason)
    local latest = {total_requests = 0, available_count = 0}
    if state.player.valid then
        latest = construction.get_requests(state.player, {
            radius = state.radius,
            kind = "deconstruct",
            offset = 0,
            available_only = false
        })
    end
    return {
        kind = "deconstruct",
        requested_count = state.requested_count,
        completed_count = state.completed_count,
        target_met = state.completed_count >= state.requested_count,
        timed_out = reason == "timed_out",
        reason = reason,
        maximum_duration_seconds = state.maximum_duration_seconds,
        attempts = state.attempts,
        remaining_requests = latest.total_requests,
        remaining_available = latest.available_count,
        missing_items = {}
    }
end

local function begin_deconstruction_mining(state, request)
    local entity, reachable = find_marked_mining_target(
        state.player,
        request.entity_name,
        request.position)
    if not entity then
        state.attempts[#state.attempts + 1] = {
            request = request,
            action = {mined = false, missing = true, reason = "marked_entity_missing"}
        }
        return true, deconstruction_result(state, "marked_entity_missing")
    elseif not reachable then
        state.current_request = request
        state.initial_action_result = {
            mined = false,
            out_of_reach = true,
            entity_present = true,
            entity = {name = entity.name, position = entity.position}
        }
        begin_path(state, request.position, 2)
        return false
    end

    local mining = {
        kind = "mine",
        player = state.player,
        use_dedicated_character = state.use_dedicated_character,
        requested_name = request.entity_name,
        requested_count = 1,
        effective_count = 1,
        resource_target = false,
        deconstruction_request = true,
        mined_count = 0,
        deadline_tick = state.deadline_tick
    }
    begin_mining_target(mining, entity)
    state.current_request = request
    state.mining = mining
    state.phase = "mining"
    return false
end

local function tick_deconstruct_marked(state)
    if not state.player.valid then
        stop_mining(state.player)
        stop_walking(state.player)
        return true, deconstruction_result(state, "player_unavailable")
    elseif game.tick >= state.deadline_tick then
        stop_mining(state.player)
        stop_walking(state.player)
        return true, deconstruction_result(state, "timed_out")
    end

    if state.phase == "scan" then
        local observation = construction.get_requests(state.player, {
            radius = state.radius,
            kind = "deconstruct",
            offset = 0,
            available_only = true
        })
        local request = observation.requests[1]
        if not request then
            local reason = observation.total_requests_before_filter == 0 and
                "no_requests" or "no_available_requests"
            return true, deconstruction_result(state, reason)
        end
        return begin_deconstruction_mining(state, request)
    elseif state.phase == "mining" then
        local done, action_result = tick_mining(state.mining)
        if not done then return false end
        action_result = finish_mining(state.mining, action_result)
        state.attempts[#state.attempts + 1] = {
            request = state.current_request,
            action = action_result,
            navigation = state.pending_navigation
        }
        if not action_result.mined then
            return true, deconstruction_result(state, action_result.reason or "request_failed")
        end
        state.completed_count = state.completed_count + 1
        state.current_request = nil
        state.mining = nil
        state.pending_navigation = nil
        state.phase = "scan"
        if state.completed_count >= state.requested_count then
            return true, deconstruction_result(state, "requested_count_met")
        end
        return false
    end

    local navigation_done, navigation = tick_navigation(state)
    if not navigation_done then return false end
    if not navigation.reached then
        state.attempts[#state.attempts + 1] = {
            request = state.current_request,
            navigation = navigation,
            action = state.initial_action_result
        }
        return true, deconstruction_result(state, "unreachable_request")
    end
    local request = state.current_request
    state.pending_navigation = navigation
    local done, result = begin_deconstruction_mining(state, request)
    if done then return done, result end
    return false
end

local function craft_result(state, completed, timed_out, stopped)
    local inventory = state.player.get_main_inventory()
    local crafted_items = {}
    local trigger_updates = {}
    if completed and inventory then
        for _, product in ipairs(state.products) do
            local crafted_count = math.max(0, inventory.get_item_count(product.name) - product.initial_count)
            if crafted_count > 0 then
                crafted_items[#crafted_items + 1] = {
                    name = product.name,
                    count = crafted_count,
                    quality = "normal"
                }
                local updates = remote.call(
                    "factoria_bridge",
                    "record_research_trigger_action",
                    state.use_dedicated_character,
                    "craft-item",
                    product.name,
                    "normal",
                    crafted_count)
                trigger_updates[#trigger_updates + 1] = {available = true, updates = updates}
            end
        end
    end
    return {
        recipe = state.recipe,
        requested = state.requested,
        queued = state.queued,
        crafting_queue_size = state.player.crafting_queue_size,
        recipe_energy_seconds = state.recipe_energy_seconds,
        products = state.products,
        completed = completed,
        stopped = stopped or false,
        timed_out = timed_out or false,
        maximum_wait_seconds = 600,
        crafted_items = crafted_items,
        research_trigger_tracking = trigger_updates,
        inventory = inventory_contents(state.player),
        reason = completed and "crafting_completed" or timed_out and "timeout" or "stopped"
    }
end

local function tick_craft(state)
    if not state.player.valid then
        return true, {completed = false, reason = "player_unavailable"}
    elseif state.player.crafting_queue_size == 0 then
        return true, craft_result(state, true, false, false)
    elseif game.tick >= state.deadline_tick then
        return true, craft_result(state, false, true, false)
    end
    return false
end

function action.tick(state)
    if state.kind == "walk_direction" then
        return tick_directional_walk(state)
    elseif state.kind == "walk_path" then
        return tick_path_walk(state)
    elseif state.kind == "walk_to" then
        return tick_navigation(state)
    elseif state.kind == "build_ghosts" then
        return tick_build_ghosts(state)
    elseif state.kind == "deconstruct_marked" then
        return tick_deconstruct_marked(state)
    elseif state.kind == "return_to_task_issuer" then
        return tick_return_to_task_issuer(state)
    elseif state.kind == "walk_to_for_placement" then
        return tick_walk_to_for_placement(state)
    elseif state.kind == "craft" then
        return tick_craft(state)
    elseif state.kind == "mine" then
        local done, result = tick_mining(state)
        return done, done and finish_mining(state, result) or result
    end
    error("Invalid FactorIA player action state")
end

function action.status(state)
    if not state.player.valid then
        return {player_unavailable = true}
    elseif state.kind == "walk_direction" then
        return {position = position_of(state.player)}
    elseif state.kind == "walk_path" then
        local waypoint = state.waypoints[state.waypoint_index] or state.target
        return {
            position = position_of(state.player),
            distance_to_waypoint = distance_to(state.player, waypoint),
            waypoint_index = state.waypoint_index,
            waypoint_count = #state.waypoints
        }
    elseif state.kind == "walk_to" or state.kind == "return_to_task_issuer" or
        state.kind == "walk_to_for_placement" then
        local walk = state.walk
        return {
            phase = state.phase,
            position = position_of(state.player),
            distance_to_waypoint = walk and
                distance_to(state.player, walk.waypoints[walk.waypoint_index] or walk.target) or nil
        }
    elseif state.kind == "build_ghosts" then
        return {
            phase = state.phase,
            completed_count = state.completed_count,
            requested_count = state.requested_count,
            position = position_of(state.player)
        }
    elseif state.kind == "deconstruct_marked" then
        return {
            phase = state.phase,
            completed_count = state.completed_count,
            requested_count = state.requested_count,
            position = position_of(state.player)
        }
    elseif state.kind == "craft" then
        return {
            crafting_queue_size = state.player.crafting_queue_size,
            requested = state.requested,
            queued = state.queued
        }
    elseif state.kind == "mine" then
        return with_mining_counts(state, {
            entity_present = state.entity.valid,
            progress = state.player.character_mining_progress,
            entity = state.entity.valid and {name = state.entity_name, position = state.entity_position} or nil
        })
    end
    return {}
end

function action.stop(state)
    if state.kind == "mine" then
        local progress = state.player.valid and state.player.character_mining_progress or nil
        local count = update_mined_count(state)
        stop_mining(state.player)
        local result = with_mining_counts(state, {
            mined = count > 0,
            target_met = count >= state.effective_count,
            entity_present = state.entity.valid,
            progress = progress,
            position = state.player.valid and position_of(state.player) or nil
        }, count)
        return finish_mining(state, result)
    elseif state.kind == "build_ghosts" then
        stop_walking(state.player)
        local result = construction_result(state, "stopped")
        result.stopped = true
        return result
    elseif state.kind == "deconstruct_marked" then
        stop_walking(state.player)
        stop_mining(state.player)
        local result = deconstruction_result(state, "stopped")
        result.stopped = true
        return result
    elseif state.kind == "return_to_task_issuer" then
        stop_walking(state.player)
        return {
            task_terminal = false,
            task_completed = false,
            returned_to_issuer = false,
            stopped = true,
            reason = "stopped",
            position = state.player.valid and position_of(state.player) or nil
        }
    elseif state.kind == "walk_to_for_placement" then
        stop_walking(state.player)
        state.plan.stopped = true
        state.plan.reached_build_position = false
        state.plan.reason = "stopped"
        return state.plan
    elseif state.kind == "craft" then
        return craft_result(state, state.player.crafting_queue_size == 0, false, true)
    end

    stop_walking(state.player)
    return {reached = false, position = state.player.valid and position_of(state.player) or nil}
end

return action
