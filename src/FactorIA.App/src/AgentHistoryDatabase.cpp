#include <FactorIA/AgentHistoryDatabase.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <sqlite3.h>

namespace factoria
{
namespace
{
using json = nlohmann::json;

constexpr std::string_view SessionColumns =
    "id, objective, mode, ai_provider, ai_model, maximum_rounds, non_stop, parent_session_id, task_id, "
    "status, started_at, updated_at, finished_at, completed_rounds, final_text, error_text";
constexpr int SessionColumnCount = 16;
constexpr std::string_view RedactedValue = "[redacted]";
constexpr std::string_view RedactedImageUrl = "[redacted data:image URL]";

[[noreturn]] void ThrowSqliteError(sqlite3* database, std::string_view action)
{
    const auto* message = database ? sqlite3_errmsg(database) : "SQLite did not return an error message";
    throw std::runtime_error(std::string(action) + ": " + message);
}

void CheckSqlite(sqlite3* database, int result, std::string_view action)
{
    if (result != SQLITE_OK)
        ThrowSqliteError(database, action);
}

void Execute(sqlite3* database, const char* sql)
{
    char* errorMessage{};
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &errorMessage);
    if (result == SQLITE_OK)
        return;

    const std::string message = errorMessage ? errorMessage : sqlite3_errmsg(database);
    sqlite3_free(errorMessage);
    throw std::runtime_error("SQLite statement failed: " + message);
}

class Transaction final
{
public:
    explicit Transaction(sqlite3* database)
        : database_(database)
    {
        Execute(database_, "BEGIN IMMEDIATE TRANSACTION;");
        active_ = true;
    }

    ~Transaction()
    {
        if (active_)
            sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }

    void Commit()
    {
        Execute(database_, "COMMIT;");
        active_ = false;
    }

private:
    sqlite3* database_{};
    bool active_{};
};

class Statement final
{
public:
    Statement(sqlite3* database, const char* sql)
        : database_(database)
    {
        CheckSqlite(
            database_,
            sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr),
            "Preparing SQLite statement");
    }

    ~Statement()
    {
        sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void BindInt64(int index, std::int64_t value)
    {
        CheckSqlite(
            database_,
            sqlite3_bind_int64(statement_, index, static_cast<sqlite3_int64>(value)),
            "Binding SQLite integer");
    }

    void BindInt(int index, int value)
    {
        CheckSqlite(database_, sqlite3_bind_int(statement_, index, value), "Binding SQLite integer");
    }

    void BindNull(int index)
    {
        CheckSqlite(database_, sqlite3_bind_null(statement_, index), "Binding SQLite null");
    }

    void BindText(int index, std::string_view value)
    {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("SQLite text value is too large");
        CheckSqlite(
            database_,
            sqlite3_bind_text(
                statement_,
                index,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT),
            "Binding SQLite text");
    }

    [[nodiscard]] bool StepRow()
    {
        const auto result = sqlite3_step(statement_);
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        ThrowSqliteError(database_, "Stepping SQLite statement");
    }

    void StepDone()
    {
        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_DONE)
            ThrowSqliteError(database_, "Stepping SQLite statement");
    }

    [[nodiscard]] sqlite3_stmt* Get() const noexcept
    {
        return statement_;
    }

private:
    sqlite3* database_{};
    sqlite3_stmt* statement_{};
};

std::string ColumnText(sqlite3_stmt* statement, int index)
{
    const auto* value = sqlite3_column_text(statement, index);
    if (!value)
        return {};
    return {
        reinterpret_cast<const char*>(value),
        static_cast<std::size_t>(sqlite3_column_bytes(statement, index)),
    };
}

std::optional<std::string> OptionalColumnText(sqlite3_stmt* statement, int index)
{
    if (sqlite3_column_type(statement, index) == SQLITE_NULL)
        return std::nullopt;
    return ColumnText(statement, index);
}

std::optional<std::int64_t> OptionalColumnInt64(sqlite3_stmt* statement, int index)
{
    if (sqlite3_column_type(statement, index) == SQLITE_NULL)
        return std::nullopt;
    return static_cast<std::int64_t>(sqlite3_column_int64(statement, index));
}

std::optional<int> OptionalColumnInt(sqlite3_stmt* statement, int index)
{
    if (sqlite3_column_type(statement, index) == SQLITE_NULL)
        return std::nullopt;
    return sqlite3_column_int(statement, index);
}

