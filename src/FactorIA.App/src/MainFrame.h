#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <wx/frame.h>

#include <FactorIA/AppSettings.h>
#include <FactorIA/AgentController.h>
#include <FactorIA/AgentHistoryDatabase.h>
#include <FactorIA/LlamaClient.h>
#include <FactorIA/RconClient.h>
#include <FactorIA/WebControlServer.h>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxGrid;
class wxNotebook;
class wxSpinCtrl;
class wxStaticText;
class wxTextCtrl;

namespace factoria
{
class MainFrame final : public wxFrame
{
public:
    MainFrame();
    ~MainFrame() override;

private:
    void BuildUi();
    void UpdateAiProviderControls();
    void UpdateOpenRouterStatusRequest();
    void RunOpenRouterStatusUpdates(std::stop_token stopToken);
    void SetOpenRouterStatus(std::uint64_t revision, const std::string& text);
    LlamaClient CreateAiClient(const AppSettings& settings);
    void RecordRequestEvent(LlamaClient::RequestEvent event);
    void UpdateRequestCounter();
    void UpdateAgentRoundControls();
    void LoadSettingsIntoControls();
    AppSettings ReadSettingsFromControls() const;
    void SaveSettings();
    void ConnectRcon();
    void DisconnectRcon();
    void TestRcon();
    void TestLlama();
    void FetchOpenRouterModels();
    void RunAgent();
    void SaveAgentState();
    void ResumeSavedState();
    void ResumeSelectedDatabaseState();
    [[nodiscard]] bool PrepareResume(const AgentSessionCheckpoint& checkpoint);
    void RefreshDatabaseView();
    void ShowDatabaseSession(std::size_t index);
    void LoadOlderDatabaseEvents();
    void RenderDatabaseSession(const AgentSessionSummary& session);
    void UpdatePersistenceControls();
    void SaveHistoryCheckpoint(std::int64_t sessionId, const AgentRunState& state);
    void RecordHistoryEvent(std::int64_t sessionId, int round, std::string kind, nlohmann::json payload);
    void FinishHistorySession(
        std::int64_t sessionId,
        std::string status,
        std::string finalText = {},
        std::string errorText = {});
    void ReportHistoryError(std::string action, std::string message);
    [[nodiscard]] FactorioTools::BridgeEventHandler CreateBridgeHistoryHandler(
        std::int64_t sessionId,
        std::shared_ptr<std::atomic<int>> activeRound);
    [[nodiscard]] AgentController::RunCallbacks CreateAgentRunCallbacks(
        std::int64_t sessionId,
        std::shared_ptr<std::atomic<int>> activeRound,
        FactorioTools& tools);
    void RunAgentTaskUpdates(std::stop_token stopToken);
    void ConfigureInventoryRefresh();
    void RunInventoryUpdates(std::stop_token stopToken);
    void ApplyInventorySnapshot(
        std::uint64_t revision,
        std::vector<WebInventoryItem> items,
        bool available,
        std::string status);
    void RenderInventory();
    void QueueAgentTask(FactorioAgentTask task);
    void StartAgentTask(FactorioAgentTask task);
    void StopAgent();
    void HandleWebControlCommand(WebControlCommand command);
    void UpdateWebControlState();
    void SetAgentStatus(const wxString& status);
    void StartWork(
        std::string description,
        std::function<void(std::stop_token)> work,
        bool enableStop = false);
    void FinishWork();
    void SetConnectionState(bool connected, const wxString& detail = {});
    void PublishModelDecision(FactorioTools& tools, const std::string& decision);
    void AppendModelDecision(const wxString& decision);
    void AppendLog(const wxString& message);
    std::string ExecuteRconCommand(const std::string& command);

    AppSettings settings_;
    std::mutex clientMutex_;
    std::unique_ptr<RconClient> rconClient_;
    std::jthread worker_;
    bool workActive_{};
    bool rconConnected_{};
    std::unique_ptr<WebControlServer> webControlServer_;
    std::unique_ptr<AgentHistoryDatabase> historyDatabase_;
    std::optional<AgentSessionCheckpoint> pendingResumeCheckpoint_;
    std::optional<std::int64_t> activeSessionId_;
    std::optional<std::int64_t> lastSessionId_;
    std::atomic<std::int64_t> latestCheckpointSessionId_{};
    std::vector<AgentSessionSummary> databaseSessions_;
    std::vector<AgentHistoryEvent> databaseEvents_;
    std::optional<std::int64_t> displayedDatabaseSessionId_;
    bool databaseHasOlderEvents_{};
    std::atomic_bool historyErrorReported_{};

    std::mutex agentTaskWaitMutex_;
    std::condition_variable_any agentTaskChanged_;
    std::atomic_bool agentTaskDispatchPending_{};
    std::optional<FactorioAgentTask> pendingAgentTask_;
    std::jthread agentTaskWorker_;

    struct InventoryRefreshRequest
    {
        bool enabled{};
        bool useDedicatedCharacter{};
        std::uint64_t revision{};
    };

    std::mutex inventoryRefreshMutex_;
    std::condition_variable_any inventoryRefreshChanged_;
    InventoryRefreshRequest inventoryRefreshRequest_;
    std::jthread inventoryWorker_;
    std::vector<WebInventoryItem> inventoryItems_;
    std::string inventoryStatus_{"Not connected"};
    bool inventoryAvailable_{};

    struct OpenRouterStatusRequest
    {
        bool enabled{};
        std::string apiKey;
        std::uint64_t revision{};
    };

    std::mutex openRouterStatusMutex_;
    std::condition_variable_any openRouterStatusChanged_;
    OpenRouterStatusRequest openRouterStatusRequest_;
    std::jthread openRouterStatusWorker_;
    std::atomic<std::uint64_t> requestsSent_{};
    std::atomic<std::uint64_t> responsesReceived_{};

    wxTextCtrl* rconHost_{};
    wxSpinCtrl* rconPort_{};
    wxTextCtrl* rconPassword_{};
    wxTextCtrl* llamaUrl_{};
    wxTextCtrl* llamaModel_{};
    wxChoice* aiProvider_{};
    wxTextCtrl* openRouterApiKey_{};
    wxChoice* openRouterModel_{};
    wxCheckBox* openRouterFreeModelsOnly_{};
    wxButton* fetchOpenRouterModelsButton_{};
    wxTextCtrl* factorioUserDataPath_{};
    wxCheckBox* useDedicatedAiCharacter_{};
    wxStaticText* llamaStatus_{};
    wxButton* llamaTestButton_{};
    wxStaticText* rconStatus_{};
    wxButton* connectButton_{};
    wxButton* disconnectButton_{};
    wxButton* testButton_{};
    wxTextCtrl* objective_{};
    wxSpinCtrl* maximumRounds_{};
    wxCheckBox* nonStopAgentRun_{};
    wxButton* agentRunButton_{};
    wxButton* agentStopButton_{};
    wxButton* saveAgentStateButton_{};
    wxButton* resumeSavedStateButton_{};
    wxStaticText* agentStatus_{};
    wxTextCtrl* modelDecisions_{};
    wxTextCtrl* log_{};
    wxStaticText* inventoryStatusLabel_{};
    wxButton* refreshInventoryButton_{};
    wxGrid* inventoryGrid_{};
    wxStaticText* databasePath_{};
    wxChoice* databaseSessionChoice_{};
    wxButton* refreshDatabaseButton_{};
    wxButton* resumeDatabaseSessionButton_{};
    wxButton* loadOlderDatabaseEventsButton_{};
    wxTextCtrl* databaseView_{};
};
}
