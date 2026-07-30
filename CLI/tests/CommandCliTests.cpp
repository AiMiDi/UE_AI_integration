#include "UECommandCli/CommandCli.h"
#include "UECommandCli/CapabilityCatalog.h"
#include "UECommandCli/SkillCatalog.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
            catalog_root / "blueprint.json",
            std::ios::trunc);
        stream << json({
            { "schema", "ue.capability-manifest.v1" },
            { "domain", "blueprint" },
            { "capabilities", json::array({
                {
                    { "id", "blueprint.test_operation" },
                    { "domain", "blueprint" },
                    { "kind", "query" },
                    { "description", "Catalog load test." },
                    { "inputSchema", {
                        { "type", "object" },
                        { "properties", json::object() },
                    } },
                    { "traits", {
                        { "readOnly", true },
                        { "destructive", false },
                        { "expensive", false },
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
        catalog->Find("blueprint.test_operation") != nullptr,
        "catalog lookup failed");
    Require(
        catalog->Find("blueprint.test_missing") == nullptr,
        "catalog lookup must not invent capabilities");
    const auto single_catalog =
        ue::command::CapabilityCatalog::LoadForCapability(
            catalog_root,
            "blueprint.test_operation",
            catalog_error);
    Require(single_catalog.has_value(), catalog_error);
    Require(
        single_catalog->Size() == 1,
        "single-domain catalog load failed");

    const auto skill_root =
        catalog_root.parent_path()
        / (catalog_root.filename().string() + "-skills");
    const auto skill_directory = skill_root / "ue-test-diagnose";
    std::filesystem::create_directories(
        skill_directory / "references");
    {
        std::ofstream entrypoint(
            skill_directory / "SKILL.md",
            std::ios::trunc);
        entrypoint << "---\nname: ue-test-diagnose\n"
                      "description: Test skill.\n---\n";
        std::ofstream reference(
            skill_directory / "references" / "guide.md",
            std::ios::trunc);
        reference << "# Guide\n";
        std::ofstream manifest(
            skill_directory / "skill.json",
            std::ios::trunc);
        manifest << json({
            { "schema", "ue.agent-skill.v1" },
            { "schemaVersion", 1 },
            { "id", "ue-test-diagnose" },
            { "version", "1.0.0" },
            { "title", "UE Test Diagnose" },
            { "description", "Diagnose a test asset." },
            { "domains", json::array({ "blueprint" }) },
            { "risk", "readOnly" },
            { "triggers", json::array({
                "diagnose test",
                "inspect test",
            }) },
            { "entrypoint", "SKILL.md" },
            { "requirements", {
                { "capabilities",
                    json::array({ "blueprint.test_operation" }) },
                { "optionalCapabilities", json::array() },
            } },
            { "recipes", json::array({
                {
                    { "id", "diagnose" },
                    { "title", "Diagnose" },
                    { "description", "Inspect one test asset." },
                    { "risk", "readOnly" },
                    { "inputs", json::array({
                        {
                            { "name", "assetPath" },
                            { "type", "string" },
                            { "required", true },
                            { "description", "Test asset path." },
                        },
                    }) },
                    { "steps", json::array({
                        {
                            { "id", "inspect" },
                            { "phase", "discover" },
                            { "purpose", "Inspect the test asset." },
                            { "operations",
                                json::array({
                                    "blueprint.test_operation",
                                }) },
                        },
                        {
                            { "id", "execute" },
                            { "phase", "execute" },
                            { "purpose", "Execute the test inspection." },
                            { "operations",
                                json::array({
                                    "blueprint.test_operation",
                                }) },
                        },
                        {
                            { "id", "verify" },
                            { "phase", "verify" },
                            { "purpose", "Verify the test inspection." },
                            { "operations",
                                json::array({
                                    "blueprint.test_operation",
                                }) },
                        },
                    }) },
                    { "result", {
                        { "summary", "Report findings." },
                        { "evidence",
                            json::array({ "asset path" }) },
                        { "success",
                            json::array({ "inspection completed" }) },
                    } },
                },
            }) },
            { "resources", json::array({
                {
                    { "path", "references/guide.md" },
                    { "description", "Detailed test guidance." },
                },
            }) },
        }).dump(2);
    }
    std::string skill_error;
    const auto skills = ue::command::SkillCatalog::Load(
        skill_root,
        *catalog,
        skill_error);
    Require(skills.has_value(), skill_error);
    Require(skills->Size() == 1, "skill catalog size must match");
    Require(
        skills->Find("ue-test-diagnose") != nullptr,
        "skill catalog lookup failed");
    Require(
        skills->Find("ue-test-missing") == nullptr,
        "skill catalog must not invent skills");

    std::istringstream command_input;
    std::ostringstream command_output;
    std::ostringstream command_error;
    const int command_result = ue::command::Run(
        {
            "skills",
            "--skill-root",
            skill_root.string(),
            "--capability-root",
            catalog_root.string(),
            "--query",
            "diagnose",
            "--domain",
            "blueprint",
            "--operation",
            "blueprint.test_operation",
            "--detail",
            "summary",
            "--json",
        },
        catalog_root / "bin" / "ue",
        command_input,
        command_output,
        command_error);
    Require(
        command_result == 0,
        "ue skills command failed: " + command_error.str());
    const json command_envelope = json::parse(command_output.str());
    Require(
        command_envelope["data"]["total"] == 1,
        "ue skills filters must retain the matching skill");
    Require(
        command_envelope["data"]["source"] == "local",
        "ue skills must report a local source");
    Require(
        command_envelope["data"]["skills"][0]["recipes"][0]
                ["operations"][0]
            == "blueprint.test_operation",
        "skill summary must expose recipe operations");

    std::istringstream phrase_input;
    std::ostringstream phrase_output;
    std::ostringstream phrase_error;
    const int phrase_result = ue::command::Run(
        {
            "skills",
            "--skill-root",
            skill_root.string(),
            "--capability-root",
            catalog_root.string(),
            "--query",
            "unrelated words diagnose asset",
            "--json",
        },
        catalog_root / "bin" / "ue",
        phrase_input,
        phrase_output,
        phrase_error);
    Require(
        phrase_result == 0,
        "multi-token skill query failed: " + phrase_error.str());
    Require(
        json::parse(phrase_output.str())["data"]["total"] == 1,
        "multi-token skill query must match any meaningful token");

    std::istringstream help_input;
    std::ostringstream help_output;
    std::ostringstream help_error;
    const int help_result = ue::command::Run(
        { "skills", "--help" },
        catalog_root / "bin" / "ue",
        help_input,
        help_output,
        help_error);
    Require(help_result == 0, "ue skills --help failed");
    Require(
        help_output.str().find("--operation") != std::string::npos,
        "skill help must document discovery filters");

    std::istringstream operation_help_input;
    std::ostringstream operation_help_output;
    std::ostringstream operation_help_error;
    const int operation_help_result = ue::command::Run(
        {
            "help",
            "blueprint.test_operation",
            "--capability-root",
            catalog_root.string(),
            "--skill-root",
            (skill_root / "does-not-exist").string(),
            "--json",
        },
        catalog_root / "bin" / "ue",
        operation_help_input,
        operation_help_output,
        operation_help_error);
    Require(
        operation_help_result == 0,
        "ordinary operation help must not load Agent Skills: "
            + operation_help_error.str());
    Require(
        json::parse(operation_help_output.str())["data"]["id"]
            == "blueprint.test_operation",
        "ordinary operation help returned the wrong descriptor");

    std::istringstream full_input;
    std::ostringstream full_output;
    std::ostringstream full_error;
    const int full_result = ue::command::Run(
        {
            "skills",
            "--skill-root",
            skill_root.string(),
            "--capability-root",
            catalog_root.string(),
            "--name",
            "ue-test-diagnose",
            "--recipe",
            "diagnose",
            "--risk",
            "readOnly",
            "--detail",
            "full",
            "--offset",
            "0",
            "--limit",
            "1",
            "--json",
        },
        catalog_root / "bin" / "ue",
        full_input,
        full_output,
        full_error);
    Require(
        full_result == 0,
        "full skill query failed: " + full_error.str());
    const json full_envelope = json::parse(full_output.str());
    Require(
        full_envelope["data"]["skills"][0]["schema"]
            == "ue.agent-skill.v1",
        "full skill detail must preserve the machine contract");

    std::istringstream shell_input(
        "skills --name ue-test-diagnose --json\nexit\n");
    std::ostringstream shell_output;
    std::ostringstream shell_error;
    const int shell_result = ue::command::Run(
        {
            "shell",
            "--skill-root",
            skill_root.string(),
            "--capability-root",
            catalog_root.string(),
            "--live-schema",
            "--json",
        },
        catalog_root / "bin" / "ue",
        shell_input,
        shell_output,
        shell_error);
    Require(
        shell_result == 0,
        "shell skill query failed: " + shell_error.str());
    const json shell_envelope = json::parse(shell_output.str());
    Require(
        shell_envelope["data"]["total"] == 1,
        "live-schema shell must reuse the local Agent Skill catalog");

    const json valid_skill = *skills->Find("ue-test-diagnose");
    {
        std::ofstream manifest(
            skill_directory / "skill.json",
            std::ios::trunc);
        json mismatched = valid_skill;
        mismatched["risk"] = "safeWrite";
        manifest << mismatched.dump(2);
    }
    const auto mismatched_risk_skills =
        ue::command::SkillCatalog::Load(
            skill_root,
            *catalog,
            skill_error);
    Require(
        !mismatched_risk_skills
            && skill_error.find("declared recipe risks")
                != std::string::npos,
        "skill risk must derive from its recipe risks");

    {
        std::ofstream manifest(
            skill_directory / "skill.json",
            std::ios::trunc);
        json invalid_route = valid_skill;
        invalid_route["recipes"][0]["steps"][1]["route"] =
            "workflow";
        manifest << invalid_route.dump(2);
    }
    const auto invalid_route_skills =
        ue::command::SkillCatalog::Load(
            skill_root,
            *catalog,
            skill_error);
    Require(
        !invalid_route_skills
            && skill_error.find("requires editStep admission")
                != std::string::npos,
        "workflow recipe routes must require editStep admission");

    {
        std::ofstream manifest(
            skill_directory / "skill.json",
            std::ios::trunc);
        json escaping = valid_skill;
        escaping["resources"][0]["path"] = "../outside.md";
        manifest << escaping.dump(2);
    }
    const auto escaping_skills = ue::command::SkillCatalog::Load(
        skill_root,
        *catalog,
        skill_error);
    Require(
        !escaping_skills
            && skill_error.find("stay inside") != std::string::npos,
        "skill resources must not escape their package directory");

    {
        std::ofstream manifest(
            skill_directory / "skill.json",
            std::ios::trunc);
        json invalid = valid_skill;
        invalid["requirements"]["capabilities"] =
            json::array({ "blueprint.test_missing" });
        invalid["recipes"][0]["steps"][0]["operations"] =
            json::array({ "blueprint.test_missing" });
        manifest << invalid.dump(2);
    }
    const auto invalid_skills = ue::command::SkillCatalog::Load(
        skill_root,
        *catalog,
        skill_error);
    Require(
        !invalid_skills
            && skill_error.find("missing required capability")
                != std::string::npos,
        "missing required skill operations must fail catalog load");

    std::error_code catalog_remove_error;
    std::filesystem::remove_all(
        catalog_root,
        catalog_remove_error);
    std::filesystem::remove_all(
        skill_root,
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
