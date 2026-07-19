#pragma once

#include <functional>
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
    using TraceHandler = std::function<void(const std::string&)>;

    LlamaClient(std::string baseUrl, std::string model, std::string bearerToken = {});

    void CheckHealth() const;
    [[nodiscard]] std::vector<std::string> ListToolModels(bool freeOnly = false) const;
    [[nodiscard]] bool SupportsImageInput() const;
    LlamaTurn Complete(
        const nlohmann::json& messages,
        const nlohmann::json& tools,
        const TraceHandler& trace = {}) const;

private:
    struct Endpoint
    {
        std::string origin;
        std::string basePath;
    };

    static Endpoint ParseEndpoint(const std::string& baseUrl);
    std::string ChatPath() const;
    std::string KeyPath() const;
    std::string ModelsPath() const;
    nlohmann::json ModelCatalog() const;
    [[nodiscard]] bool IsOpenRouter() const noexcept;

    Endpoint endpoint_;
    std::string model_;
    std::string bearerToken_;
};
}