std::string Lowercase(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto character : value)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return result;
}

bool IsReasoningKey(std::string_view key)
{
    const auto normalized = Lowercase(key);
    return normalized == "reasoning" || normalized == "reasoning_details" || normalized == "reasoningdetails" ||
        normalized == "reasoning_content";
}

bool IsCredentialKey(std::string_view key)
{
    const auto normalized = Lowercase(key);
    return normalized == "token" || normalized.find("password") != std::string::npos ||
        normalized.find("secret") != std::string::npos || normalized.find("credential") != std::string::npos ||
        normalized.find("authorization") != std::string::npos || normalized.find("api_key") != std::string::npos ||
        normalized.find("api-key") != std::string::npos || normalized.find("apikey") != std::string::npos ||
        normalized.find("access_token") != std::string::npos || normalized.find("refresh_token") != std::string::npos ||
        normalized.find("bearer_token") != std::string::npos || normalized.find("id_token") != std::string::npos ||
        normalized == "key";
}

bool IsValueDelimiter(char character)
{
    return std::isspace(static_cast<unsigned char>(character)) || character == ',' || character == ';' ||
        character == '}' || character == ']' || character == '>';
}

std::string RedactDataImageUrls(std::string value)
{
    std::size_t searchPosition{};
    while (true)
    {
        const auto normalized = Lowercase(value);
        const auto start = normalized.find("data:image/", searchPosition);
        if (start == std::string::npos)
            return value;

        auto end = start;
        while (end < value.size() && !std::isspace(static_cast<unsigned char>(value[end])) && value[end] != '"' &&
            value[end] != '\'' && value[end] != '<' && value[end] != '>')
        {
            ++end;
        }
        value.replace(start, end - start, RedactedImageUrl);
        searchPosition = start + RedactedImageUrl.size();
    }
}

void RedactValueAfterMarker(std::string& value, std::string_view marker)
{
    std::size_t searchPosition{};
    while (true)
    {
        const auto normalized = Lowercase(value);
        const auto markerPosition = normalized.find(marker, searchPosition);
        if (markerPosition == std::string::npos)
            return;

        auto separator = markerPosition + marker.size();
        const auto separatorSearchEnd = std::min(value.size(), separator + std::size_t{48});
        while (separator < separatorSearchEnd && value[separator] != ':' && value[separator] != '=')
            ++separator;
        if (separator == separatorSearchEnd)
        {
            searchPosition = markerPosition + marker.size();
            continue;
        }

        auto start = separator + 1;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
            ++start;
        if (start + 1 < value.size() && value[start] == '\\' &&
            (value[start + 1] == '"' || value[start + 1] == '\''))
        {
            start += 2;
        }
        else if (start < value.size() && (value[start] == '"' || value[start] == '\''))
        {
            ++start;
        }

        const auto afterKey = Lowercase(std::string_view(value).substr(start));
        if (afterKey.starts_with("bearer "))
            start += std::string_view{"bearer "}.size();

        auto end = start;
        while (end < value.size() && !IsValueDelimiter(value[end]) && value[end] != '"' && value[end] != '\'')
            ++end;
        if (start == end)
        {
            searchPosition = markerPosition + marker.size();
            continue;
        }

        value.replace(start, end - start, RedactedValue);
        searchPosition = start + RedactedValue.size();
    }
}

void RedactBearerValues(std::string& value)
{
    std::size_t searchPosition{};
    while (true)
    {
        const auto normalized = Lowercase(value);
        const auto markerPosition = normalized.find("bearer ", searchPosition);
        if (markerPosition == std::string::npos)
            return;

        const auto start = markerPosition + std::string_view{"bearer "}.size();
        auto end = start;
        while (end < value.size() && !IsValueDelimiter(value[end]) && value[end] != '"' && value[end] != '\'')
            ++end;
        if (start == end)
        {
            searchPosition = start;
            continue;
        }

        value.replace(start, end - start, RedactedValue);
        searchPosition = start + RedactedValue.size();
    }
}

std::string RedactDebugText(std::string value)
{
    value = RedactDataImageUrls(std::move(value));
    constexpr std::array<std::string_view, 19> redactedMarkers{
        "authorization",
        "api_key",
        "api-key",
        "apikey",
        "x_api_key",
        "x-api-key",
        "access_token",
        "refresh_token",
        "bearer_token",
        "id_token",
        "password",
        "secret",
        "reasoning",
        "reasoning_details",
        "reasoning_content",
        "?token",
        "&token",
        "?key",
        "&key",
    };
    for (const auto marker : redactedMarkers)
        RedactValueAfterMarker(value, marker);
    RedactBearerValues(value);
    return value;
}

