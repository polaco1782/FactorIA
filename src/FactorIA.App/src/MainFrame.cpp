#include "MainFrame.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/grid.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace factoria
{
namespace
{
using json = nlohmann::json;

wxString FromUtf8(const std::string& value)
{
    return wxString::FromUTF8(value);
}

wxString FromPath(const std::filesystem::path& value)
{
#ifdef _WIN32
    return wxString(value.wstring());
#else
    return wxString::FromUTF8(value.string());
#endif
}

constexpr const char* OpenRouterUrl = "https://openrouter.ai/api/v1";
constexpr auto OpenRouterStatusDebounce = std::chrono::seconds(1);
constexpr auto OpenRouterStatusRefreshInterval = std::chrono::minutes(1);
constexpr auto AgentTaskPollInterval = std::chrono::seconds(1);
constexpr auto InventoryRefreshInterval = std::chrono::seconds(2);
constexpr std::uint16_t WebControlPort = 8090;
constexpr std::size_t DatabaseEventViewerLimit = 500;
constexpr std::size_t DatabasePayloadViewerLimit = 12000;

std::string AiProviderName(const AppSettings& settings)
{
    return settings.aiProvider == "openrouter" ? "OpenRouter" : "llama.cpp";
}

std::string CompletionEventName(LlamaCompletionEvent::Kind kind)
{
    switch (kind)
    {
    case LlamaCompletionEvent::Kind::Request:
        return "model_request";
    case LlamaCompletionEvent::Kind::Attempt:
        return "model_attempt";
    case LlamaCompletionEvent::Kind::Response:
        return "model_response";
    case LlamaCompletionEvent::Kind::Retry:
        return "model_retry";
    case LlamaCompletionEvent::Kind::Failure:
        return "model_failure";
    }
    return "model_event";
}

std::string BridgeEventName(FactorioTools::BridgeCommandEvent::Kind kind)
{
    switch (kind)
    {
    case FactorioTools::BridgeCommandEvent::Kind::Sent:
        return "bridge_command_sent";
    case FactorioTools::BridgeCommandEvent::Kind::Received:
        return "bridge_response";
    case FactorioTools::BridgeCommandEvent::Kind::Failed:
        return "bridge_command_failed";
    }
    return "bridge_event";
}

std::string ViewerText(std::string value)
{
    if (value.size() <= DatabasePayloadViewerLimit)
        return value;
    const auto omitted = value.size() - DatabasePayloadViewerLimit;
    value.resize(DatabasePayloadViewerLimit);
    return value + "\n... [" + std::to_string(omitted) + " characters omitted from this view]";
}

std::string SessionLabel(const AgentSessionSummary& session)
{
    auto label = "#" + std::to_string(session.id) + " | " + session.status + " | " +
        std::string(AgentRunModeName(session.mode)) + " | " + session.updatedAt;
    if (!session.objective.empty())
        label += " | " + session.objective;
    return label;
}

bool IsResumableGeneralSession(const AgentSessionSummary& session)
{
    return session.mode == AgentRunMode::LaunchRocket && !session.taskId &&
        session.status != "completed" && session.status != "succeeded";
}

std::string FormatCredits(double credits)
{
    std::ostringstream stream;
    stream << '$' << std::fixed << std::setprecision(6) << credits;
    auto result = stream.str();
    while (result.ends_with('0'))
        result.pop_back();
    if (result.ends_with('.'))
        result += '0';
    return result;
}

std::string OpenRouterStatusText(const OpenRouterKeyUsage& usage)
{
    auto result = "OpenRouter: " + FormatCredits(usage.usageDaily) + " today | " +
        FormatCredits(usage.usage) + " total";
    if (usage.limitRemaining)
        result += " | " + FormatCredits(*usage.limitRemaining) + " key limit left";
    else if (usage.isFreeTier)
        result += " | free tier";
    return result;
}

void ValidateRconSettings(const AppSettings& settings)
{
    if (settings.rconHost.empty())
        throw std::runtime_error("Factorio RCON host cannot be empty");
    if (settings.rconPassword.empty())
        throw std::runtime_error("Factorio RCON password cannot be empty");
}

void ValidateAiSettings(const AppSettings& settings)
{
    if (settings.aiProvider == "llama_cpp" && settings.llamaUrl.empty())
        throw std::runtime_error("llama.cpp base URL cannot be empty");
    if (settings.aiProvider == "openrouter" && settings.openRouterApiKey.empty())
        throw std::runtime_error("OpenRouter API key cannot be empty");
    if (settings.aiProvider == "openrouter" && settings.openRouterModel.empty())
        throw std::runtime_error("Fetch and select an OpenRouter model first");
}
}

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "FactorIA - Disconnected", wxDefaultPosition, wxSize(780, 620))
{
    BuildUi();
    try
    {
        const auto databasePath = AppSettings::SettingsPath().parent_path() / "history.sqlite3";
        historyDatabase_ = std::make_unique<AgentHistoryDatabase>(databasePath);
        databasePath_->SetLabel("Database: " + FromPath(databasePath));
        AppendLog("Agent history database opened at " + FromPath(databasePath));
        RefreshDatabaseView();
    }
    catch (const std::exception& error)
    {
        databasePath_->SetLabel("Database unavailable");
        AppendLog("Agent history database could not be opened: " + FromUtf8(error.what()));
    }
    try
    {
        settings_ = AppSettings::Load();
        AppendLog("Settings loaded from " + FromPath(AppSettings::SettingsPath()));
    }
    catch (const std::exception& error)
    {
        AppendLog("Settings could not be loaded: " + FromUtf8(error.what()));
    }
    LoadSettingsIntoControls();
    SetConnectionState(false);
    UpdatePersistenceControls();
    webControlServer_ = std::make_unique<WebControlServer>([this](WebControlCommand command) {
        CallAfter([this, command = std::move(command)]() mutable {
            HandleWebControlCommand(std::move(command));
        });
    });
    UpdateWebControlState();
    if (webControlServer_->Start("0.0.0.0", WebControlPort))
    {
        auto webHost = wxGetHostName();
        if (webHost.empty())
            webHost = "<computer-lan-ip>";
        AppendLog("Web control is available at http://" + webHost + ":" +
            wxString::Format("%u", WebControlPort) +
            " on the local network (use this computer's LAN IP if the hostname does not resolve)");
    }
    else
    {
        AppendLog("Web control could not listen on port " + wxString::Format("%u", WebControlPort));
    }
    openRouterStatusWorker_ = std::jthread(
        [this](std::stop_token stopToken) { RunOpenRouterStatusUpdates(stopToken); });
    agentTaskWorker_ = std::jthread(
        [this](std::stop_token stopToken) { RunAgentTaskUpdates(stopToken); });
    inventoryWorker_ = std::jthread(
        [this](std::stop_token stopToken) { RunInventoryUpdates(stopToken); });
    openRouterStatusChanged_.notify_all();
    Centre();
}

MainFrame::~MainFrame()
{
    if (inventoryWorker_.joinable())
    {
        inventoryWorker_.request_stop();
        inventoryRefreshChanged_.notify_all();
        inventoryWorker_.join();
    }
    if (webControlServer_)
        webControlServer_->Stop();
    if (agentTaskWorker_.joinable())
    {
        agentTaskWorker_.request_stop();
        agentTaskChanged_.notify_all();
        agentTaskWorker_.join();
    }
    if (openRouterStatusWorker_.joinable())
    {
        openRouterStatusWorker_.request_stop();
        openRouterStatusChanged_.notify_all();
        openRouterStatusWorker_.join();
    }
    if (worker_.joinable())
    {
        worker_.request_stop();
        worker_.join();
    }
    DisconnectRcon();
}

