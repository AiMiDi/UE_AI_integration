#include "UEApiClient/UEApiClient.h"
#include "UECliPlatform/Utf8Console.h"
#include "UEWorkflowCore/WorkflowCore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#ifndef UE_WORKFLOW_VERSION
#define UE_WORKFLOW_VERSION "0.1.0"
#endif

#ifndef UE_WORKFLOW_SOURCE_ROOT
#define UE_WORKFLOW_SOURCE_ROOT ""
#endif

namespace
{

using json = nlohmann::json;

constexpr int kExitUsage = 1;
constexpr int kExitValidation = 2;
constexpr int kExitApproval = 3;
constexpr int kExitUnavailable = 4;
constexpr int kExitExecution = 5;

struct CliOptions
{
    bool json_output = false;
    bool connect = false;
    bool save_on_success = false;
    bool confirm_write = false;
    bool details_alias = false;
    bool detail_level_explicit = false;
    bool endpoint_explicit = false;
    bool help_requested = false;
    std::filesystem::path contract_root;
    std::vector<std::filesystem::path> capability_roots;
    std::filesystem::path file;
    std::filesystem::path receipt;
    std::string endpoint = "http://127.0.0.1:9847";
    std::string approve_plan;
    std::string detail_level;
    std::string capability_query;
    std::string capability_operation;
    std::string capability_domain;
    std::string capability_kind;
    std::string capability_output_kind;
    std::string capability_risk;
    std::string capability_detail = "summary";
    std::optional<bool> capability_read_only;
    std::optional<bool> capability_destructive;
    std::optional<bool> capability_expensive;
    bool capability_available_only = false;
    std::size_t capability_offset = 0;
    std::size_t capability_limit = 25;
    std::vector<std::string> sections;
    std::vector<std::string> positional;
};

std::optional<std::string> environment(std::string_view name)
{
    std::string key(name);
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, key.c_str()) != 0 || !value || length <= 1)
    {
        std::free(value);
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(key.c_str());
    return value && *value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

bool is_tty()
{
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

ue::cli::Utf8TextResult read_text(
    const std::filesystem::path& path)
{
    if (path == "-")
    {
        return ue::cli::ReadTextToUtf8(std::cin);
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return {
            false,
            {},
            "file_unreadable",
            "The file could not be opened.",
        };
    }
    return ue::cli::ReadTextToUtf8(stream);
}

bool write_text(const std::filesystem::path& path, std::string_view text)
{
    std::error_code error;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            return false;
        }
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream)
        {
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (!error)
    {
        return true;
    }
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    return !error;
}

void print_json_text(std::string_view text, bool compact)
{
    if (compact)
    {
        std::cout << text << "\n";
        return;
    }
    auto parsed = json::parse(text, nullptr, false, true);
    std::cout << (parsed.is_discarded() ? std::string(text) : parsed.dump(2)) << "\n";
}

void apply_response_options(json& request, const CliOptions& options)
{
    if (!options.detail_level.empty())
    {
        request["detailLevel"] = options.detail_level;
    }
    if (!options.sections.empty())
    {
        request["sections"] = options.sections;
    }
}

std::string project_planning_response(std::string_view response_text, const CliOptions& options)
{
    auto response = json::parse(response_text, nullptr, false, true);
    if (!response.is_object())
    {
        return std::string(response_text);
    }

    const std::string detail_level =
        options.detail_level.empty() ? "standard" : options.detail_level;
    if (detail_level != "summary")
    {
        response["detailLevel"] = detail_level;
        if (options.details_alias)
        {
            response["deprecations"] = json::array({
                "'--details' is deprecated; use '--detail-level full' instead.",
            });
        }
        return response.dump();
    }

    static const std::array<std::string_view, 13> summary_fields = {
        "ok",             "schema",          "plannerVersion",
        "contractSetDigest", "planDigest",    "corePlanDigest",
        "executionReady", "preconditions",   "contractSet",
        "validationScope", "risk",            "approval",
        "valid",
    };
    json projected = json::object();
    for (const auto field : summary_fields)
    {
        if (response.contains(field))
        {
            projected[std::string(field)] = response[std::string(field)];
        }
    }
    projected["detailLevel"] = "summary";
    projected["summary"] = json::object();
    for (const auto* field : {"initializers", "operations", "finalizers", "diagnostics"})
    {
        if (response.contains(field) && response[field].is_array())
        {
            projected["summary"][std::string(field) + "Count"] = response[field].size();
        }
    }
    json requested_sections = json::object();
    for (const auto& section : options.sections)
    {
        if ((section == "operations" || section == "finalizers" || section == "diagnostics") &&
            response.contains(section))
        {
            requested_sections[section] = response[section];
        }
    }
    if (!requested_sections.empty())
    {
        projected["sections"] = std::move(requested_sections);
    }
    return projected.dump();
}

std::string mark_offline_plan_unprepared(std::string_view plan_text)
{
    auto plan = json::parse(plan_text, nullptr, false, true);
    if (!plan.is_object() || !plan.value("ok", false))
    {
        return std::string(plan_text);
    }
    const std::string core_digest =
        plan.value("planDigest", std::string{});
    plan["corePlanDigest"] = core_digest;
    plan["executionReady"] = false;
    plan["preconditions"] = {
        { "schema", "ue.workflow-asset-preconditions.v1" },
        { "prepared", false },
        { "assets", json::array() },
        { "digest", nullptr },
    };
    return plan.dump();
}

int print_error(
    int exit_code,
    std::string code,
    std::string message,
    std::string path = {})
{
    const json payload = {
        { "ok", false },
        { "schema", "ue.workflow-cli-error.v1" },
        { "diagnostics", json::array({
            {
                { "severity", "error" },
                { "phase", "cli" },
                { "code", std::move(code) },
                { "path", std::move(path) },
                { "message", std::move(message) },
            },
        }) },
    };
    std::cout << payload.dump() << "\n";
    return exit_code;
}

json binary_help()
{
    return {
        {"ok", true},
        {"schema", "ue.workflow-cli-help.v1"},
        {"product", "UE Workflow DSL"},
        {"executable", "ue-workflow"},
        {"version", UE_WORKFLOW_VERSION},
        {"dsl", "ue.workflow"},
        {"commands", json::array({
                         "doctor [--connect]",
                         "capabilities [--connect] [--query <text>] [--operation <id>] "
                         "[--domain <domain>] [--kind <kind>] [--read-only <bool>] "
                         "[--destructive <bool>] [--expensive <bool>] "
                         "[--output-kind json|image] "
                         "[--risk readOnly|safeWrite|confirmWrite|notOpen] "
                         "[--available-only] [--offset <n>] [--limit <1..100>] "
                         "[--detail summary|full]",
                         "help composable [blueprint|widget|material]",
                         "help operation <type>",
                         "validate --file <workflow.json|->",
                         "plan --file <workflow.json|-> [--connect]",
                         "execute --file <workflow.json|-> --approve-plan <digest> --receipt "
                         "<path> [--save-on-success] [--confirm-write]",
                         "status|resume|rollback --receipt <path> [--detail-level <level>] "
                         "[--section <name>]",
                         "shell",
                     })},
        {"globalOptions", json::array({
                              "--json",
                              "--contract-root <path>",
                              "--capability-root <path>",
                              "--endpoint http://127.0.0.1:<port>",
                              "--detail-level summary|standard|full",
                              "--section "
                              "operations|finalizers|readBack|assetDiff|structures|rollback|"
                              "diagnostics",
                              "--details (deprecated alias for --detail-level full)",
                          })},
    };
}

json command_help(const CliOptions& options)
{
    if (options.positional.empty())
    {
        return binary_help();
    }

    const std::string& command = options.positional.front();
    std::string usage;
    std::string description;
    json command_options = json::array();
    if (command == "doctor")
    {
        usage = "ue-workflow doctor [--connect]";
        description =
            "Validate local Workflow contracts and optionally compare both "
            "DSL 1.0 and 2.0 digests with the running Editor.";
        command_options = json::array({ "--connect", "--endpoint <loopback-url>" });
    }
    else if (command == "capabilities")
    {
        usage = "ue-workflow capabilities [filters] [--connect]";
        description =
            "Search and page the Workflow capability catalog.";
        command_options = json::array({
            "--connect",
            "--query <text>",
            "--operation <id>",
            "--domain <domain>",
            "--kind <kind>",
            "--read-only <bool>",
            "--destructive <bool>",
            "--expensive <bool>",
            "--output-kind json|image",
            "--risk readOnly|safeWrite|confirmWrite|notOpen",
            "--available-only",
            "--offset <n>",
            "--limit <1..100>",
            "--detail summary|full",
        });
    }
    else if (command == "validate")
    {
        usage = "ue-workflow validate --file <workflow.json|->";
        description = "Validate a Workflow without contacting the Editor.";
        command_options = json::array({ "--file <workflow.json|->" });
    }
    else if (command == "plan")
    {
        usage =
            "ue-workflow plan --file <workflow.json|-> [--connect]";
        description =
            "Create an offline plan or bind it to live Editor asset "
            "preconditions.";
        command_options = json::array({
            "--file <workflow.json|->",
            "--connect",
            "--detail-level summary|standard|full",
            "--section <name>",
        });
    }
    else if (command == "execute")
    {
        usage =
            "ue-workflow execute --file <workflow.json|-> "
            "--approve-plan <digest> --receipt <path>";
        description =
            "Execute an approved, Editor-bound Workflow plan.";
        command_options = json::array({
            "--file <workflow.json|->",
            "--approve-plan <digest>",
            "--receipt <path>",
            "--save-on-success",
            "--confirm-write",
            "--detail-level summary|standard|full",
            "--section <name>",
        });
    }
    else if (
        command == "status"
        || command == "resume"
        || command == "rollback")
    {
        usage = "ue-workflow " + command + " --receipt <path>";
        description =
            command == "status"
            ? "Read a Workflow run by its compact receipt."
            : command == "resume"
            ? "Resume a durable Workflow run from its compact receipt."
            : "Rollback a Workflow run using its approved receipt.";
        command_options = json::array({
            "--receipt <path>",
            "--detail-level summary|standard|full",
            "--section <name>",
        });
    }
    else if (command == "shell")
    {
        usage = "ue-workflow shell";
        description = "Start the interactive Workflow CLI shell.";
    }
    else if (command == "help")
    {
        usage =
            "ue-workflow help composable [scope] | "
            "ue-workflow help operation <type>";
        description =
            "Inspect composable Workflow operations after loading contracts.";
    }
    else if (command == "operation")
    {
        usage = "ue <capability-id> [--field value ...]";
        description =
            "Single capability execution moved to the short `ue` CLI. "
            "`ue-workflow operation run` is not an execution command.";
        command_options = json::array({
            "--params <json-object>",
            "--params-file <path|->",
        });
    }
    else
    {
        usage = "ue-workflow <command> --help";
        description = "Unknown ue-workflow command.";
    }

    return {
        { "ok", true },
        { "schema", "ue.workflow-cli-help.v1" },
        { "product", "UE Workflow DSL" },
        { "executable", "ue-workflow" },
        { "version", UE_WORKFLOW_VERSION },
        { "command", command },
        { "usage", std::move(usage) },
        { "description", std::move(description) },
        { "options", std::move(command_options) },
    };
}

std::optional<std::string> positional_error(const CliOptions& options)
{
    if (options.positional.empty())
    {
        return std::nullopt;
    }
    const std::string& command = options.positional.front();
    const std::size_t count = options.positional.size();
    if (command == "--version" || command == "version")
    {
        return count == 1
            ? std::nullopt
            : std::optional<std::string>(
                "version does not accept positional arguments.");
    }
    if (
        command == "doctor"
        || command == "capabilities"
        || command == "validate"
        || command == "plan"
        || command == "execute"
        || command == "status"
        || command == "resume"
        || command == "rollback"
        || command == "shell")
    {
        return count == 1
            ? std::nullopt
            : std::optional<std::string>(
                command + " does not accept positional arguments.");
    }
    if (command == "help")
    {
        const bool valid =
            count == 1
            || (count == 2 && options.positional[1] == "composable")
            || (count == 3
                && (options.positional[1] == "composable"
                    || options.positional[1] == "operation"));
        return valid
            ? std::nullopt
            : std::optional<std::string>(
                "help accepts `composable [scope]` or `operation <type>`.");
    }
    return "Unknown command '" + command + "'.";
}

bool parse_arguments(const std::vector<std::string>& arguments, CliOptions& options)
{
    auto take_value = [&](std::size_t& index, std::string& destination) {
        if (index + 1 >= arguments.size())
        {
            return false;
        }
        destination = arguments[++index];
        return true;
    };
    auto take_bool = [&](std::size_t& index, std::optional<bool>& destination) {
        std::string value;
        if (!take_value(index, value))
        {
            return false;
        }
        if (value == "true" || value == "1")
        {
            destination = true;
            return true;
        }
        if (value == "false" || value == "0")
        {
            destination = false;
            return true;
        }
        return false;
    };
    auto take_size = [&](std::size_t& index, std::size_t& destination) {
        std::string value;
        if (!take_value(index, value) || value.empty())
        {
            return false;
        }
        std::size_t parsed = 0;
        const auto result =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
        {
            return false;
        }
        destination = parsed;
        return true;
    };

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const auto& argument = arguments[index];
        if (argument == "--json")
        {
            options.json_output = true;
        }
        else if (argument == "--connect")
        {
            options.connect = true;
        }
        else if (argument == "--save-on-success")
        {
            options.save_on_success = true;
        }
        else if (argument == "--confirm-write")
        {
            options.confirm_write = true;
        }
        else if (argument == "--details")
        {
            if (options.detail_level_explicit)
            {
                return false;
            }
            options.details_alias = true;
            options.detail_level = "full";
        }
        else if (argument == "--detail-level")
        {
            std::string value;
            if (options.details_alias || options.detail_level_explicit ||
                !take_value(index, value) ||
                (value != "summary" && value != "standard" && value != "full"))
            {
                return false;
            }
            options.detail_level_explicit = true;
            options.detail_level = std::move(value);
        }
        else if (argument == "--section")
        {
            std::string value;
            if (!take_value(index, value))
            {
                return false;
            }
            static const std::array<std::string_view, 7> supported_sections = {
                "operations", "finalizers", "readBack",    "assetDiff",
                "structures", "rollback",   "diagnostics",
            };
            if (std::find(supported_sections.begin(), supported_sections.end(), value) ==
                supported_sections.end())
            {
                return false;
            }
            if (std::find(options.sections.begin(), options.sections.end(), value) ==
                options.sections.end())
            {
                options.sections.push_back(std::move(value));
            }
        }
        else if (argument == "--contract-root")
        {
            std::string value;
            if (!take_value(index, value))
            {
                return false;
            }
            options.contract_root = ue::cli::Utf8Path(value);
        }
        else if (argument == "--capability-root")
        {
            std::string value;
            if (!take_value(index, value))
            {
                return false;
            }
            options.capability_roots.push_back(
                ue::cli::Utf8Path(value));
        }
        else if (argument == "--file")
        {
            std::string value;
            if (!take_value(index, value))
            {
                return false;
            }
            options.file = ue::cli::Utf8Path(value);
        }
        else if (argument == "--receipt")
        {
            std::string value;
            if (!take_value(index, value))
            {
                return false;
            }
            options.receipt = ue::cli::Utf8Path(value);
        }
        else if (argument == "--endpoint")
        {
            if (!take_value(index, options.endpoint))
            {
                return false;
            }
            options.endpoint_explicit = true;
        }
        else if (argument == "--approve-plan")
        {
            if (!take_value(index, options.approve_plan))
            {
                return false;
            }
        }
        else if (argument == "--query")
        {
            if (!take_value(index, options.capability_query))
            {
                return false;
            }
        }
        else if (argument == "--operation")
        {
            if (!take_value(index, options.capability_operation))
            {
                return false;
            }
        }
        else if (argument == "--domain")
        {
            if (!take_value(index, options.capability_domain))
            {
                return false;
            }
        }
        else if (argument == "--kind")
        {
            if (!take_value(index, options.capability_kind))
            {
                return false;
            }
        }
        else if (argument == "--output-kind")
        {
            if (!take_value(index, options.capability_output_kind)
                || (options.capability_output_kind != "json"
                    && options.capability_output_kind != "image"))
            {
                return false;
            }
        }
        else if (argument == "--risk")
        {
            if (!take_value(index, options.capability_risk)
                || (options.capability_risk != "readOnly"
                    && options.capability_risk != "safeWrite"
                    && options.capability_risk != "confirmWrite"
                    && options.capability_risk != "notOpen"))
            {
                return false;
            }
        }
        else if (argument == "--available-only")
        {
            options.capability_available_only = true;
        }
        else if (argument == "--read-only")
        {
            if (!take_bool(index, options.capability_read_only))
            {
                return false;
            }
        }
        else if (argument == "--destructive")
        {
            if (!take_bool(index, options.capability_destructive))
            {
                return false;
            }
        }
        else if (argument == "--expensive")
        {
            if (!take_bool(index, options.capability_expensive))
            {
                return false;
            }
        }
        else if (argument == "--offset")
        {
            if (!take_size(index, options.capability_offset))
            {
                return false;
            }
        }
        else if (argument == "--limit")
        {
            if (!take_size(index, options.capability_limit)
                || options.capability_limit == 0
                || options.capability_limit > 100)
            {
                return false;
            }
        }
        else if (argument == "--detail")
        {
            if (!take_value(index, options.capability_detail)
                || (options.capability_detail != "summary"
                    && options.capability_detail != "full"))
            {
                return false;
            }
        }
        else if (argument == "--help")
        {
            options.help_requested = true;
        }
        else if (argument == "--version")
        {
            options.positional.push_back(argument);
        }
        else if (argument.starts_with("--"))
        {
            return false;
        }
        else
        {
            options.positional.push_back(argument);
        }
    }
    return true;
}

