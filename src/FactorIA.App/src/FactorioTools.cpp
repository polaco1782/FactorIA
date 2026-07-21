#include <FactorIA/FactorioTools.h>

#include "FactorioRuntimeActions.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace factoria
{
namespace
{
using json = nlohmann::json;

constexpr double MaximumSearchRadius = 8192.0;

bool HasArgumentValue(const json& arguments, const std::string& name)
{
    const auto value = arguments.find(name);
    if (value == arguments.end() || value->is_null())
        return false;
    return !value->is_string() || !value->get_ref<const std::string&>().empty();
}

json FunctionTool(std::string name, std::string description, json parameters)
{
    return {
        {"type", "function"},
        {"function", {
            {"name", std::move(name)},
            {"description", std::move(description)},
            {"parameters", std::move(parameters)},
        }},
    };
}

json EmptyParameters()
{
    return {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}};
}

double RequiredNumber(const json& arguments, const std::string& name, double minimum, double maximum)
{
    const auto value = arguments.find(name);
    if (value == arguments.end() || !value->is_number())
        throw std::runtime_error("Missing numeric argument: " + name);
    const auto number = value->get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum)
    {
        throw std::runtime_error(
            "Argument " + name + " must be between " + std::to_string(minimum) +
            " and " + std::to_string(maximum));
    }
    return number;
}

double NumberOrDefault(
    const json& arguments,
    const std::string& name,
    double defaultValue,
    double minimum,
    double maximum)
{
    return HasArgumentValue(arguments, name)
        ? RequiredNumber(arguments, name, minimum, maximum)
        : defaultValue;
}

int RequiredInteger(const json& arguments, const std::string& name, int minimum, int maximum)
{
    const auto value = arguments.find(name);
    if (value == arguments.end() || !value->is_number_integer())
        throw std::runtime_error("Missing integer argument: " + name);
    const auto number = value->get<int>();
    if (number < minimum || number > maximum)
    {
        throw std::runtime_error(
            "Argument " + name + " must be between " + std::to_string(minimum) +
            " and " + std::to_string(maximum));
    }
    return number;
}

int IntegerOrDefault(
    const json& arguments,
    const std::string& name,
    int defaultValue,
    int minimum,
    int maximum)
{
    return HasArgumentValue(arguments, name)
        ? RequiredInteger(arguments, name, minimum, maximum)
        : defaultValue;
}

bool BooleanOrDefault(const json& arguments, const std::string& name, bool defaultValue)
{
    if (!HasArgumentValue(arguments, name))
        return defaultValue;
    const auto value = arguments.find(name);
    if (!value->is_boolean())
        throw std::runtime_error("Argument " + name + " must be a boolean");
    return value->get<bool>();
}

std::string RequiredText(const json& arguments, const std::string& name, std::size_t maximumLength)
{
    const auto value = arguments.find(name);
    if (value == arguments.end() || !value->is_string())
        throw std::runtime_error("Missing string argument: " + name);
    const auto text = value->get<std::string>();
    if (text.empty() || text.size() > maximumLength)
    {
        throw std::runtime_error(
            "Argument " + name + " must contain between 1 and " +
            std::to_string(maximumLength) + " characters");
    }
    return text;
}

std::string RequiredPrototypeName(const json& arguments, const std::string& name)
{
    const auto text = RequiredText(arguments, name, 128);
    static const std::regex allowed("^[A-Za-z0-9_-]{1,128}$");
    if (!std::regex_match(text, allowed))
        throw std::runtime_error("Argument " + name + " is not a valid Factorio prototype name");
    return text;
}

std::string PrototypeNameOrEmpty(const json& arguments, const std::string& name)
{
    return HasArgumentValue(arguments, name)
        ? RequiredPrototypeName(arguments, name)
        : std::string{};
}

std::string DirectionExpression(const std::string& direction)
{
    if (direction == "north") return "defines.direction.north";
    if (direction == "east") return "defines.direction.east";
    if (direction == "south") return "defines.direction.south";
    if (direction == "west") return "defines.direction.west";
    throw std::runtime_error("direction must be north, east, south, or west");
}

struct PlacementRequest
{
    std::string item;
    std::string direction;
    double x{};
    double y{};
};

PlacementRequest RequiredPlacementRequest(const json& arguments)
{
    PlacementRequest request{
        .item = RequiredPrototypeName(arguments, "item"),
        .direction = RequiredText(arguments, "direction", 16),
        .x = RequiredNumber(arguments, "x", -1000000.0, 1000000.0),
        .y = RequiredNumber(arguments, "y", -1000000.0, 1000000.0),
    };
    static_cast<void>(DirectionExpression(request.direction));
    return request;
}

json PlacementParameters(bool includeMaximumDuration)
{
    json properties{
        {"item", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
        {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
        {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
        {"direction", {{"type", "string"}, {"enum", {"north", "east", "south", "west"}}}},
    };
    if (includeMaximumDuration)
    {
        properties["maximum_duration_seconds"] = {
            {"type", "number"}, {"minimum", 1.0}, {"maximum", 120.0}, {"default", 60.0}};
    }
    return {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", {"item", "x", "y", "direction"}},
        {"additionalProperties", false},
    };
}

json ConstructionBatchParameters()
{
    return {
        {"type", "object"},
        {"properties", {
            {"radius", {{"type", "number"}, {"minimum", 1.0}, {"maximum", MaximumSearchRadius}}},
            {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}}},
            {"maximum_duration_seconds", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 600.0}}},
        }},
        {"required", {"radius", "count", "maximum_duration_seconds"}},
        {"additionalProperties", false},
    };
}

struct ConstructionBatchRequest
{
    double radius{};
    int count{};
    double maximumSeconds{};
};

ConstructionBatchRequest RequiredConstructionBatch(const json& arguments)
{
    return {
        .radius = RequiredNumber(arguments, "radius", 1.0, MaximumSearchRadius),
        .count = RequiredInteger(arguments, "count", 1, 20),
        .maximumSeconds = RequiredNumber(arguments, "maximum_duration_seconds", 1.0, 600.0),
    };
}

std::string EntityLookupLua(
    const std::string& prefixLua,
    double targetX,
    double targetY,
    std::string_view candidateExpression,
    std::string_view targetDescription,
    bool requireReach = true)
{
    return
        prefixLua +
        "local position={x=" + std::to_string(targetX) + ",y=" + std::to_string(targetY) + "} "
        "local found=p.surface.find_entities_filtered{position=position,radius=0.75} "
        "table.sort(found,function(a,b) local adx=a.position.x-position.x local ady=a.position.y-position.y "
        "local bdx=b.position.x-position.x local bdy=b.position.y-position.y "
        "return adx*adx+ady*ady<bdx*bdx+bdy*bdy end) "
        "local target=nil for _,candidate in ipairs(found) do "
        "if candidate~=p and (" + std::string(candidateExpression) + ") then target=candidate break end end "
        "if not target then error('No " + std::string(targetDescription) + " found at the requested position') end "
        + (requireReach
            ? "if not p.can_reach_entity(target) then error('Target entity is out of reach') end "
            : "");
}

std::string MachineLookupLua(
    const std::string& controlCharacterLua,
    double targetX,
    double targetY,
    bool requireReach = true)
{
    return EntityLookupLua(
        controlCharacterLua,
        targetX,
        targetY,
        "candidate.type=='furnace' or candidate.type=='assembling-machine'",
        "furnace or crafting machine",
        requireReach);
}

std::string EntityInventoryLookupLua(
    const std::string& controlCharacterLua,
    double targetX,
    double targetY,
    const std::string& inventory,
    bool requireReach = true)
{
    if (inventory != "fuel" && inventory != "input" && inventory != "output")
        throw std::runtime_error("inventory must be fuel, input, or output");

    const auto inventoryLiteral = json(inventory).dump();
    const auto prefix =
        controlCharacterLua +
        "local inventory_kind=" + inventoryLiteral + " "
        "local function factoria_inventory(entity) "
        "if inventory_kind=='fuel' then return entity.get_fuel_inventory() end "
        "if inventory_kind=='output' and (entity.type=='furnace' or entity.type=='assembling-machine') then "
        "return entity.get_inventory(defines.inventory.crafter_output) end "
        "if inventory_kind=='input' then "
        "if entity.type=='lab' then return entity.get_inventory(defines.inventory.lab_input) end "
        "if entity.type=='furnace' or entity.type=='assembling-machine' then "
        "return entity.get_inventory(defines.inventory.crafter_input) end end return nil end ";

    return EntityLookupLua(
        prefix,
        targetX,
        targetY,
        "factoria_inventory(candidate)~=nil",
        "entity with a " + inventory + " inventory",
        requireReach) +
        "local entity_inventory=factoria_inventory(target) ";
}

std::string ResearchHelpersLua(bool useDedicatedCharacter)
{
    return
        "local function factoria_technology_info(technology) "
        "local prerequisites={} local missing={} "
        "for name,prerequisite in pairs(technology.prerequisites) do "
        "prerequisites[#prerequisites+1]=name if not prerequisite.researched then missing[#missing+1]=name end end "
        "table.sort(prerequisites) table.sort(missing) local ingredients={} "
        "for _,ingredient in ipairs(technology.research_unit_ingredients) do "
        "ingredients[#ingredients+1]={name=ingredient.name,amount=ingredient.amount} end "
        "table.sort(ingredients,function(a,b) return a.name<b.name end) local unlocks={} "
        "for _,effect in ipairs(technology.prototype.effects) do "
        "if effect.type=='unlock-recipe' then unlocks[#unlocks+1]=effect.recipe end end table.sort(unlocks) "
        "local source_trigger=technology.prototype.research_trigger local research_trigger=nil "
        "if source_trigger then research_trigger={type=source_trigger.type} "
        "if source_trigger.type=='craft-item' then research_trigger.item=source_trigger.item research_trigger.count=source_trigger.count "
        "elseif source_trigger.type=='mine-entity' then research_trigger.entity=source_trigger.entity "
        "elseif source_trigger.type=='craft-fluid' then research_trigger.fluid=source_trigger.fluid research_trigger.amount=source_trigger.amount "
        "elseif source_trigger.type=='build-entity' then research_trigger.entity=source_trigger.entity "
        "elseif source_trigger.type=='send-item-to-orbit' then research_trigger.item=source_trigger.item "
        "elseif source_trigger.type=='capture-spawner' then research_trigger.entity=source_trigger.entity end end "
        "local available=technology.enabled and not technology.researched and #missing==0 "
        "local info={name=technology.name,level=technology.level,enabled=technology.enabled,"
        "researched=technology.researched,available=available,startable=available and research_trigger==nil,"
        "prerequisites=prerequisites,missing_prerequisites=missing,ingredients=ingredients,"
        "research_unit_count=technology.research_unit_count,"
        "research_unit_energy_ticks=technology.research_unit_energy,"
        "research_unit_time_seconds=technology.research_unit_energy/60,unlocks_recipes=unlocks,"
        "research_mode=research_trigger and 'trigger' or 'lab',research_trigger=research_trigger} "
        "if research_trigger then local iface=remote.interfaces['factoria_bridge'] "
        "if iface and iface.research_trigger_progress then info.research_trigger_progress=remote.call("
        "'factoria_bridge','research_trigger_progress'," +
        std::string(useDedicatedCharacter ? "true" : "false") + ",technology.name) "
        "else info.research_trigger_progress={available=false,"
        "reason='Updated FactorIA Bridge mod required for trigger progress'} end end return info end "
        "local function factoria_research_queue(force) local queue={} local queued={} "
        "for index,technology in ipairs(force.research_queue) do "
        "queue[#queue+1]={position=index,name=technology.name,level=technology.level} "
        "queued[technology.name]=true end return queue,queued end ";
}