void MainFrame::BuildUi()
{
    auto* notebook = new wxNotebook(this, wxID_ANY);
    auto* connectionPanel = new wxPanel(notebook);
    auto* agentPanel = new wxPanel(notebook);
    auto* logPanel = new wxPanel(notebook);
    auto* inventoryPanel = new wxPanel(notebook);
    auto* databasePanel = new wxPanel(notebook);
    notebook->AddPage(connectionPanel, "Connections", true);
    notebook->AddPage(agentPanel, "Agent");
    notebook->AddPage(logPanel, "Log");
    notebook->AddPage(inventoryPanel, "Inventory");
    notebook->AddPage(databasePanel, "Database");

    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(notebook, 1, wxEXPAND);
    SetSizer(root);
    CreateStatusBar(3);
    const int statusWidths[]{-1, -3, 215};
    SetStatusWidths(3, statusWidths);
    SetStatusText("Factorio: Disconnected", 0);
    SetStatusText("AI: not configured", 1);
    UpdateRequestCounter();

    auto* connectionRoot = new wxBoxSizer(wxVERTICAL);
    auto* factorioBox = new wxStaticBoxSizer(wxVERTICAL, connectionPanel, "Factorio RCON");
    auto* factorioGrid = new wxFlexGridSizer(2, 8, 10);
    factorioGrid->AddGrowableCol(1, 1);

    rconHost_ = new wxTextCtrl(connectionPanel, wxID_ANY);
    rconPort_ = new wxSpinCtrl(connectionPanel, wxID_ANY);
    rconPort_->SetRange(1, 65535);
    rconPassword_ = new wxTextCtrl(connectionPanel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    factorioUserDataPath_ = new wxTextCtrl(connectionPanel, wxID_ANY);
    useDedicatedAiCharacter_ = new wxCheckBox(connectionPanel, wxID_ANY, "Use dedicated AI character");
    factorioGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "Host"), 0, wxALIGN_CENTER_VERTICAL);
    factorioGrid->Add(rconHost_, 1, wxEXPAND);
    factorioGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "Port"), 0, wxALIGN_CENTER_VERTICAL);
    factorioGrid->Add(rconPort_, 1, wxEXPAND);
    factorioGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "Password"), 0, wxALIGN_CENTER_VERTICAL);
    factorioGrid->Add(rconPassword_, 1, wxEXPAND);
    factorioGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "User data directory"), 0, wxALIGN_CENTER_VERTICAL);
    factorioGrid->Add(factorioUserDataPath_, 1, wxEXPAND);
    factorioGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "Agent control"), 0, wxALIGN_CENTER_VERTICAL);
    factorioGrid->Add(useDedicatedAiCharacter_, 1, wxEXPAND);
    factorioBox->Add(factorioGrid, 0, wxEXPAND | wxALL, 10);

    auto* stateRow = new wxBoxSizer(wxHORIZONTAL);
    rconStatus_ = new wxStaticText(connectionPanel, wxID_ANY, "Disconnected");
    connectButton_ = new wxButton(connectionPanel, wxID_ANY, "Connect");
    disconnectButton_ = new wxButton(connectionPanel, wxID_ANY, "Disconnect");
    testButton_ = new wxButton(connectionPanel, wxID_ANY, "Read bridge status");
    stateRow->Add(rconStatus_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    stateRow->Add(connectButton_, 0, wxRIGHT, 8);
    stateRow->Add(disconnectButton_, 0, wxRIGHT, 8);
    stateRow->Add(testButton_);
    factorioBox->Add(stateRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    auto* llamaBox = new wxStaticBoxSizer(wxVERTICAL, connectionPanel, "AI provider");
    auto* llamaGrid = new wxFlexGridSizer(2, 8, 10);
    llamaGrid->AddGrowableCol(1, 1);
    llamaUrl_ = new wxTextCtrl(connectionPanel, wxID_ANY);
    llamaModel_ = new wxTextCtrl(connectionPanel, wxID_ANY);
    aiProvider_ = new wxChoice(connectionPanel, wxID_ANY);
    aiProvider_->Append("llama.cpp (local)");
    aiProvider_->Append("OpenRouter");
    openRouterApiKey_ = new wxTextCtrl(
        connectionPanel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    openRouterModel_ = new wxChoice(connectionPanel, wxID_ANY);
    openRouterFreeModelsOnly_ = new wxCheckBox(connectionPanel, wxID_ANY, "Free models only");
    fetchOpenRouterModelsButton_ = new wxButton(connectionPanel, wxID_ANY, "Fetch models");
    llamaGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "Provider"), 0, wxALIGN_CENTER_VERTICAL);
    llamaGrid->Add(aiProvider_, 1, wxEXPAND);
    llamaGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "llama.cpp Base URL"), 0, wxALIGN_CENTER_VERTICAL);
    llamaGrid->Add(llamaUrl_, 1, wxEXPAND);
    llamaGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "llama.cpp Model"), 0, wxALIGN_CENTER_VERTICAL);
    llamaGrid->Add(llamaModel_, 1, wxEXPAND);
    llamaGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "OpenRouter API key"), 0, wxALIGN_CENTER_VERTICAL);
    llamaGrid->Add(openRouterApiKey_, 1, wxEXPAND);
    llamaGrid->Add(new wxStaticText(connectionPanel, wxID_ANY, "OpenRouter Model"), 0, wxALIGN_CENTER_VERTICAL);
    auto* openRouterModelRow = new wxBoxSizer(wxHORIZONTAL);
    openRouterModelRow->Add(openRouterModel_, 1, wxEXPAND | wxRIGHT, 8);
    openRouterModelRow->Add(openRouterFreeModelsOnly_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    openRouterModelRow->Add(fetchOpenRouterModelsButton_);
    llamaGrid->Add(openRouterModelRow, 1, wxEXPAND);
    llamaBox->Add(llamaGrid, 0, wxEXPAND | wxALL, 10);
    auto* llamaStateRow = new wxBoxSizer(wxHORIZONTAL);
    llamaStatus_ = new wxStaticText(connectionPanel, wxID_ANY, "Not tested");
    llamaTestButton_ = new wxButton(connectionPanel, wxID_ANY, "Test AI provider");
    llamaStateRow->Add(llamaStatus_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    llamaStateRow->Add(llamaTestButton_);
    llamaBox->Add(llamaStateRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    auto* saveButton = new wxButton(connectionPanel, wxID_SAVE, "Save settings");
    connectionRoot->Add(factorioBox, 0, wxEXPAND | wxALL, 12);
    connectionRoot->Add(llamaBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    connectionRoot->Add(saveButton, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    connectionRoot->AddStretchSpacer();
    connectionPanel->SetSizer(connectionRoot);

    auto* agentRoot = new wxBoxSizer(wxVERTICAL);
    auto* objectiveBox = new wxStaticBoxSizer(wxVERTICAL, agentPanel, "Objective");
    objective_ = new wxTextCtrl(agentPanel, wxID_ANY,
        "Observe the controlled character and report the current position and inventory.",
        wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE);
    objectiveBox->Add(objective_, 1, wxEXPAND | wxALL, 8);

    auto* agentControls = new wxBoxSizer(wxHORIZONTAL);
    maximumRounds_ = new wxSpinCtrl(agentPanel, wxID_ANY);
    maximumRounds_->SetRange(AgentController::MinimumRounds, AgentController::MaximumRounds);
    maximumRounds_->SetValue(12);
    nonStopAgentRun_ = new wxCheckBox(agentPanel, wxID_ANY, "Non-stop run");
    agentRunButton_ = new wxButton(agentPanel, wxID_ANY, "Run objective");
    agentStopButton_ = new wxButton(agentPanel, wxID_ANY, "Stop");
    agentStatus_ = new wxStaticText(agentPanel, wxID_ANY, "Idle");
    agentControls->Add(new wxStaticText(agentPanel, wxID_ANY, "Maximum AI rounds"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    agentControls->Add(maximumRounds_, 0, wxRIGHT, 12);
    agentControls->Add(nonStopAgentRun_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    agentControls->Add(agentRunButton_, 0, wxRIGHT, 8);
    agentControls->Add(agentStopButton_, 0, wxRIGHT, 12);
    agentControls->Add(agentStatus_, 1, wxALIGN_CENTER_VERTICAL);

    auto* persistenceControls = new wxBoxSizer(wxHORIZONTAL);
    saveAgentStateButton_ = new wxButton(agentPanel, wxID_ANY, "Save checkpoint");
    resumeSavedStateButton_ = new wxButton(agentPanel, wxID_ANY, "Resume saved context");
    persistenceControls->Add(saveAgentStateButton_, 0, wxRIGHT, 8);
    persistenceControls->Add(resumeSavedStateButton_);

    auto* activityBox = new wxStaticBoxSizer(wxVERTICAL, agentPanel, "Model decisions");
    modelDecisions_ = new wxTextCtrl(agentPanel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    activityBox->Add(modelDecisions_, 1, wxEXPAND | wxALL, 8);
    agentRoot->Add(objectiveBox, 1, wxEXPAND | wxALL, 12);
    agentRoot->Add(agentControls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    agentRoot->Add(persistenceControls, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    agentRoot->Add(activityBox, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    agentPanel->SetSizer(agentRoot);

    auto* logRoot = new wxBoxSizer(wxVERTICAL);
    log_ = new wxTextCtrl(logPanel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    logRoot->Add(log_, 1, wxEXPAND | wxALL, 8);
    logPanel->SetSizer(logRoot);

    auto* inventoryRoot = new wxBoxSizer(wxVERTICAL);
    auto* inventoryControls = new wxBoxSizer(wxHORIZONTAL);
    inventoryStatusLabel_ = new wxStaticText(inventoryPanel, wxID_ANY, "Not connected");
    refreshInventoryButton_ = new wxButton(inventoryPanel, wxID_ANY, "Refresh inventory");
    inventoryControls->Add(inventoryStatusLabel_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    inventoryControls->Add(refreshInventoryButton_);
    inventoryGrid_ = new wxGrid(inventoryPanel, wxID_ANY);
    inventoryGrid_->CreateGrid(0, 3);
    inventoryGrid_->SetColLabelValue(0, "Item");
    inventoryGrid_->SetColLabelValue(1, "Count");
    inventoryGrid_->SetColLabelValue(2, "Quality");
    inventoryGrid_->SetColMinimalWidth(0, 200);
    inventoryGrid_->SetColMinimalWidth(1, 90);
    inventoryGrid_->SetColMinimalWidth(2, 120);
    inventoryGrid_->SetColSize(0, 320);
    inventoryGrid_->SetColSize(1, 110);
    inventoryGrid_->SetColSize(2, 140);
    inventoryGrid_->EnableEditing(false);
    inventoryGrid_->DisableDragRowSize();
    inventoryRoot->Add(inventoryControls, 0, wxEXPAND | wxALL, 12);
    inventoryRoot->Add(inventoryGrid_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    inventoryPanel->SetSizer(inventoryRoot);

    auto* databaseRoot = new wxBoxSizer(wxVERTICAL);
    databasePath_ = new wxStaticText(databasePanel, wxID_ANY, "Database unavailable");
    auto* databaseControls = new wxBoxSizer(wxHORIZONTAL);
    databaseSessionChoice_ = new wxChoice(databasePanel, wxID_ANY);
    refreshDatabaseButton_ = new wxButton(databasePanel, wxID_ANY, "Refresh");
    resumeDatabaseSessionButton_ = new wxButton(databasePanel, wxID_ANY, "Resume selected");
    loadOlderDatabaseEventsButton_ = new wxButton(databasePanel, wxID_ANY, "Load older events");
    databaseControls->Add(new wxStaticText(databasePanel, wxID_ANY, "Session"), 0,
        wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    databaseControls->Add(databaseSessionChoice_, 1, wxRIGHT, 8);
    databaseControls->Add(refreshDatabaseButton_, 0, wxRIGHT, 8);
    databaseControls->Add(resumeDatabaseSessionButton_, 0, wxRIGHT, 8);
    databaseControls->Add(loadOlderDatabaseEventsButton_);
    databaseView_ = new wxTextCtrl(databasePanel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    databaseRoot->Add(databasePath_, 0, wxEXPAND | wxALL, 12);
    databaseRoot->Add(databaseControls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    databaseRoot->Add(databaseView_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    databasePanel->SetSizer(databaseRoot);

    saveButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveSettings(); });
    connectButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ConnectRcon(); });
    disconnectButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DisconnectRcon(); });
    testButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { TestRcon(); });
    llamaTestButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { TestLlama(); });
    fetchOpenRouterModelsButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { FetchOpenRouterModels(); });
    aiProvider_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateAiProviderControls(); });
    openRouterApiKey_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdateOpenRouterStatusRequest(); });
    useDedicatedAiCharacter_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { ConfigureInventoryRefresh(); });
    objective_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdateWebControlState(); });
    maximumRounds_->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { UpdateWebControlState(); });
    nonStopAgentRun_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        UpdateAgentRoundControls();
        UpdateWebControlState();
    });
    agentRunButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RunAgent(); });
    agentStopButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StopAgent(); });
    saveAgentStateButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveAgentState(); });
    resumeSavedStateButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ResumeSavedState(); });
    refreshDatabaseButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RefreshDatabaseView(); });
    databaseSessionChoice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        ShowDatabaseSession(static_cast<std::size_t>(databaseSessionChoice_->GetSelection()));
    });
    resumeDatabaseSessionButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        ResumeSelectedDatabaseState();
    });
    loadOlderDatabaseEventsButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        LoadOlderDatabaseEvents();
    });
    refreshInventoryButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ConfigureInventoryRefresh(); });
    agentRunButton_->Disable();
    agentStopButton_->Disable();
    refreshInventoryButton_->Disable();
    UpdatePersistenceControls();
}

