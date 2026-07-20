#pragma once

#include <functional>
#include <optional>
#include <stop_token>
#include <string>

#include <FactorIA/FactorioTools.h>
#include <FactorIA/LlamaClient.h>

namespace factoria
{
struct AgentRunResult
{
    std::string finalText;
    int rounds{};
    bool stopped{};
};

class AgentController
{
public:
    using TraceHandler = LlamaClient::TraceHandler;
    using DecisionHandler = std::function<void(const std::string&)>;

    static constexpr int MinimumRounds = 1;
    static constexpr int MaximumRounds = 999;

    AgentController(LlamaClient llama, FactorioTools& tools);
    AgentRunResult Run(
        const std::string& objective,
        std::optional<int> maximumRounds,
        std::stop_token stopToken,
        const TraceHandler& trace = {},
        const DecisionHandler& decision = {}) const;

private:
    LlamaClient llama_;
    FactorioTools& tools_;
};
}
