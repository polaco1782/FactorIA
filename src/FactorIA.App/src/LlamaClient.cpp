#include <FactorIA/LlamaClient.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

#include <httplib.h>

namespace factoria
{
namespace
{
using json = nlohmann::json;

constexpr int MaximumCompletionAttempts = 5;

bool IsRetryableHttpStatus(int status)
{
    return status == 408 || status == 409 || status == 425 || status == 429 || status >= 500;
}

std::string Lowercase(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

bool IsRetryableProviderError(const json& body)
{
    const auto error = body.find("error");
    if (error == body.end() || !error->is_object())
        return false;

    if (const auto code = error->find("code"); code != error->end() && code->is_number_integer() &&
        IsRetryableHttpStatus(code->get<int>()))
    {
        return true;
    }

    const auto message = Lowercase(error->value("message", std::string{}));
    constexpr std::string_view transientMarkers[]{
        "resourceexhausted",
        "resource exhausted",
        "rate limit",
        "request limit",
        "temporarily unavailable",
        "timeout",
        "timed out",
        "overloaded",
        "upstream error",
    };
    return std::ranges::any_of(transientMarkers, [&message](std::string_view marker) {
        return message.find(marker) != std::string::npos;
    });
}

std::string ProviderError(const json& body)
{
    const auto error = body.find("error");
    return "AI provider error: " +
        (error != body.end() && error->is_object()
            ? error->value("message", "unknown error")
            : std::string("unknown error"));
}

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

void ThrowIfProviderError(const json& body)
{
    if (const auto error = body.find("error"); error != body.end() && error->is_object())
        throw std::runtime_error("AI provider error: " + error->value("message", "unknown error"));
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

std::string JsonText(const json& object, std::string_view key, std::string_view fallback = "unknown")
{
    if (!object.is_object())
        return std::string(fallback);
    const auto value = object.find(key);
    return value != object.end() && value->is_string()
        ? value->get<std::string>()
        : std::string(fallback);
}

std::string JsonNumber(const json& object, std::string_view key)
{
    if (!object.is_object())
        return "unknown";
    const auto value = object.find(key);
    return value != object.end() && value->is_number() ? value->dump() : "unknown";
}

std::size_t JsonArraySize(const json& object, std::string_view key)
{
    if (!object.is_object())
        return 0;
    const auto value = object.find(key);
    return value != object.end() && value->is_array() ? value->size() : 0;
}

bool IsZeroPrice(const json& value)
{
    if (value.is_number())
        return value.get<double>() == 0.0;
    if (!value.is_string())
        return false;

    const auto& text = value.get_ref<const std::string&>();
    try
    {
        std::size_t parsedCharacters = 0;
        const auto price = std::stod(text, &parsedCharacters);
        return parsedCharacters == text.size() && price == 0.0;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool HasFreeTextPricing(const json& model)
{
    const auto pricing = model.find("pricing");
    if (pricing == model.end() || !pricing->is_object())
        return false;

    const auto prompt = pricing->find("prompt");
    const auto completion = pricing->find("completion");
    return prompt != pricing->end() && completion != pricing->end() &&
        IsZeroPrice(*prompt) && IsZeroPrice(*completion);
}

void TraceModelRequest(const LlamaClient::TraceHandler& trace, const json& request)
{
    if (trace)
    {
        trace("[MODEL REQUEST] Model: " + JsonText(request, "model") +
            " | Context: " + std::to_string(JsonArraySize(request, "messages")) +
            " messages | Available tools: " + std::to_string(JsonArraySize(request, "tools")));
    }
}

void TraceModelResponse(const LlamaClient::TraceHandler& trace, const json& body)
{
    if (!trace)
        return;

    const json* choice = nullptr;
    if (const auto choices = body.find("choices");
        choices != body.end() && choices->is_array() && !choices->empty() && choices->front().is_object())
    {
        choice = &choices->front();
    }

    const json* message = nullptr;
    if (choice)
    {
        const auto candidate = choice->find("message");
        if (candidate != choice->end() && candidate->is_object())
            message = &*candidate;
    }

    const auto toolCallCount = message ? JsonArraySize(*message, "tool_calls") : 0;
    std::string summary = "[MODEL RESPONSE] Model: " + JsonText(body, "model") +
        " | Finish: " + (choice ? JsonText(*choice, "finish_reason") : "unknown") +
        " | Tool calls: " + std::to_string(toolCallCount);

    if (const auto usage = body.find("usage"); usage != body.end() && usage->is_object())
    {
        summary += "\nTokens: prompt " + JsonNumber(*usage, "prompt_tokens") +
            " | completion " + JsonNumber(*usage, "completion_tokens") +
            " | total " + JsonNumber(*usage, "total_tokens");
    }

    // Tool-taking decisions are logged by AgentController beside the actual call and result.
    if (message && toolCallCount == 0)
    {
        const auto content = message->find("content");
        if (content != message->end() && content->is_string() && !content->get_ref<const std::string&>().empty())
            summary += "\nAssistant:\n" + content->get<std::string>();
        else
        {
            const auto reasoning = message->find("reasoning");
            const auto reasoningDetails = message->find("reasoning_details");
            const auto hasReasoning =
                (reasoning != message->end() && !reasoning->is_null() && !reasoning->empty()) ||
                (reasoningDetails != message->end() && !reasoningDetails->is_null() && !reasoningDetails->empty());
            summary += "\nAssistant: <empty> | Reasoning payload: " +
                std::string(hasReasoning ? "present" : "absent");
        }
    }

    if (const auto error = body.find("error"); error != body.end() && error->is_object())
        summary += "\nProvider error: " + JsonText(*error, "message");

    trace(summary);
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

std::vector<std::string> LlamaClient::ListToolModels(bool freeOnly) const
{
    if (!IsOpenRouter())
        throw std::runtime_error("The model catalog is available only for OpenRouter");

    const auto body = ModelCatalog();
    const auto data = body.find("data");
    if (data == body.end() || !data->is_array())
        throw std::runtime_error("OpenRouter returned an invalid model catalog");

    std::vector<std::string> models;
    models.reserve(data->size());
    for (const auto& model : *data)
    {
        if (!model.is_object())
            continue;
        const auto supported = model.find("supported_parameters");
        if (supported == model.end() || !supported->is_array() ||
            std::find(supported->begin(), supported->end(), "tools") == supported->end())
        {
            continue;
        }
        if (freeOnly && !HasFreeTextPricing(model))
            continue;

        const auto id = model.find("id");
        if (id != model.end() && id->is_string() && !id->get_ref<const std::string&>().empty())
            models.push_back(id->get<std::string>());
    }

    std::ranges::sort(models);
    const auto duplicates = std::ranges::unique(models);
    models.erase(duplicates.begin(), duplicates.end());
    if (models.empty())
    {
        throw std::runtime_error(freeOnly
            ? "OpenRouter returned no free tool-capable models for this API key"
            : "OpenRouter returned no tool-capable models for this API key");
    }
    return models;
}

LlamaModelCapabilities LlamaClient::Capabilities() const
{
    // Local OpenAI-compatible servers do not expose a standard capability catalog.
    if (!IsOpenRouter())
        return {.supportsImageInput = true, .contextLength = std::nullopt};

    const auto body = ModelCatalog();
    const auto data = body.find("data");
    if (data == body.end() || !data->is_array())
        throw std::runtime_error("OpenRouter returned an invalid model catalog");

    for (const auto& model : *data)
    {
        if (!model.is_object() || model.value("id", std::string{}) != model_)
            continue;

        LlamaModelCapabilities result;
        if (const auto contextLength = model.find("context_length");
            contextLength != model.end() && contextLength->is_number_unsigned())
        {
            const auto value = contextLength->get<std::size_t>();
            if (value > 0)
                result.contextLength = value;
        }
        else if (contextLength != model.end() && contextLength->is_number_integer())
        {
            const auto value = contextLength->get<std::int64_t>();
            if (value > 0)
                result.contextLength = static_cast<std::size_t>(value);
        }

        const auto architecture = model.find("architecture");
        if (architecture == model.end() || !architecture->is_object())
            return result;
        const auto modalities = architecture->find("input_modalities");
        result.supportsImageInput = modalities != architecture->end() && modalities->is_array() &&
            std::find(modalities->begin(), modalities->end(), "image") != modalities->end();
        return result;
    }
    return {};
}

LlamaTurn LlamaClient::Complete(
    const json& messages,
    const json& tools,
    const TraceHandler& trace) const
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
    TraceModelRequest(trace, request);

    const auto action = IsOpenRouter() ? "OpenRouter completion" : "llama.cpp completion";
    const auto requestBody = request.dump();
    std::optional<json> completedBody;
    for (int attempt = 1; attempt <= MaximumCompletionAttempts; ++attempt)
    {
        httplib::Client client(endpoint_.origin);
        client.set_connection_timeout(std::chrono::seconds(10));
        client.set_read_timeout(std::chrono::minutes(5));
        client.set_write_timeout(std::chrono::seconds(30));
        const auto response = IsOpenRouter()
            ? client.Post(ChatPath(), OpenRouterHeaders(bearerToken_), requestBody, "application/json")
            : client.Post(ChatPath(), requestBody, "application/json");

        std::string failure;
        bool retryable = false;
        if (!response || response->status < 200 || response->status >= 300)
        {
            failure = HttpFailure(action, response);
            retryable = !response || IsRetryableHttpStatus(response->status);
        }
        else
        {
            try
            {
                auto body = ParseJsonBody(response, action);
                TraceModelResponse(trace, body);
                if (const auto error = body.find("error"); error != body.end() && error->is_object())
                {
                    failure = ProviderError(body);
                    retryable = IsRetryableProviderError(body);
                }
                else
                {
                    completedBody = std::move(body);
                    break;
                }
            }
            catch (const std::runtime_error& error)
            {
                failure = error.what();
                retryable = true;
            }
        }

        if (!retryable || attempt == MaximumCompletionAttempts)
            throw std::runtime_error(failure);

        const auto delay = std::chrono::seconds(1 << attempt);
        if (trace)
        {
            trace("[PROVIDER RETRY] " + failure + "\nRetry " + std::to_string(attempt) + " of " +
                std::to_string(MaximumCompletionAttempts - 1) + " in " +
                std::to_string(delay.count()) + " seconds");
        }
        std::this_thread::sleep_for(delay);
    }
    if (!completedBody)
        throw std::runtime_error("AI completion failed without a provider response");
    const auto& body = *completedBody;

    const auto choices = body.find("choices");
    if (choices == body.end() || !choices->is_array() || choices->empty())
        throw std::runtime_error("AI completion did not contain a choice");
    const auto messageIt = choices->front().find("message");
    if (messageIt == choices->front().end() || !messageIt->is_object())
        throw std::runtime_error("AI completion did not contain an assistant message");

    LlamaTurn turn;
    turn.finishReason = choices->front().value("finish_reason", std::string{});
    if (const auto usage = body.find("usage"); usage != body.end() && usage->is_object())
        turn.promptTokens = usage->value("prompt_tokens", std::size_t{});
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

std::string LlamaClient::ModelsPath() const
{
    if (endpoint_.basePath.empty())
        return "/v1/models/user";
    if (endpoint_.basePath.ends_with("/v1"))
        return endpoint_.basePath + "/models/user";
    return endpoint_.basePath + "/v1/models/user";
}

nlohmann::json LlamaClient::ModelCatalog() const
{
    httplib::Client client(endpoint_.origin);
    client.set_connection_timeout(std::chrono::seconds(10));
    client.set_read_timeout(std::chrono::seconds(30));
    const auto response = client.Get(ModelsPath(), OpenRouterHeaders(bearerToken_));
    auto body = ParseJsonBody(response, "OpenRouter model catalog");
    ThrowIfProviderError(body);
    return body;
}

bool LlamaClient::IsOpenRouter() const noexcept
{
    return !bearerToken_.empty();
}
}