void MainFrame::LoadSettingsIntoControls()
{
    rconHost_->SetValue(FromUtf8(settings_.rconHost));
    rconPort_->SetValue(settings_.rconPort);
    rconPassword_->SetValue(FromUtf8(settings_.rconPassword));
    llamaUrl_->SetValue(FromUtf8(settings_.llamaUrl));
    llamaModel_->SetValue(FromUtf8(settings_.llamaModel));
    aiProvider_->SetSelection(settings_.aiProvider == "openrouter" ? 1 : 0);
    openRouterApiKey_->SetValue(FromUtf8(settings_.openRouterApiKey));
    openRouterFreeModelsOnly_->SetValue(settings_.openRouterFreeModelsOnly);
    openRouterModel_->Clear();
    if (!settings_.openRouterModel.empty())
    {
        openRouterModel_->Append(FromUtf8(settings_.openRouterModel));
        openRouterModel_->SetSelection(0);
    }
    factorioUserDataPath_->SetValue(FromPath(settings_.factorioUserDataPath));
    useDedicatedAiCharacter_->SetValue(settings_.useDedicatedAiCharacter);
    nonStopAgentRun_->SetValue(settings_.nonStopAgentRun);
    UpdateAiProviderControls();
    UpdateAgentRoundControls();
}

void MainFrame::UpdateAiProviderControls()
{
    const auto openRouter = aiProvider_->GetSelection() == 1;
    llamaUrl_->Enable(!openRouter);
    llamaModel_->Enable(!openRouter);
    openRouterApiKey_->Enable(openRouter);
    openRouterModel_->Enable(openRouter);
    openRouterFreeModelsOnly_->Enable(openRouter);
    fetchOpenRouterModelsButton_->Enable(openRouter);
    llamaStatus_->SetLabel("Not tested");
    UpdateOpenRouterStatusRequest();
}

void MainFrame::UpdateOpenRouterStatusRequest()
{
    const auto openRouter = aiProvider_->GetSelection() == 1;
    const auto apiKey = openRouterApiKey_->GetValue().ToStdString();
    {
        std::scoped_lock lock(openRouterStatusMutex_);
        if (openRouterStatusRequest_.enabled == openRouter && openRouterStatusRequest_.apiKey == apiKey)
            return;
        openRouterStatusRequest_.enabled = openRouter;
        openRouterStatusRequest_.apiKey = apiKey;
        ++openRouterStatusRequest_.revision;
    }

    if (!openRouter)
        SetStatusText("AI: llama.cpp", 1);
    else if (apiKey.empty())
        SetStatusText("OpenRouter: enter an API key to view usage", 1);
    else
        SetStatusText("OpenRouter: checking usage...", 1);
    openRouterStatusChanged_.notify_all();
}

void MainFrame::RunOpenRouterStatusUpdates(std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        OpenRouterStatusRequest request;
        {
            std::unique_lock lock(openRouterStatusMutex_);
            openRouterStatusChanged_.wait(lock, stopToken, [this] {
                return openRouterStatusRequest_.enabled && !openRouterStatusRequest_.apiKey.empty();
            });
            if (stopToken.stop_requested())
                return;

            request = openRouterStatusRequest_;
            // Text controls emit an event for every key edit; authenticate only after the value settles.
            const auto changed = openRouterStatusChanged_.wait_until(
                lock,
                stopToken,
                std::chrono::steady_clock::now() + OpenRouterStatusDebounce,
                [this, revision = request.revision] {
                    return openRouterStatusRequest_.revision != revision;
                });
            if (stopToken.stop_requested())
                return;
            if (changed)
                continue;
        }

        try
        {
            const LlamaClient client(
                OpenRouterUrl,
                {},
                request.apiKey,
                [this](LlamaClient::RequestEvent event) { RecordRequestEvent(event); });
            const auto usage = client.GetOpenRouterKeyUsage();
            CallAfter([this, revision = request.revision, text = OpenRouterStatusText(usage)] {
                SetOpenRouterStatus(revision, text);
            });
        }
        catch (const std::exception&)
        {
            CallAfter([this, revision = request.revision] {
                SetOpenRouterStatus(revision, "OpenRouter: usage unavailable");
            });
        }

        std::unique_lock lock(openRouterStatusMutex_);
        openRouterStatusChanged_.wait_until(
            lock,
            stopToken,
            std::chrono::steady_clock::now() + OpenRouterStatusRefreshInterval,
            [this, revision = request.revision] {
                return openRouterStatusRequest_.revision != revision;
            });
    }
}

void MainFrame::SetOpenRouterStatus(std::uint64_t revision, const std::string& text)
{
    {
        std::scoped_lock lock(openRouterStatusMutex_);
        // An HTTP response must not overwrite the state for a newer key or provider selection.
        if (openRouterStatusRequest_.revision != revision || !openRouterStatusRequest_.enabled)
            return;
    }
    SetStatusText(FromUtf8(text), 1);
}

LlamaClient MainFrame::CreateAiClient(const AppSettings& settings)
{
    auto requestHandler = [this](LlamaClient::RequestEvent event) { RecordRequestEvent(event); };
    if (settings.aiProvider == "openrouter")
    {
        return LlamaClient(
            OpenRouterUrl,
            settings.openRouterModel,
            settings.openRouterApiKey,
            std::move(requestHandler));
    }
    return LlamaClient(settings.llamaUrl, settings.llamaModel, {}, std::move(requestHandler));
}

void MainFrame::RecordRequestEvent(LlamaClient::RequestEvent event)
{
    if (event == LlamaClient::RequestEvent::Sent)
        requestsSent_.fetch_add(1, std::memory_order_relaxed);
    else
        responsesReceived_.fetch_add(1, std::memory_order_relaxed);

    CallAfter([this] { UpdateRequestCounter(); });
}

