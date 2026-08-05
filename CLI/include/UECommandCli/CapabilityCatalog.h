#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ue::command
{

class CapabilityCatalog
{
public:
    static std::optional<CapabilityCatalog> Load(
        const std::filesystem::path& root,
        std::string& error);
    static std::optional<CapabilityCatalog> LoadForCapability(
        const std::filesystem::path& root,
        const std::string& capability_id,
        std::string& error);

    const nlohmann::json* Find(const std::string& id) const;
    const nlohmann::json* FindTombstone(const std::string& id) const;
    const std::map<std::string, nlohmann::json>& Descriptors() const;
    const std::filesystem::path& Root() const;
    std::size_t Size() const;

private:
    static std::optional<CapabilityCatalog> LoadFiles(
        const std::filesystem::path& root,
        const std::vector<std::filesystem::path>& manifests,
        std::string& error);

    std::filesystem::path root_;
    std::map<std::string, nlohmann::json> descriptors_;
    std::map<std::string, nlohmann::json> tombstones_;
};

std::optional<std::filesystem::path> ResolveCapabilityRoot(
    const std::filesystem::path& executable,
    const std::filesystem::path& explicit_root,
    std::vector<std::filesystem::path>* checked,
    std::string& error);

} // namespace ue::command
