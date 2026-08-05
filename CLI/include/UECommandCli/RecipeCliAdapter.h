#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace ue::command
{

int RunRecipeCliAdapter(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& executable,
    std::ostream& error);

int RunSalCliAdapter(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& executable,
    std::ostream& error);

} // namespace ue::command
