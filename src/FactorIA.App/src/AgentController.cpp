#include <FactorIA/AgentController.h>

#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

namespace factoria
{
namespace
{
using json = nlohmann::json;

constexpr const char* SystemPrompt = R"(You are controlling one player in Factorio through typed tools.
Observe the game before acting. Use only tool results as facts about the world.
Take one small gameplay action at a time, then observe its result.
Before every tool call, include a brief decision summary in the assistant message: state the relevant observation, the action chosen, and why it advances the objective.
Factorio map X increases east and map Y increases south. Prefer walk_to for known coordinates.
When an objective needs ore or another resource whose location is unknown, call find_resource_patches before moving. Start with radius 512 and increase it if needed; use the nearest_position of a returned patch as the walking target. Do not wander using repeated short walks to search for resources.
walk_to uses Factorio's collision-aware pathfinder when the FactorIA Bridge mod is installed; do not treat one direct obstruction as proof that the destination is unreachable.
To gather resources, walk within reach and call mine_entity with a useful batch count. For ore, coal, or stone, normally request 10-50 units in one continuous call and allow roughly two seconds per requested unit. Do not mine resource nodes through repeated count=1 calls. Use count=1 for a tree, rock, or other discrete entity.
When you need to decide what can be made from the current inventory, call get_craftable_recipes instead of guessing recipe availability.
To build a crafted entity, call place_entity on a nearby open tile; use the returned exact entity position for later interactions.
To operate a furnace, insert fuel with inventory='fuel', insert ore with inventory='input', inspect the furnace with get_nearby_entities, and collect plates with take_item_from_entity using inventory='output'.
To empty the player's inventory into a chest or logistic container, walk within reach and call transfer_inventory_to_container with the container's exact position.
Use take_screenshot when spatial context would materially help and image capture is available.
Nearby-entity results return distinct prototypes before duplicate instances. If the target is not present and next_offset is returned, inspect the next page before concluding that it is absent. Use exact name or type filters to inspect additional instances after discovering their prototype.
Do not request teleportation, item spawning, raw Lua, or other cheats.
When the user's objective is complete or cannot be advanced with the available tools, explain the result concisely.)";
}

AgentController::AgentController(LlamaClient llama, FactorioTools& tools)
    : llama_(std::move(llama)), tools_(tools)
{
}

AgentRunResult AgentController::Run(
    const std::string& objective,
    int maximumRounds,
    std::stop_token stopToken,
    const TraceHandler& trace) const
{
    if (objective.empty())
        throw std::runtime_error("Agent objective cannot be empty");
    if (maximumRounds < 1 || maximumRounds > 50)
        throw std::runtime_error("Agent maximum rounds must be between 1 and 50");

    json messages = json::array({
        {{"role", "system"}, {"content", SystemPrompt}},
        {{"role", "user"}, {"content", objective}},
    });

    bool supportsImageInput = false;
    try
    {
        supportsImageInput = llama_.SupportsImageInput();
    }
    catch (const std::exception& error)
    {
        // A catalog failure should not block text-only gameplay.
        if (trace)
            trace("[MODEL CAPABILITIES] Screenshot tool disabled: " + std::string(error.what()));
    }
    const auto toolDefinitions = tools_.Definitions(supportsImageInput);

    AgentRunResult result;
    for (int round = 1; round <= maximumRounds; ++round)
    {
        if (stopToken.stop_requested())
        {
            result.stopped = true;
            return result;
        }

        result.rounds = round;
        if (trace)
        {
            trace("========== AGENT ROUND " + std::to_string(round) + " OF " +
                std::to_string(maximumRounds) + " ==========");
        }
        auto turn = llama_.Complete(messages, toolDefinitions, trace);
        messages.push_back(turn.assistantMessage);

        if (trace && !turn.toolCalls.empty())
        {
            trace("[MODEL DECISION]\n" + (turn.content.empty()
                ? std::string("The model did not return a visible decision summary.")
                : turn.content));
        }

        if (turn.toolCalls.empty())
        {
            result.finalText = turn.content;
            if (result.finalText.empty())
                result.finalText = "The model ended without a final response.";
            return result;
        }

        std::vector<std::string> imageDataUrls;
        for (const auto& call : turn.toolCalls)
        {
            if (stopToken.stop_requested())
            {
                result.stopped = true;
                return result;
            }

            if (trace)
            {
                trace("[TOOL CALL] " + call.name + "\nArguments:\n" + call.arguments.dump(2));
            }
            json toolResult;
            try
            {
                toolResult = {{"ok", true}, {"result", tools_.Execute(call.name, call.arguments, stopToken)}};
            }
            catch (const std::exception& error)
            {
                toolResult = {{"ok", false}, {"error", error.what()}};
            }
            std::string imageDataUrl;
            if (toolResult.value("ok", false) && toolResult["result"].is_object())
            {
                auto image = toolResult["result"].find("_image_data_url");
                if (image != toolResult["result"].end() && image->is_string())
                {
                    imageDataUrl = image->get<std::string>();
                    toolResult["result"].erase(image);
                    toolResult["result"]["image_attached"] = true;
                }
            }
            if (trace)
            {
                trace("[TOOL RESULT] " + call.name + "\n" + toolResult.dump(2));
            }
            messages.push_back({
                {"role", "tool"},
                {"tool_call_id", call.id},
                {"content", toolResult.dump()},
            });
            if (!imageDataUrl.empty())
                imageDataUrls.push_back(std::move(imageDataUrl));
        }
        for (auto& imageDataUrl : imageDataUrls)
        {
            messages.push_back({
                {"role", "user"},
                {"content", json::array({
                    {{"type", "text"}, {"text", "This is the Factorio screenshot requested by the preceding tool call."}},
                    {{"type", "image_url"}, {"image_url", {{"url", std::move(imageDataUrl)}}}},
                })},
            });
        }
    }

    throw std::runtime_error("Agent stopped after reaching the configured maximum rounds");
}
}
