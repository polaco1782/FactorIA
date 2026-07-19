#include <FactorIA/AppSettings.h>

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <wx/stdpaths.h>

namespace factoria
{
namespace
{
using json = nlohmann::json;

std::filesystem::path UserConfigDirectory()
{
#ifdef _WIN32
    return std::filesystem::path(wxStandardPaths::Get().GetUserConfigDir().ToStdWstring());
#else
    return std::filesystem::path(wxStandardPaths::Get().GetUserConfigDir().ToStdString());
#endif
}
}

AppSettings::AppSettings()
{
#ifdef _WIN32
    factorioUserDataPath = UserConfigDirectory() / "Factorio";
#else
    factorioUserDataPath = UserConfigDirectory() / ".factorio";
#endif
}

std::filesystem::path AppSettings::SettingsPath()
{
    return UserConfigDirectory() / "FactorIA" / "settings.json";
}

AppSettings AppSettings::Load()
{
    AppSettings settings;
    const auto path = SettingsPath();
    if (!std::filesystem::exists(path))
        return settings;

    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("Unable to open settings file: " + path.string());

    const auto value = json::parse(input);
    settings.rconHost = value.value("rcon_host", settings.rconHost);
    settings.rconPort = value.value("rcon_port", settings.rconPort);
    settings.rconPassword = value.value("rcon_password", settings.rconPassword);
    settings.llamaUrl = value.value("llama_url", settings.llamaUrl);
    settings.llamaModel = value.value("llama_model", settings.llamaModel);
    settings.aiProvider = value.value("ai_provider", settings.aiProvider);
    settings.openRouterApiKey = value.value("openrouter_api_key", settings.openRouterApiKey);
    settings.openRouterModel = value.value("openrouter_model", settings.openRouterModel);
    settings.factorioUserDataPath = value.value("factorio_user_data_path", settings.factorioUserDataPath.string());
    return settings;
}

void AppSettings::Save() const
{
    const auto path = SettingsPath();
    std::filesystem::create_directories(path.parent_path());

    std::ofstream output(path, std::ios::trunc);
    if (!output)
        throw std::runtime_error("Unable to write settings file: " + path.string());

    const json value{
        {"rcon_host", rconHost},
        {"rcon_port", rconPort},
        {"rcon_password", rconPassword},
        {"llama_url", llamaUrl},
        {"llama_model", llamaModel},
        {"ai_provider", aiProvider},
        {"openrouter_api_key", openRouterApiKey},
        {"openrouter_model", openRouterModel},
        {"factorio_user_data_path", factorioUserDataPath.string()},
    };
    output << value.dump(2) << '\n';
}
}
