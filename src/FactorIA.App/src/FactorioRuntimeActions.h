#pragma once

#include <string_view>

namespace factoria::runtime
{
inline constexpr std::string_view PlayerControlActionName = "player_control";

// This trusted module is uploaded after connecting so new actions do not require a mod release.
inline constexpr std::string_view PlayerControlSource = R"lua(
local action = {}

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
    local entities = player.surface.find_entities_filtered {
        position = position,
        radius = 0.25,
        name = name,
        force = player.force,
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

function action.tick(state)
    if state.kind == "walk_direction" then
        return tick_directional_walk(state)
    elseif state.kind == "walk_path" then
        return tick_path_walk(state)
    elseif state.kind == "mine" then
        return tick_mining(state)
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
        return with_mining_counts(state, {
            mined = count > 0,
            target_met = count >= state.effective_count,
            entity_present = state.entity.valid,
            progress = progress,
            position = state.player.valid and position_of(state.player) or nil
        }, count)
    end

    stop_walking(state.player)
    return {reached = false, position = state.player.valid and position_of(state.player) or nil}
end

return action
)lua";
}
