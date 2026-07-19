#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace factoria
{
struct LlamaToolCall
{
    std::string id;
    std::string name;
    nlohmann::json arguments;
};

struct LlamaTurn
{
    std::string content;
    std::vector<LlamaToolCall> toolCalls;
    nlohmann::json assistantMessage;
};

class LlamaClient
{
public:
    LlamaClient(std::string baseUrl, std::string model);

    void CheckHealth() const;
    LlamaTurn Complete(
        const nlohmann::json& messages,
        const nlohmann::json& tools) const;

private:
    struct Endpoint
    {
        std::string origin;
        std::string basePath;
    };

    static Endpoint ParseEndpoint(const std::string& baseUrl);
    std::string ChatPath() const;

    Endpoint endpoint_;
    std::string model_;
};
}

