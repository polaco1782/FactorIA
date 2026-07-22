#include <FactorIA/FactorioTools.h>

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
constexpr std::string_view BridgeCommand = "/factoria-bridge ";
constexpr int BridgeCommandProtocolVersion = 1;

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

json ParseBridgeResponse(const std::string& response)
{
    const auto first = response.find('{');
    const auto last = response.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first)
        throw std::runtime_error("FactorIA Bridge did not return a JSON command result: " + response);

    const auto value = json::parse(response.substr(first, last - first + 1));
    if (!value.is_object() || !value.contains("ok"))
        throw std::runtime_error("FactorIA Bridge returned an invalid command envelope");
    if (value.value("protocol_version", 0) != BridgeCommandProtocolVersion)
        throw std::runtime_error("The FactorIA Bridge command protocol version is not supported");
    if (!value.value("ok", false))
        throw std::runtime_error("FactorIA Bridge command failed: " + value.value("error", "unknown bridge error"));
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

std::chrono::steady_clock::time_point DeadlineAfter(double seconds)
{
    return std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(seconds));
}

void AttachResearchWaitHint(json& result, const std::string& technologyName)
{
    if (technologyName.empty())
        return;
    result["wait_tool"] = "wait_for_research";
    result["wait_tool_arguments"] = {
        {"technology", technologyName},
        {"maximum_duration_seconds", 600},
    };
}

FactorioAgentTask ParseAgentTask(const json& value)
{
    FactorioAgentTask task;
    task.id = value.at("id").get<std::uint64_t>();
    task.kind = value.at("kind").get<std::string>();
    task.issuerName = value.at("issuer_name").get<std::string>();
    task.issuerSurface = value.at("issuer_surface").get<std::string>();
    task.issuerPlayerIndex = value.at("issuer_player_index").get<int>();
    task.issuerX = value.at("issuer_position").at("x").get<double>();
    task.issuerY = value.at("issuer_position").at("y").get<double>();
    task.searchRadius = value.at("search_radius").get<double>();
    if (task.id == 0 || task.kind.empty() || task.issuerName.empty() ||
        task.issuerPlayerIndex <= 0 || !std::isfinite(task.searchRadius) ||
        task.searchRadius < 1.0 || task.searchRadius > MaximumSearchRadius)
    {
        throw std::runtime_error("Factorio returned an invalid agent task");
    }
    return task;
}
}

