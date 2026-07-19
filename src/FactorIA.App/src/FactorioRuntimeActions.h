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
    elseif kind == "mine" then
        local position = arguments.target
        local requested_count = arguments.count
        if type(requested_count) ~= "number" or requested_count < 1 or requested_count % 1 ~= 0 then
            error("Mining count must be a positive integer")
        end
        player.update_selected_entity(position)
        local entity = player.selected
        if not entity then
            stop_mining(player)
            return nil, {mined = false, missing = true, entity_present = false}
        end
        if not entity.minable then
            stop_mining(player)
            return nil, {
                mined = false,
                not_minable = true,
                entity_present = true,
                entity = {name = entity.name, position = entity.position}
            }
        end
        -- Coordinates identify one discrete entity; batch counts only apply to resource amounts.
        local effective_count = entity.type == "resource" and requested_count or 1
        if not player.can_reach_entity(entity) then
            stop_mining(player)
            return nil, {
                mined = false,
                out_of_reach = true,
                entity_present = true,
                entity = {name = entity.name, position = entity.position}
            }
        end

        player.mining_state = {mining = true, position = position}
        return {
            kind = kind,
            player = player,
            entity = entity,
            entity_name = entity.name,
            entity_position = {x = entity.position.x, y = entity.position.y},
            initial_amount = entity.type == "resource" and entity.amount or nil,
            requested_count = requested_count,
            effective_count = effective_count,
            position = position,
            deadline_tick = game.tick + math.max(1, math.ceil(arguments.maximum_duration_seconds * 60))
        }
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

local function mined_count(state)
    if state.initial_amount then
        local remaining = state.entity.valid and state.entity.amount or 0
        return math.max(0, state.initial_amount - remaining)
    end
    return state.entity.valid and 0 or 1
end

local function with_mining_counts(state, result, count)
    result.mined_count = count or mined_count(state)
    result.requested_count = state.requested_count
    result.effective_count = state.effective_count
    result.count_adjusted = state.requested_count ~= state.effective_count
    return result
end

local function tick_mining(state)
    local count = mined_count(state)
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
    elseif not state.entity.valid then
        stop_mining(state.player)
        return true, with_mining_counts(state, {
            mined = count > 0,
            target_met = false,
            entity_present = false,
            depleted = state.initial_amount ~= nil,
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
        stop_mining(state.player)
        return true, with_mining_counts(state, {
            mined = count > 0,
            target_met = false,
            timed_out = true,
            entity_present = true,
            progress = state.player.character_mining_progress,
            entity = {name = state.entity_name, position = state.entity_position}
        }, count)
    end

    state.player.selected = state.entity
    state.player.mining_state = {mining = true, position = state.position}
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
        stop_mining(state.player)
        local count = mined_count(state)
        return with_mining_counts(state, {
            mined = count > 0,
            target_met = count >= state.effective_count,
            entity_present = state.entity.valid,
            position = state.player.valid and position_of(state.player) or nil
        }, count)
    end

    stop_walking(state.player)
    return {reached = false, position = state.player.valid and position_of(state.player) or nil}
end

return action
)lua";
}