std::string EntityConnectionDetailsLua()
{
    return
        "local function factoria_direction_name(direction) "
        "local names={[defines.direction.north]='north',[defines.direction.northeast]='northeast',"
        "[defines.direction.east]='east',[defines.direction.southeast]='southeast',"
        "[defines.direction.south]='south',[defines.direction.southwest]='southwest',"
        "[defines.direction.west]='west',[defines.direction.northwest]='northwest'} "
        "return names[direction] or tostring(direction) end "
        "local function factoria_entity_reference(entity) "
        "if not entity or not entity.valid then return nil end "
        "return {name=entity.name,type=entity.type,position={x=entity.position.x,y=entity.position.y},"
        "direction=factoria_direction_name(entity.direction),unit_number=entity.unit_number} end "
        "local function factoria_optional_property(entity,name) "
        "local ok,value=pcall(function() return entity[name] end) if ok then return value end return nil end "
        "local function factoria_entity_references(entities) local result={} "
        "for _,entity in pairs(entities or {}) do result[#result+1]=factoria_entity_reference(entity) end "
        "return result end "
        "local function factoria_attach_item_connections(info,entity) "
        "if entity.type=='mining-drill' or entity.type=='inserter' then "
        "local drop_target=entity.drop_target info.item_output={"
        "position={x=entity.drop_position.x,y=entity.drop_position.y},connected=drop_target~=nil,"
        "target=factoria_entity_reference(drop_target)} "
        "end "
        "if entity.type=='inserter' then local pickup_target=entity.pickup_target info.item_input={"
        "position={x=entity.pickup_position.x,y=entity.pickup_position.y},connected=pickup_target~=nil,"
        "target=factoria_entity_reference(pickup_target)} end "
        "if entity.type=='mining-drill' then local area=entity.mining_area "
        "info.mining_area={left_top={x=area.left_top.x,y=area.left_top.y},"
        "right_bottom={x=area.right_bottom.x,y=area.right_bottom.y}} "
        "info.mining_target=factoria_entity_reference(entity.mining_target) end end "
        "local function factoria_attach_belt_connections(info,entity) "
        "local belt_types={['transport-belt']=true,['underground-belt']=true,splitter=true,"
        "loader=true,['loader-1x1']=true,['linked-belt']=true} if not belt_types[entity.type] then return end "
        "local neighbours=entity.belt_neighbours info.belt_connections={"
        "inputs=factoria_entity_references(neighbours.inputs),"
        "outputs=factoria_entity_references(neighbours.outputs)} "
        "if entity.type=='underground-belt' then info.belt_connections.underground="
        "factoria_entity_reference(factoria_optional_property(entity,'underground_belt_neighbour') or "
        "factoria_optional_property(entity,'neighbours')) end "
        "if entity.type=='linked-belt' then info.belt_connections.linked="
        "factoria_entity_reference(factoria_optional_property(entity,'linked_belt_neighbour') or "
        "factoria_optional_property(entity,'neighbours')) end "
        "end "
        "local function factoria_attach_fluid_details(info,entity) "
        "local owner_fluidbox=entity.fluidbox if not owner_fluidbox or #owner_fluidbox==0 then return end "
        "local boxes={} local connection_count=0 local connected_count=0 "
        "local function add_prototype(target,prototype) "
        "target[#target+1]={production_type=prototype.production_type,"
        "filter=prototype.filter and prototype.filter.name or nil,"
        "minimum_temperature=prototype.minimum_temperature,maximum_temperature=prototype.maximum_temperature} end "
        "for index=1,#owner_fluidbox do local box={index=index,connections={}} "
        "local prototype=owner_fluidbox.get_prototype(index) local prototype_info={} "
        "if prototype.object_name then add_prototype(prototype_info,prototype) "
        "else for _,entry in pairs(prototype) do add_prototype(prototype_info,entry) end end "
        "box.prototypes=prototype_info local fluid=owner_fluidbox[index] "
        "if fluid then box.fluid={name=fluid.name,amount=fluid.amount,temperature=fluid.temperature} end "
        "for _,connection in ipairs(owner_fluidbox.get_pipe_connections(index)) do "
        "local target_owner=connection.target and connection.target.owner or nil "
        "local connection_info={flow_direction=connection.flow_direction,"
        "connection_type=connection.connection_type,"
        "position={x=connection.position.x,y=connection.position.y},"
        "target_position={x=connection.target_position.x,y=connection.target_position.y},"
        "connected=target_owner~=nil} "
        "if target_owner then connection_info.target=factoria_entity_reference(target_owner) "
        "connected_count=connected_count+1 end "
        "box.connections[#box.connections+1]=connection_info connection_count=connection_count+1 end "
        "boxes[#boxes+1]=box end info.fluidboxes=boxes "
        "info.fluid_connection_summary={total=connection_count,connected=connected_count,"
        "open=connection_count-connected_count} end "
        "local function factoria_attach_electric_connections(info,entity) "
        "local prototype=entity.prototype "
        "if entity.type~='electric-pole' and not prototype.electric_energy_source_prototype then return end "
        "info.electric_connection={network_id=entity.electric_network_id,"
        "connected_to_power_source=entity.is_connected_to_electric_network()} "
        "if entity.type=='electric-pole' then local radius=prototype.get_supply_area_distance(entity.quality.name) "
        "info.electric_connection.supply_radius=radius info.electric_connection.supply_area={"
        "left_top={x=entity.position.x-radius,y=entity.position.y-radius},"
        "right_bottom={x=entity.position.x+radius,y=entity.position.y+radius}} "
        "info.electric_connection.maximum_wire_distance=prototype.get_max_wire_distance(entity.quality.name) end end "
        "local function factoria_attach_heat_connections(info,entity) "
        "local prototype=entity.prototype "
        "if not prototype.heat_buffer_prototype and not prototype.heat_energy_source_prototype then return end "
        "info.heat_connections={neighbours=factoria_entity_references(entity.heat_neighbours)} end "
        "local function factoria_attach_wall_connections(info,entity) "
        "if entity.type~='wall' and entity.type~='gate' then return end "
        "local neighbours=factoria_optional_property(entity,'wall_neighbours') or "
        "factoria_optional_property(entity,'neighbours') "
        "if not neighbours then return end "
        "info.wall_connections={north=factoria_entity_reference(neighbours.north),"
        "east=factoria_entity_reference(neighbours.east),south=factoria_entity_reference(neighbours.south),"
        "west=factoria_entity_reference(neighbours.west)} end "
        "local function factoria_attach_connection_details(info,entity) local box=entity.bounding_box "
        "info.collision_box={left_top={x=box.left_top.x,y=box.left_top.y},"
        "right_bottom={x=box.right_bottom.x,y=box.right_bottom.y}} "
        "factoria_attach_item_connections(info,entity) factoria_attach_belt_connections(info,entity) "
        "factoria_attach_fluid_details(info,entity) factoria_attach_electric_connections(info,entity) "
        "factoria_attach_heat_connections(info,entity) factoria_attach_wall_connections(info,entity) end ";
}

void RejectUnknownArguments(const json& arguments, std::initializer_list<std::string_view> allowed)
{
    for (const auto& [name, value] : arguments.items())
    {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
        {
            std::string message = "Unexpected tool argument: " + name + ". ";
            if (allowed.size() == 0)
            {
                message += "This tool accepts no arguments";
            }
            else
            {
                message += "Allowed arguments: ";
                bool first = true;
                for (const auto argument : allowed)
                {
                    if (!first)
                        message += ", ";
                    message += argument;
                    first = false;
                }
            }
            throw std::runtime_error(message);
        }
    }
}

json ParseFactorioJson(const std::string& response)
{
    const auto first = response.find('{');
    const auto last = response.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first)
        throw std::runtime_error("Factorio did not return a JSON command result: " + response);

    const auto value = json::parse(response.substr(first, last - first + 1));
    if (!value.is_object() || !value.contains("ok"))
        throw std::runtime_error("Factorio returned an invalid command envelope");
    if (!value.value("ok", false))
        throw std::runtime_error("Factorio command failed: " + value.value("error", "unknown Lua error"));
    return value.value("result", json{});
}

std::string Base64Encode(const std::vector<unsigned char>& input)
{
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    for (std::size_t offset = 0; offset < input.size(); offset += 3U)
    {
        const auto remaining = input.size() - offset;
        const auto first = static_cast<unsigned int>(input[offset]);
        const auto second = remaining > 1U ? static_cast<unsigned int>(input[offset + 1U]) : 0U;
        const auto third = remaining > 2U ? static_cast<unsigned int>(input[offset + 2U]) : 0U;
        const auto packed = (first << 16U) | (second << 8U) | third;
        output.push_back(alphabet[(packed >> 18U) & 0x3fU]);
        output.push_back(alphabet[(packed >> 12U) & 0x3fU]);
        output.push_back(remaining > 1U ? alphabet[(packed >> 6U) & 0x3fU] : '=');
        output.push_back(remaining > 2U ? alphabet[packed & 0x3fU] : '=');
    }
    return output;
}

std::string LuaStringLiteral(std::string_view value)
{
    return json(std::string(value)).dump();
}

std::chrono::steady_clock::time_point DeadlineAfter(double seconds)
{
    return std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
}

double RemainingSeconds(std::chrono::steady_clock::time_point deadline)
{
    return std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
}
}

