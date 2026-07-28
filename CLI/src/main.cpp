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
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <io.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
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
    std::filesystem::path contract_root;
    std::vector<std::filesystem::path> capability_roots;
    std::filesystem::path file;
    std::filesystem::path output;
    std::filesystem::path receipt;
    std::string endpoint = "http://127.0.0.1:9847";
    std::string approve_plan;
    std::string params = "{}";
    std::string request_id;
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

std::optional<std::string> read_text(const std::filesystem::path& path)
{
    if (path == "-")
    {
        return std::string(
            std::istreambuf_iterator<char>(std::cin),
            std::istreambuf_iterator<char>());
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
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
    const auto temporary = path.string() + ".tmp";
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

std::optional<std::vector<std::uint8_t>> decode_base64(std::string_view encoded)
{
    std::array<std::int16_t, 256> table{};
    table.fill(-1);
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t index = 0; index < alphabet.size(); ++index)
    {
        table[static_cast<unsigned char>(alphabet[index])] =
            static_cast<std::int16_t>(index);
    }

    std::size_t encoded_length = 0;
    std::size_t padding = 0;
    bool saw_padding = false;
    for (const unsigned char character : encoded)
    {
        if (character == ' ' || character == '\t'
            || character == '\r' || character == '\n')
        {
            continue;
        }
        ++encoded_length;
        if (character == '=')
        {
            saw_padding = true;
            ++padding;
            continue;
        }
        if (saw_padding || table[character] < 0)
        {
            return std::nullopt;
        }
    }
    if (encoded_length == 0)
    {
        return std::vector<std::uint8_t>{};
    }
    if (encoded_length % 4 != 0 || padding > 2)
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> decoded;
    decoded.reserve(encoded_length / 4 * 3 - padding);
    std::uint32_t accumulator = 0;
    int bits = -8;
    for (const unsigned char character : encoded)
    {
        if (character == ' ' || character == '\t'
            || character == '\r' || character == '\n'
            || character == '=')
        {
            continue;
        }
        accumulator = (accumulator << 6)
            | static_cast<std::uint32_t>(table[character]);
        bits += 6;
        if (bits >= 0)
        {
            decoded.push_back(
                static_cast<std::uint8_t>((accumulator >> bits) & 0xffU));
            bits -= 8;
        }
    }
    if (decoded.size() != encoded_length / 4 * 3 - padding)
    {
        return std::nullopt;
    }
    return decoded;
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
                         "operation run <type> [--params <json>] "
                         "[--request-id <id>] [--output <path>]",
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
            options.contract_root = value;
        }
        else if (argument == "--capability-root")
        {
            std::string value;
            if (!take_value(index, value))
            {
                return false;
            }
            options.capability_roots.emplace_back(value);
        }
        else if (argument == "--file")
        {
            std::string value;
            if (!take_value(index, value))
            {
                return false;
            }
            options.file = value;
        }
        else if (argument == "--receipt")
        {
            std::string value;
            if (!take_value(index, value))
            {
                return false;
            }
            options.receipt = value;
        }
        else if (argument == "--output")
        {
            std::string value;
            if (!take_value(index, value) || value.empty())
            {
                return false;
            }
            options.output = value;
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
        else if (argument == "--params")
        {
            if (!take_value(index, options.params))
            {
                return false;
            }
        }
        else if (argument == "--request-id")
        {
            if (!take_value(index, options.request_id)
                || options.request_id.empty()
                || options.request_id.size() > 200)
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
        else if (argument == "--help" || argument == "--version")
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

struct ParsedEndpoint
{
    std::string host;
    std::string port;
};

std::optional<ParsedEndpoint> parse_endpoint(std::string_view endpoint)
{
    constexpr std::string_view prefix = "http://";
    if (!endpoint.starts_with(prefix))
    {
        return std::nullopt;
    }
    endpoint.remove_prefix(prefix.size());
    const auto colon = endpoint.rfind(':');
    if (colon == std::string_view::npos)
    {
        return std::nullopt;
    }
    const auto host = std::string(endpoint.substr(0, colon));
    const auto port = std::string(endpoint.substr(colon + 1));
    if ((host != "127.0.0.1" && host != "localhost" && host != "::1") ||
        port.empty())
    {
        return std::nullopt;
    }
    return ParsedEndpoint{ host, port };
}

std::string url_encode(std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value)
    {
        if ((character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_' || character == '.')
        {
            encoded.push_back(static_cast<char>(character));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hex[(character >> 4U) & 0x0fU]);
        encoded.push_back(hex[character & 0x0fU]);
    }
    return encoded;
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
        result += url_encode(parameters[index].first);
        result.push_back('=');
        result += url_encode(parameters[index].second);
    }
    return result;
}

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
struct SocketRuntime
{
    SocketRuntime() { available = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~SocketRuntime() { if (available) WSACleanup(); }
    WSADATA data{};
    bool available = false;
};
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void close_socket(Socket socket) { close(socket); }
struct SocketRuntime
{
    bool available = true;
};
#endif

struct HttpResult
{
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

std::optional<std::size_t> find_header_end(const std::string& response)
{
    const auto position = response.find("\r\n\r\n");
    return position == std::string::npos
        ? std::nullopt
        : std::optional<std::size_t>(position + 4);
}

HttpResult http_request(
    const std::string& endpoint,
    std::string_view method,
    std::string_view path,
    std::string_view body = {})
{
    const auto parsed = parse_endpoint(endpoint);
    if (!parsed)
    {
        return { false, 0, {}, "Endpoint must be loopback HTTP, for example http://127.0.0.1:9847." };
    }
    SocketRuntime runtime;
    if (!runtime.available)
    {
        return { false, 0, {}, "Socket runtime initialization failed." };
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(parsed->host.c_str(), parsed->port.c_str(), &hints, &addresses) != 0)
    {
        return { false, 0, {}, "Could not resolve the loopback endpoint." };
    }

    Socket socket = kInvalidSocket;
    for (auto* address = addresses; address; address = address->ai_next)
    {
        socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == kInvalidSocket)
        {
            continue;
        }
        if (::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0)
        {
            break;
        }
        close_socket(socket);
        socket = kInvalidSocket;
    }
    freeaddrinfo(addresses);
    if (socket == kInvalidSocket)
    {
        return { false, 0, {}, "Cannot connect to the running Unreal Editor." };
    }

    std::ostringstream request;
    request << method << " " << path << " HTTP/1.1\r\n"
            << "Host: " << parsed->host << ":" << parsed->port << "\r\n"
            << "Accept: application/json\r\n"
            << "Connection: close\r\n";
    if (method == "POST")
    {
        request << "Content-Type: application/json\r\n"
                << "Content-Length: " << body.size() << "\r\n";
    }
    request << "\r\n";
    if (method == "POST")
    {
        request << body;
    }
    const auto request_text = request.str();
    std::size_t sent = 0;
    while (sent < request_text.size())
    {
        const auto count = ::send(
            socket,
            request_text.data() + sent,
            static_cast<int>(request_text.size() - sent),
            0);
        if (count <= 0)
        {
            close_socket(socket);
            return { false, 0, {}, "Failed to send the HTTP request." };
        }
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    std::array<char, 8192> buffer{};
    for (;;)
    {
        const auto count = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count <= 0)
        {
            break;
        }
        response.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close_socket(socket);

    const auto header_end = find_header_end(response);
    if (!header_end)
    {
        return { false, 0, {}, "Editor returned an invalid HTTP response." };
    }
    const auto first_space = response.find(' ');
    if (first_space == std::string::npos || first_space + 4 > response.size())
    {
        return { false, 0, {}, "Editor returned an invalid HTTP status line." };
    }
    int status = 0;
    const auto status_text = std::string_view(response).substr(first_space + 1, 3);
    const auto parsed_status = std::from_chars(
        status_text.data(),
        status_text.data() + status_text.size(),
        status);
    if (parsed_status.ec != std::errc{})
    {
        return { false, 0, {}, "Editor returned an invalid HTTP status." };
    }
    const auto response_body = response.substr(*header_end);
    return { status >= 200 && status < 300, status, response_body, {} };
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

struct Base64Payload
{
    json* container = nullptr;
    std::string field;
    std::string value;
};

std::optional<Base64Payload> find_base64_payload(json& envelope)
{
    json* data = &envelope;
    if (envelope.contains("data") && envelope["data"].is_object())
    {
        data = &envelope["data"];
    }

    static constexpr std::array<std::string_view, 5> fields = {
        "contentBase64",
        "content_base64",
        "image_base64",
        "dataBase64",
        "base64",
    };
    const auto inspect = [&](json& candidate) -> std::optional<Base64Payload> {
        for (const auto field : fields)
        {
            const auto value = candidate.find(std::string(field));
            if (value != candidate.end() && value->is_string())
            {
                return Base64Payload{
                    &candidate,
                    std::string(field),
                    value->get<std::string>(),
                };
            }
        }
        return std::nullopt;
    };

    if (auto direct = inspect(*data))
    {
        return direct;
    }
    for (const auto* nested_field : { "artifact", "content" })
    {
        const auto nested = data->find(nested_field);
        if (nested != data->end() && nested->is_object())
        {
            if (auto payload = inspect(*nested))
            {
                return payload;
            }
        }
    }
    return std::nullopt;
}

int print_operation_result(
    const HttpResult& response,
    const std::string& endpoint,
    const std::string& capability,
    json params,
    const std::filesystem::path& output,
    bool compact)
{
    if (output.empty() || !response.ok)
    {
        return print_http_result(response, compact);
    }
    if (!response.error.empty())
    {
        return print_error(kExitUnavailable, "editor_unreachable", response.error);
    }

    std::error_code file_error;
    if (!output.parent_path().empty())
    {
        std::filesystem::create_directories(output.parent_path(), file_error);
    }
    if (file_error)
    {
        return print_error(
            kExitExecution,
            "artifact_write_failed",
            "The artifact output directory could not be created.",
            output.generic_string());
    }
    const std::filesystem::path temporary = output.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream)
    {
        return print_error(
            kExitExecution,
            "artifact_write_failed",
            "The temporary artifact file could not be opened.",
            temporary.generic_string());
    }

    HttpResult current = response;
    json projected;
    std::string source_field;
    std::size_t total_bytes = 0;
    std::size_t chunks = 0;
    std::uint64_t last_next_offset = 0;
    std::optional<std::uint64_t> expected_size;
    std::optional<std::string> expected_sha256;
    bool source_sha256_deferred = false;
    const auto read_unsigned = [](
        const json& object,
        std::string_view field) -> std::optional<std::uint64_t>
    {
        const auto value = object.find(std::string(field));
        if (value == object.end()
            || (!value->is_number_unsigned()
                && !value->is_number_integer()))
        {
            return std::nullopt;
        }
        if (!value->is_number_unsigned()
            && value->is_number_integer()
            && value->get<std::int64_t>() < 0)
        {
            return std::nullopt;
        }
        return value->get<std::uint64_t>();
    };
    for (;;)
    {
        auto envelope = json::parse(current.body, nullptr, false, true);
        if (!current.ok || !envelope.is_object())
        {
            stream.close();
            std::filesystem::remove(temporary, file_error);
            return current.ok
                ? print_error(
                    kExitExecution,
                    "invalid_editor_response",
                    "Unreal Editor returned non-JSON data.")
                : print_http_result(current, compact);
        }
        auto payload = find_base64_payload(envelope);
        if (!payload)
        {
            stream.close();
            std::filesystem::remove(temporary, file_error);
            return print_error(
                kExitExecution,
                "artifact_payload_missing",
                "--output requires an operation result containing a supported base64 artifact.");
        }
        const auto decoded = decode_base64(payload->value);
        if (!decoded)
        {
            stream.close();
            std::filesystem::remove(temporary, file_error);
            return print_error(
                kExitExecution,
                "artifact_payload_invalid",
                "The Editor returned an invalid base64 artifact payload.");
        }
        json* data = &envelope;
        if (envelope.contains("data") && envelope["data"].is_object())
        {
            data = &envelope["data"];
        }
        const auto offset = read_unsigned(*data, "offset");
        const auto next_offset = read_unsigned(*data, "nextOffset");
        const auto size_bytes = read_unsigned(*data, "sizeBytes");
        const auto expected_next =
            static_cast<std::uint64_t>(total_bytes)
            + static_cast<std::uint64_t>(decoded->size());
        if (!offset
            || !next_offset
            || !size_bytes
            || *offset != static_cast<std::uint64_t>(total_bytes)
            || *next_offset != expected_next
            || *next_offset > *size_bytes)
        {
            stream.close();
            std::filesystem::remove(temporary, file_error);
            return print_error(
                kExitExecution,
                "artifact_cursor_invalid",
                "The Editor returned inconsistent artifact offsets or size.");
        }
        if (!expected_size)
        {
            expected_size = *size_bytes;
            if (const auto digest = data->find("sha256");
                digest != data->end() && digest->is_string())
            {
                expected_sha256 = digest->get<std::string>();
            }
            source_sha256_deferred =
                data->value("sha256Deferred", false);
        }
        else if (*size_bytes != *expected_size)
        {
            stream.close();
            std::filesystem::remove(temporary, file_error);
            return print_error(
                kExitExecution,
                "artifact_changed",
                "The artifact size changed between chunks.");
        }
        const bool eof = data->value("eof", true);
        if (eof != (*next_offset == *size_bytes))
        {
            stream.close();
            std::filesystem::remove(temporary, file_error);
            return print_error(
                kExitExecution,
                "artifact_cursor_invalid",
                "The Editor returned an inconsistent artifact EOF marker.");
        }
        stream.write(
            reinterpret_cast<const char*>(decoded->data()),
            static_cast<std::streamsize>(decoded->size()));
        if (!stream)
        {
            stream.close();
            std::filesystem::remove(temporary, file_error);
            return print_error(
                kExitExecution,
                "artifact_write_failed",
                "The artifact chunk could not be written.",
                output.generic_string());
        }
        total_bytes += decoded->size();
        ++chunks;

        if (projected.is_null())
        {
            source_field = payload->field;
            payload->container->erase(payload->field);
            projected = std::move(envelope);
        }
        if (eof)
        {
            last_next_offset = *next_offset;
            break;
        }
        if (*next_offset <= last_next_offset)
        {
            stream.close();
            std::filesystem::remove(temporary, file_error);
            return print_error(
                kExitExecution,
                "artifact_cursor_invalid",
                "The Editor returned a non-advancing artifact cursor.");
        }
        last_next_offset = *next_offset;
        params["offset"] = *next_offset;
        const json next_request = {
            { "capability", capability },
            { "params", params },
        };
        current = http_request(
            endpoint,
            "POST",
            "/api/execute",
            next_request.dump());
    }

    stream.close();
    if (!stream)
    {
        std::filesystem::remove(temporary, file_error);
        return print_error(
            kExitExecution,
            "artifact_write_failed",
            "The artifact file could not be finalized.",
            output.generic_string());
    }
    if (!expected_size
        || static_cast<std::uint64_t>(total_bytes) != *expected_size)
    {
        std::filesystem::remove(temporary, file_error);
        return print_error(
            kExitExecution,
            "artifact_size_mismatch",
            "The exported byte count does not match the registered artifact size.");
    }
    const auto exported_sha256 =
        ue::workflow::Sha256File(temporary);
    if (!exported_sha256)
    {
        std::filesystem::remove(temporary, file_error);
        return print_error(
            kExitExecution,
            "artifact_hash_failed",
            "The exported artifact could not be hashed.");
    }
    const auto normalize_digest = [](std::string digest)
    {
        if (!digest.starts_with("sha256:"))
        {
            digest = "sha256:" + digest;
        }
        std::transform(
            digest.begin(),
            digest.end(),
            digest.begin(),
            [](const unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            });
        return digest;
    };
    if (expected_sha256
        && normalize_digest(*expected_sha256)
            != normalize_digest(*exported_sha256))
    {
        std::filesystem::remove(temporary, file_error);
        return print_error(
            kExitExecution,
            "artifact_hash_mismatch",
            "The exported artifact does not match the registered SHA-256.");
    }
    std::filesystem::rename(temporary, output, file_error);
    if (file_error)
    {
        std::filesystem::remove(output, file_error);
        file_error.clear();
        std::filesystem::rename(temporary, output, file_error);
    }
    if (file_error)
    {
        std::filesystem::remove(temporary, file_error);
        return print_error(
            kExitExecution,
            "artifact_write_failed",
            "The artifact could not be moved into place.",
            output.generic_string());
    }

    json* projected_data = &projected;
    if (projected.contains("data") && projected["data"].is_object())
    {
        projected_data = &projected["data"];
    }
    projected_data->erase("contentBase64");
    projected_data->erase("content_base64");
    projected_data->erase("image_base64");
    projected_data->erase("dataBase64");
    projected_data->erase("base64");
    (*projected_data)["eof"] = true;
    (*projected_data)["nextOffset"] = last_next_offset;
    const auto absolute = std::filesystem::absolute(output, file_error);
    (*projected_data)["artifactExport"] = {
        { "path", (file_error ? output : absolute).generic_string() },
        { "bytes", total_bytes },
        { "chunks", chunks },
        { "sha256", *exported_sha256 },
        { "verifiedAgainstReceipt", expected_sha256.has_value() },
        { "sourceSha256Deferred", source_sha256_deferred },
        { "sourceEncoding", "base64" },
        { "sourceField", source_field },
    };
    print_json_text(projected.dump(), compact);
    return 0;
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
    const std::filesystem::path& executable);

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
    const std::filesystem::path& executable)
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
        (void)run_command(std::move(options), executable);
    }
}

int run_command(
    CliOptions options,
    const std::filesystem::path& executable)
{
    if (options.positional.empty())
    {
        if (is_tty())
        {
            return run_shell(options, executable);
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
    if (command == "shell")
    {
        return run_shell(options, executable);
    }
    const bool is_operation_run =
        command == "operation"
        && options.positional.size() >= 3
        && options.positional[1] == "run";
    if ((!options.request_id.empty() || !options.output.empty())
        && !is_operation_run)
    {
        return print_error(
            kExitUsage,
            "operation_option_misplaced",
            "--request-id and --output are only valid with operation run.");
    }

    int load_exit = kExitUnavailable;
    auto engine = load_engine(options, load_exit);
    if (!engine)
    {
        return load_exit;
    }

    if (command == "doctor")
    {
        json payload = {
            { "ok", true },
            { "schema", "ue.workflow-doctor.v1" },
            { "contractRoot", options.contract_root.generic_string() },
            { "capabilityCount", engine->CapabilityCount() },
            { "composableOperationCount", engine->ComposableOperationCount() },
            { "contractSetDigest", engine->ContractSetDigest() },
            { "editor", {
                { "checked", options.connect },
                { "endpoint", options.endpoint },
                { "reachable", false },
            } },
        };
        if (options.connect)
        {
            const auto response = http_request(
                options.endpoint,
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
                std::string remote_digest;
                if (handshake.is_object())
                {
                    remote_digest =
                        handshake.value("contractSetDigest", std::string{});
                    if (remote_digest.empty() &&
                        handshake.contains("contractSet") &&
                        handshake["contractSet"].is_object())
                    {
                        remote_digest =
                            handshake["contractSet"].value("digest", std::string{});
                    }
                }
                const bool contract_match =
                    !remote_digest.empty() &&
                    remote_digest == engine->ContractSetDigest();
                payload["editor"]["contractSetDigest"] = remote_digest;
                payload["editor"]["contractMatch"] = contract_match;
                if (!contract_match)
                {
                    payload["ok"] = false;
                    payload["editor"]["error"] =
                        remote_digest.empty()
                        ? "Handshake did not provide contractSetDigest."
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
                options.endpoint,
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
        if (!input)
        {
            return print_error(
                kExitValidation,
                "workflow_file_unreadable",
                "Could not read the workflow file.",
                options.file.generic_string());
        }
        if (command == "validate")
        {
            const auto result = engine->ValidateJson(*input);
            print_json_text(project_planning_response(result.json, options), options.json_output);
            return result.exit_code;
        }

        const auto plan = engine->PlanJson(*input);
        if (!plan.ok)
        {
            print_json_text(project_planning_response(plan.json, options), options.json_output);
            return plan.exit_code;
        }

        const auto workflow = json::parse(*input, nullptr, false, true);
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
                    options.endpoint,
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
            options.endpoint,
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
            options.endpoint,
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
        if (!receipt_text)
        {
            return print_error(
                kExitApproval,
                "invalid_receipt",
                "Receipt could not be read.",
                options.receipt.generic_string());
        }
        const auto receipt_validation = engine->ValidateReceiptJson(*receipt_text);
        if (!receipt_validation.ok)
        {
            print_json_text(receipt_validation.json, options.json_output);
            return kExitApproval;
        }
        const auto receipt = json::parse(*receipt_text, nullptr, false, true);
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
            options.endpoint,
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

    if (command == "operation" &&
        options.positional.size() >= 3 &&
        options.positional[1] == "run")
    {
        const auto params = json::parse(options.params, nullptr, false, true);
        if (!params.is_object())
        {
            return print_error(
                kExitValidation,
                "params_invalid",
                "--params must contain a JSON object.");
        }
        json request = {
            { "capability", options.positional[2] },
            { "params", params },
        };
        if (!options.request_id.empty())
        {
            request["requestId"] = options.request_id;
        }
        const auto response = http_request(
            options.endpoint,
            "POST",
            "/api/execute",
            request.dump());
        return print_operation_result(
            response,
            options.endpoint,
            options.positional[2],
            params,
            options.output,
            options.json_output);
    }

    print_json_text(binary_help().dump(), options.json_output);
    return kExitUsage;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(std::max(argc - 1, 0)));
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
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
    const auto executable = std::filesystem::absolute(argv[0], error);
    resolve_contract_paths(options, error ? std::filesystem::path(argv[0]) : executable);
    return run_command(std::move(options), error ? std::filesystem::path(argv[0]) : executable);
}