FactorioTools::FactorioTools(
    CommandExecutor executeCommand,
    std::filesystem::path factorioUserDataPath,
    bool useDedicatedCharacter,
    std::optional<FactorioAgentTask> agentTask)
    : executeCommand_(std::move(executeCommand)),
      factorioUserDataPath_(std::move(factorioUserDataPath)),
      useDedicatedCharacter_(useDedicatedCharacter),
      agentTask_(std::move(agentTask))
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
            "List enabled hand-crafting recipes that the player can craft now. With no arguments, only currently craftable recipes are returned. Set recipe to inspect one exact recipe or item to inspect recipes producing one exact item; targeted results also include disabled or currently blocked matches, crafting categories, ingredients, and current inventory shortages. Set at most one filter.",
            {
                {"type", "object"},
                {"properties", {
                    {"recipe", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"item", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                }},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "get_research_status",
            "Get a snapshot of current and queued lab research, startable technologies, and trigger-based milestones such as crafting an item or building an entity. Call with {} to list every currently relevant technology. Set technology only to an exact technology name returned by this tool; item and recipe names are not technology names. Omit the optional field when listing rather than sending an empty string or null. When this reports active lab research, call wait_for_research once instead of polling this tool.",
            {
                {"type", "object"},
                {"properties", {
                    {"technology", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                }},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "start_research",
            "Start an available lab technology when research is idle, or append it to the force research queue. Trigger-based milestones cannot be queued; perform the reported trigger action instead. Use get_research_status first, then place a lab and insert the required science packs into its input inventory. When the technology becomes current, call wait_for_research once instead of polling its status.",
            {
                {"type", "object"},
                {"properties", {
                    {"technology", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                }},
                {"required", {"technology"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "wait_for_research",
            "Wait inside one tool call for the specified current lab technology to finish. Use the exact current technology name reported by get_research_status or start_research. Returns early if research completes, stops making progress, is cancelled, or the timeout expires. Do not repeatedly poll get_research_status while lab research is active.",
            {
                {"type", "object"},
                {"properties", {
                    {"technology", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"maximum_duration_seconds", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 600.0}}},
                }},
                {"required", {"technology", "maximum_duration_seconds"}},
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
            "Move items from a reachable entity into the controlled character's inventory. Use the exact inventory identifier reported by get_nearby_entities: 'chest' for chests or logistic containers, 'input' for lab or crafting input, 'fuel' for burner fuel, and 'output' for furnaces or assemblers. Mining drills emit onto the ground or transport belts rather than exposing a retrievable output inventory.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"item", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}},
                    {"inventory", {{"type", "string"}, {"enum", {"chest", "fuel", "input", "output"}}}},
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
    if (agentTask_)
    {
        definitions_.push_back(FunctionTool(
            "return_to_task_issuer",
            "Return the dedicated FactorIA character to the in-game player who issued the current task. This bounded tool follows the player's current position with normal walking. It refuses to finish while the current task's matching entity ghosts or deconstruction marks remain within its search radius, and reports task_terminal=true only after returning or an unrecoverable issuer/surface failure.",
            EmptyParameters()));
    }
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

std::optional<FactorioAgentTask> FactorioTools::PeekAgentTask() const
{
    const auto result = ExecuteBridgeCommand("peek_agent_task");
    const auto task = result.find("task");
    if (task == result.end() || task->is_null())
        return std::nullopt;
    return ParseAgentTask(*task);
}

bool FactorioTools::ClaimAgentTask(std::uint64_t taskId) const
{
    const auto result = ExecuteBridgeCommand("claim_agent_task", {{"task_id", taskId}});
    return result.value("claimed", false);
}

void FactorioTools::FinishAgentTask(std::uint64_t taskId, bool succeeded, const std::string& message) const
{
    const auto result = ExecuteBridgeCommand(
        "finish_agent_task",
        {{"task_id", taskId}, {"succeeded", succeeded}, {"message", message}});
    if (!result.value("finished", false))
        throw std::runtime_error("Factorio could not finish agent task " + std::to_string(taskId));
}

void FactorioTools::PrintModelDecision(const std::string& decision) const
{
    const auto result = ExecuteBridgeCommand("print_model_decision", {{"decision", decision}});
    if (!result.value("printed", false))
        throw std::runtime_error("Factorio rejected an empty model decision");
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
        RejectUnknownArguments(arguments, {"recipe", "item"});
        return GetCraftableRecipes(arguments);
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
    if (name == "wait_for_research")
    {
        RejectUnknownArguments(arguments, {"technology", "maximum_duration_seconds"});
        return WaitForResearch(arguments, stopToken);
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
    if (name == "return_to_task_issuer")
    {
        RejectUnknownArguments(arguments, {});
        return ReturnToTaskIssuer(stopToken);
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

json FactorioTools::GetBridgeStatus() const
{
    return ExecuteBridgeCommand("status");
}

json FactorioTools::ExecuteBridgeCommand(std::string_view operation, const json& arguments) const
{
    const json request{
        {"protocol_version", BridgeCommandProtocolVersion},
        {"operation", std::string(operation)},
        {"arguments", arguments},
    };
    return ParseBridgeResponse(executeCommand_(std::string(BridgeCommand) + request.dump()));
}

json FactorioTools::ExecuteBridgeTool(const std::string& name, const json& arguments) const
{
    return ExecuteBridgeCommand(
        "execute_tool",
        {
            {"use_dedicated_character", useDedicatedCharacter_},
            {"tool", name},
            {"arguments", arguments},
        });
}

void FactorioTools::ValidatePlayerControlRuntime() const
{
    const auto info = ExecuteBridgeCommand("runtime_info", {{"action", "player_control"}});
    if (info.value("version", 0) != 3 || !info.value("action_installed", false) ||
        !info.value("storage_backed_jobs", false))
    {
        throw std::runtime_error("The FactorIA Bridge runtime protocol version is not supported");
    }
}

json FactorioTools::StartPlayerControlAction(const json& arguments) const
{
    ValidatePlayerControlRuntime();
    auto runtimeArguments = arguments;
    runtimeArguments["use_dedicated_character"] = useDedicatedCharacter_;
    return ExecuteBridgeCommand(
        "start_action",
        {{"action", "player_control"}, {"arguments", std::move(runtimeArguments)}});
}

json FactorioTools::PollPlayerControlAction(std::uint64_t jobId) const
{
    return ExecuteBridgeCommand("poll_action", {{"job_id", jobId}});
}

json FactorioTools::StopPlayerControlAction(std::uint64_t jobId) const
{
    return ExecuteBridgeCommand("stop_action", {{"job_id", jobId}});
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
    return ExecuteBridgeTool("get_game_state", json::object());
}

json FactorioTools::GetInventory() const
{
    auto result = ExecuteBridgeTool("get_inventory", json::object());
    if (!result.value("items", json::array()).is_array())
        result["items"] = json::array();
    return result;
}

json FactorioTools::GetCraftableRecipes(const json& arguments) const
{
    const auto recipe = PrototypeNameOrEmpty(arguments, "recipe");
    const auto item = PrototypeNameOrEmpty(arguments, "item");
    if (!recipe.empty() && !item.empty())
        throw std::runtime_error("get_craftable_recipes accepts either recipe or item, not both");

    auto request = json::object();
    if (!recipe.empty())
        request["recipe"] = recipe;
    if (!item.empty())
        request["item"] = item;
    auto result = ExecuteBridgeTool("get_craftable_recipes", request);
    if (!result.value("recipes", json::array()).is_array())
        result["recipes"] = json::array();
    return result;
}

json FactorioTools::GetResearchStatus(const json& arguments) const
{
    const auto requestedTechnology = PrototypeNameOrEmpty(arguments, "technology");

    auto request = json::object();
    if (!requestedTechnology.empty())
        request["technology"] = requestedTechnology;
    auto result = ExecuteBridgeTool("get_research_status", request);
    for (const auto field : {"queue", "available", "triggered_milestones"})
    {
        const auto value = result.find(field);
        if (value != result.end() && !value->is_array())
            result[field] = json::array();
    }
    const auto activeTechnology = requestedTechnology.empty()
        ? result.find("current")
        : result.find("technology");
    if (activeTechnology != result.end() && activeTechnology->is_object() &&
        activeTechnology->contains("progress") &&
        activeTechnology->value("research_mode", std::string{}) == "lab")
    {
        AttachResearchWaitHint(result, activeTechnology->value("name", requestedTechnology));
    }
    return result;
}

json FactorioTools::StartResearch(const json& arguments) const
{
    const auto technologyName = RequiredPrototypeName(arguments, "technology");
    auto result = ExecuteBridgeTool("start_research", {{"technology", technologyName}});
    const auto currentTechnology = result.value("current_research", std::string{});
    if (result.value("accepted", false) && currentTechnology == technologyName)
        AttachResearchWaitHint(result, technologyName);
    return result;
}

json FactorioTools::WaitForResearch(const json& arguments, std::stop_token stopToken) const
{
    const auto technologyName = RequiredPrototypeName(arguments, "technology");
    const auto maximumSeconds = RequiredNumber(arguments, "maximum_duration_seconds", 1.0, 600.0);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = DeadlineAfter(maximumSeconds);
    std::optional<double> previousProgress;
    std::optional<int> initialLevel;
    int consecutiveIdlePolls = 0;
    int pollCount = 0;
    json snapshot = json::object();

    auto finish = [&](std::string reason) {
        snapshot["reason"] = std::move(reason);
        snapshot["requested_technology"] = technologyName;
        snapshot["poll_count"] = pollCount;
        snapshot["elapsed_seconds"] = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        return snapshot;
    };

    while (!stopToken.stop_requested())
    {
        snapshot = GetResearchStatus({{"technology", technologyName}});
        ++pollCount;
        const auto technology = snapshot.find("technology");
        if (technology == snapshot.end() || !technology->is_object())
            throw std::runtime_error("Factorio returned no status for technology: " + technologyName);

        if (technology->value("researched", false))
        {
            snapshot["target_met"] = true;
            snapshot["completed"] = true;
            return finish("research_completed");
        }
        if (!snapshot.value("research_enabled", true))
        {
            snapshot["target_met"] = false;
            snapshot["stalled"] = true;
            return finish("research_disabled");
        }
        if (technology->value("research_mode", std::string{}) != "lab")
        {
            snapshot["target_met"] = false;
            snapshot["stalled"] = true;
            return finish("trigger_action_required");
        }

        const auto level = technology->value("level", 0);
        if (!initialLevel)
            initialLevel = level;
        else if (level != *initialLevel)
        {
            // Infinite technologies remain unresearched, so a level change is their completion signal.
            snapshot["target_met"] = true;
            snapshot["completed"] = true;
            return finish("research_level_completed");
        }

        const auto progress = technology->find("progress");
        if (progress == technology->end() || !progress->is_number())
        {
            snapshot["target_met"] = false;
            snapshot["completed"] = false;
            return finish(technology->value("queued", false)
                ? "research_queued_not_current"
                : "research_not_current");
        }

        const auto currentProgress = progress->get<double>();
        if (!previousProgress || currentProgress > *previousProgress)
        {
            consecutiveIdlePolls = 0;
        }
        else
        {
            ++consecutiveIdlePolls;
            if (consecutiveIdlePolls >= 3)
            {
                snapshot.erase("wait_tool");
                snapshot.erase("wait_tool_arguments");
                snapshot["target_met"] = false;
                snapshot["stalled"] = true;
                return finish("research_not_progressing");
            }
        }
        previousProgress = currentProgress;

        if (std::chrono::steady_clock::now() >= deadline)
        {
            snapshot["target_met"] = false;
            snapshot["timed_out"] = true;
            return finish("timeout");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    snapshot.erase("wait_tool");
    snapshot.erase("wait_tool_arguments");
    snapshot["target_met"] = false;
    snapshot["stopped"] = true;
    return finish("stopped");
}

json FactorioTools::GetNearbyEntities(const json& arguments) const
{
    const auto radius = RequiredNumber(arguments, "radius", 1.0, MaximumSearchRadius);
    const auto offset = IntegerOrDefault(arguments, "offset", 0, 0, 10000);
    auto request = json{{"radius", radius}, {"offset", offset}};
    if (HasArgumentValue(arguments, "name"))
        request["name"] = RequiredPrototypeName(arguments, "name");
    if (HasArgumentValue(arguments, "type"))
        request["type"] = RequiredPrototypeName(arguments, "type");

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

        // Keep ECMAScript matching in C++, but let the bridge own the Factorio entity survey.
        const auto catalog = ExecuteBridgeTool("nearby_entity_prototypes", request);

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

        request.erase("name");
        request["names"] = std::move(matchingNames);
        request["regex"] = expression;
    }

    auto result = ExecuteBridgeTool("get_nearby_entities", request);
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

    auto result = ExecuteBridgeTool("get_construction_requests", {
        {"radius", radius},
        {"kind", kind},
        {"offset", offset},
        {"available_only", availableOnly},
    });
    for (const auto field : {"missing_items", "requests"})
    {
        if (!result.value(field, json::array()).is_array())
            result[field] = json::array();
    }
    return result;
}
json FactorioTools::ReturnToTaskIssuer(std::stop_token stopToken) const
{
    if (!agentTask_)
        throw std::runtime_error("return_to_task_issuer is only available during an in-game agent task");

    constexpr double MaximumReturnSeconds = 120.0;
    const auto start = StartPlayerControlAction({
        {"kind", "return_to_task_issuer"},
        {"task_id", agentTask_->id},
        {"radius", agentTask_->searchRadius},
        {"maximum_duration_seconds", MaximumReturnSeconds},
    });
    return WaitForPlayerControlAction(
        start,
        DeadlineAfter(MaximumReturnSeconds + 2.0),
        stopToken);
}
json FactorioTools::BuildGhosts(const json& arguments, std::stop_token stopToken) const
{
    const auto batch = RequiredConstructionBatch(arguments);
    const auto start = StartPlayerControlAction({
        {"kind", "build_ghosts"},
        {"radius", batch.radius},
        {"count", batch.count},
        {"maximum_duration_seconds", batch.maximumSeconds},
    });
    return WaitForPlayerControlAction(
        start,
        DeadlineAfter(batch.maximumSeconds + 2.0),
        stopToken);
}

json FactorioTools::DeconstructMarked(const json& arguments, std::stop_token stopToken) const
{
    const auto batch = RequiredConstructionBatch(arguments);
    const auto start = StartPlayerControlAction({
        {"kind", "deconstruct_marked"},
        {"radius", batch.radius},
        {"count", batch.count},
        {"maximum_duration_seconds", batch.maximumSeconds},
    });
    return WaitForPlayerControlAction(
        start,
        DeadlineAfter(batch.maximumSeconds + 2.0),
        stopToken);
}

json FactorioTools::FindResourcePatches(const json& arguments) const
{
    const auto resource = RequiredPrototypeName(arguments, "resource");
    const auto radius = RequiredNumber(arguments, "radius", 32.0, MaximumSearchRadius);
    auto result = ExecuteBridgeTool(
        "find_resource_patches",
        {{"resource", resource}, {"radius", radius}});
    if (!result.value("patches", json::array()).is_array())
        result["patches"] = json::array();
    return result;
}

json FactorioTools::FindWater(const json& arguments) const
{
    const auto radius = RequiredNumber(arguments, "radius", 32.0, MaximumSearchRadius);
    auto result = ExecuteBridgeTool("find_water", {{"radius", radius}});
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

json FactorioTools::WalkTo(const json& arguments, std::stop_token stopToken) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto stoppingDistance = NumberOrDefault(arguments, "stopping_distance", 1.5, 0.5, 10.0);
    const auto maximumSeconds = NumberOrDefault(arguments, "maximum_duration_seconds", 60.0, 1.0, 120.0);
    const auto start = StartPlayerControlAction({
        {"kind", "walk_to"},
        {"target", {{"x", targetX}, {"y", targetY}}},
        {"stopping_distance", stoppingDistance},
        {"maximum_duration_seconds", maximumSeconds},
    });
    return WaitForPlayerControlAction(
        start,
        DeadlineAfter(maximumSeconds + 2.0),
        stopToken);
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

    return ExecuteBridgeTool("find_connection_placement", {
        {"item", item},
        {"target_x", targetX},
        {"target_y", targetY},
        {"connection", connection},
        {"search_radius", searchRadius},
    });
}

json FactorioTools::WalkToForPlacement(const json& arguments, std::stop_token stopToken) const
{
    const auto request = RequiredPlacementRequest(arguments);
    const auto maximumSeconds = NumberOrDefault(
        arguments,
        "maximum_duration_seconds",
        60.0,
        1.0,
        120.0);
    const auto start = StartPlayerControlAction({
        {"kind", "walk_to_for_placement"},
        {"item", request.item},
        {"x", request.x},
        {"y", request.y},
        {"direction", request.direction},
        {"maximum_duration_seconds", maximumSeconds},
    });
    return WaitForPlayerControlAction(
        start,
        DeadlineAfter(maximumSeconds + 2.0),
        stopToken);
}

json FactorioTools::StopWalking() const
{
    return ExecuteBridgeTool("stop_walking", json::object());
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

    ExecuteBridgeTool("take_screenshot", {
        {"width", width},
        {"height", height},
        {"zoom", zoom},
        {"path", "FactorIA/" + fileName},
    });

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
        maximumSeconds,
        stopToken);
}


json FactorioTools::RunMiningAction(
    json runtimeArguments,
    double maximumSeconds,
    std::stop_token stopToken) const
{
    const auto start = StartPlayerControlAction(runtimeArguments);
    auto result = WaitForPlayerControlAction(
        start,
        DeadlineAfter(maximumSeconds + 2.0),
        stopToken);
    result["maximum_duration_seconds"] = maximumSeconds;
    result["mining_control"] = "factorio_bridge_runtime";
    return result;
}

json FactorioTools::Craft(const json& arguments, std::stop_token stopToken) const
{
    const auto recipe = RequiredPrototypeName(arguments, "recipe");
    const auto count = RequiredInteger(arguments, "count", 1, 100);
    constexpr auto MaximumCraftingWait = std::chrono::minutes(10);
    const auto start = StartPlayerControlAction({
        {"kind", "craft"},
        {"recipe", recipe},
        {"count", count},
    });
    return WaitForPlayerControlAction(
        start,
        std::chrono::steady_clock::now() + MaximumCraftingWait + std::chrono::seconds(2),
        stopToken);
}
json FactorioTools::PlaceEntity(const json& arguments) const
{
    const auto request = RequiredPlacementRequest(arguments);
    return ExecuteBridgeTool("place_entity", {
        {"item", request.item},
        {"x", request.x},
        {"y", request.y},
        {"direction", request.direction},
    });
}
json FactorioTools::SetAssemblerRecipe(const json& arguments) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto recipeName = RequiredPrototypeName(arguments, "recipe");
    return ExecuteBridgeTool("set_assembler_recipe", {
        {"x", targetX},
        {"y", targetY},
        {"recipe", recipeName},
    });
}
json FactorioTools::InsertItemIntoEntity(const json& arguments) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto item = RequiredPrototypeName(arguments, "item");
    const auto count = RequiredInteger(arguments, "count", 1, 10000);
    const auto inventory = RequiredText(arguments, "inventory", 16);
    return ExecuteBridgeTool("insert_item_into_entity", {
        {"x", targetX},
        {"y", targetY},
        {"item", item},
        {"count", count},
        {"inventory", inventory},
    });
}
json FactorioTools::TakeItemFromEntity(const json& arguments) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto item = RequiredPrototypeName(arguments, "item");
    const auto count = RequiredInteger(arguments, "count", 1, 10000);
    const auto inventory = RequiredText(arguments, "inventory", 16);
    return ExecuteBridgeTool("take_item_from_entity", {
        {"x", targetX},
        {"y", targetY},
        {"item", item},
        {"count", count},
        {"inventory", inventory},
    });
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
        snapshot = ExecuteBridgeTool("machine_output", {
            {"x", targetX},
            {"y", targetY},
            {"item", item},
        });
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
    return ExecuteBridgeTool("transfer_inventory_to_container", {
        {"x", targetX},
        {"y", targetY},
    });
}
}