bool IsUserMultimodalMessageWithImage(const json& message)
{
    const auto role = message.find("role");
    const auto content = message.find("content");
    if (role == message.end() || !role->is_string() || role->get_ref<const std::string&>() != "user" ||
        content == message.end() || !content->is_array())
    {
        return false;
    }

    return std::any_of(content->begin(), content->end(), [](const json& part) {
        if (!part.is_object())
            return false;
        const auto type = part.find("type");
        return (type != part.end() && type->is_string() && type->get_ref<const std::string&>() == "image_url") ||
            part.contains("image_url");
    });
}

void AppendPersistedText(std::string& result, std::string_view text)
{
    if (text.empty())
        return;
    if (!result.empty())
        result += '\n';
    result += RedactDataImageUrls(std::string(text));
}

std::string TextOnlyPersistedContent(const json& content)
{
    std::string result;
    for (const auto& part : content)
    {
        if (part.is_string())
        {
            AppendPersistedText(result, part.get_ref<const std::string&>());
            continue;
        }
        if (!part.is_object())
            continue;

        const auto text = part.find("text");
        if (text != part.end() && text->is_string())
            AppendPersistedText(result, text->get_ref<const std::string&>());
    }

    if (!result.empty())
        result += "\n\n";
    result += "A screenshot from the prior turn was omitted from the saved state. Re-observe the current "
        "game with take_screenshot or relevant state tools before relying on visual details.";
    return result;
}

void ScrubStoredMessages(json& value)
{
    if (value.is_object())
    {
        // Persist textual context only; live messages retain the image for the active model turn.
        if (IsUserMultimodalMessageWithImage(value))
            value["content"] = TextOnlyPersistedContent(value.at("content"));

        for (auto iterator = value.begin(); iterator != value.end();)
        {
            if (IsReasoningKey(iterator.key()))
                iterator = value.erase(iterator);
            else
            {
                ScrubStoredMessages(iterator.value());
                ++iterator;
            }
        }
        return;
    }
    if (value.is_array())
    {
        for (auto& item : value)
            ScrubStoredMessages(item);
        return;
    }
    if (value.is_string())
        value = RedactDataImageUrls(value.get<std::string>());
}

void RedactEventPayload(json& value)
{
    if (value.is_object())
    {
        for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
        {
            if (IsReasoningKey(iterator.key()))
            {
                iterator.value() = "[redacted reasoning]";
            }
            else if (IsCredentialKey(iterator.key()))
            {
                iterator.value() = std::string(RedactedValue);
            }
            else
            {
                RedactEventPayload(iterator.value());
            }
        }
        return;
    }
    if (value.is_array())
    {
        for (auto& item : value)
            RedactEventPayload(item);
        return;
    }
    if (value.is_string())
        value = RedactDebugText(value.get<std::string>());
}

json SerializeState(const AgentRunState& state)
{
    auto messages = state.messages;
    ScrubStoredMessages(messages);
    return {
        {"objective", state.objective},
        {"mode", std::string(AgentRunModeName(state.mode))},
        {"maximum_rounds", state.maximumRounds ? json(*state.maximumRounds) : json(nullptr)},
        {"completed_rounds", state.completedRounds},
        {"consecutive_invalid_terminal_turns", state.consecutiveInvalidTerminalTurns},
        {"consecutive_identical_failed_tool_calls", state.consecutiveIdenticalFailedToolCalls},
        {"last_failed_tool_call_signature", state.lastFailedToolCallSignature},
        {"consecutive_unproductive_observation_calls", state.consecutiveUnproductiveObservationCalls},
        {"last_unproductive_observation_signature", state.lastUnproductiveObservationSignature},
        {"terminal_reached", state.terminalReached},
        {"terminal_succeeded", state.terminalSucceeded},
        {"terminal_text", state.terminalText},
        {"messages", std::move(messages)},
    };
}

const json& RequiredStateValue(const json& state, std::string_view key)
{
    const auto iterator = state.find(key);
    if (iterator == state.end())
        throw std::runtime_error("Stored agent checkpoint is missing '" + std::string(key) + "'");
    return *iterator;
}

