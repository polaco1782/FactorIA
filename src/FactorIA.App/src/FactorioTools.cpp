#include <FactorIA/FactorioTools.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
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

std::string RequiredPrototypeName(const json& arguments, const std::string& name)
{
    const auto value = arguments.find(name);
    if (value == arguments.end() || !value->is_string())
        throw std::runtime_error("Missing string argument: " + name);
    const auto text = value->get<std::string>();
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
}

FactorioTools::FactorioTools(CommandExecutor executeCommand, std::filesystem::path factorioUserDataPath)
    : executeCommand_(std::move(executeCommand)),
      factorioUserDataPath_(std::move(factorioUserDataPath))
{
    if (!executeCommand_)
        throw std::runtime_error("FactorioTools requires an RCON command executor");

    definitions_ = json::array({
        FunctionTool(
            "get_game_state",
            "Get the game tick and the connected player's name, position, walking state, and crafting queue size.",
            EmptyParameters()),
        FunctionTool(
            "get_inventory",
            "List every item and count in the connected player's main inventory.",
            EmptyParameters()),
        FunctionTool(
            "get_nearby_entities",
            "Return the 40 nearest entities, sorted by distance, with player-relative deltas. Factorio X increases east and Y increases south, so a negative delta_y is north.",
            {
                {"type", "object"},
                {"properties", {{"radius", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 128.0}}}}},
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
            "Stop player walking immediately.",
            EmptyParameters()),
        FunctionTool(
            "take_screenshot",
            "Capture the visible Factorio world centered on the player and inspect the resulting image. Requires non-headless Factorio, access to its user data directory, and a vision-capable llama.cpp model.",
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
            "Mine the entity at exact map coordinates using normal player mining over time. The player must first be within reach.",
            {
                {"type", "object"},
                {"properties", {
                    {"x", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"y", {{"type", "number"}, {"minimum", -1000000.0}, {"maximum", 1000000.0}}},
                    {"maximum_duration_seconds", {{"type", "number"}, {"minimum", 1.0}, {"maximum", 30.0}}},
                }},
                {"required", {"x", "y", "maximum_duration_seconds"}},
                {"additionalProperties", false},
            }),
        FunctionTool(
            "craft",
            "Begin hand-crafting an unlocked recipe using items in the player's inventory.",
            {
                {"type", "object"},
                {"properties", {
                    {"recipe", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
                }},
                {"required", {"recipe", "count"}},
                {"additionalProperties", false},
            }),
    });
}

