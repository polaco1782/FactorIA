#include <FactorIA/AgentController.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace factoria
{
namespace
{
using json = nlohmann::json;

constexpr int MaximumConsecutiveInvalidTerminalTurns = 3;
constexpr int MaximumCompactionAttempts = 3;
constexpr std::size_t LocalPromptTokenCompactionThreshold = 12000;
constexpr std::size_t MinimumReservedContextTokens = 4096;

std::size_t CompactionThreshold(std::size_t contextLength)
{
    // Preserve at least 20% of the model context and enough room for one substantial response.
    const auto reserve = std::max(
        contextLength / 5,
        std::min(MinimumReservedContextTokens, contextLength / 2));
    return contextLength - reserve;
}

std::size_t EstimatePromptTokens(const json& messages, const json& tools)
{
    // Used only when the provider omits usage. Three JSON bytes per token is intentionally
    // conservative for tool-heavy prompts and includes the repeated tool definitions.
    return (messages.dump().size() + tools.dump().size() + 2) / 3;
}

std::string SingleLine(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    bool needsSpace = false;
    for (const auto character : text)
    {
        if (std::isspace(static_cast<unsigned char>(character)))
        {
            needsSpace = !result.empty();
            continue;
        }
        if (needsSpace)
            result.push_back(' ');
        result.push_back(character);
        needsSpace = false;
    }
    return result;
}

std::string SelectedToolsSummary(const std::vector<LlamaToolCall>& toolCalls)
{
    std::string summary = toolCalls.size() == 1 ? "Selected tool: " : "Selected tools: ";
    for (std::size_t index = 0; index < toolCalls.size(); ++index)
    {
        if (index != 0)
            summary += ", ";
        summary += toolCalls[index].name;
    }
    summary += '.';
    return summary;
}

/*
constexpr const char* SystemPrompt = R"(You are an autonomous Factorio player controlling one character through typed tools. Your only terminal objective is to launch a rocket.
Interact with the game exclusively through real calls to the provided tool-calling interface.
Until a tool result explicitly confirms that a rocket was launched, every response must contain at least one real tool call. Never return a plan, summary, explanation, table, JSON command, plain-text answer, or merely the name or description of a tool instead of invoking it. After a tool explicitly confirms the launch, stop and return only a concise final confirmation.
After every tool result, immediately choose and invoke the next tool. Continue acting indefinitely across turns; observation, inventory inspection, planning, and intermediate milestones are never completion.
Treat tool definitions as the authoritative description of available actions and their arguments.
Use specialized state and discovery tools before acting on unknown information. Do not guess names, coordinates, inventory contents, reachability, or crafting availability.
Water is terrain rather than an entity or resource. Locate it with find_water, and do not infer that the map is waterless from entity searches or a limited terrain survey.
Use exact identifiers and positions from the most recent tool results, and satisfy an action's stated preconditions before calling it.
Before placing a distant or terrain-sensitive entity, use walk_to_for_placement with the exact same item, position, and direction, then call place_entity only when placement_ready is true.
Advance through the smallest currently available milestone. Do not scale production for the final objective until the immediate prerequisite works.
Perform a reported research trigger only once, then recheck it. If the same trigger remains incomplete with no changed progress, do not craft or place duplicate trigger items; invoke a genuinely different diagnostic tool.
Take one meaningful gameplay action at a time, then inspect its result. Use batch counts when a tool supports them instead of repeating the same action one unit at a time.
Treat partial results and reported failure conditions as new observations: adapt the next action instead of blindly retrying.
Avoid redundant polling and broad unfiltered queries. Use filters and pagination, and prefer a tool that waits for completion when one is available.
Prioritize survival, power, resources, automation, science, defense, and rocket production. If uncertain, call the most relevant observation tool. If an action fails, inspect the state and try another valid action.
Factorio map X increases east and map Y increases south.
Do not request teleportation, item spawning, raw Lua, or other cheats.
Your next response must be a tool call.)";
*/

constexpr const char* SystemPrompt = R"(You are an autonomous Factorio player controlling one character through typed tools. Your only terminal objective is to launch a rocket.
Interact with the game exclusively through real calls to the provided tool-calling interface. Until a tool result explicitly confirms that a rocket was launched, every response must contain at least one real tool call. Never return a plan, summary, explanation, table, JSON command, plain-text answer, or merely the name or description of a tool instead of invoking it. After a tool explicitly confirms the launch, stop and return only a concise final confirmation.
After every tool result, immediately choose and invoke the next tool. Continue acting indefinitely across turns; observation, inventory inspection, planning, and intermediate milestones are never completion.
Treat tool definitions as the authoritative description of available actions and their arguments. Use specialized state and discovery tools before acting on unknown information. Do not guess names, coordinates, inventory contents, reachability, or crafting availability.
Never combine arguments from different tools. Omit an optional argument when its value is unknown instead of sending an empty string or null.
Treat entity ghosts and deconstruction marks as explicit player intent: inspect them with get_construction_requests and service them with the bounded build_ghosts or deconstruct_marked tools before unrelated expansion.

Movement and range rules:
- You must be within ~5-10 tiles of an entity to MINE it. If it's far away, MOVE closer first.
- You must be within ~5-10 tiles of a position to PLACE buildings. If it's far, MOVE closer first.
- Check the "distance" field on entities - if distance > 8, move closer before mining/interacting.
- You know your current position in character.position.
- Before placing a distant or terrain-sensitive entity, use walk_to_for_placement with the exact same item, position, and direction, then call place_entity only when placement_ready is true.

Water is terrain rather than an entity or resource. Locate it with find_water, and do not infer that the map is waterless from entity searches or a limited terrain survey.
Use exact identifiers and positions from the most recent tool results, and satisfy an action's stated preconditions before calling it.
Advance through the smallest currently available milestone. Do not scale production for the final objective until the immediate prerequisite works.
Perform a reported research trigger only once, then recheck it. If the same trigger remains incomplete with no changed progress, do not craft or place duplicate trigger items; invoke a genuinely different diagnostic tool.
Take one meaningful gameplay action at a time, then inspect its result. Use batch counts when a tool supports them instead of repeating the same action one unit at a time.
Treat partial results and reported failure conditions as new observations: adapt the next action instead of blindly retrying.
Avoid redundant polling and broad unfiltered queries. Use filters and pagination, and prefer a tool that waits for completion when one is available.
Prioritize survival, power, resources, automation, science, defense, and rocket production. If uncertain, call the most relevant observation tool. If an action fails, inspect the state and try another valid action.
Be proactive! Avoid just waiting - explore, gather, build, craft. Always have something to do. If nothing is nearby, MOVE to explore. If you have materials, CRAFT or PLACE. WAIT only when enemies are near or you're in danger.
Factorio map X increases east and map Y increases south.
Do not request teleportation, item spawning, raw Lua, or other cheats.
Your next response must be a tool call.)";

constexpr const char* CompactionSystemPrompt = R"(You are performing an internal context-compaction task, not a Factorio gameplay turn.
Do not call tools. Return only the requested concise plain-text progress summary so a gameplay agent can continue from it.)";
}

