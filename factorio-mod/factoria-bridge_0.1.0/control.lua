local interface_name = "factoria_bridge"
local runtime_version = 3
local command_protocol_version = 1
local result_lifetime_ticks = 60 * 60
local agent_task_search_radius = 64
local player_control = require("player_control")
local construction = require("construction")
local inspection = require("entity_inspection")
local gameplay = require("gameplay")
local placement = require("placement")

local function ensure_storage()
    storage.path_results = storage.path_results or {}
    storage.research_trigger_progress = storage.research_trigger_progress or {}
    storage.runtime_jobs = storage.runtime_jobs or {}
    storage.runtime_results = storage.runtime_results or {}
    storage.next_runtime_job_id = storage.next_runtime_job_id or 1
    storage.next_agent_task_id = storage.next_agent_task_id or 1
end

local function stored_agent_character()
    ensure_storage()
    local character = storage.agent_character
    if character and character.valid then
        return character
    end

    storage.agent_character = nil
    return nil
end

local function connected_character()
    local player = game.connected_players[1]
    if not player or not player.valid or not player.character or not player.character.valid then
        error("No connected Factorio player with a character")
    end
    return player.character
end

local function ensure_agent_character()
    local existing = stored_agent_character()
    if existing then
        return existing
    end

    local surface = game.get_surface("nauvis") or game.surfaces[1]
    if not surface then
        error("No surface is available for the FactorIA character")
    end

    local force = game.forces.player
    local anchor = force.get_spawn_position(surface)
    local position = nil
    local connected = game.connected_players[1]
    if connected and connected.valid and connected.character and connected.character.valid then
        surface = connected.surface
        anchor = {x = connected.position.x + 3, y = connected.position.y}
        position = surface.find_non_colliding_position("character", anchor, 32, 0.5)
    end

    if not position then
        surface = game.get_surface("nauvis") or game.surfaces[1]
        anchor = force.get_spawn_position(surface)
        position = surface.find_non_colliding_position("character", anchor, 32, 0.5)
    end
    if not position then
        error("No free position is available for the FactorIA character")
    end

    local character = surface.create_entity {
        name = "character",
        position = position,
        force = force
    }
    if not character then
        error("Factorio could not create the FactorIA character")
    end

    character.color = {r = 0.2, g = 0.75, b = 1.0, a = 1.0}
    storage.agent_character = character
    return character
end

local function control_character(use_dedicated_character)
    if use_dedicated_character then
        return ensure_agent_character()
    end
    return connected_character()
end

local function trigger_progress_for(force, technology_name)
    ensure_storage()
    local force_progress = storage.research_trigger_progress[tostring(force.index)] or {}
    return force_progress[technology_name] or 0
end

local function set_trigger_progress(force, technology_name, progress)
    ensure_storage()
    local force_key = tostring(force.index)
    storage.research_trigger_progress[force_key] = storage.research_trigger_progress[force_key] or {}
    storage.research_trigger_progress[force_key][technology_name] = progress
end

local function prerequisites_researched(technology)
    for _, prerequisite in pairs(technology.prerequisites) do
        if not prerequisite.researched then
            return false
        end
    end
    return true
end

local function quality_matches(filter, quality)
    if not filter.quality then
        return true
    end

    local actual = prototypes.quality[quality or "normal"]
    local expected = prototypes.quality[filter.quality]
    if not actual or not expected then
        return false
    end

    local comparator = filter.comparator or "="
    if comparator == "=" then return actual.level == expected.level end
    if comparator == ">" then return actual.level > expected.level end
    if comparator == "<" then return actual.level < expected.level end
    if comparator == "≥" or comparator == ">=" then return actual.level >= expected.level end
    if comparator == "≤" or comparator == "<=" then return actual.level <= expected.level end
    if comparator == "≠" or comparator == "!=" then return actual.level ~= expected.level end
    return false
end

local function id_filter_matches(filter, prototype_name, quality)
    if type(filter) == "string" then
        return filter == prototype_name
    end
    return filter and filter.name == prototype_name and quality_matches(filter, quality)
end

