#include <FactorIA/LlamaClient.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include <httplib.h>

namespace factoria
{
namespace
{
using json = nlohmann::json;

std::string HttpFailure(const std::string& action, const httplib::Result& response)
{
    if (!response)
        return action + " failed: " + httplib::to_string(response.error());

    std::string detail = response->body;
    if (detail.size() > 1000)
        detail.resize(1000);
    return action + " failed with HTTP " + std::to_string(response->status) +
        (detail.empty() ? std::string{} : ": " + detail);
}

json ParseJsonBody(const httplib::Result& response, const std::string& action)
{
    if (!response || response->status < 200 || response->status >= 300)
        throw std::runtime_error(HttpFailure(action, response));

    try
    {
        return json::parse(response->body);
    }
    catch (const json::exception& error)
    {
        throw std::runtime_error(action + " returned invalid JSON: " + error.what());
    }
}

json ParseToolArguments(const json& value)
{
    if (value.is_object())
        return value;
    if (!value.is_string())
        throw std::runtime_error("AI provider returned tool arguments that are neither a JSON string nor object");

    const auto text = value.get<std::string>();
    if (text.empty())
        return json::object();
    const auto parsed = json::parse(text);
    if (!parsed.is_object())
        throw std::runtime_error("AI provider returned tool arguments that are not a JSON object");
    return parsed;
}

httplib::Headers OpenRouterHeaders(const std::string& token)
{
    return {
        {"Authorization", "Bearer " + token},
        {"X-OpenRouter-Title", "FactorIA"},
    };
}
}

LlamaClient::LlamaClient(std::string baseUrl, std::string model, std::string bearerToken)
    : endpoint_(ParseEndpoint(baseUrl)),
      model_(model.empty() ? "local-model" : std::move(model)),
      bearerToken_(std::move(bearerToken))
{
}

void LlamaClient::CheckHealth() const
{
    httplib::Client client(endpoint_.origin);
    client.set_connection_timeout(std::chrono::seconds(5));
    client.set_read_timeout(std::chrono::seconds(10));
    if (IsOpenRouter())
    {
        const auto response = client.Get(KeyPath(), OpenRouterHeaders(bearerToken_));
        const auto body = ParseJsonBody(response, "OpenRouter connection test");
        const auto key = body.find("data");
        if (key == body.end() || !key->is_object())
            throw std::runtime_error("OpenRouter returned an invalid API key response");
        return;
    }

    const auto response = client.Get("/health");
    const auto body = ParseJsonBody(response, "llama.cpp health check");
    if (body.value("status", std::string{}) != "ok")
        throw std::runtime_error("llama.cpp is reachable but the model is not ready");
}

LlamaTurn LlamaClient::Complete(const json& messages, const json& tools) const
{
    if (!messages.is_array() || messages.empty())
        throw std::runtime_error("An AI completion requires at least one message");

    json request{
        {"model", model_},
        {"messages", messages},
        {"stream", false},
        {"temperature", 0.2},
        {"parallel_tool_calls", false},
    };
    if (!IsOpenRouter())
        request["parse_tool_calls"] = true;
    if (!tools.empty())
    {
        request["tools"] = tools;
        request["tool_choice"] = "auto";
    }

    httplib::Client client(endpoint_.origin);
    client.set_connection_timeout(std::chrono::seconds(10));
    client.set_read_timeout(std::chrono::minutes(5));
    client.set_write_timeout(std::chrono::seconds(30));
    const auto response = IsOpenRouter()
        ? client.Post(ChatPath(), OpenRouterHeaders(bearerToken_), request.dump(), "application/json")
        : client.Post(ChatPath(), request.dump(), "application/json");
    const auto body = ParseJsonBody(response, IsOpenRouter() ? "OpenRouter completion" : "llama.cpp completion");
    if (const auto error = body.find("error"); error != body.end() && error->is_object())
        throw std::runtime_error("AI provider error: " + error->value("message", "unknown error"));

    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty())
        throw std::runtime_error("AI completion did not contain a choice");
    const auto messageIt = choices->front().find("message");
    if (messageIt == choices->front().end() || !messageIt->is_object())
        throw std::runtime_error("AI completion did not contain an assistant message");

    LlamaTurn turn;
    turn.assistantMessage = *messageIt;
    turn.assistantMessage["role"] = "assistant";
    if (const auto content = messageIt->find("content"); content != messageIt->end() && content->is_string())
        turn.content = content->get<std::string>();

    const auto calls = messageIt->find("tool_calls");
    if (calls == messageIt->end() || calls->is_null())
        return turn;
    if (!calls->is_array())
        throw std::runtime_error("AI tool_calls value is not an array");

    for (std::size_t index = 0; index < calls->size(); ++index)
    {
        const auto& call = (*calls)[index];
        if (!call.is_object())
            throw std::runtime_error("AI provider returned a malformed tool call");
        const auto function = call.find("function");
        if (function == call.end() || !function->is_object())
            throw std::runtime_error("AI tool call has no function object");

        LlamaToolCall parsed;
        parsed.id = call.value("id", std::string{});
        parsed.name = function->value("name", std::string{});
        if (parsed.id.empty() || parsed.name.empty())
            throw std::runtime_error("AI tool call has no id or function name");
        parsed.arguments = ParseToolArguments(function->value("arguments", json::object()));
        turn.assistantMessage["tool_calls"][index]["function"]["arguments"] = parsed.arguments.dump();
        turn.toolCalls.push_back(std::move(parsed));
    }
    return turn;
}

LlamaClient::Endpoint LlamaClient::ParseEndpoint(const std::string& baseUrl)
{
    auto normalized = baseUrl;
    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();

    const auto scheme = normalized.find("://");
    if (scheme == std::string::npos)
        throw std::runtime_error("AI provider Base URL must include http:// or https://");
    const auto schemeName = normalized.substr(0, scheme);
    if (schemeName != "http" && schemeName != "https")
        throw std::runtime_error("AI provider Base URL must use HTTP or HTTPS");

    const auto pathStart = normalized.find('/', scheme + 3);
    Endpoint result;
    result.origin = pathStart == std::string::npos ? normalized : normalized.substr(0, pathStart);
    result.basePath = pathStart == std::string::npos ? std::string{} : normalized.substr(pathStart);
    if (result.origin.size() <= scheme + 3)
        throw std::runtime_error("llama.cpp Base URL has no host");
    return result;
}

std::string LlamaClient::ChatPath() const
{
    if (endpoint_.basePath.empty())
        return "/v1/chat/completions";
    if (endpoint_.basePath.ends_with("/v1"))
        return endpoint_.basePath + "/chat/completions";
    return endpoint_.basePath + "/v1/chat/completions";
}

std::string LlamaClient::KeyPath() const
{
    if (endpoint_.basePath.empty())
        return "/v1/key";
    if (endpoint_.basePath.ends_with("/v1"))
        return endpoint_.basePath + "/key";
    return endpoint_.basePath + "/v1/key";
}

bool LlamaClient::IsOpenRouter() const noexcept
{
    return !bearerToken_.empty();
}
}