void resolve_contract_paths(
    CliOptions& options,
    const std::filesystem::path& executable)
{
    if (options.contract_root.empty())
    {
        if (const auto configured = environment("UE_WORKFLOW_CONTRACT_ROOT"))
        {
            options.contract_root = *configured;
        }
        else
        {
            const std::vector<std::filesystem::path> candidates = {
                executable.parent_path() / "Resources" / "Workflow",
                executable.parent_path() / "Contracts",
                executable.parent_path() / ".." / "share" / "ue-workflow" / "Contracts",
                executable.parent_path() / ".." / ".." / "Workflow" / "Contracts",
                std::filesystem::path(UE_WORKFLOW_SOURCE_ROOT) / "Workflow" / "Contracts",
            };
            for (const auto& candidate : candidates)
            {
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate / "contract-set.v1.json", error))
                {
                    options.contract_root = candidate;
                    break;
                }
            }
        }
    }

    if (options.capability_roots.empty())
    {
        if (const auto configured = environment("UE_WORKFLOW_CAPABILITY_ROOT"))
        {
            options.capability_roots.emplace_back(*configured);
        }
        else
        {
            const std::vector<std::filesystem::path> candidates = {
                executable.parent_path() / "Resources" / "Capabilities",
                executable.parent_path() / "Capabilities",
                executable.parent_path() / ".." / "share" / "ue-workflow" / "Capabilities",
                executable.parent_path() / ".." / ".." / "Resources" / "Capabilities",
                std::filesystem::path(UE_WORKFLOW_SOURCE_ROOT) / "Resources" / "Capabilities",
            };
            for (const auto& candidate : candidates)
            {
                std::error_code error;
                if (std::filesystem::is_directory(candidate, error))
                {
                    options.capability_roots.push_back(candidate);
                    break;
                }
            }
        }
    }

    if (const auto port = environment("UE_PORT");
        port && !options.endpoint_explicit)
    {
        options.endpoint = "http://127.0.0.1:" + *port;
    }
}

