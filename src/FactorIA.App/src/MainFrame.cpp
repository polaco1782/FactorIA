#include "MainFrame.h"

#include <chrono>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/datetime.h>
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
constexpr std::uint16_t WebControlPort = 8090;

std::string AiProviderName(const AppSettings& settings)
{
    return settings.aiProvider == "openrouter" ? "OpenRouter" : "llama.cpp";
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
        settings_ = AppSettings::Load();
        AppendLog("Settings loaded from " + FromPath(AppSettings::SettingsPath()));
    }
    catch (const std::exception& error)
    {
        AppendLog("Settings could not be loaded: " + FromUtf8(error.what()));
    }
    LoadSettingsIntoControls();
    SetConnectionState(false);
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
    openRouterStatusChanged_.notify_all();
    Centre();
}

MainFrame::~MainFrame()
{
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
    notebook->AddPage(connectionPanel, "Connections", true);
    notebook->AddPage(agentPanel, "Agent");
    notebook->AddPage(logPanel, "Log");

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

    auto* activityBox = new wxStaticBoxSizer(wxVERTICAL, agentPanel, "Model decisions");
    modelDecisions_ = new wxTextCtrl(agentPanel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    activityBox->Add(modelDecisions_, 1, wxEXPAND | wxALL, 8);
    agentRoot->Add(objectiveBox, 1, wxEXPAND | wxALL, 12);
    agentRoot->Add(agentControls, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    agentRoot->Add(activityBox, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    agentPanel->SetSizer(agentRoot);

    auto* logRoot = new wxBoxSizer(wxVERTICAL);
    log_ = new wxTextCtrl(logPanel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    logRoot->Add(log_, 1, wxEXPAND | wxALL, 8);
    logPanel->SetSizer(logRoot);

    saveButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveSettings(); });
    connectButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ConnectRcon(); });
    disconnectButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DisconnectRcon(); });
    testButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { TestRcon(); });
    llamaTestButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { TestLlama(); });
    fetchOpenRouterModelsButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { FetchOpenRouterModels(); });
    aiProvider_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateAiProviderControls(); });
    openRouterApiKey_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdateOpenRouterStatusRequest(); });
    objective_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdateWebControlState(); });
    maximumRounds_->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) { UpdateWebControlState(); });
    nonStopAgentRun_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        UpdateAgentRoundControls();
        UpdateWebControlState();
    });
    agentRunButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RunAgent(); });
    agentStopButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StopAgent(); });
    agentRunButton_->Disable();
    agentStopButton_->Disable();
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
        std::scoped_lock lock(clientMutex_);
        if (!rconClient_ || !rconClient_->IsConnected())
            throw std::runtime_error("Connect to Factorio RCON before running the agent");
        settings_ = requested;
        settings_.Save();
    }
    catch (const std::exception& error)
    {
        AppendLog("Cannot run agent: " + FromUtf8(error.what()));
        UpdateWebControlState();
        return;
    }

    agentStatus_->SetLabel("Running");
    StartWork("Running agent objective", [this, requested, objective, maximumRounds](std::stop_token stopToken) {
        FactorioTools tools(
            [this](const std::string& command) { return ExecuteRconCommand(command); },
            requested.factorioUserDataPath,
            requested.useDedicatedAiCharacter);
        AgentController controller(CreateAiClient(requested), tools);
        const auto result = controller.Run(
            objective,
            maximumRounds,
            stopToken,
            AgentRunMode::LaunchRocket,
            [this](const std::string& trace) {
                CallAfter([this, text = FromUtf8(trace)] { AppendLog(text); });
            },
            [this, &tools](const std::string& decision) {
                PublishModelDecision(tools, decision);
            });
        CallAfter([this, result] {
            SetAgentStatus(result.stopped ? "Stopped" : "Completed");
            AppendLog(result.stopped
                ? "Agent stopped after round " + wxString::Format("%d", result.rounds)
                : "Agent completed after round " + wxString::Format("%d", result.rounds));
        });
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
    try
    {
        requested = ReadSettingsFromControls();
        ValidateAiSettings(requested);
        if (task.kind != "build-ghosts" && task.kind != "remove-markers")
            throw std::runtime_error("Unsupported in-game agent task: " + task.kind);
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

    const auto removeMarkers = task.kind == "remove-markers";
    const auto mode = removeMarkers ? AgentRunMode::RemoveMarkers : AgentRunMode::BuildGhosts;
    const auto requestedAction = removeMarkers
        ? "remove every entity explicitly marked for deconstruction"
        : "build every entity ghost";
    const auto objective =
        "In-game task #" + std::to_string(task.id) + " from player " + task.issuerName +
        ": " + requestedAction + " within radius " + std::to_string(task.searchRadius) +
        " of the dedicated FactorIA character. Once none remain, return to the issuing player with "
        "return_to_task_issuer.";
    const auto workDescription = "Running in-game " + task.kind + " task #" + std::to_string(task.id);
    agentStatus_->SetLabel("Running in-game task #" + wxString::Format(
        "%llu", static_cast<unsigned long long>(task.id)));
    StartWork(workDescription,
        [this, requested, task = std::move(task), objective, mode](std::stop_token stopToken) {
            FactorioTools tools(
                [this](const std::string& command) { return ExecuteRconCommand(command); },
                requested.factorioUserDataPath,
                true,
                task);
            if (!tools.ClaimAgentTask(task.id))
            {
                agentTaskDispatchPending_.store(false);
                agentTaskChanged_.notify_all();
                throw std::runtime_error("In-game agent task could not be claimed");
            }

            try
            {
                AgentController controller(CreateAiClient(requested), tools);
                const auto result = controller.Run(
                    objective,
                    std::nullopt,
                    stopToken,
                    mode,
                    [this](const std::string& trace) {
                        CallAfter([this, text = FromUtf8(trace)] { AppendLog(text); });
                    },
                    [this, &tools](const std::string& decision) {
                        PublishModelDecision(tools, decision);
                    });
                const auto message = result.stopped
                    ? std::string("Stopped by the desktop user.")
                    : result.finalText;
                tools.FinishAgentTask(task.id, result.succeeded, message);
                agentTaskDispatchPending_.store(false);
                agentTaskChanged_.notify_all();
                CallAfter([this, result, taskId = task.id] {
                    SetAgentStatus(result.stopped
                        ? "Task stopped"
                        : result.succeeded ? "Task completed" : "Task failed");
                    AppendLog("In-game task #" + wxString::Format(
                        "%llu", static_cast<unsigned long long>(taskId)) +
                        (result.succeeded ? " completed" : result.stopped ? " stopped" : " failed"));
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
                agentTaskDispatchPending_.store(false);
                agentTaskChanged_.notify_all();
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
    agentTaskChanged_.notify_all();
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
