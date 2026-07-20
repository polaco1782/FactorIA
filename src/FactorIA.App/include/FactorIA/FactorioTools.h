#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
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

    FactorioTools(
        CommandExecutor executeCommand,
        std::filesystem::path factorioUserDataPath,
        bool useDedicatedCharacter = false);

    [[nodiscard]] nlohmann::json Definitions(bool includeScreenshot = true) const;
    nlohmann::json Execute(
        const std::string& name,
        const nlohmann::json& arguments,
        std::stop_token stopToken = {}) const;

private:
    nlohmann::json ExecuteJson(const std::string& body) const;
    [[nodiscard]] std::string ControlCharacterLua() const;
    void EnsurePlayerControlAction() const;
    nlohmann::json StartPlayerControlAction(const nlohmann::json& arguments) const;
    nlohmann::json PollPlayerControlAction(std::uint64_t jobId) const;
    nlohmann::json StopPlayerControlAction(std::uint64_t jobId) const;
    nlohmann::json RecordResearchTriggerAction(
        const std::string& actionType,
        const std::string& prototypeName,
        const std::string& quality,
        double count) const;
    nlohmann::json WaitForPlayerControlAction(
        const nlohmann::json& startResult,
        std::chrono::steady_clock::time_point deadline,
        std::stop_token stopToken) const;
    nlohmann::json GetGameState() const;
    nlohmann::json GetInventory() const;
    nlohmann::json GetCraftableRecipes() const;
    nlohmann::json GetResearchStatus(const nlohmann::json& arguments) const;
    nlohmann::json StartResearch(const nlohmann::json& arguments) const;
    nlohmann::json GetNearbyEntities(const nlohmann::json& arguments) const;
    nlohmann::json FindResourcePatches(const nlohmann::json& arguments) const;
    nlohmann::json FindWater(const nlohmann::json& arguments) const;
    nlohmann::json Walk(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json WalkTo(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json WalkToForPlacement(
        const nlohmann::json& arguments,
        std::stop_token stopToken) const;
    nlohmann::json RequestPath(double targetX, double targetY, double stoppingDistance, std::stop_token stopToken) const;
    nlohmann::json StopWalking() const;
    nlohmann::json TakeScreenshot(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json MineEntity(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json Craft(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json PlaceEntity(const nlohmann::json& arguments) const;
    nlohmann::json SetAssemblerRecipe(const nlohmann::json& arguments) const;
    nlohmann::json InsertItemIntoEntity(const nlohmann::json& arguments) const;
    nlohmann::json TakeItemFromEntity(const nlohmann::json& arguments) const;
    nlohmann::json WaitForMachineOutput(
        const nlohmann::json& arguments,
        std::stop_token stopToken) const;
    nlohmann::json TransferInventoryToContainer(const nlohmann::json& arguments) const;

    CommandExecutor executeCommand_;
    std::filesystem::path factorioUserDataPath_;
    bool useDedicatedCharacter_{};
    nlohmann::json definitions_;
    mutable bool playerControlActionReady_ = false;
};
}