local function trigger_matches(trigger, action_type, prototype_name, quality)
    if not trigger or trigger.type ~= action_type then
        return false
    end
    if action_type == "craft-item" then
        return id_filter_matches(trigger.item, prototype_name, quality)
    end
    if action_type == "build-entity" then
        return id_filter_matches(trigger.entity, prototype_name, quality)
    end
    if action_type == "mine-entity" then
        return trigger.entity == prototype_name
    end
    if action_type == "craft-fluid" then
        return trigger.fluid == prototype_name
    end
    return false
end

local function trigger_required_count(trigger)
    if trigger.type == "craft-item" then
        return trigger.count
    end
    if trigger.type == "craft-fluid" then
        return trigger.amount
    end
    return 1
end

local function record_research_trigger_action(
    use_dedicated_character,
    action_type,
    prototype_name,
    quality,
    count)
    local character = control_character(use_dedicated_character == true)
    local force = character.force
    local matching = {}
    for _, technology in pairs(force.technologies) do
        local trigger = technology.prototype.research_trigger
        if technology.enabled and not technology.researched and prerequisites_researched(technology) and
            trigger_matches(trigger, action_type, prototype_name, quality) then
            matching[#matching + 1] = technology
        end
    end
    table.sort(matching, function(left, right) return left.name < right.name end)

    local updates = {}
    for _, technology in ipairs(matching) do
        local required = trigger_required_count(technology.prototype.research_trigger)
        local progress = math.min(required, trigger_progress_for(force, technology.name) + count)
        set_trigger_progress(force, technology.name, progress)
        local completed = progress >= required
        if completed then
            -- Dedicated characters do not raise player crafting, building, or mining events, so mirror the
            -- native trigger after the typed action has been verified by FactorIA.
            technology.researched = true
        end
        updates[#updates + 1] = {
            technology = technology.name,
            progress = progress,
            required = required,
            completed = completed
        }
    end
    return updates
end

local function stop_job_control(job)
    if player_control.stop then
        pcall(player_control.stop, job.state)
    end
end

local function stop_all_runtime_jobs()
    local job_ids = {}
    ensure_storage()
    for job_id in pairs(storage.runtime_jobs) do
        job_ids[#job_ids + 1] = job_id
    end
    table.sort(job_ids)

    for _, job_id in ipairs(job_ids) do
        stop_job_control(storage.runtime_jobs[job_id])
        storage.runtime_jobs[job_id] = nil
        storage.runtime_results[job_id] = nil
    end
    return #job_ids
end

local function remove_agent_character()
    local stopped_jobs = stop_all_runtime_jobs()
    local character = stored_agent_character()
    if not character then
        return {removed = false, stopped_jobs = stopped_jobs, spilled_item_entities = 0}
    end

    local spilled_item_entities = 0
    for inventory_index = 1, character.get_max_inventory_index() do
        local inventory = character.get_inventory(inventory_index)
        if inventory and inventory.valid and not inventory.is_empty() then
            local spilled = character.surface.spill_inventory {
                position = character.position,
                inventory = inventory,
                enable_looted = true,
                force = character.force,
                allow_belts = false
            }
            spilled_item_entities = spilled_item_entities + #spilled
        end
    end
    local removed = character.destroy()
    if removed then
        storage.agent_character = nil
    end
    return {
        removed = removed,
        stopped_jobs = stopped_jobs,
        spilled_item_entities = spilled_item_entities
    }
end

local function agent_task_snapshot(task)
    if not task then
        return nil
    end
    return {
        id = task.id,
        kind = task.kind,
        status = task.status,
        issuer_player_index = task.issuer_player_index,
        issuer_name = task.issuer_name,
        issuer_position = task.issuer_position,
        issuer_surface = task.issuer_surface,
        search_radius = task.search_radius,
        issued_tick = task.issued_tick,
        started_tick = task.started_tick,
        resumed_tick = task.resumed_tick
    }
end

local function claim_agent_task(task_id)
    ensure_storage()
    local task = storage.agent_task
    if not task or task.id ~= task_id then
        return {claimed = false, reason = "task_missing"}
    end
    if task.status ~= "queued" and task.status ~= "running" then
        return {claimed = false, reason = "task_not_claimable", status = task.status}
    end

    local resumed = task.status == "running"
    task.status = "running"
    task.started_tick = task.started_tick or game.tick
    task.resumed_tick = resumed and game.tick or nil
    return {claimed = true, resumed = resumed, task = agent_task_snapshot(task)}
end

local function finish_agent_task(task_id, succeeded, message)
    ensure_storage()
    local task = storage.agent_task
    if not task or task.id ~= task_id then
        return {finished = false, reason = "task_missing"}
    end

    local clean_message = tostring(message or ""):gsub("[\r\n]+", " "):sub(1, 300)
    local prefix = succeeded and "completed" or "failed"
    local text = string.format("[FactorIA] Task #%d %s", task.id, prefix)
    if clean_message ~= "" then
        text = text .. ": " .. clean_message
    end
    local issuer = game.get_player(task.issuer_player_index)
    local color = succeeded and {r = 0.3, g = 1, b = 0.5} or {r = 1, g = 0.5, b = 0.3}
    if issuer then
        issuer.print(text, {color = color})
    else
        game.print(text, {color = color})
    end
    storage.agent_task = nil
    return {finished = true, succeeded = succeeded}
end

local function print_model_decision(decision)
    local clean_decision = tostring(decision or ""):gsub("[\r\n]+", " "):sub(1, 300)
    if clean_decision == "" then
        return {printed = false, reason = "empty_decision"}
    end

    game.print("[FactorIA] " .. clean_decision, {color = {r = 0.4, g = 0.8, b = 1}})
    return {printed = true}
end

local function agent_task_return_target(task_id)
    ensure_storage()
    local task = storage.agent_task
    if not task or task.id ~= task_id or task.status ~= "running" then
        return {available = false, reason = "task_not_active"}
    end

    local issuer = game.get_player(task.issuer_player_index)
    if not issuer or not issuer.valid or not issuer.character or not issuer.character.valid then
        return {available = false, reason = "issuer_unavailable"}
    end
    local character = stored_agent_character()
    if not character then
        return {available = false, reason = "agent_not_spawned"}
    end

    local same_surface = character.surface == issuer.surface
    local result = {
        available = true,
        task_kind = task.kind,
        issuer_name = issuer.name,
        issuer_connected = issuer.connected,
        position = {x = issuer.position.x, y = issuer.position.y},
        surface = issuer.surface.name,
        same_surface = same_surface
    }
    if same_surface then
        local dx = character.position.x - issuer.position.x
        local dy = character.position.y - issuer.position.y
        result.distance_to_agent = math.sqrt(dx * dx + dy * dy)
    end
    return result
end

local function agent_status()
    local character = stored_agent_character()
    local jobs = {}
    ensure_storage()
    for job_id, job in pairs(storage.runtime_jobs) do
        jobs[#jobs + 1] = {id = job_id, action = job.action_name or "unknown"}
    end
    table.sort(jobs, function(left, right) return left.id < right.id end)

    local result = {
        spawned = character ~= nil,
        active_jobs = jobs,
        installed_actions = {"player_control"},
        runtime_version = runtime_version,
        task = agent_task_snapshot(storage.agent_task)
    }
    if character then
        result.character = {
            unit_number = character.unit_number,
            position = character.position,
            surface = character.surface.name,
            force = character.force.name
        }
    end
    return result
end

local function finish_job(job_id, result)
    ensure_storage()
    local job = storage.runtime_jobs[job_id]
    if not job then
        return
    end

    storage.runtime_jobs[job_id] = nil
    storage.runtime_results[job_id] = {
        expires = game.tick + result_lifetime_ticks,
        value = result or {completed = true}
    }
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

script.on_event(defines.events.on_tick, function()
    ensure_storage()
    for job_id, job in pairs(storage.runtime_jobs) do
        local ok, done, result = pcall(player_control.tick, job.state)
        if not ok then
            stop_job_control(job)
            finish_job(job_id, {failed = true, error = tostring(done)})
        elseif done then
            finish_job(job_id, result)
        end
    end

    for job_id, result in pairs(storage.runtime_results) do
        if result.expires <= game.tick then
            storage.runtime_results[job_id] = nil
        end
    end
end)

local bridge_interface = {
    agent_status = agent_status,

    peek_agent_task = function()
        ensure_storage()
        return agent_task_snapshot(storage.agent_task)
    end,

    claim_agent_task = claim_agent_task,

    finish_agent_task = finish_agent_task,

    print_model_decision = print_model_decision,

    agent_task_return_target = agent_task_return_target,

    spawn_agent_character = function()
        local existed = stored_agent_character() ~= nil
        local character = ensure_agent_character()
        return {
            created = not existed,
            unit_number = character.unit_number,
            position = character.position,
            surface = character.surface.name,
            force = character.force.name
        }
    end,

    remove_agent_character = remove_agent_character,

    stop_all_actions = function()
        return {stopped_jobs = stop_all_runtime_jobs()}
    end,

    get_control_character = function(use_dedicated_character)
        return control_character(use_dedicated_character == true)
    end,

    control_character_info = function(use_dedicated_character)
        local character = control_character(use_dedicated_character == true)
        return {
            dedicated = use_dedicated_character == true,
            unit_number = character.unit_number,
            position = character.position,
            surface = character.surface.name,
            force = character.force.name
        }
    end,

    runtime_info = function(action_name)
        return {
            version = runtime_version,
            action_installed = action_name == "player_control",
            storage_backed_jobs = true,
            research_trigger_actions = true
        }
    end,

    execute_tool = function(use_dedicated_character, tool_name, arguments)
        local character = control_character(use_dedicated_character == true)
        if tool_name == "get_construction_requests" then
            return construction.get_requests(character, arguments or {})
        elseif tool_name == "get_game_state" then
            return gameplay.get_game_state(character, arguments or {}, use_dedicated_character == true)
        elseif tool_name == "get_inventory" then
            return gameplay.get_inventory(character)
        elseif tool_name == "get_craftable_recipes" then
            return gameplay.get_craftable_recipes(character, arguments or {})
        elseif tool_name == "get_research_status" then
            return gameplay.get_research_status(
                character,
                arguments or {},
                use_dedicated_character == true)
        elseif tool_name == "nearby_entity_prototypes" then
            return inspection.nearby_prototypes(character, arguments or {})
        elseif tool_name == "get_nearby_entities" then
            return inspection.get_nearby_entities(character, arguments or {})
        elseif tool_name == "find_connection_placement" then
            return placement.find_connection(character, arguments or {})
        elseif tool_name == "start_research" then
            return gameplay.start_research(
                character,
                arguments or {},
                use_dedicated_character == true)
        elseif tool_name == "find_resource_patches" then
            return gameplay.find_resource_patches(character, arguments or {})
        elseif tool_name == "find_water" then
            return gameplay.find_water(character, arguments or {})
        elseif tool_name == "take_screenshot" then
            return gameplay.take_screenshot(character, arguments or {})
        elseif tool_name == "stop_walking" then
            return gameplay.stop_walking(character)
        elseif tool_name == "place_entity" then
            return gameplay.place_entity(character, arguments or {}, use_dedicated_character == true)
        elseif tool_name == "set_assembler_recipe" then
            return gameplay.set_assembler_recipe(character, arguments or {})
        elseif tool_name == "insert_item_into_entity" then
            return gameplay.insert_item(character, arguments or {})
        elseif tool_name == "take_item_from_entity" then
            return gameplay.take_item(character, arguments or {})
        elseif tool_name == "machine_output" then
            return gameplay.machine_output(character, arguments or {})
        elseif tool_name == "transfer_inventory_to_container" then
            return gameplay.transfer_to_container(character, arguments or {})
        end
        error("Unknown FactorIA bridge tool: " .. tostring(tool_name))
    end,

    record_research_trigger_action = function(
        use_dedicated_character,
        action_type,
        prototype_name,
        quality,
        count)
        if type(action_type) ~= "string" or type(prototype_name) ~= "string" or
            type(count) ~= "number" or count <= 0 then
            error("Invalid research trigger action")
        end
        return record_research_trigger_action(
            use_dedicated_character,
            action_type,
            prototype_name,
            quality,
            count)
    end,

    research_trigger_progress = function(use_dedicated_character, technology_name)
        local character = control_character(use_dedicated_character == true)
        local technology = character.force.technologies[technology_name]
        if not technology or not technology.prototype.research_trigger then
            return nil
        end
        local required = trigger_required_count(technology.prototype.research_trigger)
        local progress = technology.researched and required or
            trigger_progress_for(character.force, technology.name)
        return {
            progress = progress,
            required = required,
            remaining = math.max(0, required - progress),
            completed = technology.researched or progress >= required
        }
    end,

    start_action = function(action_name, arguments)
        ensure_storage()
        if action_name ~= "player_control" then
            error("Unknown FactorIA runtime action: " .. tostring(action_name))
        end

        local ok, state, immediate_result = pcall(player_control.start, arguments or {})
        if not ok then
            error(state)
        end

        local job_id = storage.next_runtime_job_id
        storage.next_runtime_job_id = job_id + 1
        if state == nil then
            return {active = false, job_id = job_id, result = immediate_result or {completed = true}}
        end

        storage.runtime_jobs[job_id] = {action_name = action_name, state = state}
        return {active = true, job_id = job_id}
    end,

    poll_action = function(job_id)
        ensure_storage()
        local job = storage.runtime_jobs[job_id]
        if job then
            local status = {active = true, job_id = job_id}
            if player_control.status then
                local ok, action_status = pcall(player_control.status, job.state)
                if not ok then
                    stop_job_control(job)
                    storage.runtime_jobs[job_id] = nil
                    return {active = false, job_id = job_id, result = {failed = true, error = tostring(action_status)}}
                end
                if type(action_status) == "table" then
                    for key, value in pairs(action_status) do
                        status[key] = value
                    end
                end
            end
            return status
        end

        local completed = storage.runtime_results[job_id]
        if completed then
            storage.runtime_results[job_id] = nil
            return {active = false, job_id = job_id, result = completed.value}
        end
        return {active = false, job_id = job_id, missing = true}
    end,

    stop_action = function(job_id)
        ensure_storage()
        local job = storage.runtime_jobs[job_id]
        if not job then
            storage.runtime_results[job_id] = nil
            return {active = false, job_id = job_id, missing = true}
        end

        local result = {stopped = true}
        if player_control.stop then
            local ok, stop_result = pcall(player_control.stop, job.state)
            if not ok then
                result = {stopped = true, failed = true, error = tostring(stop_result)}
            elseif type(stop_result) == "table" then
                result = stop_result
                result.stopped = true
            end
        end
        storage.runtime_jobs[job_id] = nil
        storage.runtime_results[job_id] = nil
        return {active = false, job_id = job_id, result = result}
    end
}

remote.add_interface(interface_name, bridge_interface)

local function required_command_argument(arguments, name, expected_type)
    local value = arguments[name]
    if type(value) ~= expected_type then
        error("FactorIA bridge command argument '" .. name .. "' must be a " .. expected_type)
    end
    return value
end

local bridge_command_handlers = {
    status = function()
        return {
            tick = game.tick,
            command_protocol_version = command_protocol_version,
            runtime_version = runtime_version
        }
    end,

    peek_agent_task = function()
        return {task = bridge_interface.peek_agent_task()}
    end,

    claim_agent_task = function(arguments)
        return bridge_interface.claim_agent_task(
            required_command_argument(arguments, "task_id", "number"))
    end,

    finish_agent_task = function(arguments)
        return bridge_interface.finish_agent_task(
            required_command_argument(arguments, "task_id", "number"),
            required_command_argument(arguments, "succeeded", "boolean"),
            required_command_argument(arguments, "message", "string"))
    end,

    print_model_decision = function(arguments)
        return bridge_interface.print_model_decision(
            required_command_argument(arguments, "decision", "string"))
    end,

    execute_tool = function(arguments)
        return bridge_interface.execute_tool(
            required_command_argument(arguments, "use_dedicated_character", "boolean"),
            required_command_argument(arguments, "tool", "string"),
            required_command_argument(arguments, "arguments", "table"))
    end,

    runtime_info = function(arguments)
        return bridge_interface.runtime_info(
            required_command_argument(arguments, "action", "string"))
    end,

    start_action = function(arguments)
        return bridge_interface.start_action(
            required_command_argument(arguments, "action", "string"),
            required_command_argument(arguments, "arguments", "table"))
    end,

    poll_action = function(arguments)
        return bridge_interface.poll_action(
            required_command_argument(arguments, "job_id", "number"))
    end,

    stop_action = function(arguments)
        return bridge_interface.stop_action(
            required_command_argument(arguments, "job_id", "number"))
    end
}

local function dispatch_bridge_command(parameter)
    if type(parameter) ~= "string" or parameter == "" then
        error("FactorIA bridge command requires a JSON request")
    end

    local request = helpers.json_to_table(parameter)
    if type(request) ~= "table" or request.protocol_version ~= command_protocol_version then
        error("Unsupported FactorIA bridge command protocol")
    end
    local operation = required_command_argument(request, "operation", "string")
    local arguments = required_command_argument(request, "arguments", "table")
    local handler = bridge_command_handlers[operation]
    if not handler then
        error("Unknown FactorIA bridge command operation: " .. operation)
    end
    return handler(arguments)
end

commands.add_command(
    "factoria-bridge",
    "Internal JSON command transport for the FactorIA desktop application.",
    function(command)
        if command.player_index then
            local player = game.get_player(command.player_index)
            if player then
                player.print(
                    "[FactorIA] This internal command is only available over RCON.",
                    {color = {r = 1, g = 0.5, b = 0.3}})
            end
            return
        end

        local ok, result = pcall(dispatch_bridge_command, command.parameter)
        local response = {ok = ok, protocol_version = command_protocol_version}
        if ok then
            response.result = result
        else
            response.error = tostring(result)
        end
        rcon.print(helpers.table_to_json(response))
    end)

local function command_player(command)
    if not command.player_index then
        return nil
    end
    return game.get_player(command.player_index)
end

local function command_print(command, message, color)
    local player = command_player(command)
    local print_settings = color and {color = color} or nil
    if player then
        player.print(message, print_settings)
    else
        game.print(message, print_settings)
    end
end

local function require_command_admin(command)
    local player = command_player(command)
    if not player or player.admin then
        return true
    end
    player.print(
        "[FactorIA] This command requires administrator permission.",
        {color = {r = 1, g = 0.5, b = 0.3}})
    return false
end

local agent_task_actions = {
    ["build-ghosts"] = "build entity ghosts",
    ["remove-markers"] = "remove deconstruction-marked entities"
}

local function queue_agent_task(command, kind)
    local player = command_player(command)
    local character = stored_agent_character()
    ensure_storage()
    if not player then
        command_print(
            command,
            "[FactorIA] '" .. kind .. "' must be run by an in-game player.",
            {r = 1, g = 0.5, b = 0.3})
        return
    elseif not character then
        command_print(
            command,
            "[FactorIA] Agent is not spawned. Run /factoria-agent spawn first.",
            {r = 1, g = 0.5, b = 0.3})
        return
    elseif storage.agent_task then
        command_print(
            command,
            string.format(
                "[FactorIA] Task #%d is already %s (%s).",
                storage.agent_task.id,
                storage.agent_task.status,
                storage.agent_task.kind),
            {r = 1, g = 0.75, b = 0.3})
        return
    end

    local task_id = storage.next_agent_task_id
    storage.next_agent_task_id = task_id + 1
    storage.agent_task = {
        id = task_id,
        kind = kind,
        status = "queued",
        issuer_player_index = player.index,
        issuer_name = player.name,
        issuer_position = {x = player.position.x, y = player.position.y},
        issuer_surface = player.surface.name,
        search_radius = agent_task_search_radius,
        issued_tick = game.tick
    }
    command_print(
        command,
        string.format(
            "[FactorIA] Queued task #%d: %s within %d tiles, then return to you.",
            task_id,
            agent_task_actions[kind],
            agent_task_search_radius),
        {r = 0.3, g = 1, b = 0.5})
end

local function position_beside(surface, target)
    local offsets = {
        {x = 1.5, y = 0}, {x = -1.5, y = 0}, {x = 0, y = 1.5}, {x = 0, y = -1.5},
        {x = 1.5, y = 1.5}, {x = -1.5, y = -1.5}, {x = 1.5, y = -1.5}, {x = -1.5, y = 1.5}
    }
    for _, offset in ipairs(offsets) do
        local candidate = surface.find_non_colliding_position(
            "character",
            {x = target.x + offset.x, y = target.y + offset.y},
            4,
            0.25)
        if candidate then
            local dx = candidate.x - target.x
            local dy = candidate.y - target.y
            if dx * dx + dy * dy >= 1 then
                return candidate
            end
        end
    end
    return nil
end

local function print_command_help(command)
    command_print(
        command,
        "[FactorIA] /factoria-agent status|spawn|build-ghosts|remove-markers|come|goto|remove",
        {r = 0.4, g = 0.85, b = 1})
end

commands.add_command(
    "factoria-agent",
    "Inspect, task, or manage the dedicated FactorIA character: /factoria-agent status|spawn|build-ghosts|remove-markers|come|goto|remove",
    function(command)
        local operation = (command.parameter or ""):lower():match("^%s*(%S+)")
        if not operation or operation == "help" then
            print_command_help(command)
            return
        end

        if operation == "status" then
            local status = agent_status()
            local task_suffix = status.task and string.format(
                "; task #%d %s (%s)", status.task.id, status.task.status, status.task.kind) or ""
            if not status.spawned then
                command_print(
                    command,
                    string.format(
                        "[FactorIA] Agent not spawned; %d active runtime job(s)%s.",
                        #status.active_jobs,
                        task_suffix),
                    {r = 1, g = 0.75, b = 0.3})
                return
            end
            local character = status.character
            command_print(
                command,
                string.format(
                    "[FactorIA] Agent #%s at (%.1f, %.1f) on %s; %d active runtime job(s)",
                    tostring(character.unit_number),
                    character.position.x,
                    character.position.y,
                    character.surface,
                    #status.active_jobs) .. task_suffix .. ".",
                {r = 0.4, g = 0.85, b = 1})
            return
        end

        --if not require_command_admin(command) then
        --    return
        --end

        if operation == "spawn" then
            local existed = stored_agent_character() ~= nil
            local character = ensure_agent_character()
            command_print(
                command,
                string.format(
                    "[FactorIA] Agent %s at (%.1f, %.1f) on %s.",
                    existed and "already active" or "spawned",
                    character.position.x,
                    character.position.y,
                    character.surface.name),
                {r = 0.3, g = 1, b = 0.5})
        elseif operation == "build-ghosts" or operation == "remove-markers" then
            queue_agent_task(command, operation)
        elseif operation == "remove" then
            local result = remove_agent_character()
            command_print(
                command,
                string.format(
                    "[FactorIA] Agent %s; stopped %d runtime job(s), spilled %d item stack(s).",
                    result.removed and "removed" or "was not active",
                    result.stopped_jobs,
                    result.spilled_item_entities),
                {r = 1, g = 0.65, b = 0.3})
        elseif operation == "come" then
            local player = command_player(command)
            local character = stored_agent_character()
            if not player then
                command_print(command, "[FactorIA] 'come' must be run by an in-game player.", {r = 1, g = 0.5, b = 0.3})
            elseif not character then
                command_print(command, "[FactorIA] Agent is not spawned.", {r = 1, g = 0.5, b = 0.3})
            else
                local destination = position_beside(player.surface, player.position)
                if not destination then
                    command_print(command, "[FactorIA] No free position was found beside you.", {r = 1, g = 0.5, b = 0.3})
                else
                    local stopped_jobs = stop_all_runtime_jobs()
                    local moved = character.teleport(destination, player.surface)
                    command_print(
                        command,
                        string.format(
                            "[FactorIA] Agent %s; stopped %d runtime job(s).",
                            moved and "moved beside you" or "could not be moved",
                            stopped_jobs),
                        moved and {r = 0.3, g = 1, b = 0.5} or {r = 1, g = 0.5, b = 0.3})
                end
            end
        elseif operation == "goto" then
            local player = command_player(command)
            local character = stored_agent_character()
            if not player then
                command_print(command, "[FactorIA] 'goto' must be run by an in-game player.", {r = 1, g = 0.5, b = 0.3})
            elseif not character then
                command_print(command, "[FactorIA] Agent is not spawned.", {r = 1, g = 0.5, b = 0.3})
            else
                local destination = position_beside(character.surface, character.position)
                local moved = destination and player.teleport(destination, character.surface)
                command_print(
                    command,
                    moved and "[FactorIA] Moved beside the agent." or "[FactorIA] No free position was found beside the agent.",
                    moved and {r = 0.3, g = 1, b = 0.5} or {r = 1, g = 0.5, b = 0.3})
            end
        else
            print_command_help(command)
        end
    end)