void MainFrame::UpdateRequestCounter()
{
    SetStatusText(
        wxString::Format(
            "HTTP: %llu sent | %llu received",
            static_cast<unsigned long long>(requestsSent_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(responsesReceived_.load(std::memory_order_relaxed))),
        2);
}

void MainFrame::UpdateAgentRoundControls()
{
    maximumRounds_->Enable(!nonStopAgentRun_->GetValue());
}

AppSettings MainFrame::ReadSettingsFromControls() const
{
    AppSettings result;
    result.rconHost = rconHost_->GetValue().ToStdString();
    result.rconPort = static_cast<std::uint16_t>(rconPort_->GetValue());
    result.rconPassword = rconPassword_->GetValue().ToStdString();
    result.llamaUrl = llamaUrl_->GetValue().ToStdString();
    result.llamaModel = llamaModel_->GetValue().ToStdString();
    result.aiProvider = aiProvider_->GetSelection() == 1 ? "openrouter" : "llama_cpp";
    result.openRouterApiKey = openRouterApiKey_->GetValue().ToStdString();
    result.openRouterModel = openRouterModel_->GetStringSelection().ToStdString();
    result.openRouterFreeModelsOnly = openRouterFreeModelsOnly_->GetValue();
    result.useDedicatedAiCharacter = useDedicatedAiCharacter_->GetValue();
    result.nonStopAgentRun = nonStopAgentRun_->GetValue();
#ifdef _WIN32
    result.factorioUserDataPath = std::filesystem::path(factorioUserDataPath_->GetValue().ToStdWstring());
#else
    result.factorioUserDataPath = std::filesystem::path(factorioUserDataPath_->GetValue().ToStdString());
#endif
    return result;
}

void MainFrame::SaveSettings()
{
    try
    {
        settings_ = ReadSettingsFromControls();
        settings_.Save();
        AppendLog("Settings saved to " + FromPath(AppSettings::SettingsPath()));
    }
    catch (const std::exception& error)
    {
        AppendLog("Unable to save settings: " + FromUtf8(error.what()));
    }
}

void MainFrame::ReportHistoryError(std::string action, std::string message)
{
    if (historyErrorReported_.exchange(true))
        return;

    CallAfter([this, action = std::move(action), message = std::move(message)] {
        AppendLog("Agent history database " + FromUtf8(action) + " failed: " + FromUtf8(message));
        databasePath_->SetLabel("Database unavailable after an error");
        UpdatePersistenceControls();
    });
}

void MainFrame::SaveHistoryCheckpoint(std::int64_t sessionId, const AgentRunState& state)
{
    if (!historyDatabase_)
        return;

    try
    {
        historyDatabase_->SaveCheckpoint(sessionId, state);
        if (latestCheckpointSessionId_.exchange(sessionId) != sessionId)
            CallAfter([this] { UpdatePersistenceControls(); });
    }
    catch (const std::exception& error)
    {
        ReportHistoryError("checkpoint save", error.what());
        throw;
    }
}

void MainFrame::RecordHistoryEvent(
    std::int64_t sessionId,
    int round,
    std::string kind,
    nlohmann::json payload)
{
    if (!historyDatabase_)
        return;

    try
    {
        historyDatabase_->RecordEvent(sessionId, round, kind, payload);
    }
    catch (const std::exception& error)
    {
        ReportHistoryError("event recording", error.what());
        throw;
    }
}

void MainFrame::FinishHistorySession(
    std::int64_t sessionId,
    std::string status,
    std::string finalText,
    std::string errorText)
{
    if (!historyDatabase_)
        return;

    try
    {
        historyDatabase_->FinishSession(sessionId, status, finalText, errorText);
    }
    catch (const std::exception& error)
    {
        ReportHistoryError("session finalization", error.what());
    }
}

FactorioTools::BridgeEventHandler MainFrame::CreateBridgeHistoryHandler(
    std::int64_t sessionId,
    std::shared_ptr<std::atomic<int>> activeRound)
{
    return [this, sessionId, activeRound = std::move(activeRound)](
               const FactorioTools::BridgeCommandEvent& event) {
        RecordHistoryEvent(sessionId, activeRound->load(std::memory_order_relaxed), BridgeEventName(event.kind), {
            {"operation", event.operation},
            {"command", event.command},
            {"response", event.response.empty() ? json(nullptr) : json(event.response)},
            {"error", event.error.empty() ? json(nullptr) : json(event.error)},
        });
    };
}

AgentController::RunCallbacks MainFrame::CreateAgentRunCallbacks(
    std::int64_t sessionId,
    std::shared_ptr<std::atomic<int>> activeRound,
    FactorioTools& tools)
{
    return {
        .trace = [this, sessionId, activeRound](const std::string& trace) {
            RecordHistoryEvent(
                sessionId,
                activeRound->load(std::memory_order_relaxed),
                "trace",
                {{"message", trace}});
            CallAfter([this, text = FromUtf8(trace)] { AppendLog(text); });
        },
        .decision = [this, sessionId, activeRound, &tools](const std::string& decision) {
            PublishModelDecision(tools, decision);
            RecordHistoryEvent(sessionId, activeRound->load(std::memory_order_relaxed), "model_decision", {
                {"decision", decision},
            });
        },
        .stateChanged = [this, sessionId](const AgentRunState& state) {
            SaveHistoryCheckpoint(sessionId, state);
        },
        .event = [this, sessionId, activeRound](const AgentRunEvent& event) {
            activeRound->store(event.round, std::memory_order_relaxed);
            RecordHistoryEvent(sessionId, event.round, event.kind, event.payload);
        },
        .completion = [this, sessionId, activeRound](const LlamaCompletionEvent& event) {
            RecordHistoryEvent(sessionId, activeRound->load(std::memory_order_relaxed),
                CompletionEventName(event.kind), {
                    {"attempt", event.attempt},
                    {"details", event.payload},
                });
        },
    };
}

void MainFrame::UpdatePersistenceControls()
{
    const auto databaseAvailable = historyDatabase_ && !historyErrorReported_.load();
    const auto latestCheckpointSessionId = latestCheckpointSessionId_.load();
    const auto checkpointTargetSessionId = activeSessionId_ ? activeSessionId_ : lastSessionId_;
    const auto hasSessionCheckpoint = checkpointTargetSessionId &&
        *checkpointTargetSessionId == latestCheckpointSessionId;
    const auto canResume = databaseAvailable && rconConnected_ && !workActive_;
    if (saveAgentStateButton_)
        saveAgentStateButton_->Enable(databaseAvailable && hasSessionCheckpoint);
    if (resumeSavedStateButton_)
        resumeSavedStateButton_->Enable(canResume);
    if (refreshDatabaseButton_)
        refreshDatabaseButton_->Enable(databaseAvailable);
    if (databaseSessionChoice_)
        databaseSessionChoice_->Enable(databaseAvailable);
    const auto selection = databaseSessionChoice_ ? databaseSessionChoice_->GetSelection() : wxNOT_FOUND;
    const auto selectedSessionIsResumable = selection != wxNOT_FOUND &&
        static_cast<std::size_t>(selection) < databaseSessions_.size() &&
        IsResumableGeneralSession(databaseSessions_[static_cast<std::size_t>(selection)]);
    if (resumeDatabaseSessionButton_)
        resumeDatabaseSessionButton_->Enable(canResume && selectedSessionIsResumable);
    if (loadOlderDatabaseEventsButton_)
        loadOlderDatabaseEventsButton_->Enable(databaseAvailable && databaseHasOlderEvents_);
}

void MainFrame::RefreshDatabaseView()
{
    if (!historyDatabase_)
    {
        databaseEvents_.clear();
        displayedDatabaseSessionId_.reset();
        databaseHasOlderEvents_ = false;
        databaseView_->ChangeValue("The agent history database is unavailable.");
        UpdatePersistenceControls();
        return;
    }

    try
    {
        std::optional<std::int64_t> selectedSessionId;
        if (const auto selection = databaseSessionChoice_->GetSelection(); selection != wxNOT_FOUND &&
            static_cast<std::size_t>(selection) < databaseSessions_.size())
        {
            selectedSessionId = databaseSessions_[static_cast<std::size_t>(selection)].id;
        }

        databaseSessions_ = historyDatabase_->ListSessions();
        databaseSessionChoice_->Freeze();
        databaseSessionChoice_->Clear();
        for (const auto& session : databaseSessions_)
            databaseSessionChoice_->Append(FromUtf8(SessionLabel(session)));
        databaseSessionChoice_->Thaw();

        if (databaseSessions_.empty())
        {
            databaseEvents_.clear();
            displayedDatabaseSessionId_.reset();
            databaseHasOlderEvents_ = false;
            databaseView_->ChangeValue("No persisted agent sessions yet.");
            UpdatePersistenceControls();
            return;
        }

        auto selection = std::size_t{};
        if (selectedSessionId)
        {
            const auto found = std::find_if(databaseSessions_.begin(), databaseSessions_.end(),
                [selectedSessionId](const AgentSessionSummary& session) { return session.id == *selectedSessionId; });
            if (found != databaseSessions_.end())
                selection = static_cast<std::size_t>(std::distance(databaseSessions_.begin(), found));
        }
        databaseSessionChoice_->SetSelection(static_cast<int>(selection));
        ShowDatabaseSession(selection);
    }
    catch (const std::exception& error)
    {
        ReportHistoryError("database refresh", error.what());
        AppendLog("Cannot refresh agent history database: " + FromUtf8(error.what()));
        databaseView_->ChangeValue("Unable to read the agent history database.");
    }
    UpdatePersistenceControls();
}

void MainFrame::ShowDatabaseSession(std::size_t index)
{
    if (!historyDatabase_ || index >= databaseSessions_.size())
    {
        databaseEvents_.clear();
        displayedDatabaseSessionId_.reset();
        databaseHasOlderEvents_ = false;
        databaseView_->ChangeValue("Select a persisted session to inspect its events.");
        UpdatePersistenceControls();
        return;
    }

    try
    {
        const auto& session = databaseSessions_[index];
        databaseEvents_ = historyDatabase_->ListEvents(session.id, DatabaseEventViewerLimit + 1);
        databaseHasOlderEvents_ = databaseEvents_.size() > DatabaseEventViewerLimit;
        if (databaseHasOlderEvents_)
            databaseEvents_.erase(databaseEvents_.begin());
        displayedDatabaseSessionId_ = session.id;
        RenderDatabaseSession(session);
    }
    catch (const std::exception& error)
    {
        ReportHistoryError("database session read", error.what());
        AppendLog("Cannot read selected agent session: " + FromUtf8(error.what()));
        databaseView_->ChangeValue("Unable to read this persisted session.");
    }
    UpdatePersistenceControls();
}

void MainFrame::LoadOlderDatabaseEvents()
{
    if (!historyDatabase_ || !displayedDatabaseSessionId_ || databaseEvents_.empty() || !databaseHasOlderEvents_)
        return;

    try
    {
        auto olderEvents = historyDatabase_->ListEvents(
            *displayedDatabaseSessionId_,
            DatabaseEventViewerLimit + 1,
            databaseEvents_.front().id);
        databaseHasOlderEvents_ = olderEvents.size() > DatabaseEventViewerLimit;
        if (databaseHasOlderEvents_)
            olderEvents.erase(olderEvents.begin());
        databaseEvents_.insert(databaseEvents_.begin(),
            std::make_move_iterator(olderEvents.begin()),
            std::make_move_iterator(olderEvents.end()));

        const auto session = std::find_if(databaseSessions_.begin(), databaseSessions_.end(),
            [sessionId = *displayedDatabaseSessionId_](const AgentSessionSummary& candidate) {
                return candidate.id == sessionId;
            });
        if (session == databaseSessions_.end())
            throw std::runtime_error("The selected history session no longer exists");
        RenderDatabaseSession(*session);
    }
    catch (const std::exception& error)
    {
        ReportHistoryError("older event read", error.what());
        AppendLog("Cannot load older agent history events: " + FromUtf8(error.what()));
    }
    UpdatePersistenceControls();
}

void MainFrame::RenderDatabaseSession(const AgentSessionSummary& session)
{
    const auto checkpoint = historyDatabase_->LoadCheckpoint(session.id);
    std::ostringstream output;
    output << "Session #" << session.id << "\n"
        << "Status: " << session.status << "\n"
        << "Mode: " << AgentRunModeName(session.mode) << "\n"
        << "Provider: " << session.aiProvider << " | Model: " << session.aiModel << "\n"
        << "Started: " << session.startedAt << "\n"
        << "Updated: " << session.updatedAt << "\n"
        << "Completed rounds: " << session.completedRounds << "\n"
        << "Objective:\n" << session.objective << "\n";
    if (checkpoint)
    {
        output << "Checkpoint: " << checkpoint->state.messages.size() << " messages";
        if (checkpoint->state.terminalReached)
            output << " | terminal " << (checkpoint->state.terminalSucceeded ? "success" : "failure");
        output << "\n";
    }
    if (!session.finalText.empty())
        output << "Final result:\n" << session.finalText << "\n";
    if (!session.errorText.empty())
        output << "Error:\n" << session.errorText << "\n";
    output << "\nLoaded " << databaseEvents_.size() << " event(s)";
    if (databaseHasOlderEvents_)
        output << "; use Load older events to inspect the rest.";
    else
        output << ".";
    output << " Individual event payload previews are capped at " << DatabasePayloadViewerLimit << " characters.\n";

    for (const auto& event : databaseEvents_)
    {
        output << "\n[" << event.createdAt << "] #" << event.id << " | round " << event.round <<
            " | " << event.kind << "\n" << ViewerText(event.payload.dump(2)) << "\n";
    }
    databaseView_->ChangeValue(FromUtf8(output.str()));
}

void MainFrame::SaveAgentState()
{
    if (!historyDatabase_)
    {
        AppendLog("Cannot save agent state: the history database is unavailable");
        return;
    }

    const auto sessionId = activeSessionId_ ? activeSessionId_ : lastSessionId_;
    if (!sessionId)
    {
        AppendLog("There is no agent state to save yet");
        return;
    }

    try
    {
        const auto checkpoint = historyDatabase_->LoadCheckpoint(*sessionId);
        if (!checkpoint)
            throw std::runtime_error("This session has not reached a safe checkpoint yet");
        RecordHistoryEvent(*sessionId, checkpoint->state.completedRounds, "manual_checkpoint", {
            {"message", "The latest safe conversation checkpoint was retained on user request."},
        });
        AppendLog("Marked the latest automatically persisted safe checkpoint for session #" +
            wxString::Format("%lld", *sessionId));
        RefreshDatabaseView();
    }
    catch (const std::exception& error)
    {
        ReportHistoryError("manual checkpoint", error.what());
        AppendLog("Cannot save agent state: " + FromUtf8(error.what()));
    }
}

bool MainFrame::PrepareResume(const AgentSessionCheckpoint& checkpoint)
{
    if (checkpoint.state.mode != AgentRunMode::LaunchRocket || checkpoint.summary.taskId)
        throw std::runtime_error("Only a saved general-agent context can be resumed from the desktop UI");
    if (checkpoint.summary.status == "completed" || checkpoint.summary.status == "succeeded")
        throw std::runtime_error("The selected agent session already completed successfully");
    if (checkpoint.state.terminalReached)
    {
        FinishHistorySession(
            checkpoint.summary.id,
            checkpoint.state.terminalSucceeded ? "completed" : "failed",
            checkpoint.state.terminalText);
        AppendLog("Session #" + wxString::Format("%lld", checkpoint.summary.id) +
            " already has terminal game evidence and was finalized without another model call");
        RefreshDatabaseView();
        return false;
    }

    if (checkpoint.summary.aiProvider == "llama_cpp")
    {
        aiProvider_->SetSelection(0);
        llamaModel_->ChangeValue(FromUtf8(checkpoint.summary.aiModel));
    }
    else if (checkpoint.summary.aiProvider == "openrouter")
    {
        aiProvider_->SetSelection(1);
        const auto savedModel = FromUtf8(checkpoint.summary.aiModel);
        auto selection = openRouterModel_->FindString(savedModel);
        if (selection == wxNOT_FOUND)
            selection = openRouterModel_->Append(savedModel);
        openRouterModel_->SetSelection(selection);
    }
    else
    {
        throw std::runtime_error("The saved session uses an unsupported AI provider: " + checkpoint.summary.aiProvider);
    }
    UpdateAiProviderControls();

    pendingResumeCheckpoint_ = checkpoint;
    objective_->ChangeValue(FromUtf8(checkpoint.state.objective));
    nonStopAgentRun_->SetValue(!checkpoint.state.maximumRounds.has_value());
    if (checkpoint.state.maximumRounds)
        maximumRounds_->SetValue(*checkpoint.state.maximumRounds);
    UpdateAgentRoundControls();
    AppendLog("Loaded session #" + wxString::Format("%lld", checkpoint.summary.id) +
        "; restored its provider and model, then will resume saved context after re-observing the game");
    return true;
}

void MainFrame::ResumeSavedState()
{
    if (workActive_)
    {
        AppendLog("Stop the active operation before resuming a saved context");
        return;
    }
    if (!historyDatabase_)
    {
        AppendLog("Cannot resume saved state: the history database is unavailable");
        return;
    }

    try
    {
        const auto checkpoint = historyDatabase_->LoadLatestResumableCheckpoint();
        if (!checkpoint)
            throw std::runtime_error("No resumable general-agent checkpoint is stored in the database");
        if (PrepareResume(*checkpoint))
            RunAgent();
    }
    catch (const std::exception& error)
    {
        AppendLog("Cannot resume saved state: " + FromUtf8(error.what()));
    }
}

void MainFrame::ResumeSelectedDatabaseState()
{
    if (workActive_)
    {
        AppendLog("Stop the active operation before resuming a saved context");
        return;
    }
    const auto selection = databaseSessionChoice_->GetSelection();
    if (!historyDatabase_ || selection == wxNOT_FOUND ||
        static_cast<std::size_t>(selection) >= databaseSessions_.size())
    {
        AppendLog("Select a saved session before resuming it");
        return;
    }

    try
    {
        const auto& session = databaseSessions_[static_cast<std::size_t>(selection)];
        const auto checkpoint = historyDatabase_->LoadCheckpoint(session.id);
        if (!checkpoint)
            throw std::runtime_error("The selected session has no saved checkpoint");
        if (PrepareResume(*checkpoint))
            RunAgent();
    }
    catch (const std::exception& error)
    {
        AppendLog("Cannot resume selected state: " + FromUtf8(error.what()));
    }
}

void MainFrame::ConnectRcon()
{
    AppSettings requested;
    try
    {
        requested = ReadSettingsFromControls();
        ValidateRconSettings(requested);
        settings_ = requested;
        settings_.Save();
    }
    catch (const std::exception& error)
    {
        AppendLog("Cannot connect: " + FromUtf8(error.what()));
        UpdateWebControlState();
        return;
    }

    StartWork("Connecting to Factorio RCON", [this, requested](std::stop_token) {
        auto client = std::make_unique<RconClient>();
        client->Connect(requested.rconHost, requested.rconPort, requested.rconPassword);
        {
            std::scoped_lock lock(clientMutex_);
            rconClient_ = std::move(client);
        }
        CallAfter([this] {
            SetConnectionState(true, "Authenticated");
            AppendLog("Factorio RCON authenticated successfully");
        });
    });
}

void MainFrame::DisconnectRcon()
{
    std::scoped_lock lock(clientMutex_);
    if (rconClient_)
        rconClient_->Disconnect();
    rconClient_.reset();
    if (rconStatus_)
    {
        SetConnectionState(false);
        AppendLog("Factorio RCON disconnected");
    }
}

void MainFrame::TestRcon()
{
    StartWork("Reading FactorIA Bridge status", [this](std::stop_token) {
        FactorioTools bridge(
            [this](const std::string& command) { return ExecuteRconCommand(command); },
            {});
        const auto status = bridge.GetBridgeStatus();
        const auto response =
            "connected; command protocol " + std::to_string(status.at("command_protocol_version").get<int>()) +
            ", runtime " + std::to_string(status.at("runtime_version").get<int>()) +
            ", tick " + std::to_string(status.at("tick").get<std::uint64_t>());
        CallAfter([this, response] { AppendLog("FactorIA Bridge: " + FromUtf8(response)); });
    });
}

void MainFrame::TestLlama()
{
    AppSettings requested;
    try
    {
        requested = ReadSettingsFromControls();
        ValidateAiSettings(requested);
        settings_ = requested;
        settings_.Save();
    }
    catch (const std::exception& error)
    {
        AppendLog("Cannot test AI provider: " + FromUtf8(error.what()));
        return;
    }

    llamaStatus_->SetLabel("Testing...");
    StartWork("Testing " + AiProviderName(requested), [this, requested](std::stop_token) {
        CreateAiClient(requested).CheckHealth();
        CallAfter([this, provider = FromUtf8(AiProviderName(requested))] {
            llamaStatus_->SetLabel("Ready");
            AppendLog(provider + " is ready");
        });
    });
}

void MainFrame::FetchOpenRouterModels()
{
    AppSettings requested;
    try
    {
        requested = ReadSettingsFromControls();
        if (requested.aiProvider != "openrouter")
            throw std::runtime_error("Select OpenRouter before fetching models");
        if (requested.openRouterApiKey.empty())
            throw std::runtime_error("Enter an OpenRouter API key before fetching models");
        settings_ = requested;
        settings_.Save();
    }
    catch (const std::exception& error)
    {
        AppendLog("Cannot fetch OpenRouter models: " + FromUtf8(error.what()));
        return;
    }

    StartWork(requested.openRouterFreeModelsOnly
            ? "Fetching free tool-capable OpenRouter models"
            : "Fetching tool-capable OpenRouter models",
        [this, requested](std::stop_token) {
            auto models = CreateAiClient(requested).ListToolModels(requested.openRouterFreeModelsOnly);
            CallAfter([this,
                          models = std::move(models),
                          selected = requested.openRouterModel,
                          freeOnly = requested.openRouterFreeModelsOnly] {
                openRouterModel_->Freeze();
                openRouterModel_->Clear();
                for (const auto& model : models)
                    openRouterModel_->Append(FromUtf8(model));
                const auto previous = openRouterModel_->FindString(FromUtf8(selected));
                if (previous != wxNOT_FOUND)
                    openRouterModel_->SetSelection(previous);
                openRouterModel_->Thaw();

                AppendLog("Fetched " + wxString::Format("%zu", models.size()) +
                    (freeOnly
                        ? " free tool-capable OpenRouter models"
                        : " tool-capable OpenRouter models") +
                    (previous == wxNOT_FOUND && !selected.empty()
                        ? "; the previously selected model is no longer available"
                        : ""));
            });
        });
}

void MainFrame::RunAgent()
{
    AppSettings requested;
    std::string objective;
    std::optional<int> maximumRounds;
    std::optional<AgentSessionCheckpoint> resumeCheckpoint;
    std::int64_t sessionId{};
    try
    {
        requested = ReadSettingsFromControls();
        ValidateAiSettings(requested);
        if (requested.factorioUserDataPath.empty())
            throw std::runtime_error("Factorio user data directory cannot be empty");
        objective = objective_->GetValue().ToStdString();
        if (!requested.nonStopAgentRun)
            maximumRounds = maximumRounds_->GetValue();
        if (objective.empty())
            throw std::runtime_error("Agent objective cannot be empty");
        if (!historyDatabase_ || historyErrorReported_.load())
            throw std::runtime_error("The agent history database is unavailable");
        {
            std::scoped_lock lock(clientMutex_);
            if (!rconClient_ || !rconClient_->IsConnected())
                throw std::runtime_error("Connect to Factorio RCON before running the agent");
        }

        resumeCheckpoint = pendingResumeCheckpoint_;
        if (resumeCheckpoint)
        {
            if (resumeCheckpoint->state.mode != AgentRunMode::LaunchRocket ||
                resumeCheckpoint->state.objective != objective)
            {
                throw std::runtime_error("The saved context no longer matches the requested general-agent run");
            }
        }
        settings_ = requested;
        settings_.Save();
        sessionId = historyDatabase_->BeginSession({
            .objective = objective,
            .mode = AgentRunMode::LaunchRocket,
            .aiProvider = requested.aiProvider,
            .aiModel = requested.aiProvider == "openrouter" ? requested.openRouterModel : requested.llamaModel,
            .maximumRounds = maximumRounds,
            .nonStop = requested.nonStopAgentRun,
            .parentSessionId = resumeCheckpoint
                ? std::optional<std::int64_t>(resumeCheckpoint->summary.id)
                : std::nullopt,
            .taskId = std::nullopt,
        });
        activeSessionId_ = sessionId;
        lastSessionId_ = sessionId;
        pendingResumeCheckpoint_.reset();
    }
    catch (const std::exception& error)
    {
        AppendLog("Cannot run agent: " + FromUtf8(error.what()));
        UpdateWebControlState();
        return;
    }

    agentStatus_->SetLabel("Running");
    UpdatePersistenceControls();
    const auto activeRound = std::make_shared<std::atomic<int>>();
    StartWork("Running agent objective",
        [this,
         requested,
         objective,
         maximumRounds,
         resumeState = resumeCheckpoint ? std::optional<AgentRunState>(resumeCheckpoint->state) : std::nullopt,
         sessionId,
         activeRound](std::stop_token stopToken) {
        FactorioTools tools(
            [this](const std::string& command) { return ExecuteRconCommand(command); },
            requested.factorioUserDataPath,
            requested.useDedicatedAiCharacter,
            std::nullopt,
            CreateBridgeHistoryHandler(sessionId, activeRound));
        AgentController controller(CreateAiClient(requested), tools);
        try
        {
            const auto result = controller.Run(
                objective,
                maximumRounds,
                stopToken,
                AgentRunMode::LaunchRocket,
                resumeState,
                CreateAgentRunCallbacks(sessionId, activeRound, tools));
            FinishHistorySession(
                sessionId,
                result.stopped ? "stopped" : result.succeeded ? "completed" : "failed",
                result.finalText);
            CallAfter([this, result, sessionId] {
                if (activeSessionId_ && *activeSessionId_ == sessionId)
                    activeSessionId_.reset();
                if (!activeSessionId_)
                    lastSessionId_ = sessionId;
                SetAgentStatus(result.stopped ? "Stopped" : result.succeeded ? "Completed" : "Failed");
                AppendLog(result.stopped
                    ? "Agent stopped after round " + wxString::Format("%d", result.rounds)
                    : "Agent completed after round " + wxString::Format("%d", result.rounds));
                RefreshDatabaseView();
                UpdatePersistenceControls();
            });
        }
        catch (const std::exception& error)
        {
            auto status = std::string("failed");
            auto finalText = std::string{};
            auto errorText = std::string(error.what());
            try
            {
                const auto checkpoint = historyDatabase_->LoadCheckpoint(sessionId);
                if (checkpoint && checkpoint->state.mode == AgentRunMode::LaunchRocket &&
                    checkpoint->state.terminalReached)
                {
                    status = checkpoint->state.terminalSucceeded ? "completed" : "failed";
                    finalText = checkpoint->state.terminalText;
                    errorText.clear();
                }
            }
            catch (...)
            {
                // The terminal checkpoint remains authoritative if the database cannot be read again here.
            }
            FinishHistorySession(sessionId, status, finalText, errorText);
            CallAfter([this, sessionId] {
                if (activeSessionId_ && *activeSessionId_ == sessionId)
                    activeSessionId_.reset();
                if (!activeSessionId_)
                    lastSessionId_ = sessionId;
                RefreshDatabaseView();
                UpdatePersistenceControls();
            });
            throw;
        }
    }, true);
}

void MainFrame::RunAgentTaskUpdates(std::stop_token stopToken)
{
    FactorioTools taskBridge(
        [this](const std::string& command) { return ExecuteRconCommand(command); },
        {},
        true);
    while (!stopToken.stop_requested())
    {
        bool connected = false;
        {
            std::scoped_lock lock(clientMutex_);
            connected = rconClient_ && rconClient_->IsConnected();
        }

        if (connected && !agentTaskDispatchPending_.load())
        {
            try
            {
                auto task = taskBridge.PeekAgentTask();
                bool expected = false;
                if (task && agentTaskDispatchPending_.compare_exchange_strong(expected, true))
                {
                    CallAfter([this, task = std::move(*task)]() mutable {
                        QueueAgentTask(std::move(task));
                    });
                }
            }
            catch (const std::exception& error)
            {
                // A disconnect can race this poll; normal connection UI owns that error state.
                std::scoped_lock lock(clientMutex_);
                if (rconClient_ && rconClient_->IsConnected())
                {
                    CallAfter([this, message = FromUtf8(error.what())] {
                        AppendLog("Cannot read in-game agent tasks: " + message);
                    });
                }
            }
        }

        std::unique_lock lock(agentTaskWaitMutex_);
        agentTaskChanged_.wait_for(lock, stopToken, AgentTaskPollInterval, [] { return false; });
    }
}

void MainFrame::ConfigureInventoryRefresh()
{
    {
        std::scoped_lock lock(inventoryRefreshMutex_);
        inventoryRefreshRequest_.enabled = rconConnected_;
        inventoryRefreshRequest_.useDedicatedCharacter = useDedicatedAiCharacter_->GetValue();
        ++inventoryRefreshRequest_.revision;
    }

    inventoryItems_.clear();
    inventoryAvailable_ = false;
    inventoryStatus_ = rconConnected_ ? "Refreshing inventory..." : "Not connected";
    RenderInventory();
    UpdateWebControlState();
    inventoryRefreshChanged_.notify_all();
}

void MainFrame::RunInventoryUpdates(std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        InventoryRefreshRequest request;
        {
            std::unique_lock lock(inventoryRefreshMutex_);
            inventoryRefreshChanged_.wait(lock, stopToken, [this] {
                return inventoryRefreshRequest_.enabled;
            });
            if (stopToken.stop_requested())
                return;
            request = inventoryRefreshRequest_;
        }

        std::vector<WebInventoryItem> items;
        bool available = false;
        std::string status;
        try
        {
            FactorioTools tools(
                [this](const std::string& command) { return ExecuteRconCommand(command); },
                {},
                request.useDedicatedCharacter);
            const auto result = tools.Execute("get_inventory", json::object());
            const auto& sourceItems = result.at("items");
            if (!sourceItems.is_array())
                throw std::runtime_error("Factorio returned an invalid inventory list");

            for (const auto& sourceItem : sourceItems)
            {
                if (!sourceItem.is_object() || !sourceItem.contains("name") ||
                    !sourceItem.at("name").is_string() || !sourceItem.contains("count"))
                {
                    throw std::runtime_error("Factorio returned an invalid inventory item");
                }

                std::int64_t count{};
                const auto& sourceCount = sourceItem.at("count");
                if (sourceCount.is_number_unsigned())
                {
                    const auto value = sourceCount.get<std::uint64_t>();
                    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                        throw std::runtime_error("Factorio returned an inventory count that is too large");
                    count = static_cast<std::int64_t>(value);
                }
                else if (sourceCount.is_number_integer())
                {
                    count = sourceCount.get<std::int64_t>();
                }
                else
                {
                    throw std::runtime_error("Factorio returned an invalid inventory count");
                }
                if (count < 0)
                    throw std::runtime_error("Factorio returned a negative inventory count");

                const auto name = sourceItem.at("name").get<std::string>();
                if (name.empty())
                    throw std::runtime_error("Factorio returned an inventory item without a name");
                items.push_back({
                    .name = name,
                    .count = count,
                    .quality = sourceItem.value("quality", std::string{"normal"}),
                });
            }
            std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) {
                return std::tie(left.name, left.quality) < std::tie(right.name, right.quality);
            });
            available = true;
            status = items.empty()
                ? "Updated: inventory is empty"
                : "Updated: " + std::to_string(items.size()) + " item type(s)";
        }
        catch (const std::exception& error)
        {
            status = "Inventory unavailable: " + std::string(error.what());
        }

        CallAfter([this,
                   revision = request.revision,
                   items = std::move(items),
                   available,
                   status = std::move(status)]() mutable {
            ApplyInventorySnapshot(revision, std::move(items), available, std::move(status));
        });

        std::unique_lock lock(inventoryRefreshMutex_);
        inventoryRefreshChanged_.wait_for(lock, stopToken, InventoryRefreshInterval, [this, revision = request.revision] {
            return !inventoryRefreshRequest_.enabled || inventoryRefreshRequest_.revision != revision;
        });
    }
}

