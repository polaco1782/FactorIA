#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace factoria
{
struct WebControlState
{
    bool connected{};
    bool busy{};
    bool agentRunning{};
    std::string agentStatus{"Idle"};
    std::string objective;
    int maximumRounds{12};
    bool nonStop{};
};

struct WebControlCommand
{
    enum class Action
    {
        Connect,
        Disconnect,
        Run,
        Stop,
    };

    Action action{};
    std::string objective;
    std::optional<int> maximumRounds;
};

class WebControlServer final
{
public:
    using CommandHandler = std::function<void(WebControlCommand)>;

    explicit WebControlServer(CommandHandler commandHandler);
    ~WebControlServer();

    WebControlServer(const WebControlServer&) = delete;
    WebControlServer& operator=(const WebControlServer&) = delete;

    bool Start(const std::string& host, std::uint16_t port);
    void Stop();
    void PublishState(WebControlState state);
    void PublishDecision(std::string decision);

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};
}
