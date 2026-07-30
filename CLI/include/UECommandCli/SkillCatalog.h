#pragma once

#include "UECommandCli/CapabilityCatalog.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ue::command
{

class SkillCatalog
{
public:
    static std::optional<SkillCatalog> Load(
        const std::filesystem::path& root,
        const CapabilityCatalog& capabilities,
        std::string& error);

    const nlohmann::json* Find(const std::string& id) const;
    const std::map<std::string, nlohmann::json>& Descriptors() const;
    const std::filesystem::path& Root() const;
    std::size_t Size() const;

private:
    std::filesystem::path root_;
    std::map<std::string, nlohmann::json> descriptors_;
};

std::optional<std::filesystem::path> ResolveSkillRoot(
    const std::filesystem::path& executable,
    const std::filesystem::path& explicit_root,
    std::vector<std::filesystem::path>* checked,
    std::string& error);

} // namespace ue::command