std::string capability_query_path(const CliOptions& options)
{
    std::vector<std::pair<std::string, std::string>> parameters;
    auto add = [&parameters](std::string name, const std::string& value) {
        if (!value.empty())
        {
            parameters.emplace_back(std::move(name), value);
        }
    };
    add("query", options.capability_query);
    add("operation", options.capability_operation);
    add("domain", options.capability_domain);
    add("kind", options.capability_kind);
    add("outputKind", options.capability_output_kind);
    add("risk", options.capability_risk);
    add("detail", options.capability_detail);
    auto add_bool = [&parameters](
        std::string name,
        const std::optional<bool>& value) {
        if (value.has_value())
        {
            parameters.emplace_back(
                std::move(name),
                *value ? "true" : "false");
        }
    };
    add_bool("readOnly", options.capability_read_only);
    add_bool("destructive", options.capability_destructive);
    add_bool("expensive", options.capability_expensive);
    if (options.capability_available_only)
    {
        parameters.emplace_back("availableOnly", "true");
    }
    parameters.emplace_back("offset", std::to_string(options.capability_offset));
    parameters.emplace_back("limit", std::to_string(options.capability_limit));

    std::string result = "/api/capabilities";
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        result.push_back(index == 0 ? '?' : '&');
        result += ue::api::UrlEncode(parameters[index].first);
        result.push_back('=');
        result += ue::api::UrlEncode(parameters[index].second);
    }
    return result;
}

