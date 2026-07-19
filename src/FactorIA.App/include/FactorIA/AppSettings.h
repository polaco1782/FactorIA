#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace factoria
{
struct AppSettings
{
    AppSettings();

    std::string rconHost{"127.0.0.1"};
    std::uint16_t rconPort{27015};
    std::string rconPassword{"mypassword"};
    std::string llamaUrl{"http://127.0.0.1:8080"};
    std::string llamaModel;
    std::string aiProvider{"llama_cpp"};
    std::string openRouterApiKey;
    std::string openRouterModel;
    std::filesystem::path factorioUserDataPath;

    static std::filesystem::path SettingsPath();
    static AppSettings Load();
    void Save() const;
};
}
