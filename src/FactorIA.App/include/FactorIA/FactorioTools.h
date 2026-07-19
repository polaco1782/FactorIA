#pragma once

#include <functional>
#include <stop_token>
#include <string>

#include <nlohmann/json.hpp>

namespace factoria
{
class FactorioTools
{
public:
    using CommandExecutor = std::function<std::string(const std::string&)>;

    explicit FactorioTools(CommandExecutor executeCommand);

    [[nodiscard]] const nlohmann::json& Definitions() const noexcept;
    nlohmann::json Execute(
        const std::string& name,
        const nlohmann::json& arguments,
        std::stop_token stopToken = {}) const;

private:
    nlohmann::json ExecuteJson(const std::string& body) const;
    nlohmann::json GetGameState() const;
    nlohmann::json GetInventory() const;
    nlohmann::json GetNearbyEntities(const nlohmann::json& arguments) const;
    nlohmann::json Walk(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json WalkTo(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json StopWalking() const;
    nlohmann::json MineEntity(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json Craft(const nlohmann::json& arguments) const;

    CommandExecutor executeCommand_;
    nlohmann::json definitions_;
};
}
