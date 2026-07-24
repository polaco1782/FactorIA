#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
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
    std::string finishReason;
    std::size_t promptTokens{};
    std::vector<LlamaToolCall> toolCalls;
    nlohmann::json assistantMessage;
};

struct LlamaModelCapabilities
{
    bool supportsImageInput{};
    std::optional<std::size_t> contextLength;
};

struct OpenRouterKeyUsage
{
    double usage{};
    double usageDaily{};
    std::optional<double> limit;
    std::optional<double> limitRemaining;
    bool isFreeTier{};
};

struct LlamaCompletionEvent
{
    enum class Kind
    {
        Request,
        Attempt,
        Response,
        Retry,
        Failure,
    };

    Kind kind{};
    // Request is emitted once per logical completion with attempt zero.
    int attempt{};
    nlohmann::json payload;
};

struct LlamaCompletionOptions
{
    std::chrono::seconds responseTimeout{60};
};

class LlamaClient
{
public:
    enum class RequestEvent
    {
        Sent,
        ResponseReceived,
    };

    using TraceHandler = std::function<void(const std::string&)>;
    using RequestHandler = std::function<void(RequestEvent)>;
    using CompletionObserver = std::function<void(const LlamaCompletionEvent&)>;

    LlamaClient(
        std::string baseUrl,
        std::string model,
        std::string bearerToken = {},
        RequestHandler requestHandler = {});

    void CheckHealth() const;
    [[nodiscard]] std::vector<std::string> ListToolModels(bool freeOnly = false) const;
    [[nodiscard]] LlamaModelCapabilities Capabilities() const;
    [[nodiscard]] OpenRouterKeyUsage GetOpenRouterKeyUsage() const;
    LlamaTurn Complete(
        const nlohmann::json& messages,
        const nlohmann::json& tools,
        const TraceHandler& trace = {},
        const CompletionObserver& observer = {},
        LlamaCompletionOptions options = {}) const;

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
    RequestHandler requestHandler_;
};
}
