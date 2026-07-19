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
end

local function connected_character()
    local player = game.connected_players[1]
    if not player or not player.valid or not player.character or not player.character.valid then
        error("No connected Factorio player with a character")
    end
    return player.character
end

local function ensure_agent_character()
    ensure_storage()
    if storage.agent_character and storage.agent_character.valid then
        return storage.agent_character
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

local function stop_job_control(job)
    if job.action.stop then
        pcall(job.action.stop, job.state)
    end
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
            action_installed = action_name ~= nil and runtime_actions[action_name] ~= nil
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

        runtime_jobs[job_id] = {action = action, state = state}
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
