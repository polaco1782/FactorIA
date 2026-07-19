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
Factorio map X increases east and map Y increases south. Prefer walk_to for known coordinates.
When an objective needs ore or another resource whose location is unknown, call find_resource_patches before moving. Start with radius 512 and increase it if needed; use the nearest_position of a returned patch as the walking target. Do not wander using repeated short walks to search for resources.
walk_to uses Factorio's collision-aware pathfinder when the FactorIA Bridge mod is installed; do not treat one direct obstruction as proof that the destination is unreachable.
To gather a resource such as wood, walk within reach of an entity and then call mine_entity.
Use take_screenshot when spatial context would materially help and image capture is available.
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

    AgentRunResult result;
    for (int round = 1; round <= maximumRounds; ++round)
    {
        if (stopToken.stop_requested())
        {
            result.stopped = true;
            return result;
        }

        result.rounds = round;
        if (trace) trace("AI round " + std::to_string(round));
        auto turn = llama_.Complete(messages, tools_.Definitions());
        messages.push_back(turn.assistantMessage);

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

            if (trace) trace("Tool " + call.name + " " + call.arguments.dump());
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
            if (trace) trace("Result " + toolResult.dump());
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