AgentRunState DeserializeState(const std::string& serialized)
{
    json stored;
    try
    {
        stored = json::parse(serialized);
    }
    catch (const json::exception& error)
    {
        throw std::runtime_error("Stored agent checkpoint has invalid JSON: " + std::string(error.what()));
    }
    if (!stored.is_object())
        throw std::runtime_error("Stored agent checkpoint must be a JSON object");

    try
    {
        const auto& modeName = RequiredStateValue(stored, "mode");
        if (!modeName.is_string())
            throw std::runtime_error("Stored agent checkpoint mode is not a string");
        const auto mode = AgentRunModeFromName(modeName.get<std::string>());
        if (!mode)
            throw std::runtime_error("Stored agent checkpoint has an unknown run mode");

        AgentRunState state{
            .objective = RequiredStateValue(stored, "objective").get<std::string>(),
            .mode = *mode,
            .maximumRounds = std::nullopt,
            .completedRounds = RequiredStateValue(stored, "completed_rounds").get<int>(),
            .consecutiveInvalidTerminalTurns =
                RequiredStateValue(stored, "consecutive_invalid_terminal_turns").get<int>(),
            .consecutiveIdenticalFailedToolCalls =
                RequiredStateValue(stored, "consecutive_identical_failed_tool_calls").get<int>(),
            .lastFailedToolCallSignature =
                RequiredStateValue(stored, "last_failed_tool_call_signature").get<std::string>(),
            // These fields were added after checkpoint persistence shipped; older checkpoints start cleanly.
            .consecutiveUnproductiveObservationCalls =
                stored.value("consecutive_unproductive_observation_calls", 0),
            .lastUnproductiveObservationSignature =
                stored.value("last_unproductive_observation_signature", std::string{}),
            .terminalReached = false,
            .terminalSucceeded = false,
            .terminalText = {},
            .messages = RequiredStateValue(stored, "messages"),
        };
        const auto& maximumRounds = RequiredStateValue(stored, "maximum_rounds");
        if (!maximumRounds.is_null())
            state.maximumRounds = maximumRounds.get<int>();
        if (const auto terminalReached = stored.find("terminal_reached"); terminalReached != stored.end())
            state.terminalReached = terminalReached->get<bool>();
        if (const auto terminalSucceeded = stored.find("terminal_succeeded"); terminalSucceeded != stored.end())
            state.terminalSucceeded = terminalSucceeded->get<bool>();
        if (const auto terminalText = stored.find("terminal_text"); terminalText != stored.end())
            state.terminalText = terminalText->get<std::string>();
        return state;
    }
    catch (const json::exception& error)
    {
        throw std::runtime_error("Stored agent checkpoint has an invalid field: " + std::string(error.what()));
    }
}

AgentSessionSummary ReadSessionSummary(sqlite3_stmt* statement)
{
    const auto modeName = ColumnText(statement, 2);
    const auto mode = AgentRunModeFromName(modeName);
    if (!mode)
        throw std::runtime_error("Stored agent session has an unknown run mode: " + modeName);

    return {
        .id = static_cast<std::int64_t>(sqlite3_column_int64(statement, 0)),
        .objective = ColumnText(statement, 1),
        .mode = *mode,
        .aiProvider = ColumnText(statement, 3),
        .aiModel = ColumnText(statement, 4),
        .maximumRounds = OptionalColumnInt(statement, 5),
        .nonStop = sqlite3_column_int(statement, 6) != 0,
        .parentSessionId = OptionalColumnInt64(statement, 7),
        .taskId = OptionalColumnText(statement, 8),
        .status = ColumnText(statement, 9),
        .startedAt = ColumnText(statement, 10),
        .updatedAt = ColumnText(statement, 11),
        .finishedAt = OptionalColumnText(statement, 12),
        .completedRounds = sqlite3_column_int(statement, 13),
        .finalText = ColumnText(statement, 14),
        .errorText = ColumnText(statement, 15),
    };
}

