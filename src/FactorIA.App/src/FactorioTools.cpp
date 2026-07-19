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

std::string MachineInventoryExpression(const std::string& inventory)
{
    if (inventory == "fuel") return "defines.inventory.fuel";
    if (inventory == "input") return "defines.inventory.crafter_input";
    if (inventory == "output") return "defines.inventory.crafter_output";
    throw std::runtime_error("inventory must be fuel, input, or output");
}

std::string MachineLookupLua(
    const std::string& controlCharacterLua,
    double targetX,
    double targetY,
    bool requireReach = true)
{
    return
        controlCharacterLua +
        "local position={x=" + std::to_string(targetX) + ",y=" + std::to_string(targetY) + "} "
        "local found=p.surface.find_entities_filtered{position=position,radius=0.75,"
        "type={'furnace','assembling-machine'}} "
        "table.sort(found,function(a,b) local adx=a.position.x-position.x local ady=a.position.y-position.y "
        "local bdx=b.position.x-position.x local bdy=b.position.y-position.y "
        "return adx*adx+ady*ady<bdx*bdx+bdy*bdy end) "
        "local target=found[1] if not target then error('No furnace or crafting machine found at the requested position') end "
        + (requireReach
            ? "if not p.can_reach_entity(target) then error('Target machine is out of reach') end "
            : "");
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
            "Get the game tick and the controlled character's identity, position, walking state, and crafting queue size.",
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
            "get_nearby_entities",
            "Return a page of up to 40 nearby entities with character-relative deltas. Use regex for a case-insensitive ECMAScript search over both prototype names and entity types, for example regex='furnace' finds stone-furnace and every entity of type furnace. Exact name and type filters are also available. Distinct prototypes are returned before duplicates, and next_offset retrieves another page. Factorio X increases east and Y increases south, so a negative delta_y is north.",
            {
                {"type", "object"},
                {"properties", {
                    {"radius", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 128.0}}},
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
            "Survey a large generated area for resource entities and return the nearest grouped patch regions with exact walking targets. Use resource='iron-ore', 'copper-ore', 'coal', 'stone', 'uranium-ore', or 'any'. Prefer this over wandering when locating ore.",
            {
                {"type", "object"},
                {"properties", {
                    {"resource", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"radius", {{"type", "number"}, {"minimum", 32.0}, {"maximum", 2048.0}}},
                }},
                {"required", {"resource", "radius"}},
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
            "Walk toward map coordinates using normal eight-direction player movement. Factorio X increases east and Y increases south. Stops near the target or reports an obstruction/timeout.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"stopping_distance", {{"type", "number"}, {"minimum", 0.5}, {"maximum", 10.0}}},
                    {"maximum_duration_seconds", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 30.0}}},
                }},
                {"required", {"x", "y", "stopping_distance", "maximum_duration_seconds"}},
                {"additionalProperties", false},
            }),
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
            "Continuously mine the entity at exact map coordinates using normal player mining. For resource nodes, count is the number of resource units to extract. Coordinates identify only one tree, rock, or other discrete entity, so their effective count is automatically one. The player must first be within reach.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}}},
                    {"maximum_duration_seconds", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 120.0}}},
                }},
                {"required", {"x", "y", "count", "maximum_duration_seconds"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "craft",
            "Begin hand-crafting an unlocked recipe using items in the controlled character's inventory.",
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
            "Place one buildable item from the controlled character's inventory on a reachable open tile using normal Factorio build rules. The item is consumed only when placement succeeds.",
            {
                {"type", "object"},
                {"properties", {
                    {"item", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"direction", {{"type", "string"}, {"enum", {"north", "east", "south", "west"}}}},
                }},
                {"required", {"item", "x", "y", "direction"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "insert_item_into_entity",
            "Move a specified item count from the controlled character into a reachable furnace or crafting machine. Use inventory='fuel' for fuel and inventory='input' for ingredients such as ore.",
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
            "Move a specified item count from a reachable furnace or crafting machine into the controlled character's inventory. Use inventory='output' to collect smelted plates.",
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
        RejectUnknownArguments(arguments, {"x", "y", "count", "maximum_duration_seconds"});
        return MineEntity(arguments, stopToken);
    }
    if (name == "craft")
    {
        RejectUnknownArguments(arguments, {"recipe", "count"});
        return Craft(arguments);
    }
    if (name == "place_entity")
    {
        RejectUnknownArguments(arguments, {"item", "x", "y", "direction"});
        return PlaceEntity(arguments);
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
        "environment={surface=p.surface.name,daytime=p.surface.daytime,darkness=p.surface.darkness,"
        "pollution=p.surface.get_pollution(p.position)},"
        "bridge_mod_available=bridge~=nil,"
        "bridge_runtime_available=bridge~=nil and bridge.runtime_info~=nil and bridge.start_action~=nil}");
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

json FactorioTools::GetNearbyEntities(const json& arguments) const
{
    const auto radius = RequiredNumber(arguments, "radius", 1.0, 128.0);
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
        ControlCharacterLua() +
        "local function summarize(inv,limit) if not inv then return nil end local items={} "
        "for _,item in pairs(inv.get_contents()) do items[#items+1]={name=item.name,count=item.count} end "
        "table.sort(items,function(a,b) return a.count>b.count end) "
        "while #items>limit do table.remove(items) end if #items==0 then return nil end return items end "
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
        "distance=math.sqrt(dx*dx+dy*dy),direction=e.direction,health=e.health,"
        "minable=e.minable,reachable=p.can_reach_entity(e),prototype_count_in_radius=counts[e.name]} "
        "if e.type=='resource' then info.amount=e.amount "
        "elseif e.type=='container' or e.type=='logistic-container' then "
        "info.inventory={items=summarize(e.get_inventory(defines.inventory.chest),6)} "
        "elseif e.type=='furnace' then info.crafting=e.is_crafting() "
        "if info.crafting then info.wait_tool='wait_for_machine_output' end info.inventory={"
        "fuel=summarize(e.get_inventory(defines.inventory.fuel),4),"
        "input=summarize(e.get_inventory(defines.inventory.crafter_input),4),"
        "output=summarize(e.get_inventory(defines.inventory.crafter_output),4)} "
        "elseif e.type=='assembling-machine' then info.crafting=e.is_crafting() "
        "if info.crafting then info.wait_tool='wait_for_machine_output' end end "
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
    const auto radius = RequiredNumber(arguments, "radius", 32.0, 2048.0);
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
    const auto stoppingDistance = RequiredNumber(arguments, "stopping_distance", 0.5, 10.0);
    const auto maximumSeconds = RequiredNumber(arguments, "maximum_duration_seconds", 1.0, 30.0);
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
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto count = RequiredInteger(arguments, "count", 1, 200);
    const auto maximumSeconds = RequiredNumber(arguments, "maximum_duration_seconds", 1.0, 120.0);
    const auto start = StartPlayerControlAction({
        {"kind", "mine"},
        {"target", {{"x", targetX}, {"y", targetY}}},
        {"count", count},
        {"maximum_duration_seconds", maximumSeconds},
    });
    auto result = WaitForPlayerControlAction(
        start,
        DeadlineAfter(maximumSeconds + 2.0),
        stopToken);
    if (result.value("mined", false))
        result["inventory"] = GetInventory();
    result["mining_control"] = "factorio_bridge_runtime";
    return result;
}

json FactorioTools::Craft(const json& arguments) const
{
    const auto recipe = RequiredPrototypeName(arguments, "recipe");
    const auto count = RequiredInteger(arguments, "count", 1, 100);
    return ExecuteJson(
        ControlCharacterLua() +
        "local crafted=p.begin_crafting{count=" + std::to_string(count) + ",recipe=\"" + recipe + "\"} "
        "return {queued=crafted,crafting_queue_size=p.crafting_queue_size}");
}

json FactorioTools::PlaceEntity(const json& arguments) const
{
    const auto item = RequiredPrototypeName(arguments, "item");
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto directionValue = arguments.find("direction");
    if (directionValue == arguments.end() || !directionValue->is_string())
        throw std::runtime_error("Missing string argument: direction");
    const auto direction = DirectionExpression(directionValue->get<std::string>());

    return ExecuteJson(
        ControlCharacterLua() +
        "local inv=p.get_main_inventory() if not inv then error('Controlled character has no main inventory') end "
        "local item_name=\"" + item + "\" local source=inv.find_item_stack(item_name) "
        "if not source then error('Player does not have item: " + item + "') end "
        "local place_result=source.prototype.place_result "
        "if not place_result then error('Item cannot be placed as an entity: " + item + "') end "
        "local requested={x=" + std::to_string(targetX) + ",y=" + std::to_string(targetY) + "} "
        "local accepted=p.can_place_entity{name=place_result.name,position=requested,direction=" + direction + "} "
        "local entity=nil if accepted then local quality=source.quality.name "
        "local removed=inv.remove{name=item_name,count=1,quality=quality} "
        "if removed==1 then entity=p.surface.create_entity{name=place_result.name,position=requested,"
        "direction=" + direction + ",force=p.force,quality=quality,raise_built=true} "
        "if not entity then inv.insert{name=item_name,count=1,quality=quality} end end end "
        "local placed=entity~=nil local result={placed=placed,build_accepted=accepted,item=\"" + item + "\",requested_position=requested,"
        "remaining_item_count=inv.get_item_count(\"" + item + "\")} "
        "if entity then result.entity={name=entity.name,position={x=entity.position.x,y=entity.position.y},"
        "direction=entity.direction,unit_number=entity.unit_number} end "
        "if not accepted then result.reason='Factorio build rules rejected this position; choose another open tile within reach' "
        "elseif not placed then result.reason='Factorio accepted the build but the placed entity could not be verified' end "
        "return result");
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
        throw std::runtime_error("Cannot insert items into a machine output inventory");
    const auto inventoryExpression = MachineInventoryExpression(inventory);

    return ExecuteJson(
        MachineLookupLua(ControlCharacterLua(), targetX, targetY) +
        "local source=p.get_main_inventory() if not source then error('Controlled character has no main inventory') end "
        "local item_name=\"" + item + "\" if not prototypes.item[item_name] then error('Unknown item prototype: ' .. item_name) end "
        "local destination=target.get_inventory(" + inventoryExpression + ") "
        "if not destination then error('Target machine has no " + inventory + " inventory') end "
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
    const auto inventoryExpression = MachineInventoryExpression(inventory);

    return ExecuteJson(
        MachineLookupLua(ControlCharacterLua(), targetX, targetY) +
        "local destination=p.get_main_inventory() if not destination then error('Controlled character has no main inventory') end "
        "local item_name=\"" + item + "\" if not prototypes.item[item_name] then error('Unknown item prototype: ' .. item_name) end "
        "local source=target.get_inventory(" + inventoryExpression + ") "
        "if not source then error('Target machine has no " + inventory + " inventory') end "
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
