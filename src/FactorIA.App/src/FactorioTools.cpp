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
        throw std::runtime_error("Argument " + name + " is outside the allowed range");
    return number;
}

double NumberOrDefault(
    const json& arguments,
    const std::string& name,
    double defaultValue,
    double minimum,
    double maximum)
{
    return arguments.contains(name)
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
        throw std::runtime_error("Argument " + name + " is outside the allowed range");
    return number;
}

std::string RequiredText(const json& arguments, const std::string& name, std::size_t maximumLength)
{
    const auto value = arguments.find(name);
    if (value == arguments.end() || !value->is_string())
        throw std::runtime_error("Missing string argument: " + name);
    const auto text = value->get<std::string>();
    if (text.empty() || text.size() > maximumLength)
        throw std::runtime_error("Argument " + name + " is outside the allowed length range");
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

std::string EntityFluidDetailsLua()
{
    return
        "local function factoria_direction_name(direction) "
        "local names={[defines.direction.north]='north',[defines.direction.northeast]='northeast',"
        "[defines.direction.east]='east',[defines.direction.southeast]='southeast',"
        "[defines.direction.south]='south',[defines.direction.southwest]='southwest',"
        "[defines.direction.west]='west',[defines.direction.northwest]='northwest'} "
        "return names[direction] or tostring(direction) end "
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
        "if target_owner then connection_info.target={name=target_owner.name,type=target_owner.type,"
        "position={x=target_owner.position.x,y=target_owner.position.y},"
        "direction=factoria_direction_name(target_owner.direction),unit_number=target_owner.unit_number} "
        "connected_count=connected_count+1 end "
        "box.connections[#box.connections+1]=connection_info connection_count=connection_count+1 end "
        "boxes[#boxes+1]=box end info.fluidboxes=boxes "
        "info.fluid_connection_summary={total=connection_count,connected=connected_count,"
        "open=connection_count-connected_count} "
        "info.fluid_connection_hint='For each open port, place a pipe or compatible machine port at target_position; "
        "do not infer connections from entity centers or sprite orientation.' end ";
}

void RejectUnknownArguments(const json& arguments, std::initializer_list<std::string_view> allowed)
{
    for (const auto& [name, value] : arguments.items())
    {
        static_cast<void>(value);
        if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
            throw std::runtime_error("Unexpected tool argument: " + name);
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
            "Get current and queued lab research, startable technologies, and trigger-based milestones such as crafting an item or building an entity. Results include required science packs and unlocked recipes. Optionally inspect one exact technology, including unmet prerequisites.",
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
            "Return a page of up to 40 nearby entities with character-relative deltas. Fluid-capable entities include their actual input/output pipe ports, exact target positions, and connection state; use those fields to route pipes and verify machine orientation. Use regex for a case-insensitive ECMAScript search over both prototype names and entity types, for example regex='furnace' finds stone-furnace and every entity of type furnace. Exact name and type filters are also available. Distinct prototypes are returned before duplicates, and next_offset retrieves another page. This tool cannot find terrain tiles such as water; use find_water for water. Factorio X increases east and Y increases south, so a negative delta_y is north.",
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
            "Walk using normal player movement in a cardinal direction for a short duration, then return the new position.",
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
            "Place one buildable item from the controlled character's inventory on a reachable open tile using normal Factorio build rules. The item is consumed only when placement succeeds. Results for fluid-capable entities include actual input/output ports and connection state; connect open target positions with pipes and verify the final network with get_nearby_entities. Use walk_to_for_placement first when the exact target is distant or place_entity reports out_of_build_reach.",
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
    return ExecuteJson(
        ControlCharacterLua() +
        "local inv=p.get_main_inventory() local items={} "
        "if inv then for _,item in pairs(inv.get_contents()) do "
        "items[#items+1]={name=item.name,count=item.count,quality=tostring(item.quality)} end end "
        "table.sort(items,function(a,b) return a.name<b.name end) return {items=items}");
}

json FactorioTools::GetCraftableRecipes() const
{
    return ExecuteJson(
        ControlCharacterLua() +
        "local recipes={} for name,recipe in pairs(p.force.recipes) do "
        "if recipe.enabled and name~='recipe-unknown' then local count=p.get_craftable_count(recipe) if count>0 then "
        "recipes[#recipes+1]={name=name,craftable_count=count} end end end "
        "table.sort(recipes,function(a,b) return a.name<b.name end) "
        "local total=#recipes while #recipes>100 do table.remove(recipes) end "
        "return {recipes=recipes,total_count=total,truncated=total>#recipes}");
}

json FactorioTools::GetResearchStatus(const json& arguments) const
{
    const auto requestedTechnology = arguments.contains("technology")
        ? RequiredPrototypeName(arguments, "technology")
        : std::string{};

    return ExecuteJson(
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
    auto nameFilter = arguments.contains("name")
        ? " filter.name=\"" + RequiredPrototypeName(arguments, "name") + "\" "
        : std::string{};
    const auto typeFilter = arguments.contains("type")
        ? " filter.type=\"" + RequiredPrototypeName(arguments, "type") + "\" "
        : std::string{};
    const auto offset = arguments.contains("offset")
        ? RequiredInteger(arguments, "offset", 0, 10000)
        : 0;

    std::string regexResultField;
    if (arguments.contains("regex"))
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

    return ExecuteJson(
        ControlCharacterLua() + EntityFluidDetailsLua() +
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
        "factoria_attach_fluid_details(info,e) "
        "entities[#entities+1]=info end "
        "return {radius=" + std::to_string(radius) + ",player_position={x=p.position.x,y=p.position.y},"
        "coordinate_hint='positive x is east; positive y is south',"
        + regexResultField +
        "ordering='nearest representative of each distinct prototype first, then nearest duplicate instances',"
        "offset=offset,total_entities=total,distinct_prototypes=distinct,truncated=offset+#entities<total,"
        "next_offset=offset+#entities<total and offset+#entities or nil,entities=entities}");
}

json FactorioTools::FindResourcePatches(const json& arguments) const
{
    const auto resource = RequiredPrototypeName(arguments, "resource");
    const auto radius = RequiredNumber(arguments, "radius", 32.0, MaximumSearchRadius);
    const auto nameFilter = resource == "any"
        ? std::string{}
        : " filter.name=\"" + resource + "\" ";
    return ExecuteJson(
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
}

json FactorioTools::FindWater(const json& arguments) const
{
    const auto radius = RequiredNumber(arguments, "radius", 32.0, MaximumSearchRadius);
    return ExecuteJson(
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
    const auto start = StartPlayerControlAction({
        {"kind", "mine"},
        {"name", targetName},
        {"count", count},
        {"maximum_duration_seconds", maximumSeconds},
    });
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

json FactorioTools::PlaceEntity(const json& arguments) const
{
    const auto request = RequiredPlacementRequest(arguments);
    const auto direction = DirectionExpression(request.direction);

    auto result = ExecuteJson(
        ControlCharacterLua() + EntityFluidDetailsLua() +
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
        "factoria_attach_fluid_details(result.entity,entity) end "
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