AgentHistoryEvent ReadHistoryEvent(sqlite3_stmt* statement)
{
    const auto payloadText = ColumnText(statement, 4);
    try
    {
        return {
            .id = static_cast<std::int64_t>(sqlite3_column_int64(statement, 0)),
            .sessionId = static_cast<std::int64_t>(sqlite3_column_int64(statement, 1)),
            .round = sqlite3_column_int(statement, 2),
            .kind = ColumnText(statement, 3),
            .payload = json::parse(payloadText),
            .createdAt = ColumnText(statement, 5),
        };
    }
    catch (const json::exception& error)
    {
        throw std::runtime_error("Stored agent event has invalid JSON: " + std::string(error.what()));
    }
}

AgentSessionCheckpoint ReadCheckpoint(sqlite3_stmt* statement)
{
    return {
        .summary = ReadSessionSummary(statement),
        .state = DeserializeState(ColumnText(statement, SessionColumnCount)),
    };
}

std::string PathAsUtf8(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    std::string result;
    result.reserve(utf8.size());
    for (const auto character : utf8)
        result.push_back(static_cast<char>(character));
    return result;
}

void InitializeSchema(sqlite3* database)
{
    Execute(database, "PRAGMA foreign_keys = ON;");
    CheckSqlite(database, sqlite3_busy_timeout(database, 5000), "Configuring SQLite busy timeout");
    Execute(database, "PRAGMA journal_mode = WAL;");
    Execute(database, "PRAGMA synchronous = NORMAL;");

    Transaction transaction(database);
    Execute(database, R"sql(
        CREATE TABLE IF NOT EXISTS agent_sessions (
            id INTEGER PRIMARY KEY,
            objective TEXT NOT NULL,
            mode TEXT NOT NULL,
            ai_provider TEXT NOT NULL,
            ai_model TEXT NOT NULL,
            maximum_rounds INTEGER,
            non_stop INTEGER NOT NULL,
            parent_session_id INTEGER REFERENCES agent_sessions(id) ON DELETE SET NULL,
            task_id TEXT,
            status TEXT NOT NULL DEFAULT 'running',
            started_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
            finished_at TEXT,
            completed_rounds INTEGER NOT NULL DEFAULT 0,
            final_text TEXT NOT NULL DEFAULT '',
            error_text TEXT NOT NULL DEFAULT '',
            state_json TEXT
        );
    )sql");
    Execute(database, R"sql(
        CREATE TABLE IF NOT EXISTS agent_events (
            id INTEGER PRIMARY KEY,
            session_id INTEGER NOT NULL REFERENCES agent_sessions(id) ON DELETE CASCADE,
            round INTEGER NOT NULL,
            kind TEXT NOT NULL,
            payload_json TEXT NOT NULL,
            created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
        );
    )sql");
    Execute(database, "CREATE INDEX IF NOT EXISTS agent_sessions_updated_index "
                      "ON agent_sessions(updated_at DESC, id DESC);");
    Execute(database, "CREATE INDEX IF NOT EXISTS agent_events_session_index "
                      "ON agent_events(session_id, id DESC);");
    transaction.Commit();
}

void ValidateSessionId(std::int64_t sessionId)
{
    if (sessionId <= 0)
        throw std::invalid_argument("Agent history session id must be positive");
}
}

