#pragma once

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <FactorIA/FactorioTools.h>
#include <FactorIA/LlamaClient.h>

namespace factoria
{
struct AgentRunResult
{
    std::string finalText;
    int rounds{};
    bool stopped{};
    bool succeeded{};
};

enum class AgentRunMode
{
    LaunchRocket,
    BuildGhosts,
    RemoveMarkers,
};

[[nodiscard]] std::string_view AgentRunModeName(AgentRunMode mode) noexcept;
[[nodiscard]] std::optional<AgentRunMode> AgentRunModeFromName(std::string_view name) noexcept;

// Before dispatching a game-side tool call, the controller records an outcome-unknown reply.
// A restart therefore resumes by re-observing rather than blindly replaying that action.
struct AgentRunState
{
    std::string objective;
    AgentRunMode mode{AgentRunMode::LaunchRocket};
    std::optional<int> maximumRounds;
    int completedRounds{};
    int consecutiveInvalidTerminalTurns{};
    int consecutiveIdenticalFailedToolCalls{};
    std::string lastFailedToolCallSignature;
    int consecutiveUnproductiveObservationCalls{};
    std::string lastUnproductiveObservationSignature;
    bool terminalReached{};
    bool terminalSucceeded{};
    std::string terminalText;
    nlohmann::json messages{nlohmann::json::array()};
};

struct AgentRunEvent
{
    std::string kind;
    int round{};
    nlohmann::json payload;
};

class AgentController
{
public:
    using TraceHandler = LlamaClient::TraceHandler;
    using DecisionHandler = std::function<void(const std::string&)>;
    using StateHandler = std::function<void(const AgentRunState&)>;
    using EventHandler = std::function<void(const AgentRunEvent&)>;

    struct RunCallbacks
    {
        TraceHandler trace;
        DecisionHandler decision;
        StateHandler stateChanged;
        EventHandler event;
        LlamaClient::CompletionObserver completion;
    };

    static constexpr int MinimumRounds = 1;
    static constexpr int MaximumRounds = 999;

    AgentController(LlamaClient llama, FactorioTools& tools);
    AgentRunResult Run(
        const std::string& objective,
        std::optional<int> maximumRounds,
        std::stop_token stopToken,
        AgentRunMode mode,
        const std::optional<AgentRunState>& resumedState = std::nullopt,
        const RunCallbacks& callbacks = {}) const;

private:
    LlamaClient llama_;
    FactorioTools& tools_;
};
}
