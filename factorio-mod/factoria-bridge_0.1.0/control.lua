local interface_name = "factoria_bridge"
local runtime_version = 2
local result_lifetime_ticks = 60 * 60

-- Uploaded actions are intentionally session-local because functions cannot be serialized in storage.
local runtime_actions = {}
local runtime_uploads = {}
local runtime_jobs = {}
local runtime_results = {}
local next_job_id = 1

local function ensure_storage()
    storage.path_results = storage.path_results or {}
    storage.research_trigger_progress = storage.research_trigger_progress or {}
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
    if job.action.stop then
        pcall(job.action.stop, job.state)
    end
end

local function stop_all_runtime_jobs()
    local job_ids = {}
    for job_id in pairs(runtime_jobs) do
        job_ids[#job_ids + 1] = job_id
    end
    table.sort(job_ids)

    for _, job_id in ipairs(job_ids) do
        stop_job_control(runtime_jobs[job_id])
        runtime_jobs[job_id] = nil
        runtime_results[job_id] = nil
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

local function agent_status()
    local character = stored_agent_character()
    local jobs = {}
    for job_id, job in pairs(runtime_jobs) do
        jobs[#jobs + 1] = {id = job_id, action = job.action_name or "unknown"}
    end
    table.sort(jobs, function(left, right) return left.id < right.id end)

    local installed_actions = {}
    for action_name in pairs(runtime_actions) do
        installed_actions[#installed_actions + 1] = action_name
    end
    table.sort(installed_actions)

    local result = {
        spawned = character ~= nil,
        active_jobs = jobs,
        installed_actions = installed_actions,
        runtime_version = runtime_version
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
    local job = runtime_jobs[job_id]
    if not job then
        return
    end

    runtime_jobs[job_id] = nil
    runtime_results[job_id] = {
        expires = game.tick + result_lifetime_ticks,
        value = result or {completed = true}
    }
end

local function validate_action_name(action_name)
    if type(action_name) ~= "string" or #action_name == 0 or #action_name > 128 or
        not string.match(action_name, "^[%w_-]+$") then
        error("Invalid FactorIA runtime action name")
    end
end

local function install_action_source(action_name, source)
    validate_action_name(action_name)
    if type(source) ~= "string" or #source == 0 or #source > 256 * 1024 then
        error("Invalid FactorIA runtime action source")
    end

    local chunk, load_error = load(source, "@factoria/" .. action_name, "t", _ENV)
    if not chunk then
        error(load_error)
    end

    local ok, action = pcall(chunk)
    if not ok then
        error(action)
    end
    if type(action) ~= "table" or type(action.start) ~= "function" or type(action.tick) ~= "function" then
        error("A FactorIA runtime action must return start and tick functions")
    end

    runtime_actions[action_name] = action
    return {installed = true, name = action_name, version = runtime_version}
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
    for job_id, job in pairs(runtime_jobs) do
        local ok, done, result = pcall(job.action.tick, job.state)
        if not ok then
            stop_job_control(job)
            finish_job(job_id, {failed = true, error = tostring(done)})
        elseif done then
            finish_job(job_id, result)
        end
    end

    for job_id, result in pairs(runtime_results) do
        if result.expires <= game.tick then
            runtime_results[job_id] = nil
        end
    end
end)

remote.add_interface(interface_name, {
    agent_status = agent_status,

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
            action_installed = action_name ~= nil and runtime_actions[action_name] ~= nil,
            research_trigger_actions = true
        }
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

    install_action = function(action_name, source)
        return install_action_source(action_name, source)
    end,

    begin_action_upload = function(action_name)
        validate_action_name(action_name)
        runtime_uploads[action_name] = {parts = {}, size = 0}
        return true
    end,

    append_action_upload = function(action_name, source_part)
        local upload = runtime_uploads[action_name]
        if not upload then
            error("FactorIA runtime action upload was not started")
        end
        if type(source_part) ~= "string" or #source_part == 0 or #source_part > 8 * 1024 then
            error("Invalid FactorIA runtime action upload part")
        end
        if upload.size + #source_part > 256 * 1024 then
            runtime_uploads[action_name] = nil
            error("FactorIA runtime action upload is too large")
        end

        upload.parts[#upload.parts + 1] = source_part
        upload.size = upload.size + #source_part
        return {received = upload.size}
    end,

    finish_action_upload = function(action_name)
        local upload = runtime_uploads[action_name]
        if not upload then
            error("FactorIA runtime action upload was not started")
        end
        runtime_uploads[action_name] = nil
        return install_action_source(action_name, table.concat(upload.parts))
    end,

    start_action = function(action_name, arguments)
        local action = runtime_actions[action_name]
        if not action then
            error("FactorIA runtime action is not installed: " .. tostring(action_name))
        end

        local ok, state, immediate_result = pcall(action.start, arguments or {})
        if not ok then
            error(state)
        end

        local job_id = next_job_id
        next_job_id = next_job_id + 1
        if state == nil then
            return {active = false, job_id = job_id, result = immediate_result or {completed = true}}
        end

        runtime_jobs[job_id] = {action_name = action_name, action = action, state = state}
        return {active = true, job_id = job_id}
    end,

    poll_action = function(job_id)
        local job = runtime_jobs[job_id]
        if job then
            local status = {active = true, job_id = job_id}
            if job.action.status then
                local ok, action_status = pcall(job.action.status, job.state)
                if not ok then
                    stop_job_control(job)
                    runtime_jobs[job_id] = nil
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

        local completed = runtime_results[job_id]
        if completed then
            runtime_results[job_id] = nil
            return {active = false, job_id = job_id, result = completed.value}
        end
        return {active = false, job_id = job_id, missing = true}
    end,

    stop_action = function(job_id)
        local job = runtime_jobs[job_id]
        if not job then
            runtime_results[job_id] = nil
            return {active = false, job_id = job_id, missing = true}
        end

        local result = {stopped = true}
        if job.action.stop then
            local ok, stop_result = pcall(job.action.stop, job.state)
            if not ok then
                result = {stopped = true, failed = true, error = tostring(stop_result)}
            elseif type(stop_result) == "table" then
                result = stop_result
                result.stopped = true
            end
        end
        runtime_jobs[job_id] = nil
        runtime_results[job_id] = nil
        return {active = false, job_id = job_id, result = result}
    end,

    request_path = function(use_dedicated_character, target_x, target_y, radius)
        ensure_storage()
        local character = control_character(use_dedicated_character == true)
        return character.surface.request_path {
            bounding_box = character.prototype.collision_box,
            collision_mask = character.prototype.collision_mask,
            start = character.position,
            goal = {x = target_x, y = target_y},
            force = character.force,
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
        "[FactorIA] /factoria-agent status|spawn|come|goto|remove",
        {r = 0.4, g = 0.85, b = 1})
end

commands.add_command(
    "factoria-agent",
    "Inspect or manage the dedicated FactorIA character: /factoria-agent status|spawn|come|goto|remove",
    function(command)
        local operation = (command.parameter or ""):lower():match("^%s*(%S+)")
        if not operation or operation == "help" then
            print_command_help(command)
            return
        end

        if operation == "status" then
            local status = agent_status()
            if not status.spawned then
                command_print(
                    command,
                    string.format("[FactorIA] Agent not spawned; %d active runtime job(s).", #status.active_jobs),
                    {r = 1, g = 0.75, b = 0.3})
                return
            end
            local character = status.character
            command_print(
                command,
                string.format(
                    "[FactorIA] Agent #%s at (%.1f, %.1f) on %s; %d active runtime job(s).",
                    tostring(character.unit_number),
                    character.position.x,
                    character.position.y,
                    character.surface,
                    #status.active_jobs),
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