void MainFrame::ApplyInventorySnapshot(
    std::uint64_t revision,
    std::vector<WebInventoryItem> items,
    bool available,
    std::string status)
{
    {
        std::scoped_lock lock(inventoryRefreshMutex_);
        if (!inventoryRefreshRequest_.enabled || inventoryRefreshRequest_.revision != revision)
            return;
    }

    inventoryItems_ = std::move(items);
    inventoryAvailable_ = available;
    inventoryStatus_ = std::move(status);
    RenderInventory();
    UpdateWebControlState();
}

void MainFrame::RenderInventory()
{
    inventoryStatusLabel_->SetLabel(FromUtf8(inventoryStatus_));
    refreshInventoryButton_->Enable(rconConnected_);

    const auto rowCount = inventoryGrid_->GetNumberRows();
    const auto desiredRowCount = static_cast<int>(inventoryItems_.size());
    if (rowCount < desiredRowCount)
        inventoryGrid_->AppendRows(desiredRowCount - rowCount);
    else if (rowCount > desiredRowCount)
        inventoryGrid_->DeleteRows(0, rowCount - desiredRowCount);

    for (int row = 0; row < desiredRowCount; ++row)
    {
        const auto& item = inventoryItems_[static_cast<std::size_t>(row)];
        inventoryGrid_->SetCellValue(row, 0, FromUtf8(item.name));
        inventoryGrid_->SetCellValue(row, 1, wxString::Format("%lld", static_cast<long long>(item.count)));
        inventoryGrid_->SetCellValue(row, 2, FromUtf8(item.quality));
    }
    inventoryGrid_->ForceRefresh();
}

