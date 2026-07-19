#include <FactorIA/FactorioTools.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <thread>

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
}

FactorioTools::FactorioTools(CommandExecutor executeCommand)
    : executeCommand_(std::move(executeCommand))
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
        "crafting_queue_size=p.crafting_queue_size}");
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

json FactorioTools::WalkTo(const json& arguments, std::stop_token stopToken) const
{
    const auto targetX = RequiredNumber(arguments, "x", -1000000.0, 1000000.0);
    const auto targetY = RequiredNumber(arguments, "y", -1000000.0, 1000000.0);
    const auto stoppingDistance = RequiredNumber(arguments, "stopping_distance", 0.5, 10.0);
    const auto maximumSeconds = RequiredNumber(arguments, "maximum_duration_seconds", 1.0, 30.0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(maximumSeconds);
    auto progressDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto bestDistance = std::numeric_limits<double>::infinity();
    json lastState = json::object();

    const auto pulseBody =
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "local tx=" + std::to_string(targetX) + " local ty=" + std::to_string(targetY) + " "
        "local dx=tx-p.position.x local dy=ty-p.position.y local distance=math.sqrt(dx*dx+dy*dy) "
        "if distance<=" + std::to_string(stoppingDistance) + " then "
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
            return lastState;

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
            result["distance"] = distance;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto result = StopWalking();
    result["reached"] = false;
    result["stopped"] = stopToken.stop_requested();
    result["timed_out"] = !stopToken.stop_requested();
    if (lastState.contains("distance")) result["distance"] = lastState["distance"];
    return result;
}

json FactorioTools::StopWalking() const
{
    return ExecuteJson(
        "local p=game.connected_players[1] if not p then error('No connected player') end "
        "p.walking_state={walking=false,direction=defines.direction.north} "
        "return {position={x=p.position.x,y=p.position.y}}");
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
