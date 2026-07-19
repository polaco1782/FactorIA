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
    LlamaClient(std::string baseUrl, std::string model, std::string bearerToken = {});

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
    std::string KeyPath() const;
    [[nodiscard]] bool IsOpenRouter() const noexcept;

    Endpoint endpoint_;
    std::string model_;
    std::string bearerToken_;
};
}
