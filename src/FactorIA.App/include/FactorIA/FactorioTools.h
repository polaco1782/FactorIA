#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace factoria
{
struct FactorioAgentTask
{
    std::uint64_t id{};
    std::string kind;
    std::string issuerName;
    std::string issuerSurface;
    int issuerPlayerIndex{};
    double issuerX{};
    double issuerY{};
    double searchRadius{};
};

class FactorioTools
{
public:
    using CommandExecutor = std::function<std::string(const std::string&)>;

    FactorioTools(
        CommandExecutor executeCommand,
        std::filesystem::path factorioUserDataPath,
        bool useDedicatedCharacter = false,
        std::optional<FactorioAgentTask> agentTask = std::nullopt);

    [[nodiscard]] nlohmann::json Definitions(bool includeScreenshot = true) const;
    [[nodiscard]] nlohmann::json GetBridgeStatus() const;
    [[nodiscard]] std::optional<FactorioAgentTask> PeekAgentTask() const;
    [[nodiscard]] bool ClaimAgentTask(std::uint64_t taskId) const;
    void FinishAgentTask(std::uint64_t taskId, bool succeeded, const std::string& message) const;
    void PrintModelDecision(const std::string& decision) const;
    nlohmann::json Execute(
        const std::string& name,
        const nlohmann::json& arguments,
        std::stop_token stopToken = {}) const;

private:
    nlohmann::json ExecuteBridgeCommand(
        std::string_view operation,
        const nlohmann::json& arguments = nlohmann::json::object()) const;
    nlohmann::json ExecuteBridgeTool(
        const std::string& name,
        const nlohmann::json& arguments) const;
    void ValidatePlayerControlRuntime() const;
    nlohmann::json StartPlayerControlAction(const nlohmann::json& arguments) const;
    nlohmann::json PollPlayerControlAction(std::uint64_t jobId) const;
    nlohmann::json StopPlayerControlAction(std::uint64_t jobId) const;
    nlohmann::json WaitForPlayerControlAction(
        const nlohmann::json& startResult,
        std::chrono::steady_clock::time_point deadline,
        std::stop_token stopToken) const;
    nlohmann::json GetGameState() const;
    nlohmann::json GetInventory() const;
    nlohmann::json GetCraftableRecipes(const nlohmann::json& arguments) const;
    nlohmann::json GetResearchStatus(const nlohmann::json& arguments) const;
    nlohmann::json StartResearch(const nlohmann::json& arguments) const;
    nlohmann::json WaitForResearch(
        const nlohmann::json& arguments,
        std::stop_token stopToken) const;
    nlohmann::json GetNearbyEntities(const nlohmann::json& arguments) const;
    nlohmann::json GetConstructionRequests(const nlohmann::json& arguments) const;
    nlohmann::json ReturnToTaskIssuer(std::stop_token stopToken) const;
    nlohmann::json BuildGhosts(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json DeconstructMarked(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json FindResourcePatches(const nlohmann::json& arguments) const;
    nlohmann::json FindWater(const nlohmann::json& arguments) const;
    nlohmann::json Walk(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json WalkTo(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json FindConnectionPlacement(const nlohmann::json& arguments) const;
    nlohmann::json WalkToForPlacement(
        const nlohmann::json& arguments,
        std::stop_token stopToken) const;
    nlohmann::json StopWalking() const;
    nlohmann::json TakeScreenshot(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json MineEntity(const nlohmann::json& arguments, std::stop_token stopToken) const;
    nlohmann::json RunMiningAction(
        nlohmann::json runtimeArguments,
        double maximumSeconds,
        std::stop_token stopToken) const;
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
    std::optional<FactorioAgentTask> agentTask_;
    nlohmann::json definitions_;
};
}