AgentHistoryDatabase::AgentHistoryDatabase(std::filesystem::path databasePath)
    : databasePath_(std::move(databasePath))
{
    if (databasePath_.empty())
        throw std::invalid_argument("Agent history database path cannot be empty");
    if (databasePath_.string() != ":memory:" && !databasePath_.parent_path().empty())
        std::filesystem::create_directories(databasePath_.parent_path());

    const auto path = PathAsUtf8(databasePath_);
    const auto result = sqlite3_open_v2(
        path.c_str(),
        &database_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (result != SQLITE_OK)
    {
        const std::string message = database_
            ? sqlite3_errmsg(database_)
            : "Unable to allocate SQLite connection";
        if (database_)
            sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error("Unable to open agent history database: " + message);
    }

    try
    {
        InitializeSchema(database_);
    }
    catch (...)
    {
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

AgentHistoryDatabase::~AgentHistoryDatabase()
{
    if (database_)
        sqlite3_close(database_);
}

const std::filesystem::path& AgentHistoryDatabase::DatabasePath() const noexcept
{
    return databasePath_;
}

std::int64_t AgentHistoryDatabase::BeginSession(const AgentSessionStart& start)
{
    if (start.objective.empty())
        throw std::invalid_argument("Agent history objective cannot be empty");
    if (start.maximumRounds && *start.maximumRounds <= 0)
        throw std::invalid_argument("Agent history maximum rounds must be positive");
    if (start.parentSessionId && *start.parentSessionId <= 0)
        throw std::invalid_argument("Agent history parent session id must be positive");

    std::scoped_lock lock(mutex_);
    Transaction transaction(database_);
    Statement statement(database_, R"sql(
        INSERT INTO agent_sessions (
            objective, mode, ai_provider, ai_model, maximum_rounds, non_stop, parent_session_id, task_id)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?);
    )sql");
    statement.BindText(1, start.objective);
    statement.BindText(2, AgentRunModeName(start.mode));
    statement.BindText(3, start.aiProvider);
    statement.BindText(4, start.aiModel);
    if (start.maximumRounds)
        statement.BindInt(5, *start.maximumRounds);
    else
        statement.BindNull(5);
    statement.BindInt(6, start.nonStop ? 1 : 0);
    if (start.parentSessionId)
        statement.BindInt64(7, *start.parentSessionId);
    else
        statement.BindNull(7);
    if (start.taskId)
        statement.BindText(8, *start.taskId);
    else
        statement.BindNull(8);
    statement.StepDone();

    const auto sessionId = static_cast<std::int64_t>(sqlite3_last_insert_rowid(database_));
    transaction.Commit();
    return sessionId;
}

void AgentHistoryDatabase::SaveCheckpoint(std::int64_t sessionId, const AgentRunState& state)
{
    ValidateSessionId(sessionId);
    const auto serialized = SerializeState(state).dump();
    // Focused tasks still need their game-side completion acknowledgement after a terminal tool result.
    const auto finalizesSession = state.mode == AgentRunMode::LaunchRocket && state.terminalReached;

    std::scoped_lock lock(mutex_);
    Transaction transaction(database_);
    Statement statement(database_, R"sql(
        UPDATE agent_sessions
        SET state_json = ?, completed_rounds = ?,
            status = CASE WHEN ? THEN ? ELSE status END,
            final_text = CASE WHEN ? THEN ? ELSE final_text END,
            finished_at = CASE WHEN ? THEN COALESCE(finished_at, strftime('%Y-%m-%dT%H:%M:%fZ', 'now')) ELSE finished_at END,
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?;
    )sql");
    statement.BindText(1, serialized);
    statement.BindInt(2, state.completedRounds);
    statement.BindInt(3, finalizesSession ? 1 : 0);
    statement.BindText(4, state.terminalSucceeded ? "completed" : "failed");
    statement.BindInt(5, finalizesSession ? 1 : 0);
    statement.BindText(6, state.terminalText);
    statement.BindInt(7, finalizesSession ? 1 : 0);
    statement.BindInt64(8, sessionId);
    statement.StepDone();
    if (sqlite3_changes(database_) != 1)
        throw std::runtime_error("Agent history session does not exist");
    transaction.Commit();
}

void AgentHistoryDatabase::RecordEvent(
    std::int64_t sessionId,
    int round,
    std::string_view kind,
    const nlohmann::json& payload)
{
    ValidateSessionId(sessionId);
    if (round < 0)
        throw std::invalid_argument("Agent history event round cannot be negative");
    if (kind.empty())
        throw std::invalid_argument("Agent history event kind cannot be empty");

    auto redactedPayload = payload;
    RedactEventPayload(redactedPayload);
    const auto serialized = redactedPayload.dump();

    std::scoped_lock lock(mutex_);
    Transaction transaction(database_);
    Statement insert(database_, R"sql(
        INSERT INTO agent_events (session_id, round, kind, payload_json)
        VALUES (?, ?, ?, ?);
    )sql");
    insert.BindInt64(1, sessionId);
    insert.BindInt(2, round);
    insert.BindText(3, kind);
    insert.BindText(4, serialized);
    insert.StepDone();

    Statement update(
        database_,
        "UPDATE agent_sessions SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?;");
    update.BindInt64(1, sessionId);
    update.StepDone();
    if (sqlite3_changes(database_) != 1)
        throw std::runtime_error("Agent history session does not exist");
    transaction.Commit();
}

void AgentHistoryDatabase::FinishSession(
    std::int64_t sessionId,
    std::string_view status,
    std::string_view finalText,
    std::string_view errorText)
{
    ValidateSessionId(sessionId);
    if (status.empty())
        throw std::invalid_argument("Agent history session status cannot be empty");

    std::scoped_lock lock(mutex_);
    Transaction transaction(database_);
    Statement statement(database_, R"sql(
        UPDATE agent_sessions
        SET status = ?, final_text = ?, error_text = ?,
            finished_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),
            updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        WHERE id = ?;
    )sql");
    statement.BindText(1, status);
    statement.BindText(2, finalText);
    statement.BindText(3, errorText);
    statement.BindInt64(4, sessionId);
    statement.StepDone();
    if (sqlite3_changes(database_) != 1)
        throw std::runtime_error("Agent history session does not exist");
    transaction.Commit();
}

std::vector<AgentSessionSummary> AgentHistoryDatabase::ListSessions() const
{
    std::scoped_lock lock(mutex_);
    const auto sql = std::string("SELECT ") + std::string(SessionColumns) +
        " FROM agent_sessions AS history_session"
        " ORDER BY history_session.updated_at DESC,"
        " (SELECT MAX(history_event.id) FROM agent_events AS history_event"
        "  WHERE history_event.session_id = history_session.id) DESC,"
        " history_session.id DESC;";
    Statement statement(database_, sql.c_str());

    std::vector<AgentSessionSummary> sessions;
    while (statement.StepRow())
        sessions.push_back(ReadSessionSummary(statement.Get()));
    return sessions;
}

std::vector<AgentHistoryEvent> AgentHistoryDatabase::ListEvents(
    std::int64_t sessionId,
    std::size_t limit,
    std::optional<std::int64_t> beforeEventId) const
{
    ValidateSessionId(sessionId);
    if (limit == 0)
        return {};
    if (limit > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("Agent history event limit is too large");
    if (beforeEventId && *beforeEventId <= 0)
        throw std::invalid_argument("Agent history event cursor must be positive");

    std::scoped_lock lock(mutex_);
    auto sql = std::string(R"sql(
        SELECT id, session_id, round, kind, payload_json, created_at
        FROM (
            SELECT id, session_id, round, kind, payload_json, created_at
            FROM agent_events
            WHERE session_id = ?
    )sql");
    if (beforeEventId)
        sql += " AND id < ?";
    sql += R"sql(
            ORDER BY id DESC
            LIMIT ?
        )
        ORDER BY id ASC;
    )sql";
    Statement statement(database_, sql.c_str());
    statement.BindInt64(1, sessionId);
    auto bindIndex = 2;
    if (beforeEventId)
        statement.BindInt64(bindIndex++, *beforeEventId);
    statement.BindInt64(bindIndex, static_cast<std::int64_t>(limit));

    std::vector<AgentHistoryEvent> events;
    while (statement.StepRow())
        events.push_back(ReadHistoryEvent(statement.Get()));
    return events;
}

std::optional<AgentSessionCheckpoint> AgentHistoryDatabase::LoadCheckpoint(std::int64_t sessionId) const
{
    ValidateSessionId(sessionId);
    std::scoped_lock lock(mutex_);
    const auto sql = std::string("SELECT ") + std::string(SessionColumns) +
        ", state_json FROM agent_sessions WHERE id = ?;";
    Statement statement(database_, sql.c_str());
    statement.BindInt64(1, sessionId);
    if (!statement.StepRow())
        return std::nullopt;
    if (sqlite3_column_type(statement.Get(), SessionColumnCount) == SQLITE_NULL)
        return std::nullopt;

    return ReadCheckpoint(statement.Get());
}

std::optional<AgentSessionCheckpoint> AgentHistoryDatabase::LoadLatestResumableCheckpoint() const
{
    std::scoped_lock lock(mutex_);
    const auto sql = std::string("SELECT ") + std::string(SessionColumns) + R"sql(
        , state_json
        FROM agent_sessions AS history_session
        WHERE state_json IS NOT NULL
          AND mode = 'launch_rocket'
          AND task_id IS NULL
          AND lower(status) IN ('running', 'stopped', 'failed', 'error', 'interrupted')
        ORDER BY history_session.updated_at DESC,
                 (SELECT MAX(history_event.id) FROM agent_events AS history_event
                  WHERE history_event.session_id = history_session.id) DESC,
                 history_session.id DESC;
    )sql";
    Statement statement(database_, sql.c_str());
    while (statement.StepRow())
    {
        auto checkpoint = ReadCheckpoint(statement.Get());
        if (!checkpoint.state.terminalReached)
            return checkpoint;
    }
    return std::nullopt;
}
}
