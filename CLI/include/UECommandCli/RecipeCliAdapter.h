#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace ue::command
{

struct LocalCapabilityCliResult
{
    int exit_code = 5;
    std::string envelope;
    std::string error;
};

int RunRecipeCliAdapter(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& executable,
    std::ostream& error);

int RunSalCliAdapter(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& executable,
    std::ostream& error);

LocalCapabilityCliResult RunLocalCapabilityCliAdapter(
    const std::string& capability,
    const std::string& backend,
    const std::string& params_json,
    const std::string& request_id,
    const std::filesystem::path& executable);

} // namespace ue::command