const json& FactorioTools::Definitions() const noexcept
{
    return definitions_;
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
    if (name == "get_nearby_entities")
    {
        RejectUnknownArguments(arguments, {"radius"});
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
        RejectUnknownArguments(arguments, {"x", "y", "maximum_duration_seconds"});
        return MineEntity(arguments, stopToken);
    }
    if (name == "craft")
    {
        RejectUnknownArguments(arguments, {"recipe", "count"});
        return Craft(arguments);
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

json FactorioTools::GetGameState() const
{
    return ExecuteJson(
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "return {tick=game.tick,player={name=p.name,position={x=p.position.x,y=p.position.y},"
        "walking=p.walking_state.walking,direction=p.walking_state.direction},"
        "crafting_queue_size=p.crafting_queue_size,"
        "bridge_mod_available=remote.interfaces['factoria_bridge']~=nil}");
}

json FactorioTools::GetInventory() const
{
    return ExecuteJson(
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "local inv=p.get_main_inventory() local items={} "
        "if inv then for _,item in pairs(inv.get_contents()) do "
        "items[#items+1]={name=item.name,count=item.count,quality=tostring(item.quality)} end end "
        "table.sort(items,function(a,b) return a.name<b.name end) return {items=items}");
}

json FactorioTools::GetNearbyEntities(const json& arguments) const
{
    const auto radius = RequiredNumber(arguments, "radius", 1.0, 128.0);
    return ExecuteJson(
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "local found=p.surface.find_entities_filtered{position=p.position,radius=" + std::to_string(radius) + "} "
        "table.sort(found,function(a,b) local adx=a.position.x-p.position.x local ady=a.position.y-p.position.y "
        "local bdx=b.position.x-p.position.x local bdy=b.position.y-p.position.y "
        "return adx*adx+ady*ady<bdx*bdx+bdy*bdy end) "
        "local entities={} for _,e in pairs(found) do if #entities>=40 then break end "
        "local dx=e.position.x-p.position.x local dy=e.position.y-p.position.y "
        "entities[#entities+1]={name=e.name,type=e.type,position={x=e.position.x,y=e.position.y},"
        "delta={x=dx,y=dy},distance=math.sqrt(dx*dx+dy*dy),direction=e.direction,health=e.health} end "
        "return {radius=" + std::to_string(radius) + ",player_position={x=p.position.x,y=p.position.y},"
        "coordinate_hint='positive x is east; positive y is south',entities=entities}");
}

json FactorioTools::FindResourcePatches(const json& arguments) const
{
    const auto resource = RequiredPrototypeName(arguments, "resource");
    const auto radius = RequiredNumber(arguments, "radius", 32.0, 2048.0);
    const auto nameFilter = resource == "any"
        ? std::string{}
        : " filter.name=\"" + resource + "\" ";
    return ExecuteJson(
        "local p=game.connected_players[1] if not p then error('No connected player') end "
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
    const auto direction = DirectionExpression(directionValue->get<std::string>());
    const auto seconds = RequiredNumber(arguments, "duration_seconds", 0.1, 10.0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    const std::string pulse =
        "/sc local p=game.connected_players[1] if p then p.walking_state={walking=true,direction=" +
        direction + "} end";

    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline)
    {
        executeCommand_(pulse);
        std::this_thread::sleep_for(std::chrono::milliseconds(14));
    }
    return StopWalking();
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
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "local id=remote.call('factoria_bridge','request_path',p.index," +
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(maximumSeconds);
    const auto pathResult = RequestPath(targetX, targetY, stoppingDistance, stopToken);
    const auto pathfinderAvailable = pathResult.value("available", false);
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

    std::vector<std::array<double, 2>> waypoints;
    if (pathfinderAvailable)
    {
        for (const auto& waypoint : pathResult.value("path", json::array()))
        {
            const auto& position = waypoint.at("position");
            waypoints.push_back({position.at("x").get<double>(), position.at("y").get<double>()});
        }
    }
    if (waypoints.empty())
        waypoints.push_back({targetX, targetY});

    json lastState = json::object();
    for (std::size_t index = 0; index < waypoints.size(); ++index)
    {
        const auto waypointX = waypoints[index][0];
        const auto waypointY = waypoints[index][1];
        const auto isFinal = index + 1U == waypoints.size();
        const auto waypointStoppingDistance = isFinal
            ? (pathfinderAvailable ? 0.35 : stoppingDistance)
            : 0.65;
        auto progressDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        auto bestDistance = std::numeric_limits<double>::infinity();

        const auto pulseBody =
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "local tx=" + std::to_string(waypointX) + " local ty=" + std::to_string(waypointY) + " "
        "local dx=tx-p.position.x local dy=ty-p.position.y local distance=math.sqrt(dx*dx+dy*dy) "
        "if distance<=" + std::to_string(waypointStoppingDistance) + " then "
        "p.walking_state={walking=false,direction=defines.direction.north} "
        "return {reached=true,distance=distance,position={x=p.position.x,y=p.position.y}} end "
        "local ax=math.abs(dx) local ay=math.abs(dy) local direction "
        "if ax<ay*0.4 then direction=dy<0 and defines.direction.north or defines.direction.south "
        "elseif ay<ax*0.4 then direction=dx<0 and defines.direction.west or defines.direction.east "
        "elseif dx>=0 and dy<0 then direction=defines.direction.northeast "
        "elseif dx>=0 and dy>=0 then direction=defines.direction.southeast "
        "elseif dx<0 and dy>=0 then direction=defines.direction.southwest "
        "else direction=defines.direction.northwest end "
        "p.walking_state={walking=true,direction=direction} "
        "return {reached=false,distance=distance,position={x=p.position.x,y=p.position.y}}";

        while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline)
        {
            lastState = ExecuteJson(pulseBody);
            if (lastState.value("reached", false))
                break;

            const auto distance = lastState.value("distance", bestDistance);
            if (distance < bestDistance - 0.15)
            {
                bestDistance = distance;
                progressDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
            else if (std::chrono::steady_clock::now() >= progressDeadline)
            {
                auto result = StopWalking();
                result["reached"] = false;
                result["blocked"] = true;
                result["distance_to_waypoint"] = distance;
                result["navigation"] = pathfinderAvailable ? "factorio_pathfinder" : "direct_fallback";
                result["pathfinder_available"] = pathfinderAvailable;
                if (!pathfinderAvailable)
                    result["hint"] = "Install and enable the bundled FactorIA Bridge mod so walk_to can route around water and other obstacles.";
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!lastState.value("reached", false))
            break;
    }

    auto result = StopWalking();
    const auto finalState = ExecuteJson(
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "local dx=" + std::to_string(targetX) + "-p.position.x local dy=" +
        std::to_string(targetY) + "-p.position.y return {distance=math.sqrt(dx*dx+dy*dy)}");
    const auto finalDistance = finalState.value("distance", std::numeric_limits<double>::infinity());
    result["reached"] = finalDistance <= stoppingDistance;
    result["distance"] = finalDistance;
    result["navigation"] = pathfinderAvailable ? "factorio_pathfinder" : "direct_fallback";
    result["pathfinder_available"] = pathfinderAvailable;
    result["waypoint_count"] = waypoints.size();
    result["stopped"] = stopToken.stop_requested();
    result["timed_out"] = !result.value("reached", false) && !stopToken.stop_requested();
    return result;
}

json FactorioTools::StopWalking() const
{
    return ExecuteJson(
        "local p=game.connected_players[1] if not p then error('No connected player') end "
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
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "game.take_screenshot{player=p,resolution={x=" + std::to_string(width) +
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
    const auto maximumSeconds = RequiredNumber(arguments, "maximum_duration_seconds", 1.0, 30.0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(maximumSeconds);
    json lastState = json::object();
    bool sawEntity = false;
    bool finished = false;

    const auto pulseBody =
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "local target={x=" + std::to_string(targetX) + ",y=" + std::to_string(targetY) + "} "
        "p.update_selected_entity(target) local e=p.selected "
        "if not e then p.mining_state={mining=false} return {entity_present=false} end "
        "if not p.can_reach_entity(e) then return {mined=false,out_of_reach=true,"
        "entity_present=true,entity={name=e.name,position={x=e.position.x,y=e.position.y}}} end "
        "p.mining_state={mining=true,position=target} "
        "return {mined=false,out_of_reach=false,entity_present=true,progress=p.character_mining_progress,"
        "entity={name=e.name,position={x=e.position.x,y=e.position.y}}}";

    while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline)
    {
        lastState = ExecuteJson(pulseBody);
        if (!lastState.value("entity_present", false))
        {
            lastState["mined"] = sawEntity;
            lastState["missing"] = !sawEntity;
            finished = true;
            break;
        }
        sawEntity = true;
        if (lastState.value("out_of_reach", false))
        {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ExecuteJson(
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "p.mining_state={mining=false} return true");
    if (lastState.value("mined", false))
        lastState["inventory"] = GetInventory();
    else if (!finished)
    {
        lastState["stopped"] = stopToken.stop_requested();
        lastState["timed_out"] = !stopToken.stop_requested();
    }
    return lastState;
}

json FactorioTools::Craft(const json& arguments) const
{
    const auto recipe = RequiredPrototypeName(arguments, "recipe");
    const auto count = RequiredInteger(arguments, "count", 1, 100);
    return ExecuteJson(
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "local crafted=p.begin_crafting{count=" + std::to_string(count) + ",recipe=\"" + recipe + "\"} "
        "return {queued=crafted,crafting_queue_size=p.crafting_queue_size}");
}
}
