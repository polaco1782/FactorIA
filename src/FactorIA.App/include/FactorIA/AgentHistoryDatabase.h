#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <FactorIA/AgentController.h>

struct sqlite3;

namespace factoria
{
struct AgentSessionStart
{
    std::string objective;
    AgentRunMode mode{AgentRunMode::LaunchRocket};
    std::string aiProvider;
    std::string aiModel;
    std::optional<int> maximumRounds;
    bool nonStop{};
    std::optional<std::int64_t> parentSessionId;
    std::optional<std::string> taskId;
};

struct AgentSessionSummary
{
    std::int64_t id{};
    std::string objective;
    AgentRunMode mode{AgentRunMode::LaunchRocket};
    std::string aiProvider;
    std::string aiModel;
    std::optional<int> maximumRounds;
    bool nonStop{};
    std::optional<std::int64_t> parentSessionId;
    std::optional<std::string> taskId;
    std::string status;
    std::string startedAt;
    std::string updatedAt;
    std::optional<std::string> finishedAt;
    int completedRounds{};
    std::string finalText;
    std::string errorText;
};

struct AgentSessionCheckpoint
{
    AgentSessionSummary summary;
    AgentRunState state;
};

struct AgentHistoryEvent
{
    std::int64_t id{};
    std::int64_t sessionId{};
    int round{};
    std::string kind;
    nlohmann::json payload;
    std::string createdAt;
};

// Serializes access to one SQLite connection so agent and UI threads can safely share it.
class AgentHistoryDatabase final
{
public:
    explicit AgentHistoryDatabase(std::filesystem::path databasePath);
    ~AgentHistoryDatabase();

    AgentHistoryDatabase(const AgentHistoryDatabase&) = delete;
    AgentHistoryDatabase& operator=(const AgentHistoryDatabase&) = delete;
    AgentHistoryDatabase(AgentHistoryDatabase&&) = delete;
    AgentHistoryDatabase& operator=(AgentHistoryDatabase&&) = delete;

    [[nodiscard]] const std::filesystem::path& DatabasePath() const noexcept;

    [[nodiscard]] std::int64_t BeginSession(const AgentSessionStart& start);
    void SaveCheckpoint(std::int64_t sessionId, const AgentRunState& state);
    void RecordEvent(std::int64_t sessionId, int round, std::string_view kind, const nlohmann::json& payload);
    void FinishSession(
        std::int64_t sessionId,
        std::string_view status,
        std::string_view finalText,
        std::string_view errorText);

    [[nodiscard]] std::vector<AgentSessionSummary> ListSessions() const;
    [[nodiscard]] std::vector<AgentHistoryEvent> ListEvents(
        std::int64_t sessionId,
        std::size_t limit = 500,
        std::optional<std::int64_t> beforeEventId = std::nullopt) const;
    [[nodiscard]] std::optional<AgentSessionCheckpoint> LoadCheckpoint(std::int64_t sessionId) const;
    [[nodiscard]] std::optional<AgentSessionCheckpoint> LoadLatestResumableCheckpoint() const;

private:
    std::filesystem::path databasePath_;
    sqlite3* database_{};
    mutable std::mutex mutex_;
};
}
