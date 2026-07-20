#include <FactorIA/AgentController.h>

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

constexpr int MaximumConsecutiveEmptyTurns = 3;
constexpr int MaximumCompactionAttempts = 3;
constexpr std::size_t ContextMessageCompactionThreshold = 48;
constexpr std::size_t PromptTokenCompactionThreshold = 12000;

constexpr const char* SystemPrompt = R"(You are controlling one character in Factorio through typed tools.
Work autonomously toward the user's objective, but use only tool results as facts about the world.
Treat tool definitions as the authoritative description of available actions and their arguments.
Before every tool call, briefly state the relevant observation, the chosen action, and how it advances the objective.
Use specialized state and discovery tools before acting on unknown information. Do not guess names, coordinates, inventory contents, reachability, or crafting availability.
Use exact identifiers and positions from the most recent tool results, and satisfy an action's stated preconditions before calling it.
Take one meaningful gameplay action at a time, then inspect its result. Use batch counts when a tool supports them instead of repeating the same action one unit at a time.
Treat partial results and reported failure conditions as new observations: adapt the next action instead of blindly retrying.
Avoid redundant polling and broad unfiltered queries. Use filters and pagination, and prefer a tool that waits for completion when one is available.
Factorio map X increases east and map Y increases south.
Do not request teleportation, item spawning, raw Lua, or other cheats.
When the user's objective is complete or cannot be advanced with the available tools, explain the result concisely.)";
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
    const StatusHandler& status) const
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

    TraceHandler modelTrace;
    if (trace || status)
    {
        modelTrace = [&trace, &status](const std::string& message) {
            if (trace)
                trace(message);
            constexpr std::string_view retryPrefix = "[PROVIDER RETRY] ";
            if (status && message.starts_with(retryPrefix))
                status(message.substr(retryPrefix.size()));
        };
    }

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
    int consecutiveEmptyTurns = 0;
    for (int round = 1; !maximumRounds || round <= *maximumRounds; ++round)
    {
        if (stopToken.stop_requested())
        {
            result.stopped = true;
            return result;
        }

        result.rounds = round;
        if (status)
            status("Round " + std::to_string(round) + ": waiting for the model's next decision...");
        if (trace)
        {
            trace("========== AGENT ROUND " + std::to_string(round) +
                (maximumRounds ? " OF " + std::to_string(*maximumRounds) : " (NON-STOP)") +
                " ==========");
        }
        auto turn = llama_.Complete(messages, toolDefinitions, modelTrace);
        messages.push_back(turn.assistantMessage);

        if (trace && !turn.toolCalls.empty())
        {
            trace("[MODEL DECISION]\n" + (turn.content.empty()
                ? std::string("The model did not return a visible decision summary.")
                : turn.content));
        }

        if (turn.toolCalls.empty())
        {
            if (turn.content.empty())
            {
                ++consecutiveEmptyTurns;
                if (consecutiveEmptyTurns >= MaximumConsecutiveEmptyTurns)
                {
                    throw std::runtime_error("AI model returned " +
                        std::to_string(MaximumConsecutiveEmptyTurns) +
                        " consecutive terminal responses without a tool call or final answer");
                }
                if (trace)
                {
                    trace("[MODEL RETRY] Empty terminal response" +
                        (turn.finishReason.empty() ? std::string{} : " (finish: " + turn.finishReason + ")") +
                        "; requesting continuation (" + std::to_string(consecutiveEmptyTurns) + " of " +
                        std::to_string(MaximumConsecutiveEmptyTurns - 1) + ")");
                }
                if (status)
                    status("The model returned no visible action or answer. Requesting another decision...");
                messages.push_back({
                    {"role", "user"},
                    {"content", "Your previous response contained neither a tool call nor a visible final answer. "
                        "Continue working toward the original objective. Call the next tool, or provide a concise "
                        "final answer only if the objective is complete or genuinely cannot be advanced."},
                });
                continue;
            }
            result.finalText = turn.content;
            if (status)
                status(result.finalText);
            return result;
        }

        consecutiveEmptyTurns = 0;

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
            if (status)
            {
                auto activity = turn.content.empty()
                    ? "The model selected its next action."
                    : turn.content;
                activity += "\n\nExecuting " + call.name;
                if (!call.arguments.empty())
                    activity += "\n" + call.arguments.dump(2);
                status(activity);
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

        if (messages.size() >= ContextMessageCompactionThreshold ||
            turn.promptTokens >= PromptTokenCompactionThreshold)
        {
            const auto previousMessageCount = messages.size();
            if (status)
                status("Compacting the accumulated agent context before continuing...");
            if (trace)
            {
                trace("[CONTEXT COMPACTION] Summarizing " + std::to_string(previousMessageCount) +
                    " messages after a " + std::to_string(turn.promptTokens) + "-token prompt");
            }

            auto summaryMessages = messages;
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
                auto summaryTurn = llama_.Complete(summaryMessages, json::array(), modelTrace);
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
            if (status)
                status("Context compacted. Continuing the objective...");
        }
    }

    throw std::runtime_error("Agent stopped after reaching the configured maximum rounds");
}
}
