#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace factoria
{
class RconClient
{
public:
    explicit RconClient(std::chrono::milliseconds timeout = std::chrono::seconds(5));
    ~RconClient();

    RconClient(const RconClient&) = delete;
    RconClient& operator=(const RconClient&) = delete;

    void Connect(const std::string& host, std::uint16_t port, const std::string& password);
    void Disconnect() noexcept;
    [[nodiscard]] bool IsConnected() const noexcept;
    std::string Execute(const std::string& command);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