using HttpResult = ue::api::HttpResult;

void ConfigureWorkflowCaller(
    ue::api::Client& client,
    const std::string& command)
{
    const std::string invocation_id = ue::api::NewInvocationId();
    client.ConfigureBestEffortCliSession({
        .name = "ue-workflow",
        .version = UE_WORKFLOW_VERSION,
        .command = command,
        .invocation_id = invocation_id,
        .instance_id = invocation_id,
        .process_id = ue::api::CurrentProcessId(),
    });
}

HttpResult http_request(
    ue::api::Client& client,
    std::string_view method,
    std::string_view path,
    std::string_view body = {})
{
    return method == "POST"
        ? client.Post(path, body)
        : client.Get(path);
}

int print_http_result(const HttpResult& response, bool compact)
{
    if (!response.error.empty())
    {
        return print_error(kExitUnavailable, "editor_unreachable", response.error);
    }
    const auto payload = json::parse(response.body, nullptr, false, true);
    if (payload.is_discarded())
    {
        return print_error(
            kExitExecution,
            "invalid_editor_response",
            "Unreal Editor returned non-JSON data.");
    }
    print_json_text(payload.dump(), compact);
    return response.ok ? 0 : kExitExecution;
}

int print_capabilities_http_result(
    const HttpResult& response,
    bool compact)
{
    if (!response.error.empty())
    {
        return print_error(
            kExitUnavailable,
            "editor_unreachable",
            response.error);
    }
    const auto envelope =
        json::parse(response.body, nullptr, false, true);
    if (!envelope.is_object())
    {
        return print_error(
            kExitExecution,
            "invalid_editor_response",
            "Unreal Editor returned a non-object capability response.");
    }

    json result = {
        { "schema", "ue.workflow-capabilities.v1" },
        { "ok", false },
        { "diagnostics", json::array() },
    };
    if (response.ok && envelope.value("ok", false))
    {
        const auto data = envelope.find("data");
        if (data == envelope.end() || !data->is_object())
        {
            return print_error(
                kExitExecution,
                "invalid_editor_response",
                "Unreal Editor capability response did not contain an object data field.");
        }
        result.update(*data);
        result["schema"] = "ue.workflow-capabilities.v1";
        result["ok"] = true;
        result["diagnostics"] = json::array();
        print_json_text(result.dump(), compact);
        return 0;
    }

    const auto error = envelope.value("error", json::object());
    result["diagnostics"].push_back({
        { "severity", "error" },
        { "phase", "capabilities" },
        { "code", error.value("code", "capability_request_failed") },
        { "path", "" },
        { "message",
            error.value(
                "message",
                "Unreal Editor capability request failed.") },
    });
    print_json_text(result.dump(), compact);
    return kExitExecution;
}