AgentController::AgentController(LlamaClient llama, FactorioTools& tools)
    : llama_(std::move(llama)), tools_(tools)
{
}

AgentRunResult AgentController::Run(
    const std::string& objective,
    std::optional<int> maximumRounds,
    std::stop_token stopToken,
    const TraceHandler& trace,
    const DecisionHandler& decision) const
{
    if (objective.empty())
        throw std::runtime_error("Agent objective cannot be empty");
    if (maximumRounds && (*maximumRounds < MinimumRounds || *maximumRounds > MaximumRounds))
    {
        throw std::runtime_error("Agent maximum rounds must be between " +
            std::to_string(MinimumRounds) + " and " + std::to_string(MaximumRounds));
    }

    json messages = json::array({
        {{"role", "system"}, {"content", SystemPrompt}},
        {{"role", "user"}, {"content", objective}},
    });

    LlamaModelCapabilities modelCapabilities;
    try
    {
        modelCapabilities = llama_.Capabilities();
    }
    catch (const std::exception& error)
    {
        // Catalog metadata should not block text-only gameplay.
        if (trace)
            trace("[MODEL CAPABILITIES] OpenRouter metadata unavailable: " + std::string(error.what()));
    }
    const auto toolDefinitions = tools_.Definitions(modelCapabilities.supportsImageInput);
    const auto promptTokenCompactionThreshold = modelCapabilities.contextLength
        ? CompactionThreshold(*modelCapabilities.contextLength)
        : LocalPromptTokenCompactionThreshold;
    if (trace)
    {
        trace("[MODEL CAPABILITIES] Context limit: " +
            (modelCapabilities.contextLength
                ? std::to_string(*modelCapabilities.contextLength) + " tokens"
                : std::string("unknown")) +
            " | Compaction threshold: " + std::to_string(promptTokenCompactionThreshold) + " prompt tokens");
    }

    AgentRunResult result;
    int consecutiveInvalidTerminalTurns = 0;
    bool rocketLaunchConfirmed = false;
    for (int round = 1; !maximumRounds || round <= *maximumRounds; ++round)
    {
        if (stopToken.stop_requested())
        {
            result.stopped = true;
            return result;
        }

        result.rounds = round;
        if (trace)
        {
            trace("========== AGENT ROUND " + std::to_string(round) +
                (maximumRounds ? " OF " + std::to_string(*maximumRounds) : " (NON-STOP)") +
                " ==========");
        }
        auto turn = llama_.Complete(messages, toolDefinitions, trace);
        messages.push_back(turn.assistantMessage);

        if (!turn.toolCalls.empty())
        {
            const auto visibleDecision = SingleLine(turn.content);
            const auto summary = visibleDecision.empty()
                ? SelectedToolsSummary(turn.toolCalls)
                : visibleDecision;
            if (trace)
                trace("[MODEL DECISION] " + summary);
            if (decision)
                decision(summary);
        }

        if (turn.toolCalls.empty())
        {
            ++consecutiveInvalidTerminalTurns;
            if (consecutiveInvalidTerminalTurns >= MaximumConsecutiveInvalidTerminalTurns)
            {
                throw std::runtime_error("AI model returned " +
                    std::to_string(MaximumConsecutiveInvalidTerminalTurns) +
                    " consecutive responses without the required tool call before rocket launch confirmation");
            }
            if (trace)
            {
                trace("[MODEL RETRY] " + std::string(turn.content.empty() ? "Empty" : "Terminal") +
                    " response before rocket launch confirmation" +
                    (turn.finishReason.empty() ? std::string{} : " (finish: " + turn.finishReason + ")") +
                    "; requiring a tool call (" + std::to_string(consecutiveInvalidTerminalTurns) + " of " +
                    std::to_string(MaximumConsecutiveInvalidTerminalTurns - 1) + ")");
            }
            messages.push_back({
                {"role", "user"},
                {"content", "No tool result has explicitly confirmed a rocket launch. Plain-text terminal "
                    "responses are not allowed. Invoke the next real gameplay or observation tool now."},
            });
            continue;
        }

        consecutiveInvalidTerminalTurns = 0;

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
                toolResult = {
                    {"ok", false},
                    {"error", error.what()},
                    {"recovery", "Do not repeat this failed call unchanged. Re-read the tool schema and "
                        "latest discovery results, use only exact returned identifiers, and omit unknown "
                        "optional arguments instead of sending empty strings or null."},
                };
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
            if (toolResult.value("ok", false) && toolResult["result"].is_object() &&
                toolResult["result"].value("rocket_launch_confirmed", false))
            {
                rocketLaunchConfirmed = true;
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
        if (rocketLaunchConfirmed)
        {
            result.finalText = "Rocket launch confirmed by the game state.";
            return result;
        }

        const auto estimatedPromptTokens = turn.promptTokens == 0
            ? EstimatePromptTokens(messages, toolDefinitions)
            : std::size_t{};
        const auto promptTokens = turn.promptTokens == 0 ? estimatedPromptTokens : turn.promptTokens;
        if (promptTokens >= promptTokenCompactionThreshold)
        {
            const auto previousMessageCount = messages.size();
            const auto compactionMeasurement = turn.promptTokens == 0 ? "estimated" : "provider-reported";
            if (trace)
            {
                trace("[CONTEXT COMPACTION] Summarizing " + std::to_string(previousMessageCount) +
                    " messages at " + std::to_string(promptTokens) + " " + compactionMeasurement +
                    " prompt tokens; threshold " + std::to_string(promptTokenCompactionThreshold));
            }

            auto summaryMessages = messages;
            summaryMessages.at(0)["content"] = CompactionSystemPrompt;
            summaryMessages.push_back({
                {"role", "user"},
                {"content", "Pause gameplay and compact the conversation for another instance of yourself. "
                    "Summarize only established facts from tool results and visible decisions: completed work, "
                    "current progress, important inventory and exact locations, failures, and the next intended "
                    "step. Preserve details needed to continue the original objective, but omit obsolete attempts "
                    "and repeated output. Do not continue playing or call tools. Return only a concise visible "
                    "plain-text summary of at most 800 words."},
            });

            std::string summary;
            for (int attempt = 1; attempt <= MaximumCompactionAttempts && summary.empty(); ++attempt)
            {
                auto summaryTurn = llama_.Complete(summaryMessages, json::array(), trace);
                if (summaryTurn.toolCalls.empty())
                    summary = std::move(summaryTurn.content);
                if (!summary.empty())
                    break;

                if (attempt < MaximumCompactionAttempts)
                {
                    if (summaryTurn.toolCalls.empty())
                        summaryMessages.push_back(std::move(summaryTurn.assistantMessage));
                    summaryMessages.push_back({
                        {"role", "user"},
                        {"content", "The compaction response was empty or attempted an action. Return the "
                            "requested summary as visible plain text without tool calls."},
                    });
                }
            }
            if (summary.empty())
                throw std::runtime_error("AI model could not produce a visible context summary");

            messages = json::array({
                messages.at(0),
                messages.at(1),
                {{"role", "assistant"}, {"content", "Progress summary from the earlier context:\n" + summary}},
                {{"role", "user"}, {"content", "Continue the original objective from this summary. Re-observe "
                    "dynamic game state before relying on values that may have changed."}},
            });
            if (trace)
            {
                trace("[CONTEXT COMPACTION] Replaced " + std::to_string(previousMessageCount) +
                    " messages with " + std::to_string(messages.size()) + " compact messages");
            }
        }
    }

    throw std::runtime_error("Agent stopped after reaching the configured maximum rounds");
}
}
