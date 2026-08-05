#include "UECommandCli/RecipeCliAdapter.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#ifndef UE_CLI_SOURCE_ROOT
#define UE_CLI_SOURCE_ROOT ""
#endif

namespace ue::command
{
namespace
{

std::optional<std::string> Environment(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0
        || !value
        || length <= 1)
    {
        std::free(value);
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value && *value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::filesystem::path LocateRecipeCli(
    const std::filesystem::path& executable)
{
    std::vector<std::filesystem::path> candidates;
    if (const auto configured = Environment("UE_RECIPE_CLI"))
    {
        candidates.emplace_back(*configured);
    }
    std::error_code error;
    const auto absolute_executable =
        std::filesystem::absolute(executable, error);
    if (!error && !absolute_executable.empty())
    {
        const auto bin = absolute_executable.parent_path();
        candidates.push_back(
            bin.parent_path().parent_path()
                / "MCP" / "dist" / "recipe-cli.js");
    }
    if (std::string(UE_CLI_SOURCE_ROOT).size() > 0)
    {
        candidates.push_back(
            std::filesystem::path(UE_CLI_SOURCE_ROOT)
                / "MCP" / "dist" / "recipe-cli.js");
    }
    candidates.push_back(
        std::filesystem::current_path()
            / "MCP" / "dist" / "recipe-cli.js");
    for (auto candidate : candidates)
    {
        candidate = std::filesystem::absolute(candidate, error)
            .lexically_normal();
        if (!error && std::filesystem::is_regular_file(candidate, error))
        {
            return candidate;
        }
        error.clear();
    }
    return {};
}

std::filesystem::path LocateSalCli(
    const std::filesystem::path& executable)
{
    std::vector<std::filesystem::path> candidates;
    if (const auto configured = Environment("UE_SAL_CLI"))
    {
        candidates.emplace_back(*configured);
    }
    std::error_code error;
    const auto absolute_executable = std::filesystem::absolute(executable, error);
    if (!error && !absolute_executable.empty())
    {
        const auto bin = absolute_executable.parent_path();
        candidates.push_back(bin.parent_path().parent_path() / "MCP" / "dist" / "sal-cli.js");
    }
    if (std::string(UE_CLI_SOURCE_ROOT).size() > 0)
    {
        candidates.push_back(std::filesystem::path(UE_CLI_SOURCE_ROOT) / "MCP" / "dist" / "sal-cli.js");
    }
    candidates.push_back(std::filesystem::current_path() / "MCP" / "dist" / "sal-cli.js");
    for (auto candidate : candidates)
    {
        candidate = std::filesystem::absolute(candidate, error).lexically_normal();
        if (!error && std::filesystem::is_regular_file(candidate, error)) return candidate;
        error.clear();
    }
    return {};
}

std::string Quote(const std::filesystem::path& path)
{
    std::string escaped = "\"";
    for (const char character : path.string())
    {
        escaped += character == '"' ? "\\\"" : std::string(1, character);
    }
    return escaped + "\"";
}

} // namespace

int RunRecipeCliAdapter(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& executable,
    std::ostream& error)
{
    const auto script = LocateRecipeCli(executable);
    if (script.empty())
    {
        error
            << "Recipe Runner is unavailable. The packaged MCP/dist/recipe-cli.js "
               "file is missing.\n";
        return 4;
    }
    const auto stamp = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    std::mt19937_64 random(
        static_cast<std::mt19937_64::result_type>(stamp));
    const auto args_file = std::filesystem::temp_directory_path()
        / ("ue-recipe-args-" + std::to_string(stamp) + "-"
            + std::to_string(random()) + ".json");
    {
        std::ofstream stream(
            args_file,
            std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            error << "Recipe Runner could not create its bounded args file.\n";
            return 5;
        }
        stream << nlohmann::json(arguments).dump() << '\n';
    }
    const std::string command =
        "node " + Quote(script)
        + " --args-file " + Quote(args_file);
    const int raw_exit = std::system(command.c_str());
    std::error_code ignored;
    std::filesystem::remove(args_file, ignored);
    if (raw_exit == -1)
    {
        error << "Recipe Runner could not launch Node.js.\n";
        return 4;
    }
#if defined(_WIN32)
    return raw_exit;
#else
    return WIFEXITED(raw_exit) ? WEXITSTATUS(raw_exit) : 5;
#endif
}

int RunSalCliAdapter(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& executable,
    std::ostream& error)
{
    const auto script = LocateSalCli(executable);
    if (script.empty())
    {
        error << "SAL is unavailable. The packaged MCP/dist/sal-cli.js file is missing.\n";
        return 4;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto args_file = std::filesystem::temp_directory_path()
        / ("ue-sal-args-" + std::to_string(stamp) + ".json");
    {
        std::ofstream stream(args_file, std::ios::binary | std::ios::trunc);
        if (!stream) return 5;
        stream << nlohmann::json(arguments).dump() << '\n';
    }
    const int raw_exit = std::system(
        ("node " + Quote(script) + " --args-file " + Quote(args_file)).c_str());
    std::error_code ignored;
    std::filesystem::remove(args_file, ignored);
    if (raw_exit == -1) return 4;
#if defined(_WIN32)
    return raw_exit;
#else
    return WIFEXITED(raw_exit) ? WEXITSTATUS(raw_exit) : 5;
#endif
}

} // namespace ue::command
