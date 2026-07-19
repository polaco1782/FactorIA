#pragma once

#include <functional>
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
    using TraceHandler = std::function<void(const std::string&)>;

    AgentController(LlamaClient llama, FactorioTools& tools);
    AgentRunResult Run(
        const std::string& objective,
        int maximumRounds,
        std::stop_token stopToken,
        const TraceHandler& trace = {}) const;

private:
    LlamaClient llama_;
    FactorioTools& tools_;
};
}