FactorioTools::FactorioTools(
    CommandExecutor executeCommand,
    std::filesystem::path factorioUserDataPath,
    bool useDedicatedCharacter)
    : executeCommand_(std::move(executeCommand)),
      factorioUserDataPath_(std::move(factorioUserDataPath)),
      useDedicatedCharacter_(useDedicatedCharacter)
{
    if (!executeCommand_)
        throw std::runtime_error("FactorioTools requires an RCON command executor");

    definitions_ = json::array({
        FunctionTool(
            "get_game_state",
            "Get the game tick, controlled character state, crafting queue, and force rocket-launch count. Only rocket_launch_confirmed=true is terminal evidence that the rocket objective is complete.",
            EmptyParameters()),
        FunctionTool(
            "get_inventory",
            "List every item and count in the controlled character's main inventory.",
            EmptyParameters()),
        FunctionTool(
            "get_craftable_recipes",
            "List enabled hand-crafting recipes that the player can craft now, including the currently craftable count.",
            EmptyParameters()),
        FunctionTool(
            "get_research_status",
            "Get current and queued lab research, startable technologies, and trigger-based milestones such as crafting an item or building an entity. Call with {} to list every currently relevant technology. Set technology only to an exact technology name returned by this tool; item and recipe names are not technology names. Omit the optional field when listing rather than sending an empty string or null.",
            {
                {"type", "object"},
                {"properties", {
                    {"technology", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                }},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "start_research",
            "Start an available lab technology when research is idle, or append it to the force research queue. Trigger-based milestones cannot be queued; perform the reported trigger action instead. Use get_research_status first, then place a lab and insert the required science packs into its input inventory.",
            {
                {"type", "object"},
                {"properties", {
                    {"technology", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                }},
                {"required", {"technology"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "get_nearby_entities",
            "Return a page of up to 40 nearby entities with character-relative deltas and Factorio-resolved connection geometry. A minimal unfiltered call contains only radius. Omit name, type, and regex when they are unknown; never fill optional filters with empty strings or null. type must be a real Factorio entity type such as furnace, resource, or container; 'entity' is not a wildcard. regex searches both prototype names and entity types. Relevant entities report verified item, belt, fluid, electric, heat, wall, and collision geometry. Distinct prototypes precede duplicates and next_offset retrieves another page. This cannot find water tiles; use find_water. Factorio X increases east and Y increases south.",
            {
                {"type", "object"},
                {"properties", {
                    {"radius", {{"type", "number"}, {"minimum", 1.0}, {"maximum", MaximumSearchRadius}}},
                    {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"type", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"regex", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}},
                }},
                {"required", {"radius"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "get_construction_requests",
            "List nearby entity ghosts and entities explicitly marked for deconstruction, nearest first. Build requests include the exact prototype, direction, quality, required placement item, inventory count, and reachability. Use available_only=true to retain requests for which the controlled character has the required material or which are currently minable; distant requests remain available because the bounded service tools can walk to them.",
            {
                {"type", "object"},
                {"properties", {
                    {"radius", {{"type", "number"}, {"minimum", 1.0}, {"maximum", MaximumSearchRadius}}},
                    {"kind", {{"type", "string"}, {"enum", {"all", "build", "deconstruct"}}, {"default", "all"}}},
                    {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}},
                    {"available_only", {{"type", "boolean"}, {"default", false}}},
                }},
                {"required", {"radius"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "build_ghosts",
            "Build up to count entity ghosts within radius, nearest first, using items from the controlled character's inventory. This bounded tool walks normally, checks reach, consumes the exact item quality, and revives the ghost so blueprint settings and item requests are preserved. It stops early on cancellation, timeout, missing materials, or an unreachable request.",
            ConstructionBatchParameters()),
        FunctionTool(
            "deconstruct_marked",
            "Mine up to count entities explicitly marked for deconstruction within radius, nearest first. This bounded tool walks normally and holds the character's mining input through the bridge runtime; it never sweeps unmarked buildings.",
            ConstructionBatchParameters()),
        FunctionTool(
            "find_resource_patches",
            "Survey a large generated area for resource entities and return the nearest grouped patch regions with exact walking targets. Use resource='iron-ore', 'copper-ore', 'coal', 'stone', 'uranium-ore', or 'any'. Water is a terrain tile, not a resource; use find_water for water. Prefer this over wandering when locating ore.",
            {
                {"type", "object"},
                {"properties", {
                    {"resource", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"radius", {{"type", "number"}, {"minimum", 32.0}, {"maximum", MaximumSearchRadius}}},
                }},
                {"required", {"resource", "radius"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "find_water",
            "Survey generated terrain for pumpable water tiles and return the nearest shoreline positions with exact walk targets. Use this before planning steam power or placing an offshore pump. Water is not an entity or resource, so do not search for it with get_nearby_entities or find_resource_patches. A result with found=false covers only generated terrain within the requested radius and does not prove that the whole map is waterless.",
            {
                {"type", "object"},
                {"properties", {
                    {"radius", {{"type", "number"}, {"minimum", 32.0}, {"maximum", MaximumSearchRadius}}},
                }},
                {"required", {"radius"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "walk",
            "Explore by walking in one cardinal direction for a short duration. This tool accepts only direction and duration_seconds; it does not accept coordinates or items. Use walk_to for map coordinates and walk_to_for_placement for an item plus build coordinates.",
            {
                {"type", "object"},
                {"properties", {
                    {"direction", {{"type", "string"}, {"enum", {"north", "east", "south", "west"}}}},
                    {"duration_seconds", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 10.0}}},
                }},
                {"required", {"direction", "duration_seconds"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "walk_to",
            "Walk toward map coordinates using normal eight-direction player movement. Factorio X increases east and Y increases south. Stops near the target or reports an obstruction/timeout. stopping_distance defaults to 1.5 and maximum_duration_seconds defaults to 60.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"stopping_distance", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 10.0}, {"default", 1.5}}},
                    {"maximum_duration_seconds", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 120.0}, {"default", 60.0}}},
                }},
                {"required", {"x", "y"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "find_connection_placement",
            "Find exact coordinates and direction for placing an item so Factorio confirms an automatic spatial connection to one existing entity. This planner does not consume the item or leave a preview entity behind. Use item-from-target when the new entity must receive or pick up items from the target, item-to-target when it must drop items into the target, and the corresponding fluid, belt, electric, heat, or wall mode for those networks. Call the returned next_tool unchanged; do not calculate an offset from the target yourself.",
            {
                {"type", "object"},
                {"properties", {
                    {"item", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"target_x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"target_y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"connection", {{"type", "string"}, {"enum", {
                        "item-from-target", "item-to-target", "fluid", "belt-from-target",
                        "belt-to-target", "electric", "heat", "wall"}}}},
                    {"search_radius", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 64.0}, {"default", 8.0}}},
                }},
                {"required", {"item", "target_x", "target_y", "connection"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "walk_to_for_placement",
            "Validate an exact future place_entity request, find a pathable stand position within the character's real build reach, and walk there without consuming the item. Call this with the same item, x, y, and direction before placing terrain-sensitive or distant entities. It returns placement_ready and the exact next place_entity arguments. If the build coordinate or direction is invalid, use one of its verified valid_alternatives instead of guessing more coordinates.",
            PlacementParameters(true)),
        FunctionTool(
            "stop_walking",
            "Stop the controlled character walking immediately.",
            EmptyParameters()),
        FunctionTool(
            "take_screenshot",
            "Capture the visible Factorio world centered on the controlled character and inspect the resulting image. Requires non-headless Factorio, access to its user data directory, and a vision-capable llama.cpp model.",
            {
                {"type", "object"},
                {"properties", {
                    {"width", {{"type", "integer"}, {"minimum", 320}, {"maximum", 1280}}},
                    {"height", {{"type", "integer"}, {"minimum", 240}, {"maximum", 960}}},
                    {"zoom", {{"type", "number"}, {"minimum", 0.25}, {"maximum", 2.0}}},
                }},
                {"required", {"width", "height", "zoom"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "mine_entity",
            "Continuously mine the nearest reachable entity with the exact prototype name using normal player mining. For resources, count is the total number of units to extract across adjacent nodes. Trees, rocks, and other discrete entities are mined once. Walk near the target first; the tool resolves the exact entity position and chooses a sufficient timeout.",
            {
                {"type", "object"},
                {"properties", {
                    {"name", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}},
                }},
                {"required", {"name", "count"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "craft",
            "Hand-craft an unlocked recipe and wait for the queued batch to finish. Returns completed, crafted item deltas, and any research trigger progress or unlock caused by the verified craft. Call only when the current crafting queue is empty.",
            {
                {"type", "object"},
                {"properties", {
                    {"recipe", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
                }},
                {"required", {"recipe", "count"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "place_entity",
            "Place one buildable item from the controlled character's inventory on a reachable open tile using normal Factorio build rules. The item is consumed only when placement succeeds. The result includes Factorio-resolved connection geometry and actual connection targets. Use find_connection_placement whenever this entity must connect at a particular location, and use walk_to_for_placement before a distant exact placement.",
            PlacementParameters(false)),
        FunctionTool(
            "set_assembler_recipe",
            "Set the enabled recipe on a reachable assembling machine, chemical plant, or oil refinery. Any ingredients ejected by changing recipes are returned to the character or safely spilled beside the machine if the inventory is full.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"recipe", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                }},
                {"required", {"x", "y", "recipe"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "insert_item_into_entity",
            "Move items from the controlled character into a reachable production entity. Use inventory='input' for furnace or assembler ingredients and lab science packs. Use inventory='fuel' for any burner entity, including furnaces, mining drills, boilers, and burner inserters.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"item", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
                    {"inventory", {{"type", "string"}, {"enum", {"fuel", "input"}}}},
                }},
                {"required", {"x", "y", "item", "count", "inventory"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "take_item_from_entity",
            "Move items from a reachable production entity into the controlled character's inventory. Supports lab or crafting input, burner fuel, and output from furnaces or assemblers. Mining drills emit onto the ground or transport belts rather than exposing a retrievable output inventory.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"item", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
                    {"inventory", {{"type", "string"}, {"enum", {"fuel", "input", "output"}}}},
                }},
                {"required", {"x", "y", "item", "count", "inventory"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "wait_for_machine_output",
            "Wait inside one tool call until a furnace or crafting machine has the requested total output count. Use this after loading a production batch instead of repeatedly polling get_nearby_entities. Returns early if the target is met, the machine becomes idle or stalled, cancellation is requested, or the timeout expires.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"item", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
                    {"maximum_duration_seconds", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 600.0}}},
                }},
                {"required", {"x", "y", "item", "count", "maximum_duration_seconds"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "transfer_inventory_to_container",
            "Move as much of the controlled character's main inventory as possible into the reachable chest or logistic container at the given exact map coordinates. Reports any items left behind when the container is full.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                }},
                {"required", {"x", "y"}},
                {"additionalProperties", false},
            }),
    });
}

json FactorioTools::Definitions(bool includeScreenshot) const
{
    if (includeScreenshot)
        return definitions_;

    auto definitions = definitions_;
    for (auto definition = definitions.begin(); definition != definitions.end();)
    {
        if (definition->value("function", json::object()).value("name", std::string{}) == "take_screenshot")
            definition = definitions.erase(definition);
        else
            ++definition;
    }
    return definitions;
}

json FactorioTools::Execute(const std::string& name, const json& arguments, std::stop_token stopToken) const
{
    if (!arguments.is_object())
        throw std::runtime_error("Tool arguments must be a JSON object");
    if (name == "get_game_state")
    {
        RejectUnknownArguments(arguments, {});
        return GetGameState();
    }
    if (name == "get_inventory")
    {
        RejectUnknownArguments(arguments, {});
        return GetInventory();
    }
    if (name == "get_craftable_recipes")
    {
        RejectUnknownArguments(arguments, {});
        return GetCraftableRecipes();
    }
    if (name == "get_research_status")
    {
        RejectUnknownArguments(arguments, {"technology"});
        return GetResearchStatus(arguments);
    }
    if (name == "start_research")
    {
        RejectUnknownArguments(arguments, {"technology"});
        return StartResearch(arguments);
    }
    if (name == "get_nearby_entities")
    {
        RejectUnknownArguments(arguments, {"radius", "name", "type", "regex", "offset"});
        return GetNearbyEntities(arguments);
    }
    if (name == "get_construction_requests")
    {
        RejectUnknownArguments(arguments, {"radius", "kind", "offset", "available_only"});
        return GetConstructionRequests(arguments);
    }
    if (name == "build_ghosts")
    {
        RejectUnknownArguments(arguments, {"radius", "count", "maximum_duration_seconds"});
        return BuildGhosts(arguments, stopToken);
    }
    if (name == "deconstruct_marked")
    {
        RejectUnknownArguments(arguments, {"radius", "count", "maximum_duration_seconds"});
        return DeconstructMarked(arguments, stopToken);
    }
    if (name == "find_resource_patches")
    {
        RejectUnknownArguments(arguments, {"resource", "radius"});
        return FindResourcePatches(arguments);
    }
    if (name == "find_water")
    {
        RejectUnknownArguments(arguments, {"radius"});
        return FindWater(arguments);
    }
    if (name == "walk")
    {
        RejectUnknownArguments(arguments, {"direction", "duration_seconds"});
        return Walk(arguments, stopToken);
    }
    if (name == "walk_to")
    {
        RejectUnknownArguments(arguments, {"x", "y", "stopping_distance", "maximum_duration_seconds"});
        return WalkTo(arguments, stopToken);
    }
    if (name == "find_connection_placement")
    {
        RejectUnknownArguments(arguments, {"item", "target_x", "target_y", "connection", "search_radius"});
        return FindConnectionPlacement(arguments);
    }
    if (name == "walk_to_for_placement")
    {
        RejectUnknownArguments(arguments, {"item", "x", "y", "direction", "maximum_duration_seconds"});
        return WalkToForPlacement(arguments, stopToken);
    }
    if (name == "stop_walking")
    {
        RejectUnknownArguments(arguments, {});
        return StopWalking();
    }
    if (name == "take_screenshot")
    {
        RejectUnknownArguments(arguments, {"width", "height", "zoom"});
        return TakeScreenshot(arguments, stopToken);
    }
    if (name == "mine_entity")
    {
        RejectUnknownArguments(arguments, {"name", "count"});
        return MineEntity(arguments, stopToken);
    }
    if (name == "craft")
    {
        RejectUnknownArguments(arguments, {"recipe", "count"});
        return Craft(arguments, stopToken);
    }
    if (name == "place_entity")
    {
        RejectUnknownArguments(arguments, {"item", "x", "y", "direction"});
        return PlaceEntity(arguments);
    }
    if (name == "set_assembler_recipe")
    {
        RejectUnknownArguments(arguments, {"x", "y", "recipe"});
        return SetAssemblerRecipe(arguments);
    }
    if (name == "insert_item_into_entity")
    {
        RejectUnknownArguments(arguments, {"x", "y", "item", "count", "inventory"});
        return InsertItemIntoEntity(arguments);
    }
    if (name == "take_item_from_entity")
    {
        RejectUnknownArguments(arguments, {"x", "y", "item", "count", "inventory"});
        return TakeItemFromEntity(arguments);
    }
    if (name == "wait_for_machine_output")
    {
        RejectUnknownArguments(arguments, {"x", "y", "item", "count", "maximum_duration_seconds"});
        return WaitForMachineOutput(arguments, stopToken);
    }
    if (name == "transfer_inventory_to_container")
    {
        RejectUnknownArguments(arguments, {"x", "y"});
        return TransferInventoryToContainer(arguments);
    }
    throw std::runtime_error("Unknown FactorIA tool: " + name);
}

json FactorioTools::ExecuteJson(const std::string& body) const
{
    const std::string command =
        "/sc local function factoria_main() " + body + " end "
        "local ok,result=pcall(factoria_main) "
        "if ok then rcon.print(helpers.table_to_json({ok=true,result=result})) "
        "else rcon.print(helpers.table_to_json({ok=false,error=tostring(result)})) end";
    return ParseFactorioJson(executeCommand_(command));
}

std::string FactorioTools::ControlCharacterLua() const
{
    return
        "local iface=remote.interfaces['factoria_bridge'] "
        "if not iface or not iface.get_control_character then "
        "error('The updated FactorIA Bridge mod is required to select a control character') end "
        "local p=remote.call('factoria_bridge','get_control_character'," +
        std::string(useDedicatedCharacter_ ? "true" : "false") + ") ";
}

void FactorioTools::EnsurePlayerControlAction() const
{
    const auto actionName = std::string(runtime::PlayerControlActionName);
    const auto info = ExecuteJson(
        "local iface=remote.interfaces['factoria_bridge'] "
        "if not iface or not iface.runtime_info or not iface.begin_action_upload or "
        "not iface.append_action_upload or not iface.finish_action_upload or not iface.start_action or "
        "not iface.poll_action or not iface.stop_action then return {available=false} end "
        "local result=remote.call('factoria_bridge','runtime_info'," + LuaStringLiteral(actionName) + ") "
        "result.available=true return result");
    if (!info.value("available", false))
        throw std::runtime_error(
            "The updated FactorIA Bridge mod is required for smooth walking and mining");
    if (info.value("version", 0) != 2)
        throw std::runtime_error("The FactorIA Bridge runtime protocol version is not supported");
    if (playerControlActionReady_ && info.value("action_installed", false))
        return;

    ExecuteJson(
        "return remote.call('factoria_bridge','begin_action_upload'," + LuaStringLiteral(actionName) + ")");
    constexpr std::size_t UploadPartSize = 1536;
    for (std::size_t offset = 0; offset < runtime::PlayerControlSource.size(); offset += UploadPartSize)
    {
        const auto sourcePart = runtime::PlayerControlSource.substr(offset, UploadPartSize);
        ExecuteJson(
            "return remote.call('factoria_bridge','append_action_upload'," + LuaStringLiteral(actionName) + "," +
            LuaStringLiteral(sourcePart) + ")");
    }
    const auto installed = ExecuteJson(
        "return remote.call('factoria_bridge','finish_action_upload'," + LuaStringLiteral(actionName) + ")");
    if (!installed.value("installed", false))
        throw std::runtime_error("Factorio did not install the FactorIA player-control action");
    playerControlActionReady_ = true;
}

json FactorioTools::StartPlayerControlAction(const json& arguments) const
{
    EnsurePlayerControlAction();
    auto runtimeArguments = arguments;
    runtimeArguments["use_dedicated_character"] = useDedicatedCharacter_;
    return ExecuteJson(
        "local arguments=helpers.json_to_table(" + LuaStringLiteral(runtimeArguments.dump()) + ") "
        "return remote.call('factoria_bridge','start_action'," +
        LuaStringLiteral(runtime::PlayerControlActionName) + ",arguments)");
}

json FactorioTools::PollPlayerControlAction(std::uint64_t jobId) const
{
    return ExecuteJson(
        "return remote.call('factoria_bridge','poll_action'," + std::to_string(jobId) + ")");
}

json FactorioTools::StopPlayerControlAction(std::uint64_t jobId) const
{
    return ExecuteJson(
        "return remote.call('factoria_bridge','stop_action'," + std::to_string(jobId) + ")");
}

json FactorioTools::RecordResearchTriggerAction(
    const std::string& actionType,
    const std::string& prototypeName,
    const std::string& quality,
    double count) const
{
    return ExecuteJson(
        "local iface=remote.interfaces['factoria_bridge'] "
        "if not iface or not iface.record_research_trigger_action then "
        "return {available=false,reason='Updated FactorIA Bridge mod required for research trigger actions'} end "
        "local updates=remote.call('factoria_bridge','record_research_trigger_action'," +
        std::string(useDedicatedCharacter_ ? "true" : "false") + "," +
        LuaStringLiteral(actionType) + "," + LuaStringLiteral(prototypeName) + "," +
        LuaStringLiteral(quality) + "," + std::to_string(count) + ") "
        "return {available=true,updates=updates}");
}

json FactorioTools::WaitForPlayerControlAction(
    const json& startResult,
    std::chrono::steady_clock::time_point deadline,
    std::stop_token stopToken) const
{
    const auto jobId = startResult.value("job_id", std::uint64_t{0});
    if (jobId == 0)
        throw std::runtime_error("FactorIA Bridge returned an invalid runtime job id");

    auto extractResult = [](const json& envelope) {
        auto result = envelope.value("result", json::object());
        if (result.value("failed", false))
            throw std::runtime_error("FactorIA runtime action failed: " + result.value("error", "unknown error"));
        return result;
    };

    if (!startResult.value("active", false))
        return extractResult(startResult);

    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto status = PollPlayerControlAction(jobId);
        if (!status.value("active", false))
        {
            if (status.value("missing", false))
                throw std::runtime_error("FactorIA runtime job disappeared before completion");
            return extractResult(status);
        }
    }

    auto result = extractResult(StopPlayerControlAction(jobId));
    result["stopped"] = stopToken.stop_requested();
    result["timed_out"] = !stopToken.stop_requested();
    return result;
}

json FactorioTools::GetGameState() const
{
    return ExecuteJson(
        ControlCharacterLua() +
        "local bridge=remote.interfaces['factoria_bridge'] "
        "local connected=p.player "
        "return {tick=game.tick,player={name=connected and connected.name or 'FactorIA AI',"
        "dedicated=" + std::string(useDedicatedCharacter_ ? "true" : "false") + ","
        "position={x=p.position.x,y=p.position.y},health=p.health,max_health=p.max_health,"
        "walking=p.walking_state.walking,direction=p.walking_state.direction,"
        "mining=p.mining_state.mining,selected=p.selected and p.selected.name or nil,"
        "cursor_item=p.cursor_stack and p.cursor_stack.valid_for_read and p.cursor_stack.name or nil},"
        "crafting_queue_size=p.crafting_queue_size,"
        "rockets_launched=p.force.rockets_launched,items_launched=p.force.items_launched,"
        "rocket_launch_confirmed=p.force.rockets_launched>0,"
        "environment={surface=p.surface.name,daytime=p.surface.daytime,darkness=p.surface.darkness,"
        "pollution=p.surface.get_pollution(p.position)},"
        "bridge_mod_available=bridge~=nil,"
        "bridge_runtime_available=bridge~=nil and bridge.runtime_info~=nil and bridge.start_action~=nil,"
        "bridge_research_trigger_actions_available=bridge~=nil and "
        "bridge.record_research_trigger_action~=nil and bridge.research_trigger_progress~=nil}");
}

json FactorioTools::GetInventory() const
{
    auto result = ExecuteJson(
        ControlCharacterLua() +
        "local inv=p.get_main_inventory() local items={} "
        "if inv then for _,item in pairs(inv.get_contents()) do "
        "items[#items+1]={name=item.name,count=item.count,quality=tostring(item.quality)} end end "
        "table.sort(items,function(a,b) return a.name<b.name end) return {items=items}");
    if (!result.value("items", json::array()).is_array())
        result["items"] = json::array();
    return result;
}

json FactorioTools::GetCraftableRecipes() const
{
    auto result = ExecuteJson(
        ControlCharacterLua() +
        "local recipes={} for name,recipe in pairs(p.force.recipes) do "
        "if recipe.enabled and name~='recipe-unknown' then local count=p.get_craftable_count(recipe) if count>0 then "
        "recipes[#recipes+1]={name=name,craftable_count=count} end end end "
        "table.sort(recipes,function(a,b) return a.name<b.name end) "
        "local total=#recipes while #recipes>100 do table.remove(recipes) end "
        "return {recipes=recipes,total_count=total,truncated=total>#recipes}");
    if (!result.value("recipes", json::array()).is_array())
        result["recipes"] = json::array();
    return result;
}

json FactorioTools::GetResearchStatus(const json& arguments) const
{
    const auto requestedTechnology = PrototypeNameOrEmpty(arguments, "technology");

    auto result = ExecuteJson(
        ControlCharacterLua() + ResearchHelpersLua(useDedicatedCharacter_) +
        "local force=p.force local queue,queued=factoria_research_queue(force) "
        + (requestedTechnology.empty()
            ? std::string(
                "local available={} local triggered={} for _,technology in pairs(force.technologies) do "
                "local info=factoria_technology_info(technology) "
                "if info.startable and not queued[technology.name] then available[#available+1]=info "
                "elseif info.available and info.research_trigger then triggered[#triggered+1]=info end end "
                "table.sort(available,function(a,b) return a.name<b.name end) "
                "table.sort(triggered,function(a,b) return a.name<b.name end) local total=#available "
                "while #available>80 do table.remove(available) end "
                "local trigger_total=#triggered while #triggered>80 do table.remove(triggered) end "
                "local current=force.current_research and factoria_technology_info(force.current_research) or nil "
                "if current then current.progress=force.research_progress end "
                "return {research_enabled=force.research_enabled,current=current,queue=queue,"
                "available=available,total_available=total,truncated=total>#available,"
                "triggered_milestones=triggered,total_triggered_milestones=trigger_total,"
                "triggered_milestones_truncated=trigger_total>#triggered}")
            : "local technology=force.technologies[" + LuaStringLiteral(requestedTechnology) + "] "
                "if not technology then error('Unknown technology: " + requestedTechnology + "') end "
                "local info=factoria_technology_info(technology) info.queued=queued[technology.name] or false "
                "if force.current_research==technology then info.progress=force.research_progress end "
                "return {research_enabled=force.research_enabled,technology=info,queue=queue}"));
    for (const auto field : {"queue", "available", "triggered_milestones"})
    {
        const auto value = result.find(field);
        if (value != result.end() && !value->is_array())
            result[field] = json::array();
    }
    return result;
}

json FactorioTools::StartResearch(const json& arguments) const
{
    const auto technologyName = RequiredPrototypeName(arguments, "technology");
    return ExecuteJson(
        ControlCharacterLua() + ResearchHelpersLua(useDedicatedCharacter_) +
        "local force=p.force if not force.research_enabled then error('Research is disabled for this force') end "
        "local technology=force.technologies[" + LuaStringLiteral(technologyName) + "] "
        "if not technology then error('Unknown technology: " + technologyName + "') end "
        "local info=factoria_technology_info(technology) local queue,queued=factoria_research_queue(force) "
        "if technology.researched then return {accepted=false,reason='already_researched',technology=info,queue=queue} end "
        "if not technology.enabled then return {accepted=false,reason='technology_disabled',technology=info,queue=queue} end "
        "if #info.missing_prerequisites>0 then "
        "return {accepted=false,reason='missing_prerequisites',technology=info,queue=queue} end "
        "if info.research_trigger then "
        "return {accepted=false,reason='trigger_action_required',technology=info,queue=queue} end "
        "local already_queued=queued[technology.name] or false "
        "if not already_queued then local updated={} for _,entry in ipairs(force.research_queue) do "
        "updated[#updated+1]=entry end updated[#updated+1]=technology force.research_queue=updated end "
        "queue,queued=factoria_research_queue(force) local accepted=queued[technology.name] or false "
        "local current=force.current_research and force.current_research.name or nil "
        "return {accepted=accepted,already_queued=already_queued,technology=info,"
        "current_research=current,research_progress=force.research_progress,queue=queue,"
        "reason=accepted and (already_queued and 'already_queued' or 'queued') or 'rejected_by_factorio'}");
}

json FactorioTools::GetNearbyEntities(const json& arguments) const
{
    const auto radius = RequiredNumber(arguments, "radius", 1.0, MaximumSearchRadius);
    auto nameFilter = HasArgumentValue(arguments, "name")
        ? " filter.name=\"" + RequiredPrototypeName(arguments, "name") + "\" "
        : std::string{};
    const auto typeFilter = HasArgumentValue(arguments, "type")
        ? " filter.type=\"" + RequiredPrototypeName(arguments, "type") + "\" "
        : std::string{};
    const auto offset = IntegerOrDefault(arguments, "offset", 0, 0, 10000);

    std::string regexResultField;
    if (HasArgumentValue(arguments, "regex"))
    {
        const auto expression = RequiredText(arguments, "regex", 128);
        std::regex matcher;
        try
        {
            matcher = std::regex(
                expression,
                std::regex_constants::ECMAScript | std::regex_constants::icase);
        }
        catch (const std::regex_error& error)
        {
            throw std::runtime_error("Argument regex is not a valid ECMAScript expression: " +
                std::string(error.what()));
        }

        // Factorio exposes Lua patterns rather than regular expressions. Fetch only the distinct
        // nearby prototypes, match them locally, then give Factorio the exact matching name set.
        const auto catalog = ExecuteJson(
            ControlCharacterLua() +
            "local filter={position=p.position,radius=" + std::to_string(radius) + "} " +
            nameFilter + typeFilter +
            "local found=p.surface.find_entities_filtered(filter) local seen={} local entries={} "
            "for _,e in ipairs(found) do if e~=p and not seen[e.name] then seen[e.name]=true "
            "entries[#entries+1]={name=e.name,type=e.type} end end "
            "return {player_position={x=p.position.x,y=p.position.y},prototypes=entries}");

        json matchingNames = json::array();
        for (const auto& prototype : catalog.value("prototypes", json::array()))
        {
            const auto name = prototype.value("name", std::string{});
            const auto type = prototype.value("type", std::string{});
            if (std::regex_search(name, matcher) || std::regex_search(type, matcher))
                matchingNames.push_back(name);
        }

        if (matchingNames.empty())
        {
            return {
                {"radius", radius},
                {"player_position", catalog.value("player_position", json::object())},
                {"coordinate_hint", "positive x is east; positive y is south"},
                {"regex", expression},
                {"offset", offset},
                {"total_entities", 0},
                {"distinct_prototypes", 0},
                {"truncated", false},
                {"entities", json::array()},
            };
        }

        nameFilter = " filter.name=helpers.json_to_table(" +
            LuaStringLiteral(matchingNames.dump()) + ") ";
        regexResultField = "regex=" + LuaStringLiteral(expression) + ",";
    }

    auto result = ExecuteJson(
        ControlCharacterLua() + EntityConnectionDetailsLua() +
        "local function summarize(inv,limit) if not inv then return nil end local items={} "
        "for _,item in pairs(inv.get_contents()) do items[#items+1]={name=item.name,count=item.count} end "
        "table.sort(items,function(a,b) return a.count>b.count end) "
        "while #items>limit do table.remove(items) end return items end "
        "local function attach_inventory(info,name,inv,limit) if not inv then return end "
        "info.inventory=info.inventory or {} info.inventory[name]={items=summarize(inv,limit),"
        "empty=inv.is_empty(),full=inv.is_full()} end "
        "local filter={position=p.position,radius=" + std::to_string(radius) + "} " +
        nameFilter + typeFilter +
        "local found=p.surface.find_entities_filtered(filter) "
        "table.sort(found,function(a,b) local adx=a.position.x-p.position.x local ady=a.position.y-p.position.y "
        "local bdx=b.position.x-p.position.x local bdy=b.position.y-p.position.y "
        "return adx*adx+ady*ady<bdx*bdx+bdy*bdy end) "
        "local counts={} local total=0 local distinct=0 for _,e in ipairs(found) do if e~=p then "
        "if not counts[e.name] then counts[e.name]=0 distinct=distinct+1 end "
        "counts[e.name]=counts[e.name]+1 total=total+1 end end "
        "local ordered={} local duplicates={} local seen={} for _,e in ipairs(found) do if e~=p then "
        "if not seen[e.name] then seen[e.name]=true ordered[#ordered+1]=e else duplicates[#duplicates+1]=e end end end "
        "for _,e in ipairs(duplicates) do ordered[#ordered+1]=e end "
        "local offset=" + std::to_string(offset) + " local entities={} "
        "for index=offset+1,#ordered do if #entities>=40 then break end local e=ordered[index] "
        "local dx=e.position.x-p.position.x local dy=e.position.y-p.position.y "
        "local info={name=e.name,type=e.type,force=e.force and e.force.name or nil,"
        "position={x=e.position.x,y=e.position.y},delta={x=dx,y=dy},"
        "distance=math.sqrt(dx*dx+dy*dy),direction=e.direction,"
        "direction_name=factoria_direction_name(e.direction),health=e.health,"
        "minable=e.minable,reachable=p.can_reach_entity(e),prototype_count_in_radius=counts[e.name]} "
        "if e.status then for name,value in pairs(defines.entity_status) do "
        "if value==e.status then info.status=name break end end end "
        "if e.type=='resource' then info.amount=e.amount "
        "elseif e.type=='container' or e.type=='logistic-container' then "
        "attach_inventory(info,'chest',e.get_inventory(defines.inventory.chest),6) end "
        "local fuel=e.get_fuel_inventory() attach_inventory(info,'fuel',fuel,4) "
        "if e.type=='lab' then attach_inventory(info,'input',e.get_inventory(defines.inventory.lab_input),6) "
        "elseif e.type=='furnace' or e.type=='assembling-machine' then "
        "attach_inventory(info,'input',e.get_inventory(defines.inventory.crafter_input),6) "
        "attach_inventory(info,'output',e.get_inventory(defines.inventory.crafter_output),4) "
        "info.crafting=e.is_crafting() local recipe=e.get_recipe() info.recipe=recipe and recipe.name or nil "
        "if info.crafting then info.wait_tool='wait_for_machine_output' end end "
        "factoria_attach_connection_details(info,e) "
        "entities[#entities+1]=info end "
        "return {radius=" + std::to_string(radius) + ",player_position={x=p.position.x,y=p.position.y},"
        "coordinate_hint='positive x is east; positive y is south',"
        + regexResultField +
        "ordering='nearest representative of each distinct prototype first, then nearest duplicate instances',"
        "offset=offset,total_entities=total,distinct_prototypes=distinct,truncated=offset+#entities<total,"
        "next_offset=offset+#entities<total and offset+#entities or nil,entities=entities}");
    if (!result.value("entities", json::array()).is_array())
        result["entities"] = json::array();
    return result;
}

json FactorioTools::GetConstructionRequests(const json& arguments) const
{
    const auto radius = RequiredNumber(arguments, "radius", 1.0, MaximumSearchRadius);
    const auto kind = HasArgumentValue(arguments, "kind")
        ? RequiredText(arguments, "kind", 16)
        : std::string("all");
    if (kind != "all" && kind != "build" && kind != "deconstruct")
        throw std::runtime_error("kind must be all, build, or deconstruct");
    const auto offset = IntegerOrDefault(arguments, "offset", 0, 0, 10000);
    const auto availableOnly = BooleanOrDefault(arguments, "available_only", false);

    auto result = ExecuteJson(
        ControlCharacterLua() + EntityConnectionDetailsLua() +
        "local radius=" + std::to_string(radius) + " local kind=" + LuaStringLiteral(kind) + " "
        "local available_only=" + std::string(availableOnly ? "true" : "false") + " "
        "local inventory=p.get_main_inventory() local requests={} local missing={} "
        "local function distance_to(entity) local dx=entity.position.x-p.position.x "
        "local dy=entity.position.y-p.position.y return math.sqrt(dx*dx+dy*dy),dx,dy end "
        "if kind=='all' or kind=='build' then "
        "local ghosts=p.surface.find_entities_filtered{position=p.position,radius=radius,"
        "type='entity-ghost',force=p.force} "
        "for _,ghost in ipairs(ghosts) do if ghost.valid then "
        "local prototype=prototypes.entity[ghost.ghost_name] "
        "local quality=ghost.quality and ghost.quality.name or 'normal' "
        "local required_item=nil local required_count=1 local selected_item=nil local item_count=0 "
        "for _,item in ipairs(prototype and prototype.items_to_place_this or {}) do "
        "local count=inventory and inventory.get_item_count{name=item.name,quality=quality} or 0 "
        "if not required_item then required_item=item.name required_count=item.count or 1 item_count=count end "
        "if not selected_item and count>=(item.count or 1) then selected_item=item.name "
        "required_count=item.count or 1 item_count=count end end "
        "local distance,dx,dy=distance_to(ghost) local available=selected_item~=nil "
        "if not available and required_item then local key=required_item..':'..quality "
        "local entry=missing[key] or {name=required_item,quality=quality,count=0} "
        "entry.count=entry.count+required_count missing[key]=entry end "
        "requests[#requests+1]={kind='build',entity_name=ghost.ghost_name,ghost_type=ghost.ghost_type,"
        "position={x=ghost.position.x,y=ghost.position.y},delta={x=dx,y=dy},distance=distance,"
        "direction=ghost.direction,direction_name=factoria_direction_name(ghost.direction),quality=quality,"
        "item=selected_item or required_item,item_count=item_count,required_count=required_count,"
        "available=available,reachable=p.can_reach_entity(ghost)} end end end "
        "if kind=='all' or kind=='deconstruct' then "
        "local marked=p.surface.find_entities_filtered{position=p.position,radius=radius,"
        "force=p.force,to_be_deconstructed=true} "
        "for _,entity in ipairs(marked) do if entity.valid and entity~=p and entity.type~='entity-ghost' "
        "and entity.to_be_deconstructed(p.force) then "
        "local distance,dx,dy=distance_to(entity) requests[#requests+1]={kind='deconstruct',"
        "entity_name=entity.name,entity_type=entity.type,position={x=entity.position.x,y=entity.position.y},"
        "delta={x=dx,y=dy},distance=distance,direction=entity.direction,"
        "direction_name=factoria_direction_name(entity.direction),minable=entity.minable,"
        "available=entity.minable,reachable=entity.minable and p.can_reach_entity(entity) or false} end end end "
        "table.sort(requests,function(a,b) if a.distance~=b.distance then return a.distance<b.distance end "
        "if a.kind~=b.kind then return a.kind<b.kind end if a.entity_name~=b.entity_name then "
        "return a.entity_name<b.entity_name end if a.position.x~=b.position.x then "
        "return a.position.x<b.position.x end return a.position.y<b.position.y end) "
        "local total_all=#requests local available_count=0 local filtered={} "
        "for _,request in ipairs(requests) do if request.available then available_count=available_count+1 end "
        "if not available_only or request.available then filtered[#filtered+1]=request end end "
        "local total=#filtered local page={} local offset=" + std::to_string(offset) + " "
        "for index=offset+1,total do if #page>=40 then break end page[#page+1]=filtered[index] end "
        "local missing_items={} for _,entry in pairs(missing) do missing_items[#missing_items+1]=entry end "
        "table.sort(missing_items,function(a,b) if a.name~=b.name then return a.name<b.name end "
        "return a.quality<b.quality end) "
        "return {radius=radius,kind=kind,available_only=available_only,"
        "player_position={x=p.position.x,y=p.position.y},coordinate_hint='positive x is east; positive y is south',"
        "offset=offset,total_requests=total,total_requests_before_filter=total_all,"
        "available_count=available_count,truncated=offset+#page<total,"
        "next_offset=offset+#page<total and offset+#page or nil,missing_items=missing_items,requests=page}");
    for (const auto field : {"missing_items", "requests"})
    {
        if (!result.value(field, json::array()).is_array())
            result[field] = json::array();
    }
    return result;
}

json FactorioTools::BuildGhosts(const json& arguments, std::stop_token stopToken) const
{
    return ServiceConstructionRequests(arguments, "build", stopToken);
}

json FactorioTools::DeconstructMarked(const json& arguments, std::stop_token stopToken) const
{
    return ServiceConstructionRequests(arguments, "deconstruct", stopToken);
}

json FactorioTools::ServiceConstructionRequests(
    const json& arguments,
    const std::string& kind,
    std::stop_token stopToken) const
{
    const auto batch = RequiredConstructionBatch(arguments);
    const auto deadline = DeadlineAfter(batch.maximumSeconds);
    json attempts = json::array();
    json latestObservation = json::object();
    int completedCount = 0;
    std::string reason = "requested_count_met";

    auto performRequest = [&](const json& request) {
        if (kind == "build")
            return BuildGhostAt(request);

        auto miningRequest = request;
        miningRequest["maximum_duration_seconds"] = std::clamp(RemainingSeconds(deadline), 1.0, 120.0);
        return MineMarkedEntity(miningRequest, stopToken);
    };

    while (completedCount < batch.count)
    {
        if (stopToken.stop_requested())
        {
            reason = "stopped";
            break;
        }
        if (RemainingSeconds(deadline) < 1.0)
        {
            reason = "timed_out";
            break;
        }

        latestObservation = GetConstructionRequests({
            {"radius", batch.radius},
            {"kind", kind},
            {"offset", 0},
            {"available_only", true},
        });
        const auto requests = latestObservation.value("requests", json::array());
        if (requests.empty())
        {
            reason = latestObservation.value("total_requests_before_filter", 0) == 0
                ? "no_requests"
                : "no_available_requests";
            break;
        }

        const auto& request = requests.front();
        auto actionResult = performRequest(request);
        json navigation = json::object();
        if (actionResult.value("out_of_reach", false))
        {
            const auto remaining = RemainingSeconds(deadline);
            if (remaining < 1.0)
            {
                reason = "timed_out";
                break;
            }
            const auto& position = request.at("position");
            navigation = WalkTo(
                {
                    {"x", position.at("x")},
                    {"y", position.at("y")},
                    {"stopping_distance", 2.0},
                    {"maximum_duration_seconds", std::clamp(remaining, 1.0, 120.0)},
                },
                stopToken);
            if (stopToken.stop_requested())
            {
                attempts.push_back({{"request", request}, {"navigation", navigation}, {"action", actionResult}});
                reason = "stopped";
                break;
            }
            if (!navigation.value("reached", false))
            {
                attempts.push_back({{"request", request}, {"navigation", navigation}, {"action", actionResult}});
                reason = "unreachable_request";
                break;
            }
            actionResult = performRequest(request);
        }

        json attempt{{"request", request}, {"action", actionResult}};
        if (!navigation.empty())
            attempt["navigation"] = std::move(navigation);
        attempts.push_back(std::move(attempt));

        const auto completed = kind == "build"
            ? actionResult.value("fulfilled", false)
            : actionResult.value("mined", false);
        if (!completed)
        {
            reason = actionResult.value("reason", "request_failed");
            break;
        }
        ++completedCount;
    }

    if (!stopToken.stop_requested() && RemainingSeconds(deadline) > 0.0)
    {
        latestObservation = GetConstructionRequests({
            {"radius", batch.radius},
            {"kind", kind},
            {"offset", 0},
            {"available_only", false},
        });
    }

    return {
        {"kind", kind},
        {"requested_count", batch.count},
        {"completed_count", completedCount},
        {"target_met", completedCount >= batch.count},
        {"stopped", stopToken.stop_requested()},
        {"timed_out", reason == "timed_out"},
        {"reason", reason},
        {"maximum_duration_seconds", batch.maximumSeconds},
        {"attempts", std::move(attempts)},
        {"remaining_requests", latestObservation.value("total_requests", 0)},
        {"remaining_available", latestObservation.value("available_count", 0)},
        {"missing_items", latestObservation.value("missing_items", json::array())},
    };
}

json FactorioTools::FindResourcePatches(const json& arguments) const
{
    const auto resource = RequiredPrototypeName(arguments, "resource");
    const auto radius = RequiredNumber(arguments, "radius", 32.0, MaximumSearchRadius);
    const auto nameFilter = resource == "any"
        ? std::string{}
        : " filter.name=\"" + resource + "\" ";
    auto result = ExecuteJson(
        ControlCharacterLua() +
        "local filter={position=p.position,radius=" + std::to_string(radius) + ",type='resource'} " +
        nameFilter +
        "local found=p.surface.find_entities_filtered(filter) local cell_size=32 local grouped={} "
        "for _,e in pairs(found) do "
        "local cell_x=math.floor(e.position.x/cell_size) local cell_y=math.floor(e.position.y/cell_size) "
        "local key=e.name..':'..cell_x..':'..cell_y local patch=grouped[key] "
        "local dx=e.position.x-p.position.x local dy=e.position.y-p.position.y local distance=math.sqrt(dx*dx+dy*dy) "
        "if not patch then patch={resource=e.name,count=0,total_amount=0,sum_x=0,sum_y=0,"
        "nearest_distance=distance,nearest_position={x=e.position.x,y=e.position.y},"
        "bounds={left=e.position.x,right=e.position.x,top=e.position.y,bottom=e.position.y}} grouped[key]=patch end "
        "patch.count=patch.count+1 patch.total_amount=patch.total_amount+(e.amount or 0) "
        "patch.sum_x=patch.sum_x+e.position.x patch.sum_y=patch.sum_y+e.position.y "
        "patch.bounds.left=math.min(patch.bounds.left,e.position.x) patch.bounds.right=math.max(patch.bounds.right,e.position.x) "
        "patch.bounds.top=math.min(patch.bounds.top,e.position.y) patch.bounds.bottom=math.max(patch.bounds.bottom,e.position.y) "
        "if distance<patch.nearest_distance then patch.nearest_distance=distance "
        "patch.nearest_position={x=e.position.x,y=e.position.y} end end "
        "local patches={} for _,patch in pairs(grouped) do "
        "patch.center={x=patch.sum_x/patch.count,y=patch.sum_y/patch.count} "
        "patch.sum_x=nil patch.sum_y=nil "
        "patch.delta={x=patch.nearest_position.x-p.position.x,y=patch.nearest_position.y-p.position.y} "
        "patches[#patches+1]=patch end "
        "table.sort(patches,function(a,b) return a.nearest_distance<b.nearest_distance end) "
        "while #patches>24 do table.remove(patches) end "
        "return {requested_resource=\"" + resource + "\",radius=" + std::to_string(radius) +
        ",player_position={x=p.position.x,y=p.position.y},coordinate_hint='positive x is east; positive y is south',"
        "grouping='32-tile survey cells; adjacent results can belong to one physical patch',patches=patches} ");
    if (!result.value("patches", json::array()).is_array())
        result["patches"] = json::array();
    return result;
}

json FactorioTools::FindWater(const json& arguments) const
{
    const auto radius = RequiredNumber(arguments, "radius", 32.0, MaximumSearchRadius);
    auto result = ExecuteJson(
        ControlCharacterLua() +
        "local radius=" + std::to_string(radius) + " local water_names={} "
        "for name,prototype in pairs(prototypes.tile) do "
        "if prototype.fluid and prototype.fluid.name=='water' then water_names[#water_names+1]=name end end "
        "table.sort(water_names) "
        "local base={radius=radius,player_position={x=p.position.x,y=p.position.y},"
        "coordinate_hint='positive x is east; positive y is south',terrain_scope='generated terrain only',"
        "water_tile_prototypes=water_names} "
        "if #water_names==0 then base.found=false base.reason='No tile prototype produces water for an offshore pump' "
        "return base end "
        "local function contains_water(search_radius) "
        "return p.surface.count_tiles_filtered{position=p.position,radius=search_radius,name=water_names,limit=1}>0 end "
        "local lower=0 local upper=math.min(32,radius) "
        "while upper<radius and not contains_water(upper) do lower=upper upper=math.min(radius,upper*2) end "
        "if not contains_water(upper) then base.found=false "
        "base.reason='No water was found in generated terrain within the requested radius; explore or search a larger radius' "
        "return base end "
        "for _=1,14 do if upper-lower<=0.25 then break end local middle=(lower+upper)/2 "
        "if contains_water(middle) then upper=middle else lower=middle end end "
        "local tiles=p.surface.find_tiles_filtered{position=p.position,radius=math.min(radius,upper+2),"
        "name=water_names,limit=256} "
        "table.sort(tiles,function(a,b) local adx=a.position.x+0.5-p.position.x "
        "local ady=a.position.y+0.5-p.position.y local bdx=b.position.x+0.5-p.position.x "
        "local bdy=b.position.y+0.5-p.position.y return adx*adx+ady*ady<bdx*bdx+bdy*bdy end) "
        "local offsets={{x=0,y=-1,direction='north'},{x=1,y=0,direction='east'},"
        "{x=0,y=1,direction='south'},{x=-1,y=0,direction='west'}} "
        "local pump_directions={{name='north',value=defines.direction.north},"
        "{name='east',value=defines.direction.east},{name='south',value=defines.direction.south},"
        "{name='west',value=defines.direction.west}} "
        "local shorelines={} local seen={} "
        "for _,tile in ipairs(tiles) do if #shorelines>=16 then break end "
        "local water={x=tile.position.x+0.5,y=tile.position.y+0.5} "
        "for _,offset in ipairs(offsets) do local land={x=water.x+offset.x,y=water.y+offset.y} "
        "local land_tile=p.surface.get_tile(land) "
        "if not land_tile.prototype.fluid and not land_tile.collides_with('player') then "
        "local key=land.x..':'..land.y if not seen[key] then seen[key]=true "
        "local walk=p.surface.find_non_colliding_position(p.name,land,0.49,0.1) "
        "if walk then local dx=walk.x-p.position.x local dy=walk.y-p.position.y "
        "local shoreline={water_tile=tile.name,fluid=tile.prototype.fluid.name,"
        "water_position=water,shore_position={x=land.x,y=land.y},walk_position={x=walk.x,y=walk.y},"
        "water_from_shore=({north='south',east='west',south='north',west='east'})[offset.direction],"
        "distance=math.sqrt(dx*dx+dy*dy)} "
        "if prototypes.entity['offshore-pump'] then for _,pump_direction in ipairs(pump_directions) do "
        "if p.surface.can_place_entity{name='offshore-pump',position=land,direction=pump_direction.value,"
        "force=p.force,build_check_type=defines.build_check_type.manual} then "
        "shoreline.offshore_pump_placement={item='offshore-pump',x=land.x,y=land.y,"
        "direction=pump_direction.name} break end end end "
        "shorelines[#shorelines+1]=shoreline end end end end end "
        "table.sort(shorelines,function(a,b) return a.distance<b.distance end) "
        "base.found=#tiles>0 base.nearest_water_distance=upper base.shorelines=shorelines "
        "if #shorelines==0 then base.reason='Water was found, but no adjacent walkable shoreline was found in the nearest sample' end "
        "return base");
    for (const auto field : {"water_tile_prototypes", "shorelines"})
    {
        const auto value = result.find(field);
        if (value != result.end() && !value->is_array())
            result[field] = json::array();
    }
    return result;
}

json FactorioTools::Walk(const json& arguments, std::stop_token stopToken) const
{
    const auto directionValue = arguments.find("direction");
    if (directionValue == arguments.end() || !directionValue->is_string())
        throw std::runtime_error("Missing string argument: direction");
    const auto direction = directionValue->get<std::string>();
    static_cast<void>(DirectionExpression(direction));
    const auto seconds = RequiredNumber(arguments, "duration_seconds", 0.1, 10.0);
    const auto start = StartPlayerControlAction({
        {"kind", "walk_direction"},
        {"direction", direction},
        {"duration_seconds", seconds},
    });
    auto result = WaitForPlayerControlAction(
        start,
        DeadlineAfter(seconds + 2.0),
        stopToken);
    result["walking_control"] = "factorio_bridge_runtime";
    return result;
}

json FactorioTools::RequestPath(
    double targetX,
    double targetY,
    double stoppingDistance,
    std::stop_token stopToken) const
{
    const auto request = ExecuteJson(
        "local iface=remote.interfaces['factoria_bridge'] "
        "if not iface or not iface.request_path or not iface.take_path_result then "
        "return {available=false} end "
        "local id=remote.call('factoria_bridge','request_path'," +
        std::string(useDedicatedCharacter_ ? "true" : "false") + "," +
        std::to_string(targetX) + "," + std::to_string(targetY) + "," +
        std::to_string(std::max(0.1, stoppingDistance - 0.4)) + ") return {available=true,id=id}");
    if (!request.value("available", false))
        return {{"available", false}};

    const auto id = request.value("id", 0U);
    if (id == 0U)
        throw std::runtime_error("FactorIA Bridge returned an invalid path request id");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline)
    {
        auto poll = ExecuteJson(
            "local result=remote.call('factoria_bridge','take_path_result'," + std::to_string(id) + ") "
            "if result then return {pending=false,result=result} end return {pending=true}");
        if (!poll.value("pending", true))
        {
            auto result = poll.value("result", json::object());
            result["available"] = true;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (stopToken.stop_requested())
        return {{"available", true}, {"stopped", true}};
    return {{"available", true}, {"timed_out", true}};
}

json FactorioTools::WalkTo(const json& arguments, std::stop_token stopToken) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto stoppingDistance = NumberOrDefault(arguments, "stopping_distance", 1.5, 0.5, 10.0);
    const auto maximumSeconds = NumberOrDefault(arguments, "maximum_duration_seconds", 60.0, 1.0, 120.0);
    EnsurePlayerControlAction();
    const auto pathResult = RequestPath(targetX, targetY, stoppingDistance, stopToken);
    const auto pathfinderAvailable = pathResult.value("available", false);
    if (!pathfinderAvailable)
        throw std::runtime_error("The FactorIA Bridge pathfinder is unavailable");
    if (pathfinderAvailable && !pathResult.value("found", false))
    {
        auto result = StopWalking();
        result["reached"] = false;
        result["unreachable"] = !pathResult.value("try_again_later", false) &&
            !pathResult.value("timed_out", false) && !pathResult.value("stopped", false);
        result["pathfinder_busy"] = pathResult.value("try_again_later", false);
        result["pathfinder_timed_out"] = pathResult.value("timed_out", false);
        result["stopped"] = pathResult.value("stopped", false);
        result["navigation"] = "factorio_pathfinder";
        return result;
    }

    json waypoints = json::array();
    for (const auto& waypoint : pathResult.value("path", json::array()))
    {
        const auto& position = waypoint.at("position");
        waypoints.push_back({{"x", position.at("x")}, {"y", position.at("y")}});
    }
    if (waypoints.empty())
        waypoints.push_back({{"x", targetX}, {"y", targetY}});

    const auto start = StartPlayerControlAction({
        {"kind", "walk_path"},
        {"waypoints", std::move(waypoints)},
        {"target", {{"x", targetX}, {"y", targetY}}},
        {"stopping_distance", stoppingDistance},
        {"maximum_duration_seconds", maximumSeconds},
    });
    auto result = WaitForPlayerControlAction(
        start,
        DeadlineAfter(maximumSeconds + 2.0),
        stopToken);
    result["navigation"] = "factorio_pathfinder";
    result["pathfinder_available"] = true;
    result["walking_control"] = "factorio_bridge_runtime";
    return result;
}

json FactorioTools::FindConnectionPlacement(const json& arguments) const
{
    const auto item = RequiredPrototypeName(arguments, "item");
    const auto targetX = RequiredNumber(arguments, "target_x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "target_y", -1000000.0, 1000000.0);
    const auto connection = RequiredText(arguments, "connection", 32);
    const auto searchRadius = NumberOrDefault(arguments, "search_radius", 8.0, 1.0, 64.0);
    static const std::vector<std::string_view> connectionKinds{
        "item-from-target",
        "item-to-target",
        "fluid",
        "belt-from-target",
        "belt-to-target",
        "electric",
        "heat",
        "wall",
    };
    if (std::find(connectionKinds.begin(), connectionKinds.end(), connection) == connectionKinds.end())
        throw std::runtime_error("Unknown spatial connection type: " + connection);

    return ExecuteJson(
        EntityLookupLua(
            ControlCharacterLua() + EntityConnectionDetailsLua(),
            targetX,
            targetY,
            "candidate.valid and candidate.force==p.force and candidate.type~='resource' "
                "and candidate.type~='item-entity' and candidate.type~='character'",
            "connectable entity",
            false) +
        "local item_name=" + LuaStringLiteral(item) + " local item=prototypes.item[item_name] "
        "if not item then error('Unknown item prototype: ' .. item_name) end "
        "local place_result=item.place_result "
        "if not place_result then error('Item cannot be placed as an entity: ' .. item_name) end "
        "local connection_kind=" + LuaStringLiteral(connection) + " "
        "local search_radius=" + std::to_string(searchRadius) + " "
        "local function position_info(position) if not position then return nil end "
        "return {x=position.x,y=position.y} end "
        "local function contains(entities,expected) for _,entity in pairs(entities or {}) do "
        "if entity==expected then return true end end return false end "
        "local function item_connection(candidate) "
        "if connection_kind=='item-from-target' then "
        "if factoria_optional_property(target,'drop_target')==candidate then return {kind='item',flow='target-to-new',"
        "endpoint_owner='target',endpoint=position_info(factoria_optional_property(target,'drop_position'))} end "
        "if factoria_optional_property(candidate,'pickup_target')==target then return {kind='item',flow='target-to-new',"
        "endpoint_owner='new',endpoint=position_info(factoria_optional_property(candidate,'pickup_position'))} end "
        "elseif connection_kind=='item-to-target' then "
        "if factoria_optional_property(candidate,'drop_target')==target then return {kind='item',flow='new-to-target',"
        "endpoint_owner='new',endpoint=position_info(factoria_optional_property(candidate,'drop_position'))} end "
        "if factoria_optional_property(target,'pickup_target')==candidate then return {kind='item',flow='new-to-target',"
        "endpoint_owner='target',endpoint=position_info(factoria_optional_property(target,'pickup_position'))} end end "
        "return nil end "
        "local function fluid_connection(candidate) "
        "if connection_kind~='fluid' then return nil end local owner_fluidbox=candidate.fluidbox "
        "if not owner_fluidbox then return nil end for index=1,#owner_fluidbox do "
        "for connection_index,pipe in ipairs(owner_fluidbox.get_pipe_connections(index)) do "
        "local owner=pipe.target and pipe.target.owner or nil if owner==target then return {kind='fluid',"
        "fluidbox_index=index,connection_index=connection_index,position=position_info(pipe.position),"
        "target_position=position_info(pipe.target_position),flow_direction=pipe.flow_direction} end end end "
        "return nil end "
        "local function belt_connection(candidate) "
        "if connection_kind~='belt-from-target' and connection_kind~='belt-to-target' then return nil end "
        "local candidate_neighbours=factoria_optional_property(candidate,'belt_neighbours') "
        "local target_neighbours=factoria_optional_property(target,'belt_neighbours') "
        "if connection_kind=='belt-from-target' and ((candidate_neighbours and "
        "contains(candidate_neighbours.inputs,target)) or (target_neighbours and "
        "contains(target_neighbours.outputs,candidate))) then return {kind='belt',flow='target-to-new'} end "
        "if connection_kind=='belt-to-target' and ((candidate_neighbours and "
        "contains(candidate_neighbours.outputs,target)) or (target_neighbours and "
        "contains(target_neighbours.inputs,candidate))) then return {kind='belt',flow='new-to-target'} end "
        "local candidate_underground=factoria_optional_property(candidate,'underground_belt_neighbour') or "
        "factoria_optional_property(candidate,'neighbours') "
        "if candidate_underground==target then local candidate_type=factoria_optional_property(candidate,'belt_to_ground_type') "
        "local target_type=factoria_optional_property(target,'belt_to_ground_type') "
        "if connection_kind=='belt-from-target' and target_type=='input' and candidate_type=='output' then "
        "return {kind='underground-belt',flow='target-to-new'} end "
        "if connection_kind=='belt-to-target' and candidate_type=='input' and target_type=='output' then "
        "return {kind='underground-belt',flow='new-to-target'} end end return nil end "
        "local function electric_connection(candidate) if connection_kind~='electric' then return nil end "
        "local candidate_network=factoria_optional_property(candidate,'electric_network_id') "
        "local target_network=factoria_optional_property(target,'electric_network_id') "
        "if candidate_network and candidate_network==target_network then return {kind='electric',"
        "network_id=candidate_network} end return nil end "
        "local function heat_connection(candidate) if connection_kind~='heat' then return nil end "
        "if contains(factoria_optional_property(candidate,'heat_neighbours'),target) or "
        "contains(factoria_optional_property(target,'heat_neighbours'),candidate) then return {kind='heat'} end return nil end "
        "local function wall_connection(candidate) if connection_kind~='wall' then return nil end "
        "local neighbours=factoria_optional_property(candidate,'wall_neighbours') or "
        "factoria_optional_property(candidate,'neighbours') "
        "if neighbours and (neighbours.north==target or neighbours.east==target or "
        "neighbours.south==target or neighbours.west==target) then return {kind='wall'} end return nil end "
        "local function find_connection(candidate) return item_connection(candidate) or fluid_connection(candidate) "
        "or belt_connection(candidate) or electric_connection(candidate) or heat_connection(candidate) "
        "or wall_connection(candidate) end "
        "local directions={{name='north',value=defines.direction.north,swap=false},"
        "{name='east',value=defines.direction.east,swap=true},"
        "{name='south',value=defines.direction.south,swap=false},"
        "{name='west',value=defines.direction.west,swap=true}} "
        "if not place_result.supports_direction then directions={directions[1]} end "
        "local candidates={} for _,direction in ipairs(directions) do "
        "local width=direction.swap and place_result.tile_height or place_result.tile_width "
        "local height=direction.swap and place_result.tile_width or place_result.tile_height "
        "local x_offset=width%2==0 and 0 or 0.5 local y_offset=height%2==0 and 0 or 0.5 "
        "local minimum_x=math.ceil(target.position.x-search_radius-x_offset) "
        "local maximum_x=math.floor(target.position.x+search_radius-x_offset) "
        "local minimum_y=math.ceil(target.position.y-search_radius-y_offset) "
        "local maximum_y=math.floor(target.position.y+search_radius-y_offset) "
        "for grid_x=minimum_x,maximum_x do for grid_y=minimum_y,maximum_y do "
        "local x=grid_x+x_offset local y=grid_y+y_offset local target_dx=x-target.position.x "
        "local target_dy=y-target.position.y if target_dx*target_dx+target_dy*target_dy<=search_radius*search_radius then "
        "local player_dx=x-p.position.x local player_dy=y-p.position.y candidates[#candidates+1]={"
        "x=x,y=y,direction=direction.name,direction_value=direction.value,"
        "distance_from_player=math.sqrt(player_dx*player_dx+player_dy*player_dy),"
        "distance_from_target=math.sqrt(target_dx*target_dx+target_dy*target_dy)} end end end end "
        "table.sort(candidates,function(a,b) if a.distance_from_target==b.distance_from_target then "
        "return a.distance_from_player<b.distance_from_player end return a.distance_from_target<b.distance_from_target end) "
        "local inventory=p.get_main_inventory() local source=inventory and inventory.find_item_stack(item_name) "
        "local quality=source and source.quality.name or 'normal' local options={} "
        "for _,candidate in ipairs(candidates) do if #options>=12 then break end "
        "local build={name=place_result.name,position={x=candidate.x,y=candidate.y},"
        "direction=candidate.direction_value,force=p.force,quality=quality,create_build_effect_smoke=false} "
        "if p.surface.can_place_entity{name=place_result.name,position=build.position,direction=build.direction,"
        "force=p.force,build_check_type=defines.build_check_type.manual} then "
        "local created,preview=pcall(function() return p.surface.create_entity(build) end) "
        "if created and preview then local inspected,option=pcall(function() "
        "pcall(function() preview.update_connections() end) pcall(function() target.update_connections() end) "
        "local matched=find_connection(preview) if not matched then return nil end "
        "local entity_info={name=preview.name,type=preview.type,"
        "position={x=preview.position.x,y=preview.position.y},direction_name=factoria_direction_name(preview.direction)} "
        "factoria_attach_connection_details(entity_info,preview) return {"
        "x=candidate.x,y=candidate.y,direction=candidate.direction,"
        "distance_from_player=candidate.distance_from_player,"
        "distance_from_target=candidate.distance_from_target,connection=matched,preview=entity_info} end) "
        "preview.destroy() pcall(function() target.update_connections() end) "
        "if not inspected then error(option) end if option then options[#options+1]=option end end end end "
        "table.sort(options,function(a,b) return a.distance_from_player<b.distance_from_player end) "
        "local result={found=#options>0,item=item_name,entity_name=place_result.name,"
        "item_count=inventory and inventory.get_item_count(item_name) or 0,"
        "connection=connection_kind,search_radius=search_radius,target=factoria_entity_reference(target),"
        "options=options,preview_entities_removed=true} "
        "if #options>0 then local recommended=options[1] local next_arguments={item=item_name,x=recommended.x,"
        "y=recommended.y,direction=recommended.direction} result.recommended=recommended "
        "result.next_tool={name='walk_to_for_placement',arguments=next_arguments} "
        "result.verification='After placement, require the reported connection target or network to match this target.' "
        "else result.reason='no_valid_connected_placement_in_search_radius' end return result");
}

json FactorioTools::WalkToForPlacement(const json& arguments, std::stop_token stopToken) const
{
    const auto request = RequiredPlacementRequest(arguments);
    const auto direction = DirectionExpression(request.direction);
    const auto maximumSeconds = NumberOrDefault(
        arguments,
        "maximum_duration_seconds",
        60.0,
        1.0,
        120.0);

    auto plan = ExecuteJson(
        ControlCharacterLua() +
        "local item_name=" + LuaStringLiteral(request.item) + " local item=prototypes.item[item_name] "
        "if not item then error('Unknown item prototype: ' .. item_name) end "
        "local place_result=item.place_result "
        "if not place_result then error('Item cannot be placed as an entity: ' .. item_name) end "
        "local requested={x=" + std::to_string(request.x) + ",y=" + std::to_string(request.y) + "} "
        "local direction=" + direction + " local build={name=place_result.name,position=requested,direction=direction} "
        "local inventory=p.get_main_inventory() local result={item=item_name,entity_name=place_result.name,"
        "requested_position=requested,requested_direction=" + LuaStringLiteral(request.direction) + ","
        "current_position={x=p.position.x,y=p.position.y},build_distance=p.build_distance,"
        "item_count=inventory and inventory.get_item_count(item_name) or 0,"
        "target_valid_before_move=p.surface.can_place_entity{name=place_result.name,position=requested,"
        "direction=direction,force=p.force,build_check_type=defines.build_check_type.manual},"
        "placement_ready=p.can_place_entity(build)} "
        "if result.placement_ready then result.candidates={} return result end "
        "local box=place_result.collision_box "
        "local extent=math.max(math.abs(box.left_top.x),math.abs(box.left_top.y),"
        "math.abs(box.right_bottom.x),math.abs(box.right_bottom.y)) "
        "local inner=extent+1.25 local outer=extent+math.max(2,math.min(4,p.build_distance-0.5)) "
        "if outer<inner then outer=inner end local candidates={} local seen={} "
        "for ring=inner,outer,0.75 do for index=0,15 do local angle=index*math.pi/8 "
        "local anchor={x=requested.x+math.cos(angle)*ring,y=requested.y+math.sin(angle)*ring} "
        "local candidate=p.surface.find_non_colliding_position(p.name,anchor,0.7,0.2) "
        "if candidate then local key=math.floor(candidate.x*4+0.5)..':'..math.floor(candidate.y*4+0.5) "
        "local tx=candidate.x-requested.x local ty=candidate.y-requested.y "
        "local edge_distance=math.max(0,math.sqrt(tx*tx+ty*ty)-extent) "
        "if not seen[key] and edge_distance<=p.build_distance-0.25 then seen[key]=true "
        "local px=candidate.x-p.position.x local py=candidate.y-p.position.y "
        "candidates[#candidates+1]={x=candidate.x,y=candidate.y,"
        "distance_from_player=math.sqrt(px*px+py*py),edge_distance=edge_distance} end end end end "
        "table.sort(candidates,function(a,b) return a.distance_from_player<b.distance_from_player end) "
        "while #candidates>24 do table.remove(candidates) end result.candidates=candidates return result");

    const json nextArguments{
        {"item", request.item},
        {"x", request.x},
        {"y", request.y},
        {"direction", request.direction},
    };
    plan["next_tool"] = {{"name", "place_entity"}, {"arguments", nextArguments}};
    auto candidates = plan.value("candidates", json::array());
    plan.erase("candidates");
    if (plan.value("placement_ready", false))
    {
        plan["reached_build_position"] = true;
        plan["reason"] = "already_ready_for_place_entity";
        return plan;
    }
    if (candidates.empty())
    {
        plan["reached_build_position"] = false;
        plan["reason"] = "no_collision_free_build_position";
        return plan;
    }

    const auto deadline = DeadlineAfter(maximumSeconds);
    for (const auto& candidate : candidates)
    {
        if (stopToken.stop_requested())
        {
            plan["stopped"] = true;
            plan["reached_build_position"] = false;
            return plan;
        }
        const auto remaining = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining < 1.0)
            break;

        auto movement = WalkTo({
            {"x", candidate.at("x")},
            {"y", candidate.at("y")},
            {"stopping_distance", 0.5},
            {"maximum_duration_seconds", std::min(remaining, 120.0)},
        }, stopToken);
        plan["movement"] = movement;
        if (!movement.value("reached", false))
            continue;

        const auto verification = ExecuteJson(
            ControlCharacterLua() +
            "local item=prototypes.item[" + LuaStringLiteral(request.item) + "] "
            "local place_result=item and item.place_result "
            "if not place_result then error('Placement item is no longer available') end "
            "local requested={x=" + std::to_string(request.x) + ",y=" + std::to_string(request.y) + "} "
            "local direction=" + direction + " local result={current_position={x=p.position.x,y=p.position.y},"
            "build_distance=p.build_distance,target_valid=p.surface.can_place_entity{name=place_result.name,"
            "position=requested,direction=direction,force=p.force,"
            "build_check_type=defines.build_check_type.manual},"
            "placement_ready=p.can_place_entity{name=place_result.name,position=requested,direction=direction}} "
            "if result.target_valid then return result end "
            "local directions={{name='north',value=defines.direction.north},"
            "{name='east',value=defines.direction.east},{name='south',value=defines.direction.south},"
            "{name='west',value=defines.direction.west}} local alternatives={} local seen={} "
            "local function test_position(x,y,distance) local key=x..':'..y "
            "for _,candidate_direction in ipairs(directions) do local direction_key=key..':'..candidate_direction.name "
            "if not seen[direction_key] and p.surface.can_place_entity{name=place_result.name,position={x=x,y=y},"
            "direction=candidate_direction.value,force=p.force,"
            "build_check_type=defines.build_check_type.manual} then seen[direction_key]=true "
            "alternatives[#alternatives+1]={x=x,y=y,direction=candidate_direction.name,"
            "distance_from_requested=distance} end end end "
            "for ring=1,8 do for offset=-ring,ring do test_position(requested.x+offset,requested.y-ring,ring) "
            "test_position(requested.x+offset,requested.y+ring,ring) end "
            "for offset=-ring+1,ring-1 do test_position(requested.x-ring,requested.y+offset,ring) "
            "test_position(requested.x+ring,requested.y+offset,ring) end if #alternatives>=12 then break end end "
            "while #alternatives>12 do table.remove(alternatives) end result.valid_alternatives=alternatives "
            "local nearby=p.surface.find_entities_filtered{position=requested,radius=6} local blockers={} "
            "for _,entity in ipairs(nearby) do if entity~=p and #blockers<12 then "
            "blockers[#blockers+1]={name=entity.name,type=entity.type,"
            "position={x=entity.position.x,y=entity.position.y}} end end "
            "result.nearby_entities=blockers local tile=p.surface.get_tile(requested) "
            "result.target_tile={name=tile.name,position={x=tile.position.x,y=tile.position.y}} "
            "return result");
        plan["current_position"] = verification.at("current_position");
        plan["build_distance"] = verification.at("build_distance");
        plan["target_valid"] = verification.at("target_valid");
        plan["placement_ready"] = verification.at("placement_ready");
        plan["approach_position"] = {{"x", candidate.at("x")}, {"y", candidate.at("y")}};
        plan["reached_build_position"] = verification.value("placement_ready", false);
        if (verification.value("placement_ready", false))
        {
            plan["reason"] = "ready_for_place_entity";
            return plan;
        }
        if (!verification.value("target_valid", false))
        {
            plan["reason"] = "invalid_build_target_or_direction";
            plan["valid_alternatives"] = verification.value("valid_alternatives", json::array());
            plan["nearby_entities"] = verification.value("nearby_entities", json::array());
            plan["target_tile"] = verification.value("target_tile", json::object());
            return plan;
        }
    }

    plan["placement_ready"] = false;
    plan["reached_build_position"] = false;
    plan["reason"] = "no_pathable_position_within_build_reach";
    return plan;
}

json FactorioTools::StopWalking() const
{
    return ExecuteJson(
        ControlCharacterLua() +
        "p.walking_state={walking=false,direction=defines.direction.north} "
        "return {position={x=p.position.x,y=p.position.y}}");
}

json FactorioTools::TakeScreenshot(const json& arguments, std::stop_token stopToken) const
{
    const auto width = RequiredInteger(arguments, "width", 320, 1280);
    const auto height = RequiredInteger(arguments, "height", 240, 960);
    const auto zoom = RequiredNumber(arguments, "zoom", 0.25, 2.0);
    if (factorioUserDataPath_.empty())
        throw std::runtime_error("Factorio user data directory is not configured");

    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto fileName = "screenshot-" + std::to_string(stamp) + ".png";
    const auto relativePath = std::filesystem::path("FactorIA") / fileName;
    const auto absolutePath = factorioUserDataPath_ / "script-output" / relativePath;

    ExecuteJson(
        ControlCharacterLua() +
        "game.take_screenshot{surface=p.surface,position=p.position,resolution={x=" + std::to_string(width) +
        ",y=" + std::to_string(height) + "},zoom=" + std::to_string(zoom) +
        ",path=\"FactorIA/" + fileName + "\",show_gui=false,show_entity_info=true,force_render=true} "
        "return {requested=true}");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::uintmax_t previousSize = 0;
    int stableChecks = 0;
    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline)
    {
        std::error_code error;
        const auto size = std::filesystem::file_size(absolutePath, error);
        if (!error && size > 0U)
        {
            if (size == previousSize)
                ++stableChecks;
            else
                stableChecks = 0;
            previousSize = size;
            if (stableChecks >= 2)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (stopToken.stop_requested())
        throw std::runtime_error("Screenshot capture was stopped");
    if (!std::filesystem::exists(absolutePath))
        throw std::runtime_error(
            "Factorio did not create the screenshot. Check the user data directory and ensure Factorio is not headless: " +
            absolutePath.string());
    const auto size = std::filesystem::file_size(absolutePath);
    if (size == 0U || size > 12U * 1024U * 1024U)
        throw std::runtime_error("Factorio screenshot has an invalid or excessive file size");

    std::ifstream input(absolutePath, std::ios::binary);
    if (!input)
        throw std::runtime_error("Unable to open Factorio screenshot: " + absolutePath.string());
    const std::vector<unsigned char> bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}};
    return {
        {"captured", true},
        {"path", absolutePath.string()},
        {"width", width},
        {"height", height},
        {"zoom", zoom},
        {"_image_data_url", "data:image/png;base64," + Base64Encode(bytes)},
    };
}

json FactorioTools::MineEntity(const json& arguments, std::stop_token stopToken) const
{
    const auto targetName = RequiredPrototypeName(arguments, "name");
    const auto count = RequiredInteger(arguments, "count", 1, 200);
    const auto maximumSeconds = std::max(10.0, static_cast<double>(count) * 2.5 + 5.0);
    return RunMiningAction(
        {
            {"kind", "mine"},
            {"name", targetName},
            {"count", count},
            {"maximum_duration_seconds", maximumSeconds},
        },
        targetName,
        maximumSeconds,
        stopToken);
}

json FactorioTools::MineMarkedEntity(const json& request, std::stop_token stopToken) const
{
    const auto targetName = RequiredPrototypeName(request, "entity_name");
    const auto targetX = request.at("position").at("x").get<double>();
    const auto targetY = request.at("position").at("y").get<double>();
    const auto maximumSeconds = RequiredNumber(request, "maximum_duration_seconds", 1.0, 120.0);
    return RunMiningAction(
        {
            {"kind", "mine_marked"},
            {"name", targetName},
            {"count", 1},
            {"target_x", targetX},
            {"target_y", targetY},
            {"maximum_duration_seconds", maximumSeconds},
        },
        targetName,
        maximumSeconds,
        stopToken);
}

json FactorioTools::RunMiningAction(
    json runtimeArguments,
    const std::string& targetName,
    double maximumSeconds,
    std::stop_token stopToken) const
{
    const auto start = StartPlayerControlAction(runtimeArguments);
    auto result = WaitForPlayerControlAction(
        start,
        DeadlineAfter(maximumSeconds + 2.0),
        stopToken);
    if (result.value("mined", false))
    {
        result["inventory"] = GetInventory();
        result["research_trigger_tracking"] = RecordResearchTriggerAction(
            "mine-entity",
            targetName,
            "normal",
            result.value("mined_count", 1.0));
    }
    result["maximum_duration_seconds"] = maximumSeconds;
    result["mining_control"] = "factorio_bridge_runtime";
    return result;
}

json FactorioTools::Craft(const json& arguments, std::stop_token stopToken) const
{
    const auto recipe = RequiredPrototypeName(arguments, "recipe");
    const auto count = RequiredInteger(arguments, "count", 1, 100);
    auto result = ExecuteJson(
        ControlCharacterLua() +
        "local recipe_name=" + LuaStringLiteral(recipe) + " local selected=p.force.recipes[recipe_name] "
        "if not selected then error('Unknown recipe: ' .. recipe_name) end "
        "if not selected.enabled then error('Recipe is not enabled: ' .. recipe_name) end "
        "local inventory=p.get_main_inventory() "
        "if not inventory then error('Controlled character has no main inventory') end "
        "local queue_before=p.crafting_queue_size "
        "if queue_before>0 then return {requested=" + std::to_string(count) + ",queued=0,completed=false,"
        "crafting_queue_size=queue_before,reason='crafting_queue_not_empty'} end "
        "local products={} local seen={} for _,product in ipairs(selected.products) do "
        "if product.type=='item' and not seen[product.name] then seen[product.name]=true "
        "products[#products+1]={name=product.name,initial_count=inventory.get_item_count(product.name)} end end "
        "local queued=p.begin_crafting{count=" + std::to_string(count) + ",recipe=recipe_name,silent=true} "
        "return {recipe=recipe_name,requested=" + std::to_string(count) + ",queued=queued,"
        "crafting_queue_size=p.crafting_queue_size,recipe_energy_seconds=selected.energy,products=products,"
        "reason=queued>0 and 'crafting_started' or 'recipe_could_not_be_queued'}");

    if (result.value("queued", 0) == 0)
        return result;

    constexpr auto MaximumCraftingWait = std::chrono::minutes(10);
    const auto deadline = std::chrono::steady_clock::now() + MaximumCraftingWait;
    int queueSize = result.value("crafting_queue_size", 0);
    while (!stopToken.stop_requested() && queueSize > 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto status = ExecuteJson(
            ControlCharacterLua() +
            "return {crafting_queue_size=p.crafting_queue_size}");
        queueSize = status.value("crafting_queue_size", queueSize);
    }

    result["crafting_queue_size"] = queueSize;
    result["completed"] = queueSize == 0;
    result["stopped"] = stopToken.stop_requested();
    result["timed_out"] = queueSize > 0 && !stopToken.stop_requested();
    result["maximum_wait_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(MaximumCraftingWait).count();
    if (queueSize > 0)
        return result;

    const auto inventory = GetInventory();
    json craftedItems = json::array();
    json triggerUpdates = json::array();
    for (const auto& product : result.value("products", json::array()))
    {
        const auto name = product.value("name", std::string{});
        int finalCount = 0;
        for (const auto& item : inventory.value("items", json::array()))
        {
            if (item.value("name", std::string{}) == name)
                finalCount += item.value("count", 0);
        }
        const auto craftedCount = std::max(0, finalCount - product.value("initial_count", 0));
        if (craftedCount == 0)
            continue;

        craftedItems.push_back({{"name", name}, {"count", craftedCount}, {"quality", "normal"}});
        triggerUpdates.push_back(RecordResearchTriggerAction(
            "craft-item",
            name,
            "normal",
            craftedCount));
    }
    result["crafted_items"] = std::move(craftedItems);
    result["research_trigger_tracking"] = std::move(triggerUpdates);
    result["inventory"] = inventory;
    result["reason"] = "crafting_completed";
    return result;
}

json FactorioTools::BuildGhostAt(const json& request) const
{
    const auto entityName = RequiredPrototypeName(request, "entity_name");
    const auto itemName = RequiredPrototypeName(request, "item");
    const auto targetX = request.at("position").at("x").get<double>();
    const auto targetY = request.at("position").at("y").get<double>();

    auto result = ExecuteJson(
        ControlCharacterLua() + EntityConnectionDetailsLua() +
        "local entity_name=" + LuaStringLiteral(entityName) + " local item_name=" + LuaStringLiteral(itemName) + " "
        "local position={x=" + std::to_string(targetX) + ",y=" + std::to_string(targetY) + "} "
        "local ghosts=p.surface.find_entities_filtered{position=position,radius=0.25,"
        "type='entity-ghost',ghost_name=entity_name} local ghost=ghosts[1] "
        "if not ghost or not ghost.valid then return {fulfilled=false,missing=true,reason='ghost_missing'} end "
        "if not p.can_reach_entity(ghost) then return {fulfilled=false,out_of_reach=true,"
        "reason='ghost_out_of_reach',position=position} end "
        "local inventory=p.get_main_inventory() if not inventory then "
        "error('Controlled character has no main inventory') end "
        "local quality=ghost.quality and ghost.quality.name or 'normal' "
        "local prototype=prototypes.entity[ghost.ghost_name] local required_count=nil "
        "for _,item in ipairs(prototype and prototype.items_to_place_this or {}) do "
        "if item.name==item_name then required_count=item.count or 1 break end end "
        "if not required_count then return {fulfilled=false,reason='item_does_not_build_ghost',"
        "item=item_name,entity_name=ghost.ghost_name} end "
        "local available=inventory.get_item_count{name=item_name,quality=quality} "
        "if available<required_count then return {fulfilled=false,reason='missing_item',item=item_name,"
        "quality=quality,required_count=required_count,item_count=available} end "
        "local removed=inventory.remove{name=item_name,count=required_count,quality=quality} "
        "if removed~=required_count then if removed>0 then inventory.insert{name=item_name,count=removed,quality=quality} end "
        "return {fulfilled=false,reason='item_could_not_be_reserved',item=item_name,quality=quality} end "
        "local revived,collided,entity,proxy=pcall(function() "
        "return ghost.revive{raise_revive=true,overflow=inventory} end) "
        "if not revived or not entity then inventory.insert{name=item_name,count=removed,quality=quality} "
        "return {fulfilled=false,reason='ghost_could_not_be_revived',"
        "error=not revived and tostring(collided) or nil,item=item_name,quality=quality} end "
        "local info={name=entity.name,type=entity.type,quality=entity.quality.name,"
        "position={x=entity.position.x,y=entity.position.y},direction=entity.direction,"
        "direction_name=factoria_direction_name(entity.direction),unit_number=entity.unit_number} "
        "factoria_attach_connection_details(info,entity) "
        "return {fulfilled=true,item=item_name,item_quality=quality,consumed_count=removed,"
        "remaining_item_count=inventory.get_item_count{name=item_name,quality=quality},"
        "collided_items=collided or {},item_request_proxy_created=proxy~=nil,entity=info}");

    if (result.value("fulfilled", false))
    {
        const auto& entity = result.at("entity");
        result["research_trigger_tracking"] = RecordResearchTriggerAction(
            "build-entity",
            entity.at("name").get<std::string>(),
            entity.value("quality", "normal"),
            1.0);
    }
    return result;
}

json FactorioTools::PlaceEntity(const json& arguments) const
{
    const auto request = RequiredPlacementRequest(arguments);
    const auto direction = DirectionExpression(request.direction);

    auto result = ExecuteJson(
        ControlCharacterLua() + EntityConnectionDetailsLua() +
        "local inv=p.get_main_inventory() if not inv then error('Controlled character has no main inventory') end "
        "local item_name=" + LuaStringLiteral(request.item) + " local source=inv.find_item_stack(item_name) "
        "if not source then error('Player does not have item: " + request.item + "') end "
        "local place_result=source.prototype.place_result "
        "if not place_result then error('Item cannot be placed as an entity: " + request.item + "') end "
        "local requested={x=" + std::to_string(request.x) + ",y=" + std::to_string(request.y) + "} "
        "local target_valid=p.surface.can_place_entity{name=place_result.name,position=requested,"
        "direction=" + direction + ",force=p.force,build_check_type=defines.build_check_type.manual} "
        "local accepted=p.can_place_entity{name=place_result.name,position=requested,direction=" + direction + "} "
        "local entity=nil if accepted then local quality=source.quality.name "
        "local removed=inv.remove{name=item_name,count=1,quality=quality} "
        "if removed==1 then entity=p.surface.create_entity{name=place_result.name,position=requested,"
        "direction=" + direction + ",force=p.force,quality=quality,raise_built=true} "
        "if not entity then inv.insert{name=item_name,count=1,quality=quality} end end end "
        "local placed=entity~=nil local result={placed=placed,build_accepted=accepted,target_valid=target_valid,"
        "item=item_name,requested_position=requested,requested_direction=" + LuaStringLiteral(request.direction) + ","
        "player_position={x=p.position.x,y=p.position.y},build_distance=p.build_distance,"
        "remaining_item_count=inv.get_item_count(item_name)} "
        "if entity then result.entity={name=entity.name,type=entity.type,quality=entity.quality.name,"
        "position={x=entity.position.x,y=entity.position.y},direction=entity.direction,"
        "direction_name=factoria_direction_name(entity.direction),unit_number=entity.unit_number} "
        "factoria_attach_connection_details(result.entity,entity) end "
        "if not accepted and target_valid then result.reason='out_of_build_reach'; result.suggested_tool='walk_to_for_placement' "
        "elseif not accepted then result.reason='invalid_build_target_or_direction' "
        "elseif not placed then result.reason='Factorio accepted the build but the placed entity could not be verified' end "
        "return result");
    if (result.value("placed", false))
    {
        const auto& entity = result.at("entity");
        result["research_trigger_tracking"] = RecordResearchTriggerAction(
            "build-entity",
            entity.at("name").get<std::string>(),
            entity.value("quality", "normal"),
            1.0);
    }
    return result;
}

json FactorioTools::SetAssemblerRecipe(const json& arguments) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto recipeName = RequiredPrototypeName(arguments, "recipe");

    return ExecuteJson(
        EntityLookupLua(
            ControlCharacterLua(),
            targetX,
            targetY,
            "candidate.type=='assembling-machine'",
            "assembling machine",
            true) +
        "local recipe_name=" + LuaStringLiteral(recipeName) + " local recipe=p.force.recipes[recipe_name] "
        "if not recipe then error('Unknown recipe: ' .. recipe_name) end "
        "if not recipe.enabled then error('Recipe is not enabled: ' .. recipe_name) end "
        "local previous=target.get_recipe() local removed=target.set_recipe(recipe_name) "
        "local destination=p.get_main_inventory() local returned={} local spilled={} "
        "for _,item in ipairs(removed) do local inserted=destination and destination.insert(item) or 0 "
        "if inserted>0 then returned[#returned+1]={name=item.name,quality=item.quality,count=inserted} end "
        "local remainder=item.count-inserted if remainder>0 then "
        "target.surface.spill_item_stack{position=target.position,"
        "stack={name=item.name,quality=item.quality,count=remainder},enable_looted=true,force=p.force} "
        "spilled[#spilled+1]={name=item.name,quality=item.quality,count=remainder} end end "
        "local selected=target.get_recipe() return {entity={name=target.name,type=target.type,"
        "position={x=target.position.x,y=target.position.y}},previous_recipe=previous and previous.name or nil,"
        "recipe=selected and selected.name or nil,recipe_set=selected~=nil and selected.name==recipe_name,"
        "returned_to_player=returned,spilled_beside_machine=spilled}");
}

json FactorioTools::InsertItemIntoEntity(const json& arguments) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto item = RequiredPrototypeName(arguments, "item");
    const auto count = RequiredInteger(arguments, "count", 1, 10000);
    const auto inventoryValue = arguments.find("inventory");
    if (inventoryValue == arguments.end() || !inventoryValue->is_string())
        throw std::runtime_error("Missing string argument: inventory");
    const auto inventory = inventoryValue->get<std::string>();
    if (inventory == "output")
        throw std::runtime_error("Cannot insert items into an entity output inventory");

    return ExecuteJson(
        EntityInventoryLookupLua(ControlCharacterLua(), targetX, targetY, inventory) +
        "local source=p.get_main_inventory() if not source then error('Controlled character has no main inventory') end "
        "local item_name=\"" + item + "\" if not prototypes.item[item_name] then error('Unknown item prototype: ' .. item_name) end "
        "local destination=entity_inventory "
        "local available=source.get_item_count(item_name) if available==0 then error('Player does not have item: ' .. item_name) end "
        "local requested=math.min(" + std::to_string(count) + ",available,destination.get_insertable_count(item_name)) "
        "local removed=0 local inserted=0 if requested>0 then "
        "removed=source.remove{name=item_name,count=requested} "
        "inserted=destination.insert{name=item_name,count=removed} "
        "if inserted<removed then source.insert{name=item_name,count=removed-inserted} end end "
        "return {entity={name=target.name,position={x=target.position.x,y=target.position.y}},"
        "inventory=\"" + inventory + "\",item=item_name,requested_count=" + std::to_string(count) + ","
        "inserted_count=inserted,player_remaining=source.get_item_count(item_name),"
        "machine_item_count=destination.get_item_count(item_name),complete=inserted==" + std::to_string(count) + "}");
}

json FactorioTools::TakeItemFromEntity(const json& arguments) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto item = RequiredPrototypeName(arguments, "item");
    const auto count = RequiredInteger(arguments, "count", 1, 10000);
    const auto inventoryValue = arguments.find("inventory");
    if (inventoryValue == arguments.end() || !inventoryValue->is_string())
        throw std::runtime_error("Missing string argument: inventory");
    const auto inventory = inventoryValue->get<std::string>();

    return ExecuteJson(
        EntityInventoryLookupLua(ControlCharacterLua(), targetX, targetY, inventory) +
        "local destination=p.get_main_inventory() if not destination then error('Controlled character has no main inventory') end "
        "local item_name=\"" + item + "\" if not prototypes.item[item_name] then error('Unknown item prototype: ' .. item_name) end "
        "local source=entity_inventory "
        "local available=source.get_item_count(item_name) "
        "local requested=math.min(" + std::to_string(count) + ",available,destination.get_insertable_count(item_name)) "
        "local removed=0 local inserted=0 if requested>0 then "
        "removed=source.remove{name=item_name,count=requested} "
        "inserted=destination.insert{name=item_name,count=removed} "
        "if inserted<removed then source.insert{name=item_name,count=removed-inserted} end end "
        "return {entity={name=target.name,position={x=target.position.x,y=target.position.y}},"
        "inventory=\"" + inventory + "\",item=item_name,requested_count=" + std::to_string(count) + ","
        "taken_count=inserted,available_before=available,machine_remaining=source.get_item_count(item_name),"
        "complete=inserted==" + std::to_string(count) + "}");
}

json FactorioTools::WaitForMachineOutput(const json& arguments, std::stop_token stopToken) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto item = RequiredPrototypeName(arguments, "item");
    const auto count = RequiredInteger(arguments, "count", 1, 10000);
    const auto maximumSeconds = RequiredNumber(arguments, "maximum_duration_seconds", 1.0, 600.0);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = DeadlineAfter(maximumSeconds);
    int consecutiveIdlePolls = 0;
    int pollCount = 0;
    json snapshot = json::object();

    auto finish = [&](std::string reason) {
        snapshot["reason"] = std::move(reason);
        snapshot["requested_item"] = item;
        snapshot["requested_output_count"] = count;
        snapshot["poll_count"] = pollCount;
        snapshot["elapsed_seconds"] = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        return snapshot;
    };

    while (!stopToken.stop_requested())
    {
        snapshot = ExecuteJson(
            MachineLookupLua(ControlCharacterLua(), targetX, targetY, false) +
            "local item_name=\"" + item + "\" "
            "if not prototypes.item[item_name] then error('Unknown item prototype: ' .. item_name) end "
            "local output=target.get_inventory(defines.inventory.crafter_output) "
            "if not output then error('Target machine has no output inventory') end "
            "local input=target.get_inventory(defines.inventory.crafter_input) "
            "local fuel=target.get_inventory(defines.inventory.fuel) "
            "return {entity={name=target.name,position={x=target.position.x,y=target.position.y}},"
            "crafting=target.is_crafting(),output_count=output.get_item_count(item_name),"
            "input_empty=input==nil or input.is_empty(),output_full=output.is_full(),"
            "fuel_empty=fuel and fuel.is_empty() or nil}");
        ++pollCount;

        if (snapshot.value("output_count", 0) >= count)
        {
            snapshot["target_met"] = true;
            snapshot["completed"] = true;
            return finish("target_output_reached");
        }

        if (snapshot.value("crafting", false))
        {
            consecutiveIdlePolls = 0;
        }
        else
        {
            ++consecutiveIdlePolls;
            if (consecutiveIdlePolls >= 3)
            {
                const auto inputEmpty = snapshot.value("input_empty", false);
                snapshot["target_met"] = false;
                snapshot["completed"] = inputEmpty;
                snapshot["stalled"] = !inputEmpty;
                if (inputEmpty)
                    return finish("machine_input_empty");
                if (snapshot.value("fuel_empty", false))
                    return finish("machine_out_of_fuel");
                if (snapshot.value("output_full", false))
                    return finish("machine_output_full");
                return finish("machine_idle_with_input");
            }
        }

        if (std::chrono::steady_clock::now() >= deadline)
        {
            snapshot["target_met"] = false;
            snapshot["timed_out"] = true;
            return finish("timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    snapshot["target_met"] = false;
    snapshot["stopped"] = true;
    return finish("stopped");
}

json FactorioTools::TransferInventoryToContainer(const json& arguments) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);

    return ExecuteJson(
        ControlCharacterLua() +
        "local source=p.get_main_inventory() if not source then error('Controlled character has no main inventory') end "
        "local position={x=" + std::to_string(targetX) + ",y=" + std::to_string(targetY) + "} "
        "local found=p.surface.find_entities_filtered{position=position,radius=1,type={'container','logistic-container'}} "
        "table.sort(found,function(a,b) local adx=a.position.x-position.x local ady=a.position.y-position.y "
        "local bdx=b.position.x-position.x local bdy=b.position.y-position.y "
        "return adx*adx+ady*ady<bdx*bdx+bdy*bdy end) "
        "local target=found[1] if not target then error('No container found at the requested position') end "
        "if not p.can_reach_entity(target) then error('Container is out of reach') end "
        "local destination=target.get_inventory(defines.inventory.chest) "
        "if not destination then error('Target entity has no chest inventory') end "
        "local destination_limit=#destination if destination.supports_bar() then "
        "destination_limit=math.min(destination_limit,destination.get_bar()-1) end "
        "local moved_by_item={} local total=0 for source_index=1,#source do "
        "local stack=source[source_index] if stack.valid_for_read then "
        "local item_name=stack.name local quality=tostring(stack.quality) local before=stack.count "
        "for destination_index=1,destination_limit do "
        "destination[destination_index].transfer_stack(stack) if not stack.valid_for_read then break end end "
        "local after=stack.valid_for_read and stack.count or 0 local transferred=before-after "
        "if transferred>0 then local key=item_name..':'..quality local moved=moved_by_item[key] "
        "if not moved then moved={name=item_name,count=0,quality=quality} moved_by_item[key]=moved end "
        "moved.count=moved.count+transferred total=total+transferred end end end "
        "local moved={} for _,item in pairs(moved_by_item) do moved[#moved+1]=item end "
        "table.sort(moved,function(a,b) return a.name<b.name end) "
        "local remaining={} for _,item in pairs(source.get_contents()) do "
        "remaining[#remaining+1]={name=item.name,count=item.count,quality=tostring(item.quality)} end "
        "table.sort(remaining,function(a,b) return a.name<b.name end) "
        "return {container={name=target.name,position={x=target.position.x,y=target.position.y},"
        "unit_number=target.unit_number},moved=moved,moved_total=total,complete=source.is_empty(),remaining=remaining}");
}
}