std::optional<json> http_data_object(const HttpResult& response)
{
    if (!response.ok || !response.error.empty())
    {
        return std::nullopt;
    }
    auto envelope = json::parse(response.body, nullptr, false, true);
    if (!envelope.is_object())
    {
        return std::nullopt;
    }
    if (const auto data = envelope.find("data");
        data != envelope.end() && data->is_object())
    {
        return *data;
    }
    return envelope;
}

std::string contract_digest_from_handshake(
    const json& handshake,
    std::string_view version)
{
    if (!handshake.is_object())
    {
        return {};
    }
    const bool is_v2 = version == "2.0";
    if (const auto direct = handshake.find(
            is_v2 ? "contractSetDigestV2" : "contractSetDigest");
        direct != handshake.end() && direct->is_string())
    {
        return direct->get<std::string>();
    }

    const std::array<std::string_view, 3> containers = {
        "contractSetDigests",
        "contractSets",
        "contracts",
    };
    const std::array<std::string_view, 3> keys = is_v2
        ? std::array<std::string_view, 3>{ "2.0", "v2", "2" }
        : std::array<std::string_view, 3>{ "1.0", "v1", "1" };
    for (const std::string_view container_name : containers)
    {
        const auto container =
            handshake.find(std::string(container_name));
        if (container == handshake.end() || !container->is_object())
        {
            continue;
        }
        for (const std::string_view key : keys)
        {
            const auto value = container->find(std::string(key));
            if (value == container->end())
            {
                continue;
            }
            if (value->is_string())
            {
                return value->get<std::string>();
            }
            if (value->is_object())
            {
                for (const auto* digest_field :
                     { "contractSetDigest", "digest" })
                {
                    const auto digest = value->find(digest_field);
                    if (digest != value->end() && digest->is_string())
                    {
                        return digest->get<std::string>();
                    }
                }
            }
        }
    }
    return {};
}


std::optional<int> persist_editor_receipt(
    const HttpResult& response,
    const ue::workflow::Engine& engine,
    const std::filesystem::path& path,
    bool compact)
{
    const auto envelope = json::parse(response.body, nullptr, false, true);
    if (envelope.is_discarded())
    {
        return print_error(
            kExitExecution,
            "invalid_editor_response",
            "Unreal Editor returned non-JSON data.");
    }
    const json* result = &envelope;
    if (const auto data = envelope.find("data");
        data != envelope.end() && data->is_object())
    {
        result = &*data;
    }

    const auto result_validation = engine.ValidateResultJson(result->dump());
    if (!result_validation.ok)
    {
        print_json_text(result_validation.json, compact);
        return kExitExecution;
    }
    const auto receipt_it = result->find("receipt");
    if (receipt_it == result->end() || !receipt_it->is_object())
    {
        return print_error(
            kExitExecution,
            "editor_receipt_missing",
            "A successful workflow result must contain a persisted run receipt.");
    }
    const auto receipt_validation = engine.ValidateReceiptJson(receipt_it->dump());
    if (!receipt_validation.ok)
    {
        print_json_text(receipt_validation.json, compact);
        return kExitExecution;
    }

    for (const auto* field : { "runId", "planDigest", "contractSetDigest", "status" })
    {
        if (!result->contains(field) || !receipt_it->contains(field) ||
            (*result)[field] != (*receipt_it)[field])
        {
            return print_error(
                kExitExecution,
                "editor_receipt_mismatch",
                std::string("Workflow result and receipt disagree on ") + field + ".",
                std::string("/receipt/") + field);
        }
    }
    if (receipt_it->value("contractSetDigest", std::string{}) !=
        engine.ContractSetDigest())
    {
        return print_error(
            kExitExecution,
            "editor_contract_mismatch",
            "The Editor receipt was produced from a different workflow contract set.",
            "/receipt/contractSetDigest");
    }
    if (!write_text(path, receipt_it->dump(2) + "\n"))
    {
        return print_error(
            kExitExecution,
            "receipt_write_failed",
            "Workflow action succeeded, but the validated receipt could not be written.",
            path.generic_string());
    }
    return std::nullopt;
}

std::optional<ue::workflow::Engine> load_engine(
    const CliOptions& options,
    int& exit_code)
{
    ue::workflow::Engine engine;
    const auto loaded = engine.Load({
        options.contract_root,
        options.capability_roots,
    });
    if (!loaded.ok)
    {
        print_json_text(loaded.json, options.json_output);
        exit_code = loaded.exit_code;
        return std::nullopt;
    }
    return engine;
}

