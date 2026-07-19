#include <FactorIA/RconClient.h>

#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <span>
#include <thread>
#include <vector>

#include <asio.hpp>

namespace factoria
{
namespace
{
constexpr std::int32_t ServerDataResponseValue = 0;
constexpr std::int32_t ServerDataExecCommand = 2;
constexpr std::int32_t ServerDataAuthResponse = 2;
constexpr std::int32_t ServerDataAuth = 3;
constexpr std::size_t MaximumPacketSize = 4 * 1024 * 1024;

void AppendInt32(std::vector<std::uint8_t>& output, std::int32_t value)
{
    const auto unsignedValue = static_cast<std::uint32_t>(value);
    output.push_back(static_cast<std::uint8_t>(unsignedValue));
    output.push_back(static_cast<std::uint8_t>(unsignedValue >> 8));
    output.push_back(static_cast<std::uint8_t>(unsignedValue >> 16));
    output.push_back(static_cast<std::uint8_t>(unsignedValue >> 24));
}

std::int32_t ReadInt32(const std::uint8_t* bytes)
{
    const auto value = static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
    return static_cast<std::int32_t>(value);
}

struct Packet
{
    std::int32_t id{};
    std::int32_t type{};
    std::string body;
};

std::vector<std::uint8_t> EncodePacket(std::int32_t id, std::int32_t type, const std::string& body)
{
    if (body.size() > MaximumPacketSize - 14)
        throw std::runtime_error("RCON command is too large");

    std::vector<std::uint8_t> bytes;
    bytes.reserve(body.size() + 14);
    AppendInt32(bytes, static_cast<std::int32_t>(body.size() + 10));
    AppendInt32(bytes, id);
    AppendInt32(bytes, type);
    bytes.insert(bytes.end(), body.begin(), body.end());
    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}
}

struct RconClient::Impl
{
    explicit Impl(std::chrono::milliseconds value)
        : socket(context), timeout(value)
    {
    }

    void ReadExact(std::span<std::uint8_t> destination)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::size_t offset = 0;
        while (offset < destination.size())
        {
            asio::error_code error;
            const auto read = socket.read_some(asio::buffer(destination.data() + offset, destination.size() - offset), error);
            if (!error)
            {
                if (read == 0)
                    throw std::runtime_error("Factorio closed the RCON connection");
                offset += read;
                continue;
            }

            if (error != asio::error::would_block && error != asio::error::try_again)
                throw std::runtime_error("RCON receive failed: " + error.message());
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Timed out waiting for an RCON response");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Packet ReadPacket()
    {
        std::array<std::uint8_t, 4> sizeBytes{};
        ReadExact(sizeBytes);
        const auto size = ReadInt32(sizeBytes.data());
        if (size < 10 || static_cast<std::size_t>(size) > MaximumPacketSize)
            throw std::runtime_error("Factorio returned an invalid RCON packet size");

        std::vector<std::uint8_t> payload(static_cast<std::size_t>(size));
        ReadExact(payload);
        if (payload[payload.size() - 1] != 0 || payload[payload.size() - 2] != 0)
            throw std::runtime_error("Factorio returned a malformed RCON packet");

        Packet packet;
        packet.id = ReadInt32(payload.data());
        packet.type = ReadInt32(payload.data() + 4);
        packet.body.assign(reinterpret_cast<const char*>(payload.data() + 8), payload.size() - 10);
        return packet;
    }

    std::string ReadCommandResponse(std::int32_t requestId)
    {
        auto packet = ReadPacket();
        if (packet.id != requestId || packet.type != ServerDataResponseValue)
            throw std::runtime_error("Factorio returned an unexpected RCON response");

        std::string body = std::move(packet.body);
        auto idleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
        while (std::chrono::steady_clock::now() < idleDeadline)
        {
            asio::error_code error;
            const auto available = socket.available(error);
            if (error)
                throw std::runtime_error("Unable to inspect the RCON receive buffer: " + error.message());
            if (available < 4)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            packet = ReadPacket();
            if (packet.id != requestId || packet.type != ServerDataResponseValue)
                throw std::runtime_error("Factorio returned an unexpected packet in a split RCON response");
            body += packet.body;
            idleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
        }
        return body;
    }

    void WritePacket(std::int32_t id, std::int32_t type, const std::string& body)
    {
        const auto packet = EncodePacket(id, type, body);
        asio::write(socket, asio::buffer(packet));
    }

    asio::io_context context;
    asio::ip::tcp::socket socket;
    std::chrono::milliseconds timeout;
    std::int32_t nextRequestId{1};
};

RconClient::RconClient(std::chrono::milliseconds timeout)
    : impl_(std::make_unique<Impl>(timeout))
{
}

RconClient::~RconClient()
{
    Disconnect();
}

void RconClient::Connect(const std::string& host, std::uint16_t port, const std::string& password)
{
    Disconnect();
    impl_->context.restart();

    asio::ip::tcp::resolver resolver(impl_->context);
    const auto endpoints = resolver.resolve(host, std::to_string(port));
    asio::connect(impl_->socket, endpoints);
    impl_->socket.set_option(asio::ip::tcp::no_delay(true));
    impl_->socket.non_blocking(true);

    constexpr std::int32_t authId = 1;
    impl_->WritePacket(authId, ServerDataAuth, password);

    for (int packets = 0; packets < 2; ++packets)
    {
        const auto response = impl_->ReadPacket();
        if (response.type != ServerDataAuthResponse)
            continue;
        if (response.id == -1)
        {
            Disconnect();
            throw std::runtime_error("Factorio rejected the RCON password");
        }
        if (response.id == authId)
            return;
    }

    Disconnect();
    throw std::runtime_error("Factorio did not return an RCON authentication response");
}

void RconClient::Disconnect() noexcept
{
    if (!impl_ || !impl_->socket.is_open())
        return;

    asio::error_code ignored;
    impl_->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    impl_->socket.close(ignored);
}

bool RconClient::IsConnected() const noexcept
{
    return impl_ && impl_->socket.is_open();
}

std::string RconClient::Execute(const std::string& command)
{
    if (!IsConnected())
        throw std::runtime_error("RCON is not connected");

    const auto requestId = ++impl_->nextRequestId;
    impl_->WritePacket(requestId, ServerDataExecCommand, command);
    return impl_->ReadCommandResponse(requestId);
}
}
