#include "UECommandCli/CommandCli.h"
#include "UECommandCli/CapabilityCatalog.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{

using json = nlohmann::json;

void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

json Descriptor()
{
    return {
        { "operation", "test.operation" },
        { "inputSchema", {
            { "type", "object" },
            { "required", json::array({ "assetPath", "enabled" }) },
            { "properties", {
                { "assetPath", { { "type", "string" } } },
                { "enabled", { { "type", "boolean" } } },
                { "count", { { "type", "integer" } } },
                { "ratio", { { "type", "number" } } },
                { "tags", {
                    { "type", "array" },
                    { "items", { { "type", "string" } } },
                } },
                { "settings", { { "type", "object" } } },
                { "apply", { { "const", true } } },
                { "confirmWrite", { { "type", "boolean" } } },
                { "requestId", { { "type", "string" } } },
            } },
        } },
    };
}

} // namespace

int main()
{
    const auto catalog_root =
        std::filesystem::temp_directory_path()
        / ("ue-command-cli-catalog-"
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count()));
    std::filesystem::create_directories(catalog_root);
    {
        std::ofstream stream(
            catalog_root / "test.json",
            std::ios::trunc);
        stream << json({
            { "schema", "ue.capability-manifest.v1" },
            { "domain", "test" },
            { "capabilities", json::array({
                {
                    { "id", "test.operation" },
                    { "domain", "test" },
                    { "kind", "query" },
                    { "description", "Catalog load test." },
                    { "inputSchema", {
                        { "type", "object" },
                        { "properties", json::object() },
                    } },
                    { "output", { { "kind", "json" } } },
                },
            }) },
        }).dump();
    }
    std::string catalog_error;
    const auto catalog =
        ue::command::CapabilityCatalog::Load(
            catalog_root,
            catalog_error);
    Require(catalog.has_value(), catalog_error);
    Require(catalog->Size() == 1, "catalog size must match manifests");
    Require(
        catalog->Find("test.operation") != nullptr,
        "catalog lookup failed");
    Require(
        catalog->Find("test.missing") == nullptr,
        "catalog lookup must not invent capabilities");
    const auto single_catalog =
        ue::command::CapabilityCatalog::LoadForCapability(
            catalog_root,
            "test.operation",
            catalog_error);
    Require(single_catalog.has_value(), catalog_error);
    Require(
        single_catalog->Size() == 1,
        "single-domain catalog load failed");
    std::error_code catalog_remove_error;
    std::filesystem::remove_all(
        catalog_root,
        catalog_remove_error);

    Require(
        ue::command::CamelToKebab("assetPath") == "asset-path",
        "camelCase must map to kebab-case");
    Require(
        ue::command::CamelToKebab("URLValue") == "u-r-l-value",
        "uppercase runs must remain deterministic");

    const auto converted = ue::command::ConvertParameters(
        Descriptor(),
        {
            { "asset-path", "/Game/UI/WBP_Login", false },
            { "enabled", "false", false },
            { "count", "42", false },
            { "ratio", "1.25", false },
            { "tags", "first", false },
            { "tags", "[\"second\",\"third\"]", false },
            { "settings", "{\"mode\":\"compact\"}", false },
            { "apply", std::nullopt, false },
        },
        true);
    Require(converted.ok, converted.message);
    Require(
        converted.params["assetPath"] == "/Game/UI/WBP_Login",
        "kebab alias must use the schema property name");
    Require(
        converted.params["enabled"] == false,
        "explicit false boolean must be preserved");
    Require(converted.params["count"] == 42, "integer conversion failed");
    Require(converted.params["ratio"] == 1.25, "number conversion failed");
    Require(
        converted.params["tags"]
            == json::array({ "first", "second", "third" }),
        "repeated and JSON array values must append");
    Require(
        converted.params["settings"]["mode"] == "compact",
        "object JSON conversion failed");
    Require(converted.params["apply"] == true, "const true flag failed");
    Require(
        converted.params["confirmWrite"] == true,
        "confirmWrite injection failed");
    Require(
        converted.schema_accepts_request_id,
        "requestId declaration must be detected");

    const auto negated = ue::command::ConvertParameters(
        Descriptor(),
        {
            { "assetPath", "/Game/A", false },
            { "enabled", std::nullopt, true },
        },
        false);
    Require(negated.ok, negated.message);
    Require(negated.params["enabled"] == false, "--no-enabled failed");

    const auto duplicate = ue::command::ConvertParameters(
        Descriptor(),
        {
            { "asset-path", "/Game/A", false },
            { "assetPath", "/Game/B", false },
            { "enabled", std::nullopt, false },
        },
        false);
    Require(
        !duplicate.ok && duplicate.code == "duplicate_parameter",
        "duplicate scalar flags must be rejected across aliases");

    const auto unknown = ue::command::ConvertParameters(
        Descriptor(),
        {
            { "asset-path", "/Game/A", false },
            { "enabled", std::nullopt, false },
            { "missing", "value", false },
        },
        false);
    Require(
        !unknown.ok && unknown.code == "unknown_parameter",
        "unknown flags must be rejected");

    const auto missing = ue::command::ConvertParameters(
        Descriptor(),
        { { "enabled", std::nullopt, false } },
        false);
    Require(
        !missing.ok && missing.code == "required_parameter_missing",
        "required fields must be enforced");

    const auto invalid_const = ue::command::ConvertParameters(
        Descriptor(),
        {
            { "asset-path", "/Game/A", false },
            { "enabled", std::nullopt, false },
            { "apply", "false", false },
        },
        false);
    Require(
        !invalid_const.ok && invalid_const.code == "invalid_parameter",
        "const true must reject false");

    const auto parameter_file =
        std::filesystem::temp_directory_path()
        / "ue-command-cli-settings.json";
    {
        std::ofstream stream(parameter_file, std::ios::trunc);
        stream << "{\"quality\":\"high\"}";
    }
    const auto from_file = ue::command::ConvertParameters(
        Descriptor(),
        {
            { "asset-path", "@@literal", false },
            { "enabled", std::nullopt, false },
            { "settings", "@" + parameter_file.string(), false },
        },
        false);
    std::error_code remove_error;
    std::filesystem::remove(parameter_file, remove_error);
    Require(from_file.ok, from_file.message);
    Require(
        from_file.params["assetPath"] == "@literal",
        "@@ must escape a literal leading @");
    Require(
        from_file.params["settings"]["quality"] == "high",
        "@file object conversion failed");

    const std::string summary = ue::command::FormatSuccessSummary(
        "test.operation",
        {
            { "ok", true },
            { "data", {
                { "assetPath", "/Game/A" },
                { "graphs", json::array({ 1, 2, 3 }) },
                { "contentBase64", std::string(2048, 'A') },
            } },
        });
    Require(summary.starts_with("OK test.operation "), "summary prefix failed");
    Require(summary.find("graphs=3") != std::string::npos, "array count missing");
    Require(summary.find("contentBase64") == std::string::npos, "Base64 leaked");
    Require(summary.size() <= 1024, "summary exceeds 1 KiB");

    return 0;
}