void MainFrame::QueueAgentTask(FactorioAgentTask task)
{
    pendingAgentTask_ = std::move(task);
    if (workActive_)
    {
        if (worker_.joinable())
            worker_.request_stop();
        SetAgentStatus("Switching to in-game task...");
        AppendLog("In-game task #" + wxString::Format(
            "%llu", static_cast<unsigned long long>(pendingAgentTask_->id)) +
            " is waiting for the current operation to stop");
        return;
    }

    auto queued = std::move(*pendingAgentTask_);
    pendingAgentTask_.reset();
    StartAgentTask(std::move(queued));
}

void MainFrame::StartAgentTask(FactorioAgentTask task)
{
    AppSettings requested;
    AgentRunMode mode{AgentRunMode::BuildGhosts};
    std::string objective;
    std::int64_t sessionId{};
    try
    {
        requested = ReadSettingsFromControls();
        ValidateAiSettings(requested);
        if (task.kind != "build-ghosts" && task.kind != "remove-markers")
            throw std::runtime_error("Unsupported in-game agent task: " + task.kind);
        if (!historyDatabase_ || historyErrorReported_.load())
            throw std::runtime_error("The agent history database is unavailable");

        const auto removeMarkers = task.kind == "remove-markers";
        mode = removeMarkers ? AgentRunMode::RemoveMarkers : AgentRunMode::BuildGhosts;
        const auto requestedAction = removeMarkers
            ? "remove every entity explicitly marked for deconstruction"
            : "build every entity ghost";
        objective =
            "In-game task #" + std::to_string(task.id) + " from player " + task.issuerName +
            ": " + requestedAction + " within radius " + std::to_string(task.searchRadius) +
            " of the dedicated FactorIA character. Once none remain, return to the issuing player with "
            "return_to_task_issuer.";
        sessionId = historyDatabase_->BeginSession({
            .objective = objective,
            .mode = mode,
            .aiProvider = requested.aiProvider,
            .aiModel = requested.aiProvider == "openrouter" ? requested.openRouterModel : requested.llamaModel,
            .maximumRounds = std::nullopt,
            .nonStop = true,
            .parentSessionId = std::nullopt,
            .taskId = std::to_string(task.id),
        });
        activeSessionId_ = sessionId;
        lastSessionId_ = sessionId;
    }
    catch (const std::exception& error)
    {
        const auto message = std::string("Desktop configuration error: ") + error.what();
        SetAgentStatus("Task rejected");
        StartWork("Rejecting in-game agent task", [this, task = std::move(task), message](std::stop_token) {
            FactorioTools tools(
                [this](const std::string& command) { return ExecuteRconCommand(command); },
                {},
                true,
                task);
            try
            {
                tools.FinishAgentTask(task.id, false, message);
            }
            catch (...)
            {
                agentTaskDispatchPending_.store(false);
                agentTaskChanged_.notify_all();
                throw;
            }
            agentTaskDispatchPending_.store(false);
            agentTaskChanged_.notify_all();
        });
        return;
    }

    const auto workDescription = "Running in-game " + task.kind + " task #" + std::to_string(task.id);
    agentStatus_->SetLabel("Running in-game task #" + wxString::Format(
        "%llu", static_cast<unsigned long long>(task.id)));
    UpdatePersistenceControls();
    const auto activeRound = std::make_shared<std::atomic<int>>();
    StartWork(workDescription,
        [this, requested, task = std::move(task), objective, mode, sessionId, activeRound](std::stop_token stopToken) {
            FactorioTools tools(
                [this](const std::string& command) { return ExecuteRconCommand(command); },
                requested.factorioUserDataPath,
                true,
                task,
                CreateBridgeHistoryHandler(sessionId, activeRound));

            try
            {
                if (!tools.ClaimAgentTask(task.id))
                    throw std::runtime_error("In-game agent task could not be claimed");

                AgentController controller(CreateAiClient(requested), tools);
                const auto result = controller.Run(
                    objective,
                    std::nullopt,
                    stopToken,
                    mode,
                    std::nullopt,
                    CreateAgentRunCallbacks(sessionId, activeRound, tools));
                const auto message = result.stopped
                    ? std::string("Stopped by the desktop user.")
                    : result.finalText;
                tools.FinishAgentTask(task.id, result.succeeded, message);
                FinishHistorySession(
                    sessionId,
                    result.stopped ? "stopped" : result.succeeded ? "completed" : "failed",
                    message);
                agentTaskDispatchPending_.store(false);
                agentTaskChanged_.notify_all();
                CallAfter([this, result, taskId = task.id, sessionId] {
                    if (activeSessionId_ && *activeSessionId_ == sessionId)
                        activeSessionId_.reset();
                    if (!activeSessionId_)
                        lastSessionId_ = sessionId;
                    SetAgentStatus(result.stopped
                        ? "Task stopped"
                        : result.succeeded ? "Task completed" : "Task failed");
                    AppendLog("In-game task #" + wxString::Format(
                        "%llu", static_cast<unsigned long long>(taskId)) +
                        (result.succeeded ? " completed" : result.stopped ? " stopped" : " failed"));
                    RefreshDatabaseView();
                    UpdatePersistenceControls();
                });
            }
            catch (const std::exception& error)
            {
                try
                {
                    tools.FinishAgentTask(task.id, false, error.what());
                }
                catch (...)
                {
                }
                FinishHistorySession(sessionId, "failed", {}, error.what());
                agentTaskDispatchPending_.store(false);
                agentTaskChanged_.notify_all();
                CallAfter([this, sessionId] {
                    if (activeSessionId_ && *activeSessionId_ == sessionId)
                        activeSessionId_.reset();
                    if (!activeSessionId_)
                        lastSessionId_ = sessionId;
                    RefreshDatabaseView();
                    UpdatePersistenceControls();
                });
                throw;
            }
        },
        true);
}