int run_command(
    CliOptions options,
    const std::filesystem::path& executable,
    ue::api::Client* shared_client = nullptr);

std::vector<std::string> tokenize(std::string_view line)
{
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    char quote = '\0';
    for (std::size_t index = 0; index < line.size(); ++index)
    {
        const char c = line[index];
        if (quoted)
        {
            if (c == quote)
            {
                quoted = false;
            }
            else if (c == '\\' && index + 1 < line.size() && line[index + 1] == quote)
            {
                current.push_back(line[++index]);
            }
            else
            {
                current.push_back(c);
            }
        }
        else if (c == '"' || c == '\'')
        {
            quoted = true;
            quote = c;
        }
        else if (c == ' ' || c == '\t')
        {
            if (!current.empty())
            {
                tokens.push_back(std::move(current));
                current.clear();
            }
        }
        else
        {
            current.push_back(c);
        }
    }
    if (!current.empty())
    {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

int run_shell(
    const CliOptions& base_options,
    const std::filesystem::path& executable,
    ue::api::Client& client)
{
    if (!is_tty())
    {
        return print_error(
            kExitUsage,
            "shell_requires_tty",
            "ue-workflow shell requires an interactive terminal.");
    }
    std::cerr << "UE Workflow shell. Type 'help' or 'exit'.\n";
    std::string line;
    for (;;)
    {
        std::cerr << "ue-workflow> ";
        if (!std::getline(std::cin, line))
        {
            return 0;
        }
        auto tokens = tokenize(line);
        if (tokens.empty())
        {
            continue;
        }
        if (tokens.front() == "exit" || tokens.front() == "quit")
        {
            return 0;
        }
        CliOptions options = base_options;
        options.positional.clear();
        if (!parse_arguments(tokens, options))
        {
            print_error(kExitUsage, "invalid_arguments", "Invalid shell command.");
            continue;
        }
        resolve_contract_paths(options, executable);
        (void)run_command(std::move(options), executable, &client);
    }
}

int run_command(
    CliOptions options,
    const std::filesystem::path& executable,
    ue::api::Client* shared_client)
{
    if (options.help_requested)
    {
        const bool removed_operation_help =
            options.positional.size() == 3
            && options.positional[0] == "operation"
            && options.positional[1] == "run";
        if (!removed_operation_help)
        {
            if (const auto invalid = positional_error(options))
            {
                return print_error(
                    kExitUsage,
                    "invalid_arguments",
                    *invalid);
            }
        }
        print_json_text(command_help(options).dump(), options.json_output);
        return 0;
    }
    if (options.positional.empty())
    {
        if (is_tty())
        {
            resolve_contract_paths(options, executable);
            ue::api::Client client(options.endpoint);
            ConfigureWorkflowCaller(client, "shell");
            return run_shell(options, executable, client);
        }
        print_json_text(binary_help().dump(), options.json_output);
        return 0;
    }

    const auto& command = options.positional[0];
    if (command == "--help" || command == "help" && options.positional.size() == 1)
    {
        print_json_text(binary_help().dump(), options.json_output);
        return 0;
    }
    if (const auto invalid = positional_error(options))
    {
        return print_error(
            kExitUsage,
            "invalid_arguments",
            *invalid);
    }
    if (command == "--version" || command == "version")
    {
        print_json_text(json({
            { "ok", true },
            { "product", "UE Workflow DSL" },
            { "executable", "ue-workflow" },
            { "version", UE_WORKFLOW_VERSION },
            { "dsl", "ue.workflow" },
            { "dslVersion", ue::workflow::DslVersion() },
            { "plannerVersion", ue::workflow::PlannerVersion() },
        }).dump(), options.json_output);
        return 0;
    }
    resolve_contract_paths(options, executable);

    std::unique_ptr<ue::api::Client> owned_client;
    if (!shared_client)
    {
        owned_client = std::make_unique<ue::api::Client>(
            options.endpoint);
        ConfigureWorkflowCaller(*owned_client, command);
        shared_client = owned_client.get();
    }
    if (command == "shell")
    {
        return run_shell(options, executable, *shared_client);
    }

    int load_exit = kExitUnavailable;
    auto engine = load_engine(options, load_exit);
    if (!engine)
    {
        return load_exit;
    }

    if (command == "doctor")
    {
        const std::string& local_v1 = engine->ContractSetDigest();
        const std::string& local_v2 = engine->ContractSetDigestV2();
        json payload = {
            { "ok", true },
            { "schema", "ue.workflow-doctor.v1" },
            { "contractRoot", options.contract_root.generic_string() },
            { "capabilityCount", engine->CapabilityCount() },
            { "composableOperationCount", engine->ComposableOperationCount() },
            { "contractSetDigest", local_v1 },
            { "contracts", {
                { "v1", {
                    { "dslVersion", "1.0" },
                    { "contractSetDigest", local_v1 },
                } },
                { "v2", {
                    { "dslVersion", "2.0" },
                    { "contractSetDigest", local_v2 },
                } },
            } },
            { "editor", {
                { "checked", options.connect },
                { "endpoint", options.endpoint },
                { "reachable", false },
            } },
        };
        if (options.connect)
        {
            const auto response = http_request(
                *shared_client,
                "GET",
                "/api/v1/workflow/handshake");
            payload["editor"]["reachable"] = response.ok;
            if (response.ok)
            {
                const auto response_json =
                    json::parse(response.body, nullptr, false, true);
                const auto handshake =
                    response_json.is_object() && response_json.contains("data")
                    ? response_json["data"]
                    : response_json;
                payload["editor"]["handshake"] = handshake;
                const std::string remote_v1 =
                    contract_digest_from_handshake(handshake, "1.0");
                const std::string remote_v2 =
                    contract_digest_from_handshake(handshake, "2.0");
                const bool v1_match =
                    !remote_v1.empty() && remote_v1 == local_v1;
                const bool v2_match =
                    !remote_v2.empty() && remote_v2 == local_v2;
                const bool all_contracts_match = v1_match && v2_match;
                payload["editor"]["contractSetDigest"] = remote_v1;
                payload["editor"]["contractMatch"] = all_contracts_match;
                payload["editor"]["contracts"] = {
                    { "v1", {
                        { "dslVersion", "1.0" },
                        { "contractSetDigest",
                            remote_v1.empty() ? json(nullptr) : json(remote_v1) },
                        { "match", v1_match },
                    } },
                    { "v2", {
                        { "dslVersion", "2.0" },
                        { "contractSetDigest",
                            remote_v2.empty() ? json(nullptr) : json(remote_v2) },
                        { "match", v2_match },
                    } },
                };
                if (!all_contracts_match)
                {
                    payload["ok"] = false;
                    payload["editor"]["error"] =
                        remote_v1.empty() || remote_v2.empty()
                        ? "Handshake did not provide both v1 and v2 "
                          "contract digests."
                        : "Editor and CLI workflow contracts do not match.";
                }
            }
            else
            {
                payload["ok"] = false;
                payload["editor"]["error"] =
                    response.error.empty() ? response.body : response.error;
            }
        }
        print_json_text(payload.dump(), options.json_output);
        return payload.value("ok", false) ? 0 : kExitUnavailable;
    }

    if (command == "capabilities")
    {
        if (options.connect)
        {
            const auto response = http_request(
                *shared_client,
                "GET",
                capability_query_path(options));
            return print_capabilities_http_result(
                response,
                options.json_output);
        }

        ue::workflow::CapabilityQuery query;
        query.query = options.capability_query;
        query.operation = options.capability_operation;
        query.domain = options.capability_domain;
        query.kind = options.capability_kind;
        query.output_kind = options.capability_output_kind;
        query.risk = options.capability_risk;
        query.read_only = options.capability_read_only;
        query.destructive = options.capability_destructive;
        query.expensive = options.capability_expensive;
        query.available_only =
            options.capability_available_only;
        query.offset = options.capability_offset;
        query.limit = options.capability_limit;
        query.detail =
            options.capability_detail == "full"
            ? ue::workflow::CapabilityDetail::Full
            : ue::workflow::CapabilityDetail::Summary;
        const auto result = engine->CapabilitiesJson(query);
        print_json_text(result.json, options.json_output);
        return result.exit_code;
    }

    if (command == "help")
    {
        if (options.positional.size() >= 3 && options.positional[1] == "operation")
        {
            const auto result = engine->ExplainOperation(options.positional[2]);
            print_json_text(result.json, options.json_output);
            return result.exit_code;
        }
        if (options.positional.size() >= 2 && options.positional[1] == "composable")
        {
            const auto scope =
                options.positional.size() >= 3 ? options.positional[2] : std::string{};
            const auto result = engine->HelpJson(scope);
            print_json_text(result.json, options.json_output);
            return result.exit_code;
        }
        print_json_text(binary_help().dump(), options.json_output);
        return 0;
    }

    if (command == "validate" || command == "plan" || command == "execute")
    {
        if (options.file.empty())
        {
            return print_error(
                kExitUsage,
                "file_required",
                "This command requires --file <workflow.json|->.");
        }
        const auto input = read_text(options.file);
        if (!input.ok)
        {
            return print_error(
                kExitValidation,
                input.code == "invalid_text_encoding"
                    ? input.code
                    : "workflow_file_unreadable",
                input.code == "invalid_text_encoding"
                    ? input.message
                    : "Could not read the workflow file.",
                options.file.generic_string());
        }
        if (command == "validate")
        {
            const auto result = engine->ValidateJson(input.text);
            print_json_text(project_planning_response(result.json, options), options.json_output);
            return result.exit_code;
        }

        const auto plan = engine->PlanJson(input.text);
        if (!plan.ok)
        {
            print_json_text(project_planning_response(plan.json, options), options.json_output);
            return plan.exit_code;
        }

        const auto workflow =
            json::parse(input.text, nullptr, false, true);
        if (!workflow.is_object())
        {
            return print_error(
                kExitValidation,
                "workflow_json_invalid",
                "The workflow file must contain one JSON object.");
        }

        if (command == "plan")
        {
            if (options.connect)
            {
                json request = {
                    { "action", "plan" },
                    { "workflow", workflow },
                };
                apply_response_options(request, options);
                const auto response = http_request(
                    *shared_client,
                    "POST",
                    "/api/v1/workflow",
                    request.dump());
                return print_http_result(response, options.json_output);
            }
            const std::string offline_plan =
                mark_offline_plan_unprepared(plan.json);
            print_json_text(
                project_planning_response(offline_plan, options),
                options.json_output);
            return plan.exit_code;
        }

        const auto core_plan = json::parse(plan.json, nullptr, false, true);
        const auto core_digest =
            core_plan.value("planDigest", std::string{});
        if (options.approve_plan.empty())
        {
            return print_error(
                kExitApproval,
                "approval_required",
                "Execute requires the exact planDigest from a reviewed "
                "ue-workflow plan --connect.",
                "/approvePlanDigest");
        }
        json prepare_request = {
            { "action", "plan" },
            { "workflow", workflow },
            { "detailLevel", "summary" },
        };
        const auto prepare_response = http_request(
            *shared_client,
            "POST",
            "/api/v1/workflow",
            prepare_request.dump());
        if (!prepare_response.ok)
        {
            return print_http_result(
                prepare_response,
                options.json_output);
        }
        const auto prepared_plan = http_data_object(prepare_response);
        if (!prepared_plan)
        {
            return print_error(
                kExitExecution,
                "invalid_editor_response",
                "Editor workflow plan response did not contain a JSON object.");
        }
        const auto prepared_core_digest =
            prepared_plan->value("corePlanDigest", std::string{});
        const auto prepared_contract_digest =
            prepared_plan->value("contractSetDigest", std::string{});
        if (prepared_core_digest != core_digest ||
            prepared_contract_digest !=
                core_plan.value("contractSetDigest", std::string{}))
        {
            return print_error(
                kExitApproval,
                "workflow_contract_mismatch",
                "Editor and CLI produced different Core workflow plans; "
                "refresh the installed CLI/plugin pair.");
        }
        const auto expected_digest =
            prepared_plan->value("planDigest", std::string{});
        if (!prepared_plan->value("executionReady", false) ||
            expected_digest.empty())
        {
            return print_error(
                kExitApproval,
                "asset_precondition_required",
                "Editor did not return an execution-ready asset-bound plan; "
                "run ue-workflow plan --connect again.");
        }
        if (options.approve_plan != expected_digest)
        {
            return print_error(
                kExitApproval,
                "plan_changed",
                "Execute requires the exact planDigest from a reviewed "
                "ue-workflow plan --connect.",
                "/approvePlanDigest");
        }
        const json approval =
            prepared_plan->value("approval", json::object());
        if (approval.value("confirmWriteRequired", false) &&
            !options.confirm_write)
        {
            return print_error(
                kExitApproval,
                "confirm_write_required",
                "This plan contains confirmWrite operations and requires --confirm-write.",
                "/confirmWrite");
        }
        if (options.receipt.empty())
        {
            return print_error(
                kExitApproval,
                "receipt_required",
                "Workflow execution requires --receipt <path>.");
        }
        json request = {
            {"action", "execute"},
            {"workflow", workflow},
            {"approvePlanDigest", options.approve_plan},
            {"saveOnSuccess", options.save_on_success},
            {"confirmWrite", options.confirm_write},
        };
        apply_response_options(request, options);
        const auto response = http_request(
            *shared_client,
            "POST",
            "/api/v1/workflow",
            request.dump());
        if (response.ok)
        {
            if (const auto receipt_error = persist_editor_receipt(
                    response,
                    *engine,
                    options.receipt,
                    options.json_output))
            {
                return *receipt_error;
            }
        }
        return print_http_result(response, options.json_output);
    }

    if (command == "status" || command == "resume" || command == "rollback")
    {
        if (options.receipt.empty())
        {
            return print_error(
                kExitUsage,
                "receipt_required",
                "This command requires --receipt <path>.");
        }
        const auto receipt_text = read_text(options.receipt);
        if (!receipt_text.ok)
        {
            return print_error(
                kExitApproval,
                receipt_text.code == "invalid_text_encoding"
                    ? receipt_text.code
                    : "invalid_receipt",
                receipt_text.code == "invalid_text_encoding"
                    ? receipt_text.message
                    : "Receipt could not be read.",
                options.receipt.generic_string());
        }
        const auto receipt_validation =
            engine->ValidateReceiptJson(receipt_text.text);
        if (!receipt_validation.ok)
        {
            print_json_text(receipt_validation.json, options.json_output);
            return kExitApproval;
        }
        const auto receipt =
            json::parse(receipt_text.text, nullptr, false, true);
        json request = {
            {"action", command},
            {"runId", receipt["runId"]},
        };
        apply_response_options(request, options);
        if (command == "rollback")
        {
            if (!receipt.contains("planDigest") || !receipt["planDigest"].is_string())
            {
                return print_error(
                    kExitApproval,
                    "receipt_plan_digest_missing",
                    "Rollback requires the original receipt planDigest.",
                    options.receipt.generic_string());
            }
            request["approvePlanDigest"] = receipt["planDigest"];
        }
        const auto response = http_request(
            *shared_client,
            "POST",
            "/api/v1/workflow",
            request.dump());
        if (response.ok)
        {
            if (const auto receipt_error = persist_editor_receipt(
                    response,
                    *engine,
                    options.receipt,
                    options.json_output))
            {
                return *receipt_error;
            }
        }
        return print_http_result(response, options.json_output);
    }

    print_json_text(binary_help().dump(), options.json_output);
    return kExitUsage;
}

} // namespace

int main_from_utf8_arguments(
    std::vector<std::string> all_arguments,
    const std::filesystem::path& argument_zero)
{
    std::vector<std::string> arguments;
    if (all_arguments.size() > 1)
    {
        arguments.assign(
            std::make_move_iterator(all_arguments.begin() + 1),
            std::make_move_iterator(all_arguments.end()));
    }

    CliOptions options;
    if (!parse_arguments(arguments, options))
    {
        return print_error(
            kExitUsage,
            "invalid_arguments",
            "Invalid or incomplete command-line arguments.");
    }

    std::error_code error;
    const auto executable =
        std::filesystem::absolute(argument_zero, error);
    return run_command(
        std::move(options),
        error ? argument_zero : executable);
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
{
    ue::cli::InitializeUtf8Console();
    return main_from_utf8_arguments(
        ue::cli::Utf8Arguments(argc, argv),
        argc > 0 && argv[0]
            ? std::filesystem::path(argv[0])
            : std::filesystem::path{});
}
#else
int main(int argc, char** argv)
{
    ue::cli::InitializeUtf8Console();
    return main_from_utf8_arguments(
        ue::cli::Utf8Arguments(argc, argv),
        argc > 0 && argv[0]
            ? std::filesystem::path(argv[0])
            : std::filesystem::path{});
}
#endif
