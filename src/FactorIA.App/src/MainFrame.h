#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <wx/frame.h>

#include <FactorIA/AppSettings.h>
#include <FactorIA/AgentController.h>
#include <FactorIA/LlamaClient.h>
#include <FactorIA/RconClient.h>

class wxButton;
class wxCheckBox;
class wxChoice;
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
    void StopAgent();
    void StartWork(
        std::string description,
        std::function<void(std::stop_token)> work,
        bool enableStop = false);
    void FinishWork();
    void SetConnectionState(bool connected, const wxString& detail = {});
    void SetAgentActivity(const wxString& activity);
    void AppendLog(const wxString& message);

    AppSettings settings_;
    std::mutex clientMutex_;
    std::unique_ptr<RconClient> rconClient_;
    std::jthread worker_;

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
    wxStaticText* agentStatus_{};
    wxTextCtrl* agentActivity_{};
    wxTextCtrl* log_{};
};
}