void MainFrame::StopAgent()
{
    if (workActive_ && worker_.joinable())
    {
        worker_.request_stop();
        SetAgentStatus("Stopping...");
        AppendLog("Stop requested; an active AI HTTP request must finish before cancellation completes");
    }
}

void MainFrame::HandleWebControlCommand(WebControlCommand command)
{
    if (command.action != WebControlCommand::Action::Stop && workActive_)
    {
        AppendLog("Web control action ignored because another operation is already running");
        UpdateWebControlState();
        return;
    }

    switch (command.action)
    {
    case WebControlCommand::Action::Connect:
        ConnectRcon();
        break;
    case WebControlCommand::Action::Disconnect:
        DisconnectRcon();
        break;
    case WebControlCommand::Action::Run:
        objective_->ChangeValue(FromUtf8(command.objective));
        nonStopAgentRun_->SetValue(!command.maximumRounds.has_value());
        if (command.maximumRounds)
            maximumRounds_->SetValue(*command.maximumRounds);
        UpdateAgentRoundControls();
        RunAgent();
        break;
    case WebControlCommand::Action::Stop:
        StopAgent();
        break;
    }
}

void MainFrame::UpdateWebControlState()
{
    if (!webControlServer_)
        return;

    webControlServer_->PublishState({
        .connected = rconConnected_,
        .busy = workActive_,
        .agentRunning = agentStopButton_->IsEnabled(),
        .agentStatus = agentStatus_->GetLabel().ToStdString(),
        .objective = objective_->GetValue().ToStdString(),
        .maximumRounds = maximumRounds_->GetValue(),
        .nonStop = nonStopAgentRun_->GetValue(),
        .inventoryAvailable = inventoryAvailable_,
        .inventoryStatus = inventoryStatus_,
        .inventory = inventoryItems_,
    });
}

