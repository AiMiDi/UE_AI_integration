#include "UEWorkflowCore/WorkflowCore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
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
    bool details = false;
    std::filesystem::path contract_root;
    std::vector<std::filesystem::path> capability_roots;
    std::filesystem::path file;
    std::filesystem::path receipt;
    std::string endpoint = "http://127.0.0.1:9847";
    std::string approve_plan;
    std::string params = "{}";
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
        { "ok", true },
        { "schema", "ue.workflow-cli-help.v1" },
        { "product", "UE Workflow DSL" },
        { "executable", "ue-workflow" },
        { "version", UE_WORKFLOW_VERSION },
        { "dsl", "ue.workflow" },
        { "commands", json::array({
            "doctor [--connect]",
            "help composable [blueprint|widget|material]",
            "help operation <type>",
            "validate --file <workflow.json|->",
            "plan --file <workflow.json|->",
            "execute --file <workflow.json|-> --approve-plan <digest> --receipt <path> [--save-on-success] [--confirm-write]",
            "status|resume|rollback --receipt <path>",
            "operation run <type> [--params <json>]",
            "shell",
        }) },
        { "globalOptions", json::array({
            "--json",
            "--contract-root <path>",
            "--capability-root <path>",
            "--endpoint http://127.0.0.1:<port>",
        }) },
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
            options.details = true;
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
        else if (argument == "--endpoint")
        {
            if (!take_value(index, options.endpoint))
            {
                return false;
            }
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
        port && options.endpoint == "http://127.0.0.1:9847")
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
            print_json_text(result.json, options.json_output);
            return result.exit_code;
        }

        const auto plan = engine->PlanJson(*input);
        if (command == "plan" || !plan.ok)
        {
            print_json_text(plan.json, options.json_output);
            return plan.exit_code;
        }

        const auto plan_json = json::parse(plan.json, nullptr, false, true);
        const auto expected_digest = plan_json.value("planDigest", std::string{});
        if (options.approve_plan.empty() || options.approve_plan != expected_digest)
        {
            return print_error(
                kExitApproval,
                options.approve_plan.empty() ? "approval_required" : "plan_changed",
                "Execute requires the exact planDigest from a reviewed ue-workflow plan.",
                "/approvePlanDigest");
        }
        if (plan_json["approval"].value("confirmWriteRequired", false) &&
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
        const auto workflow = json::parse(*input, nullptr, false, true);
        const json request = {
            { "action", "execute" },
            { "workflow", workflow },
            { "approvePlanDigest", options.approve_plan },
            { "saveOnSuccess", options.save_on_success },
            { "confirmWrite", options.confirm_write },
            { "details", options.details },
        };
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
            { "action", command },
            { "runId", receipt["runId"] },
            { "details", options.details },
        };
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
        const json request = {
            { "capability", options.positional[2] },
            { "params", params },
        };
        const auto response = http_request(
            options.endpoint,
            "POST",
            "/api/execute",
            request.dump());
        return print_http_result(response, options.json_output);
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
