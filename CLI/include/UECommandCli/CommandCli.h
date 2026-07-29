#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ue::command
{

struct RawOption
{
    std::string name;
    std::optional<std::string> value;
    bool negated = false;
};

struct ConversionResult
{
    bool ok = false;
    nlohmann::json params = nlohmann::json::object();
    std::string code;
    std::string message;
    bool schema_accepts_request_id = false;
};

std::string CamelToKebab(std::string_view value);

ConversionResult ConvertParameters(
    const nlohmann::json& capability_descriptor,
    const std::vector<RawOption>& options,
    bool confirm_write);

std::string FormatSuccessSummary(
    std::string_view capability,
    const nlohmann::json& envelope,
    std::size_t max_bytes = 1024);

int Run(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& executable,
    std::istream& input,
    std::ostream& output,
    std::ostream& error);

int Run(
    const std::vector<std::string>& arguments,
    std::ostream& output,
    std::ostream& error);

} // namespace ue::command