void MainFrame::SetAgentStatus(const wxString& status)
{
    agentStatus_->SetLabel(status);
    UpdateWebControlState();
}

void MainFrame::StartWork(
    std::string description,
    std::function<void(std::stop_token)> work,
    bool enableStop)
{
    if (worker_.joinable())
        worker_.join();
    workActive_ = true;
    connectButton_->Disable();
    disconnectButton_->Disable();
    testButton_->Disable();
    llamaTestButton_->Disable();
    openRouterFreeModelsOnly_->Disable();
    fetchOpenRouterModelsButton_->Disable();
    agentRunButton_->Disable();
    agentStopButton_->Enable(enableStop);
    UpdateWebControlState();
    UpdatePersistenceControls();
    AppendLog(FromUtf8(description) + "...");
    worker_ = std::jthread([this, work = std::move(work)](std::stop_token stopToken) {
        try
        {
            work(stopToken);
        }
        catch (const std::exception& error)
        {
            const auto message = FromUtf8(error.what());
            CallAfter([this, message] {
                AppendLog("Operation failed: " + message);
                if (llamaStatus_->GetLabel() == "Testing...")
                    llamaStatus_->SetLabel("Failed");
                if (agentStatus_->GetLabel() == "Running" ||
                    agentStatus_->GetLabel() == "Stopping..." ||
                    agentStatus_->GetLabel().StartsWith("Running in-game task") ||
                    agentStatus_->GetLabel() == "Switching to in-game task...")
                {
                    SetAgentStatus("Failed");
                }
                std::scoped_lock lock(clientMutex_);
                if (!rconClient_ || !rconClient_->IsConnected())
                    SetConnectionState(false, "Connection failed");
            });
        }
        CallAfter([this] { FinishWork(); });
    });
}

void MainFrame::FinishWork()
{
    workActive_ = false;
    connectButton_->Enable();
    llamaTestButton_->Enable();
    const auto openRouter = aiProvider_->GetSelection() == 1;
    openRouterFreeModelsOnly_->Enable(openRouter);
    fetchOpenRouterModelsButton_->Enable(openRouter);
    agentStopButton_->Disable();
    bool connected = false;
    {
        std::scoped_lock lock(clientMutex_);
        connected = rconClient_ && rconClient_->IsConnected();
    }
    disconnectButton_->Enable(connected);
    testButton_->Enable(connected);
    agentRunButton_->Enable(connected);
    UpdatePersistenceControls();
    UpdateWebControlState();
    if (pendingAgentTask_)
    {
        auto task = std::move(*pendingAgentTask_);
        pendingAgentTask_.reset();
        StartAgentTask(std::move(task));
    }
}

void MainFrame::SetConnectionState(bool connected, const wxString& detail)
{
    rconConnected_ = connected;
    const wxString state = connected ? "Connected" : "Disconnected";
    rconStatus_->SetLabel(detail.empty() ? state : state + " - " + detail);
    SetStatusText("Factorio: " + state, 0);
    SetTitle("FactorIA - " + state);
    disconnectButton_->Enable(connected);
    testButton_->Enable(connected);
    agentRunButton_->Enable(connected);
    ConfigureInventoryRefresh();
    agentTaskChanged_.notify_all();
    UpdatePersistenceControls();
    UpdateWebControlState();
}

void MainFrame::PublishModelDecision(FactorioTools& tools, const std::string& decision)
{
    try
    {
        // Publish before tool execution so players can see the agent's next action in advance.
        tools.PrintModelDecision(decision);
    }
    catch (const std::exception& error)
    {
        CallAfter([this, message = FromUtf8(error.what())] {
            AppendLog("Cannot print model decision to game chat: " + message);
        });
    }
    CallAfter([this, text = FromUtf8(decision)] { AppendModelDecision(text); });
}

void MainFrame::AppendModelDecision(const wxString& decision)
{
    // Decisions are already normalized by AgentController; keep one entry per line here.
    modelDecisions_->AppendText(decision + "\n");
    if (webControlServer_)
        webControlServer_->PublishDecision(decision.ToStdString());
}

void MainFrame::AppendLog(const wxString& message)
{
    const auto timestamp = wxDateTime::Now().FormatISOTime();
    auto indented = message;
    indented.Replace("\r\n", "\n");
    indented.Replace("\n", "\n           ");
    log_->AppendText("[" + timestamp + "] " + indented + "\n");
    if (message.Contains("\n"))
        log_->AppendText("\n");
}

std::string MainFrame::ExecuteRconCommand(const std::string& command)
{
    std::scoped_lock lock(clientMutex_);
    if (!rconClient_ || !rconClient_->IsConnected())
        throw std::runtime_error("Factorio RCON is not connected");
    return rconClient_->Execute(command);
}
}
