#include "MainFrame.h"

#include <chrono>
#include <functional>
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

LlamaClient CreateAiClient(const AppSettings& settings)
{
    if (settings.aiProvider == "openrouter")
        return LlamaClient(OpenRouterUrl, settings.openRouterModel, settings.openRouterApiKey);
    return LlamaClient(settings.llamaUrl, settings.llamaModel);
}

std::string AiProviderName(const AppSettings& settings)
{
    return settings.aiProvider == "openrouter" ? "OpenRouter" : "llama.cpp";
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
    Centre();
}

MainFrame::~MainFrame()
{
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
    testButton_ = new wxButton(connectionPanel, wxID_ANY, "Read game tick");
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

    auto* activityBox = new wxStaticBoxSizer(wxVERTICAL, agentPanel, "Current status");
    agentActivity_ = new wxTextCtrl(agentPanel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    activityBox->Add(agentActivity_, 1, wxEXPAND | wxALL, 8);
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
    nonStopAgentRun_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateAgentRoundControls(); });
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
    StartWork("Reading Factorio game tick", [this](std::stop_token) {
        std::string response;
        {
            std::scoped_lock lock(clientMutex_);
            if (!rconClient_ || !rconClient_->IsConnected())
                throw std::runtime_error("Factorio RCON is not connected");
            response = rconClient_->Execute("/sc rcon.print(\"FACTORIA_OK tick=\" .. game.tick)");
        }
        CallAfter([this, response] { AppendLog("Factorio response: " + FromUtf8(response)); });
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
        return;
    }

    SetAgentActivity("Starting agent...");
    agentStatus_->SetLabel("Running");
    StartWork("Running agent objective", [this, requested, objective, maximumRounds](std::stop_token stopToken) {
        FactorioTools tools([this](const std::string& command) {
            std::scoped_lock lock(clientMutex_);
            if (!rconClient_ || !rconClient_->IsConnected())
                throw std::runtime_error("Factorio RCON disconnected during the agent run");
            return rconClient_->Execute(command);
        }, requested.factorioUserDataPath, requested.useDedicatedAiCharacter);
        AgentController controller(CreateAiClient(requested), tools);
        const auto result = controller.Run(
            objective,
            maximumRounds,
            stopToken,
            [this](const std::string& trace) {
                CallAfter([this, text = FromUtf8(trace)] { AppendLog(text); });
            },
            [this](const std::string& status) {
                CallAfter([this, text = FromUtf8(status)] { SetAgentActivity(text); });
            });
        CallAfter([this, result] {
            agentStatus_->SetLabel(result.stopped ? "Stopped" : "Completed");
            if (result.stopped)
                SetAgentActivity("Stopped.");
            else
                SetAgentActivity(result.finalText.empty() ? "Completed." : FromUtf8(result.finalText));
            AppendLog(result.stopped
                ? "Agent stopped after round " + wxString::Format("%d", result.rounds)
                : "Agent completed after round " + wxString::Format("%d", result.rounds));
        });
    }, true);
}

void MainFrame::StopAgent()
{
    if (worker_.joinable())
    {
        worker_.request_stop();
        agentStatus_->SetLabel("Stopping...");
        SetAgentActivity("Stop requested; waiting for the active operation to finish...");
        AppendLog("Stop requested; an active AI HTTP request must finish before cancellation completes");
    }
}

void MainFrame::StartWork(
    std::string description,
    std::function<void(std::stop_token)> work,
    bool enableStop)
{
    if (worker_.joinable())
        worker_.join();
    connectButton_->Disable();
    disconnectButton_->Disable();
    testButton_->Disable();
    llamaTestButton_->Disable();
    openRouterFreeModelsOnly_->Disable();
    fetchOpenRouterModelsButton_->Disable();
    agentRunButton_->Disable();
    agentStopButton_->Enable(enableStop);
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
                if (agentStatus_->GetLabel() == "Running" || agentStatus_->GetLabel() == "Stopping...")
                {
                    agentStatus_->SetLabel("Failed");
                    SetAgentActivity("Failed: " + message);
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
    connectButton_->Enable();
    llamaTestButton_->Enable();
    const auto openRouter = aiProvider_->GetSelection() == 1;
    openRouterFreeModelsOnly_->Enable(openRouter);
    fetchOpenRouterModelsButton_->Enable(openRouter);
    agentStopButton_->Disable();
    std::scoped_lock lock(clientMutex_);
    const auto connected = rconClient_ && rconClient_->IsConnected();
    disconnectButton_->Enable(connected);
    testButton_->Enable(connected);
    agentRunButton_->Enable(connected);
}

void MainFrame::SetConnectionState(bool connected, const wxString& detail)
{
    const wxString state = connected ? "Connected" : "Disconnected";
    rconStatus_->SetLabel(detail.empty() ? state : state + " - " + detail);
    SetTitle("FactorIA - " + state);
    disconnectButton_->Enable(connected);
    testButton_->Enable(connected);
    agentRunButton_->Enable(connected);
}

void MainFrame::SetAgentActivity(const wxString& activity)
{
    // This view represents only the latest activity; the Log tab retains the full history.
    agentActivity_->ChangeValue(activity);
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
}
