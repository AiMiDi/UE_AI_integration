#include "UECommandCli/CommandCli.h"

#include "UEApiClient/UEApiClient.h"
#include "UECliPlatform/Utf8Console.h"
#include "UECommandCli/CapabilityCatalog.h"
#include "UECommandCli/SkillCatalog.h"
#include "UEWorkflowCore/WorkflowCore.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef UE_CLI_VERSION
#define UE_CLI_VERSION "0.7.0"
#endif

namespace ue::command
{
namespace
{

using json = nlohmann::json;

constexpr int kExitUsage = 2;
constexpr int kExitUnavailable = 4;
constexpr int kExitExecution = 5;

struct Options
{
    std::string command;
    std::string help_capability;
    std::string endpoint = "http://127.0.0.1:9847";
    std::uint32_t timeout_ms = 300000;
    std::string request_id;
    std::optional<std::string> params_json;
    std::optional<std::filesystem::path> params_file;
    std::filesystem::path output_path;
    std::filesystem::path capability_root;
    std::filesystem::path skill_root;
    bool endpoint_explicit = false;
    bool timeout_explicit = false;
    bool json_output = false;
    bool confirm_write = false;
    bool help = false;
    bool live_schema = false;
    std::vector<RawOption> raw_options;
};

struct ParsedEnvelope
{
    bool ok = false;
    json value;
    std::string code;
    std::string message;
};

std::optional<std::string> Environment(const std::string_view name)
{
    const std::string key(name);
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, key.c_str()) != 0
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
    const char* value = std::getenv(key.c_str());
    return value && *value
        ? std::optional<std::string>(value)
        : std::nullopt;
#endif
}

bool ParseUint32(
    const std::string_view text,
    std::uint32_t& destination)
{
    std::uint64_t parsed_value = 0;
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        parsed_value);
    if (parsed.ec != std::errc{}
        || parsed.ptr != text.data() + text.size()
        || parsed_value == 0
        || parsed_value > std::numeric_limits<std::uint32_t>::max())
    {
        return false;
    }
    destination = static_cast<std::uint32_t>(parsed_value);
    return true;
}

bool StartsWithOption(const std::string_view value)
{
    return value.starts_with("--");
}

std::pair<std::string, std::optional<std::string>> SplitOption(
    std::string_view argument)
{
    argument.remove_prefix(2);
    const auto equals = argument.find('=');
    if (equals == std::string_view::npos)
    {
        return { std::string(argument), std::nullopt };
    }
    return {
        std::string(argument.substr(0, equals)),
        std::string(argument.substr(equals + 1)),
    };
}

bool TakeValue(
    const std::vector<std::string>& arguments,
    std::size_t& index,
    std::optional<std::string>& inline_value,
    std::string& destination)
{
    if (inline_value)
    {
        destination = std::move(*inline_value);
        return true;
    }
    if (index + 1 >= arguments.size()
        || StartsWithOption(arguments[index + 1]))
    {
        return false;
    }
    destination = arguments[++index];
    return true;
}

bool ParseArguments(
    const std::vector<std::string>& arguments,
    Options& options,
    std::string& error)
{
    if (const auto port = Environment("UE_PORT"))
    {
        std::uint32_t parsed_port = 0;
        if (ParseUint32(*port, parsed_port) && parsed_port <= 65535)
        {
            options.endpoint =
                "http://127.0.0.1:" + std::to_string(parsed_port);
        }
    }
    if (const auto timeout = Environment("UE_TIMEOUT_MS"))
    {
        (void)ParseUint32(*timeout, options.timeout_ms);
    }

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string& argument = arguments[index];
        if (!StartsWithOption(argument))
        {
            if (options.command.empty())
            {
                options.command = argument;
                continue;
            }
            if (options.command == "help"
                && options.help_capability.empty())
            {
                options.help_capability = argument;
                continue;
            }
            error = "Unexpected positional argument '" + argument + "'.";
            return false;
        }

        auto [name, inline_value] = SplitOption(argument);
        if (name == "json")
        {
            if (inline_value)
            {
                error = "--json does not accept a value.";
                return false;
            }
            options.json_output = true;
            continue;
        }
        if (name == "help")
        {
            if (inline_value)
            {
                error = "--help does not accept a value.";
                return false;
            }
            options.help = true;
            continue;
        }
        if (name == "version")
        {
            if (inline_value)
            {
                error = "--version does not accept a value.";
                return false;
            }
            if (!options.command.empty())
            {
                error = "--version cannot be combined with a command.";
                return false;
            }
            options.command = "--version";
            continue;
        }
        if (name == "confirm-write")
        {
            if (inline_value)
            {
                error = "--confirm-write does not accept a value.";
                return false;
            }
            options.confirm_write = true;
            continue;
        }
        if (name == "live-schema")
        {
            if (inline_value)
            {
                error = "--live-schema does not accept a value.";
                return false;
            }
            options.live_schema = true;
            continue;
        }
        if (name == "endpoint"
            || name == "timeout-ms"
            || name == "request-id"
            || name == "params"
            || name == "params-file"
            || name == "output"
            || name == "capability-root"
            || name == "skill-root")
        {
            std::string value;
            if (!TakeValue(
                    arguments,
                    index,
                    inline_value,
                    value)
                || value.empty())
            {
                error = "--" + name + " requires a value.";
                return false;
            }
            if (name == "endpoint")
            {
                options.endpoint = std::move(value);
                options.endpoint_explicit = true;
            }
            else if (name == "timeout-ms")
            {
                if (!ParseUint32(value, options.timeout_ms))
                {
                    error =
                        "--timeout-ms must be a positive integer.";
                    return false;
                }
                options.timeout_explicit = true;
            }
            else if (name == "request-id")
            {
                options.request_id = std::move(value);
            }
            else if (name == "params" || name == "params-file")
            {
                if (options.params_json || options.params_file)
                {
                    error =
                        "--params and --params-file are mutually exclusive "
                        "and cannot be repeated.";
                    return false;
                }
                if (name == "params")
                {
                    options.params_json = std::move(value);
                }
                else
                {
                    options.params_file =
                        ue::cli::Utf8Path(value);
                }
            }
            else if (name == "output")
            {
                options.output_path = ue::cli::Utf8Path(value);
            }
            else if (name == "capability-root")
            {
                options.capability_root = ue::cli::Utf8Path(value);
            }
            else
            {
                options.skill_root = ue::cli::Utf8Path(value);
            }
            continue;
        }

        RawOption raw;
        raw.name = std::move(name);
        if (raw.name.starts_with("no-"))
        {
            raw.name.erase(0, 3);
            raw.negated = true;
        }
        if (inline_value)
        {
            raw.value = std::move(*inline_value);
        }
        else if (index + 1 < arguments.size()
            && !StartsWithOption(arguments[index + 1]))
        {
            raw.value = arguments[++index];
        }
        options.raw_options.push_back(std::move(raw));
    }

    if (!options.endpoint_explicit)
    {
        // UE_PORT was applied before parsing; an explicit endpoint always wins.
    }
    return true;
}

void PrintGeneralHelp(std::ostream& output)
{
    output
        << "UE short-operation CLI\n"
        << "Usage:\n"
        << "  ue <capability-id> [--field value ...] [options]\n"
        << "  ue status [--json]\n"
        << "  ue capabilities [filters] [--json]\n"
        << "  ue skills [filters] [--json]\n"
        << "  ue help <capability-id>\n"
        << "  ue shell [--live-schema]\n"
        << "  ue --version\n\n"
        << "Global options:\n"
        << "  --endpoint <loopback-url>  Editor API endpoint\n"
        << "  --timeout-ms <n>           Socket timeout (default 300000)\n"
        << "  --request-id <id>          Explicit idempotency key\n"
        << "  --params <json-object>     Supply the complete params object\n"
        << "  --params-file <path|->     Read params JSON from a file or stdin\n"
        << "  --confirm-write            Set confirmWrite=true when supported\n"
        << "  --output <path>            Export image or artifact payload\n"
        << "  --live-schema              Fetch exact schema from Editor\n"
        << "  --capability-root <path>   Override packaged local manifests\n"
        << "  --skill-root <path>        Override packaged Agent Skills\n"
        << "  --json                     Print the full JSON envelope\n";
}

void PrintSkillsHelp(std::ostream& output)
{
    output
        << "UE Agent Skill catalog\n"
        << "Usage:\n"
        << "  ue skills [filters] [--json]\n\n"
        << "The command reads packaged skill.json files locally and does "
           "not connect to Unreal Editor.\n\n"
        << "Filters:\n"
        << "  --query <text>             Search id, title, description, "
           "and triggers\n"
        << "  --name <skill-id>          Match an exact skill id\n"
        << "  --recipe <recipe-id>       Require a matching recipe id\n"
        << "  --domain <domain>          Require a declared domain\n"
        << "  --operation <id>           Require a recipe step operation\n"
        << "  --risk <risk>              Match skill or recipe risk\n"
        << "  --detail summary|full      Projection detail "
           "(default summary)\n"
        << "  --offset <n>               Zero-based result offset\n"
        << "  --limit <1..100>           Maximum results (default 25)\n\n"
        << "Catalog options:\n"
        << "  --skill-root <path>        Override packaged Agent Skills\n"
        << "  --capability-root <path>   Override manifests used to "
           "validate operations\n"
        << "  --json                     Print the stable JSON envelope\n";
}

void PrintCommandHelp(
    const Options& options,
    std::ostream& output)
{
    if (options.command.empty())
    {
        PrintGeneralHelp(output);
        return;
    }
    if (options.command == "skills")
    {
        PrintSkillsHelp(output);
        return;
    }
    if (options.command == "status")
    {
        output
            << "Usage: ue status [--json] [connection options]\n"
            << "Read the running Editor and plugin health envelope.\n";
        return;
    }
    if (options.command == "capabilities")
    {
        output
            << "Usage: ue capabilities [filters] [--live-schema] [--json]\n"
            << "Filters: --query, --operation, --domain, --kind, "
               "--read-only, --destructive, --expensive, --output-kind, "
               "--risk, --offset, --limit, --detail.\n";
        return;
    }
    if (options.command == "shell")
    {
        output
            << "Usage: ue shell [--live-schema] [connection options]\n"
            << "Run multiple short capability commands over one connection.\n";
        return;
    }
    if (options.command == "help")
    {
        output
            << "Usage: ue help <capability-id> [--live-schema] [--json]\n"
            << "Load the local or live descriptor and print exact parameters.\n";
        return;
    }

    output
        << "Usage: ue " << options.command
        << " [--field value ...] [--params <json> | "
           "--params-file <path|->] [options]\n"
        << "This syntactic help performs no catalog load or Editor request.\n"
        << "Run `ue help " << options.command
        << "` to inspect the schema-derived parameters.\n";
}

std::optional<std::string> ReadParameterText(
    const Options& options,
    std::istream& input,
    std::string& error_code,
    std::string& error_message)
{
    if (options.params_json)
    {
        return *options.params_json;
    }
    if (!options.params_file)
    {
        return std::nullopt;
    }
    if (*options.params_file == std::filesystem::path("-"))
    {
        const ue::cli::Utf8TextResult decoded =
            ue::cli::ReadTextToUtf8(input);
        if (!decoded.ok)
        {
            error_code = decoded.code;
            error_message = decoded.message;
            return std::nullopt;
        }
        return decoded.text;
    }
    std::ifstream stream(*options.params_file, std::ios::binary);
    if (!stream)
    {
        error_code = "params_file_unreadable";
        error_message =
            "Could not read params file '"
            + options.params_file->generic_string() + "'.";
        return std::nullopt;
    }
    const ue::cli::Utf8TextResult decoded =
        ue::cli::ReadTextToUtf8(stream);
    if (!decoded.ok)
    {
        error_code = decoded.code;
        error_message =
            "Could not decode params file '"
            + options.params_file->generic_string()
            + "': " + decoded.message;
        return std::nullopt;
    }
    return decoded.text;
}

ConversionResult ConvertParameterObject(
    const json& capability_descriptor,
    const json& params,
    const bool confirm_write)
{
    ConversionResult result;
    if (!params.is_object())
    {
        result.code = "params_invalid";
        result.message = "Params JSON must contain one object.";
        return result;
    }

    std::vector<RawOption> raw_options;
    raw_options.reserve(params.size());
    for (auto iterator = params.begin(); iterator != params.end(); ++iterator)
    {
        std::string value;
        if (iterator->is_string())
        {
            value = iterator->get<std::string>();
            if (value.starts_with('@'))
            {
                value.insert(value.begin(), '@');
            }
        }
        else
        {
            value = iterator->dump();
        }
        raw_options.push_back({
            iterator.key(),
            std::move(value),
            false,
        });
    }
    return ConvertParameters(
        capability_descriptor,
        raw_options,
        confirm_write);
}

ParsedEnvelope ParseEnvelope(const ue::api::HttpResult& response)
{
    if (!response.error.empty())
    {
        return {
            false,
            json(),
            "editor_unreachable",
            response.error,
        };
    }
    auto envelope = json::parse(
        response.body,
        nullptr,
        false,
        true);
    if (!envelope.is_object())
    {
        return {
            false,
            json(),
            "invalid_editor_response",
            "Unreal Editor returned a non-object JSON response.",
        };
    }
    if (response.ok && envelope.value("ok", false))
    {
        return { true, std::move(envelope), {}, {} };
    }
    const auto error = envelope.value("error", json::object());
    return {
        false,
        std::move(envelope),
        error.value("code", "execution_failed"),
        error.value(
            "message",
            "Unreal Editor rejected the request."),
    };
}

int PrintFailure(
    const ParsedEnvelope& envelope,
    const bool json_output,
    const int exit_code,
    std::ostream& output,
    std::ostream& error)
{
    if (json_output)
    {
        if (envelope.value.is_object())
        {
            output << envelope.value.dump() << "\n";
        }
        else
        {
            output << json({
                { "ok", false },
                { "error", {
                    { "code", envelope.code },
                    { "message", envelope.message },
                } },
            }).dump() << "\n";
        }
        return exit_code;
    }

    error << "ERROR " << envelope.code << ": "
          << envelope.message;
    if (envelope.value.is_object())
    {
        const auto error_value = envelope.value.find("error");
        if (error_value != envelope.value.end()
            && error_value->is_object())
        {
            const auto details = error_value->find("details");
            if (details != error_value->end()
                && details->is_object())
            {
                const auto validation =
                    details->find("validationErrors");
                if (validation != details->end()
                    && validation->is_array()
                    && !validation->empty()
                    && (*validation)[0].is_string())
                {
                    error << " (" << (*validation)[0].get<std::string>()
                          << ")";
                }
            }
        }
    }
    error << "\n";
    return exit_code;
}

std::optional<json> ReadJsonFile(
    const std::filesystem::path& path,
    std::string& message)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        message =
            "Could not open parameter file '" + path.generic_string() + "'.";
        return std::nullopt;
    }
    const std::string text{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    auto value = json::parse(text, nullptr, false, true);
    if (value.is_discarded())
    {
        message =
            "Parameter file '" + path.generic_string()
            + "' does not contain valid JSON.";
        return std::nullopt;
    }
    return value;
}

std::optional<json> ConvertScalar(
    const json& schema,
    std::string raw,
    std::string& message)
{
    if (raw.starts_with("@@"))
    {
        raw.erase(0, 1);
    }
    else if (raw.starts_with("@"))
    {
        const auto from_file =
            ReadJsonFile(ue::cli::Utf8Path(raw.substr(1)), message);
        if (!from_file)
        {
            return std::nullopt;
        }
        const std::string type =
            schema.value("type", std::string{});
        if ((type == "object" && !from_file->is_object())
            || (type == "array" && !from_file->is_array())
            || (type == "string" && !from_file->is_string())
            || (type == "boolean" && !from_file->is_boolean())
            || (type == "integer" && !from_file->is_number_integer())
            || (type == "number" && !from_file->is_number()))
        {
            message =
                "Parameter file JSON does not match schema type '" + type + "'.";
            return std::nullopt;
        }
        return *from_file;
    }

    if (schema.contains("const"))
    {
        const json& expected = schema["const"];
        auto parsed = json::parse(raw, nullptr, false, true);
        if (parsed.is_discarded() && expected.is_string())
        {
            parsed = raw;
        }
        if (parsed != expected)
        {
            message =
                "Value must equal schema const " + expected.dump() + ".";
            return std::nullopt;
        }
        return parsed;
    }

    const std::string type =
        schema.value("type", std::string{});
    if (type == "string")
    {
        return json(std::move(raw));
    }
    if (type == "boolean")
    {
        if (raw == "true" || raw == "1")
        {
            return json(true);
        }
        if (raw == "false" || raw == "0")
        {
            return json(false);
        }
        message = "Boolean value must be true or false.";
        return std::nullopt;
    }
    if (type == "integer")
    {
        std::int64_t value = 0;
        const auto parsed = std::from_chars(
            raw.data(),
            raw.data() + raw.size(),
            value);
        if (parsed.ec != std::errc{}
            || parsed.ptr != raw.data() + raw.size())
        {
            message = "Integer parameter contains an invalid value.";
            return std::nullopt;
        }
        return json(value);
    }
    if (type == "number")
    {
        double value = 0.0;
        const auto parsed = std::from_chars(
            raw.data(),
            raw.data() + raw.size(),
            value);
        if (parsed.ec != std::errc{}
            || parsed.ptr != raw.data() + raw.size())
        {
            message = "Number parameter contains an invalid value.";
            return std::nullopt;
        }
        return json(value);
    }
    if (type == "object")
    {
        auto value = json::parse(raw, nullptr, false, true);
        if (!value.is_object())
        {
            message = "Object parameter must contain a JSON object.";
            return std::nullopt;
        }
        return value;
    }

    auto value = json::parse(raw, nullptr, false, true);
    return value.is_discarded()
        ? std::optional<json>(json(std::move(raw)))
        : std::optional<json>(std::move(value));
}

std::optional<json> ConvertArrayValue(
    const json& schema,
    const std::string& raw,
    std::string& message)
{
    if (raw.starts_with("@") && !raw.starts_with("@@"))
    {
        const auto from_file =
            ReadJsonFile(ue::cli::Utf8Path(raw.substr(1)), message);
        if (!from_file)
        {
            return std::nullopt;
        }
        return *from_file;
    }
    auto parsed = json::parse(raw, nullptr, false, true);
    if (parsed.is_array())
    {
        return parsed;
    }
    const json items =
        schema.value("items", json::object());
    return ConvertScalar(items, raw, message);
}

std::string GenerateRequestId()
{
    std::array<std::uint32_t, 4> values{};
    std::random_device random;
    for (auto& value : values)
    {
        value = random();
    }
    std::ostringstream stream;
    stream << "ue-" << std::hex << std::setfill('0')
           << std::setw(8) << values[0] << "-"
           << std::setw(8) << values[1] << "-"
           << std::setw(8) << values[2] << "-"
           << std::setw(8) << values[3];
    return stream.str();
}

std::optional<json> ExtractCapability(
    const ParsedEnvelope& envelope)
{
    if (!envelope.ok)
    {
        return std::nullopt;
    }
    const auto data = envelope.value.find("data");
    if (data == envelope.value.end() || !data->is_object())
    {
        return std::nullopt;
    }
    const auto capabilities = data->find("capabilities");
    if (capabilities == data->end()
        || !capabilities->is_array()
        || capabilities->size() != 1
        || !(*capabilities)[0].is_object())
    {
        return std::nullopt;
    }
    return (*capabilities)[0];
}

std::string CapabilityPath(const std::string_view capability)
{
    return "/api/capabilities?operation="
        + ue::api::UrlEncode(capability)
        + "&detail=full";
}

std::string ShortValue(const json& value)
{
    if (value.is_string())
    {
        std::string text = value.get<std::string>();
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');
        if (text.size() > 120)
        {
            text.resize(117);
            text += "...";
        }
        if (text.find_first_of(" \t") != std::string::npos)
        {
            return json(text).dump();
        }
        return text;
    }
    if (value.is_boolean())
    {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number() || value.is_null())
    {
        return value.dump();
    }
    if (value.is_array())
    {
        return std::to_string(value.size()) + " items";
    }
    return std::to_string(value.size()) + " fields";
}

struct SummaryCandidate
{
    std::string path;
    std::string value;
    int priority = 100;
};

int SummaryPriority(const std::string_view path)
{
    static constexpr std::array<std::string_view, 11> preferred = {
        "status",
        "state",
        "action",
        "message",
        "id",
        "jobId",
        "runId",
        "assetPath",
        "name",
        "count",
        "requested",
    };
    const auto dot = path.rfind('.');
    const std::string_view leaf =
        dot == std::string_view::npos ? path : path.substr(dot + 1);
    for (std::size_t index = 0; index < preferred.size(); ++index)
    {
        if (leaf == preferred[index])
        {
            return static_cast<int>(index);
        }
    }
    return 100;
}

bool IsPayloadField(const std::string_view field)
{
    std::string lower(field);
    std::transform(
        lower.begin(),
        lower.end(),
        lower.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return lower.find("base64") != std::string::npos;
}

void CollectSummary(
    const json& value,
    const std::string& prefix,
    const int depth,
    std::vector<SummaryCandidate>& output,
    bool& omitted_payload)
{
    if (!value.is_object())
    {
        return;
    }
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
    {
        if (IsPayloadField(iterator.key()))
        {
            omitted_payload = true;
            continue;
        }
        const std::string path =
            prefix.empty()
                ? iterator.key()
                : prefix + "." + iterator.key();
        if (iterator->is_object()
            && depth < 1
            && !iterator->empty())
        {
            CollectSummary(
                *iterator,
                path,
                depth + 1,
                output,
                omitted_payload);
            continue;
        }
        output.push_back({
            path,
            ShortValue(*iterator),
            SummaryPriority(path),
        });
    }
}

std::optional<std::vector<std::uint8_t>> DecodeBase64(
    const std::string_view encoded)
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
        if (std::isspace(character))
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
    std::vector<std::uint8_t> result;
    result.reserve(encoded_length / 4 * 3 - padding);
    std::uint32_t accumulator = 0;
    int bits = -8;
    for (const unsigned char character : encoded)
    {
        if (std::isspace(character) || character == '=')
        {
            continue;
        }
        accumulator = (accumulator << 6)
            | static_cast<std::uint32_t>(table[character]);
        bits += 6;
        if (bits >= 0)
        {
            result.push_back(static_cast<std::uint8_t>(
                (accumulator >> bits) & 0xffU));
            bits -= 8;
        }
    }
    if (result.size() != encoded_length / 4 * 3 - padding)
    {
        return std::nullopt;
    }
    return result;
}

struct Base64Payload
{
    json* container = nullptr;
    std::string field;
    std::string value;
};

std::optional<Base64Payload> FindBase64Payload(json& envelope)
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
    const auto inspect = [&](json& candidate)
        -> std::optional<Base64Payload>
    {
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

class Sha256
{
public:
    void Update(const std::uint8_t* data, std::size_t length)
    {
        for (std::size_t index = 0; index < length; ++index)
        {
            buffer_[buffer_size_++] = data[index];
            if (buffer_size_ == 64)
            {
                Transform(buffer_.data());
                bit_length_ += 512;
                buffer_size_ = 0;
            }
        }
    }

    std::string Finish()
    {
        std::size_t index = buffer_size_;
        buffer_[index++] = 0x80;
        if (index > 56)
        {
            while (index < 64)
            {
                buffer_[index++] = 0;
            }
            Transform(buffer_.data());
            index = 0;
        }
        while (index < 56)
        {
            buffer_[index++] = 0;
        }
        bit_length_ += static_cast<std::uint64_t>(buffer_size_) * 8U;
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            buffer_[index++] =
                static_cast<std::uint8_t>(bit_length_ >> shift);
        }
        Transform(buffer_.data());
        std::ostringstream output;
        output << "sha256:" << std::hex << std::setfill('0');
        for (const std::uint32_t value : state_)
        {
            output << std::setw(8) << value;
        }
        return output.str();
    }

private:
    static std::uint32_t Rotate(
        const std::uint32_t value,
        const std::uint32_t amount)
    {
        return (value >> amount) | (value << (32U - amount));
    }

    void Transform(const std::uint8_t* block)
    {
        static constexpr std::array<std::uint32_t, 64> constants = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16; ++index)
        {
            schedule[index] =
                static_cast<std::uint32_t>(block[index * 4]) << 24U
                | static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U
                | static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U
                | static_cast<std::uint32_t>(block[index * 4 + 3]);
        }
        for (std::size_t index = 16; index < 64; ++index)
        {
            const std::uint32_t s0 =
                Rotate(schedule[index - 15], 7)
                ^ Rotate(schedule[index - 15], 18)
                ^ (schedule[index - 15] >> 3U);
            const std::uint32_t s1 =
                Rotate(schedule[index - 2], 17)
                ^ Rotate(schedule[index - 2], 19)
                ^ (schedule[index - 2] >> 10U);
            schedule[index] =
                schedule[index - 16] + s0
                + schedule[index - 7] + s1;
        }
        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < 64; ++index)
        {
            const std::uint32_t sum1 =
                Rotate(e, 6) ^ Rotate(e, 11) ^ Rotate(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 =
                h + sum1 + choose + constants[index] + schedule[index];
            const std::uint32_t sum0 =
                Rotate(a, 2) ^ Rotate(a, 13) ^ Rotate(a, 22);
            const std::uint32_t majority =
                (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_ = {
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t bit_length_ = 0;
};

std::optional<std::string> HashFile(
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    Sha256 hash;
    std::array<std::uint8_t, 65536> buffer{};
    while (stream)
    {
        stream.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0)
        {
            hash.Update(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    if (!stream.eof())
    {
        return std::nullopt;
    }
    return hash.Finish();
}

ParsedEnvelope ExportPayload(
    ue::api::Client& client,
    const std::string& capability,
    json params,
    const std::string& request_id,
    ParsedEnvelope initial,
    const std::filesystem::path& output_path)
{
    std::error_code file_error;
    if (!output_path.parent_path().empty())
    {
        std::filesystem::create_directories(
            output_path.parent_path(),
            file_error);
    }
    if (file_error)
    {
        return {
            false,
            json(),
            "artifact_write_failed",
            "The artifact output directory could not be created.",
        };
    }
    std::filesystem::path temporary = output_path;
    temporary += ".tmp";
    std::ofstream output(
        temporary,
        std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return {
            false,
            json(),
            "artifact_write_failed",
            "The temporary artifact file could not be opened.",
        };
    }

    json projected;
    std::string source_field;
    std::size_t total_bytes = 0;
    std::size_t chunks = 0;
    std::optional<std::uint64_t> expected_size;
    std::optional<std::string> expected_sha256;
    bool source_sha_deferred = false;
    ParsedEnvelope current = std::move(initial);
    for (;;)
    {
        if (!current.ok || !current.value.is_object())
        {
            output.close();
            std::filesystem::remove(temporary, file_error);
            return current;
        }
        auto payload = FindBase64Payload(current.value);
        if (!payload)
        {
            output.close();
            std::filesystem::remove(temporary, file_error);
            return {
                false,
                json(),
                "artifact_payload_missing",
                "--output requires a Base64 image or artifact payload.",
            };
        }
        const auto decoded = DecodeBase64(payload->value);
        if (!decoded)
        {
            output.close();
            std::filesystem::remove(temporary, file_error);
            return {
                false,
                json(),
                "artifact_payload_invalid",
                "The Editor returned invalid Base64 content.",
            };
        }
        json* data = &current.value;
        if (current.value.contains("data")
            && current.value["data"].is_object())
        {
            data = &current.value["data"];
        }
        const bool chunked =
            data->contains("offset")
            || data->contains("nextOffset")
            || data->contains("sizeBytes");
        if (chunked)
        {
            if (!data->contains("offset")
                || !data->contains("nextOffset")
                || !data->contains("sizeBytes")
                || !(*data)["offset"].is_number_integer()
                || !(*data)["nextOffset"].is_number_integer()
                || !(*data)["sizeBytes"].is_number_integer())
            {
                output.close();
                std::filesystem::remove(temporary, file_error);
                return {
                    false,
                    json(),
                    "artifact_cursor_invalid",
                    "The Editor returned incomplete artifact cursor fields.",
                };
            }
            const auto is_non_negative_integer = [](const json& value)
            {
                return value.is_number_unsigned()
                    || (value.is_number_integer()
                        && value.get<std::int64_t>() >= 0);
            };
            if (!is_non_negative_integer((*data)["offset"])
                || !is_non_negative_integer((*data)["nextOffset"])
                || !is_non_negative_integer((*data)["sizeBytes"]))
            {
                output.close();
                std::filesystem::remove(temporary, file_error);
                return {
                    false,
                    json(),
                    "artifact_cursor_invalid",
                    "Artifact cursor fields must be non-negative integers.",
                };
            }
            const auto offset =
                (*data)["offset"].get<std::uint64_t>();
            const auto next_offset =
                (*data)["nextOffset"].get<std::uint64_t>();
            const auto size_bytes =
                (*data)["sizeBytes"].get<std::uint64_t>();
            const auto expected_next =
                static_cast<std::uint64_t>(total_bytes + decoded->size());
            if (offset != total_bytes
                || next_offset != expected_next
                || next_offset > size_bytes)
            {
                output.close();
                std::filesystem::remove(temporary, file_error);
                return {
                    false,
                    json(),
                    "artifact_cursor_invalid",
                    "The Editor returned inconsistent artifact offsets.",
                };
            }
            if (!expected_size)
            {
                expected_size = size_bytes;
                if (data->contains("sha256")
                    && (*data)["sha256"].is_string())
                {
                    expected_sha256 =
                        (*data)["sha256"].get<std::string>();
                }
                source_sha_deferred =
                    data->value("sha256Deferred", false);
            }
            else if (*expected_size != size_bytes)
            {
                output.close();
                std::filesystem::remove(temporary, file_error);
                return {
                    false,
                    json(),
                    "artifact_changed",
                    "The artifact size changed between chunks.",
                };
            }
        }

        output.write(
            reinterpret_cast<const char*>(decoded->data()),
            static_cast<std::streamsize>(decoded->size()));
        if (!output)
        {
            output.close();
            std::filesystem::remove(temporary, file_error);
            return {
                false,
                json(),
                "artifact_write_failed",
                "The artifact chunk could not be written.",
            };
        }
        total_bytes += decoded->size();
        ++chunks;
        if (projected.is_null())
        {
            source_field = payload->field;
            payload->container->erase(payload->field);
            projected = current.value;
        }

        const bool eof = !chunked || data->value("eof", true);
        if (chunked
            && eof
            && expected_size
            && total_bytes != *expected_size)
        {
            output.close();
            std::filesystem::remove(temporary, file_error);
            return {
                false,
                json(),
                "artifact_cursor_invalid",
                "The Editor signaled EOF before the declared artifact size.",
            };
        }
        if (chunked
            && !eof
            && expected_size
            && total_bytes >= *expected_size)
        {
            output.close();
            std::filesystem::remove(temporary, file_error);
            return {
                false,
                json(),
                "artifact_cursor_invalid",
                "The Editor did not signal EOF at the declared artifact size.",
            };
        }
        if (eof)
        {
            break;
        }
        params["offset"] = total_bytes;
        const json request = {
            { "capability", capability },
            { "params", params },
        };
        json request_with_id = request;
        if (!request_id.empty())
        {
            request_with_id["requestId"] = request_id;
        }
        current = ParseEnvelope(
            client.Post("/api/execute", request_with_id.dump()));
    }

    output.close();
    if (!output)
    {
        std::filesystem::remove(temporary, file_error);
        return {
            false,
            json(),
            "artifact_write_failed",
            "The artifact file could not be finalized.",
        };
    }
    if (expected_size
        && total_bytes != static_cast<std::size_t>(*expected_size))
    {
        std::filesystem::remove(temporary, file_error);
        return {
            false,
            json(),
            "artifact_size_mismatch",
            "The exported byte count does not match sizeBytes.",
        };
    }
    const auto digest = HashFile(temporary);
    if (!digest)
    {
        std::filesystem::remove(temporary, file_error);
        return {
            false,
            json(),
            "artifact_hash_failed",
            "The exported artifact could not be hashed.",
        };
    }
    const auto normalize = [](std::string value)
    {
        if (!value.starts_with("sha256:"))
        {
            value = "sha256:" + value;
        }
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](const unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
        return value;
    };
    if (expected_sha256
        && normalize(*expected_sha256) != normalize(*digest))
    {
        std::filesystem::remove(temporary, file_error);
        return {
            false,
            json(),
            "artifact_hash_mismatch",
            "The exported artifact does not match the registered SHA-256.",
        };
    }
    std::filesystem::rename(temporary, output_path, file_error);
    if (file_error)
    {
        std::filesystem::remove(output_path, file_error);
        file_error.clear();
        std::filesystem::rename(temporary, output_path, file_error);
    }
    if (file_error)
    {
        std::filesystem::remove(temporary, file_error);
        return {
            false,
            json(),
            "artifact_write_failed",
            "The artifact could not be moved into place.",
        };
    }
    json* data = &projected;
    if (projected.contains("data") && projected["data"].is_object())
    {
        data = &projected["data"];
    }
    (*data)["artifactExport"] = {
        { "path",
            std::filesystem::absolute(output_path).generic_string() },
        { "bytes", total_bytes },
        { "chunks", chunks },
        { "sha256", *digest },
        { "verifiedAgainstReceipt", expected_sha256.has_value() },
        { "sourceSha256Deferred", source_sha_deferred },
        { "sourceEncoding", "base64" },
        { "sourceField", source_field },
    };
    return { true, std::move(projected), {}, {} };
}

int PrintSuccess(
    const std::string_view capability,
    const ParsedEnvelope& envelope,
    const bool json_output,
    std::ostream& output)
{
    output
        << (json_output
                ? envelope.value.dump()
                : FormatSuccessSummary(capability, envelope.value, 1023))
        << "\n";
    return 0;
}

std::string CapabilityHelp(const json& descriptor)
{
    std::ostringstream output;
    const std::string id =
        descriptor.value("id", std::string{});
    output << id << "\n";
    output << descriptor.value("description", std::string{}) << "\n";
    output << "kind=" << descriptor.value("kind", "unknown");
    if (descriptor.contains("available"))
    {
        output << " available="
               << (descriptor.value("available", true)
                       ? "true"
                       : "false");
    }
    std::string risk = descriptor.value("risk", std::string{});
    if (risk.empty()
        && descriptor.contains("dsl")
        && descriptor["dsl"].is_object())
    {
        risk = descriptor["dsl"].value("risk", std::string{});
    }
    output << " risk=" << (risk.empty() ? "notDeclared" : risk);
    const json traits =
        descriptor.value("traits", json::object());
    if (traits.is_object() && !traits.empty())
    {
        output << " traits="
               << (traits.value("readOnly", false) ? "readOnly" : "write");
        if (traits.value("destructive", false))
        {
            output << ",destructive";
        }
        if (traits.value("expensive", false))
        {
            output << ",expensive";
        }
    }
    output << "\n\nParameters:\n";
    const json schema =
        descriptor.value("inputSchema", json::object());
    const json properties =
        schema.value("properties", json::object());
    const std::set<std::string> required = [&]()
    {
        std::set<std::string> values;
        for (const auto& item :
             schema.value("required", json::array()))
        {
            if (item.is_string())
            {
                values.insert(item.get<std::string>());
            }
        }
        return values;
    }();
    if (properties.empty())
    {
        output << "  (none)\n";
    }
    for (auto iterator = properties.begin();
         iterator != properties.end();
         ++iterator)
    {
        if (iterator.key() == "requestId")
        {
            continue;
        }
        output << "  --" << CamelToKebab(iterator.key())
               << " <"
               << iterator->value(
                      "type",
                      iterator->contains("const")
                          ? "const"
                          : "json")
               << ">";
        if (required.contains(iterator.key()))
        {
            output << " required";
        }
        output << "\n";
        const std::string description =
            iterator->value("description", std::string{});
        if (!description.empty())
        {
            output << "      " << description << "\n";
        }
    }
    return output.str();
}

int RunLiveCapabilities(
    ue::api::Client& client,
    const Options& options,
    std::ostream& output,
    std::ostream& error)
{
    static const std::map<std::string, std::string> supported = {
        { "query", "query" },
        { "operation", "operation" },
        { "domain", "domain" },
        { "kind", "kind" },
        { "read-only", "readOnly" },
        { "destructive", "destructive" },
        { "expensive", "expensive" },
        { "output-kind", "outputKind" },
        { "risk", "risk" },
        { "available-only", "availableOnly" },
        { "offset", "offset" },
        { "limit", "limit" },
        { "detail", "detail" },
    };
    std::string path = "/api/capabilities";
    bool first = true;
    for (const RawOption& option : options.raw_options)
    {
        const auto mapped = supported.find(option.name);
        if (mapped == supported.end())
        {
            return PrintFailure(
                {
                    false,
                    json(),
                    "invalid_arguments",
                    "Unknown capabilities option --" + option.name + ".",
                },
                options.json_output,
                kExitUsage,
                output,
                error);
        }
        std::string value;
        if (option.negated)
        {
            value = "false";
        }
        else if (option.value)
        {
            value = *option.value;
        }
        else
        {
            value = "true";
        }
        path += first ? "?" : "&";
        first = false;
        path += ue::api::UrlEncode(mapped->second);
        path += "=";
        path += ue::api::UrlEncode(value);
    }
    const ParsedEnvelope response =
        ParseEnvelope(client.Get(path));
    if (!response.ok)
    {
        return PrintFailure(
            response,
            options.json_output,
            response.code == "editor_unreachable"
                ? kExitUnavailable
                : kExitExecution,
            output,
            error);
    }
    return PrintSuccess(
        "capabilities",
        response,
        options.json_output,
        output);
}

std::string Lower(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string DescriptorRisk(const json& descriptor)
{
    if (descriptor.contains("risk")
        && descriptor["risk"].is_string())
    {
        return descriptor["risk"].get<std::string>();
    }
    if (descriptor.contains("dsl")
        && descriptor["dsl"].is_object())
    {
        return descriptor["dsl"].value("risk", std::string{});
    }
    return {};
}

json LocalCapabilitySummary(const json& descriptor)
{
    json summary = {
        { "id", descriptor.value("id", std::string{}) },
        { "domain", descriptor.value("domain", std::string{}) },
        { "kind", descriptor.value("kind", std::string{}) },
        { "description",
            descriptor.value("description", std::string{}) },
        { "traits",
            descriptor.value("traits", json::object()) },
        { "output",
            descriptor.value("output", json::object()) },
        { "available", nullptr },
        { "availabilityReasons",
            json::array({ "editor_validation_deferred" }) },
    };
    const std::string risk = DescriptorRisk(descriptor);
    if (!risk.empty())
    {
        summary["risk"] = risk;
    }
    return summary;
}

ue::workflow::CapabilitySearchDocument CapabilitySearchDocument(
    const json& descriptor)
{
    ue::workflow::CapabilitySearchDocument document;
    document.id = descriptor.value("id", std::string{});
    document.description =
        descriptor.value("description", std::string{});
    const auto search = descriptor.find("search");
    if (search == descriptor.end() || !search->is_object())
    {
        return document;
    }
    document.title = search->value("title", std::string{});
    const auto read_array =
        [&search](const char* field)
    {
        std::vector<std::string> values;
        const auto iterator = search->find(field);
        if (iterator == search->end() || !iterator->is_array())
        {
            return values;
        }
        for (const auto& value : *iterator)
        {
            if (value.is_string())
            {
                values.push_back(value.get<std::string>());
            }
        }
        return values;
    };
    document.keywords = read_array("keywords");
    document.aliases = read_array("aliases");
    return document;
}

json CapabilitySearchMatchJson(
    const ue::workflow::CapabilitySearchMatch& match)
{
    return {
        { "score", match.score },
        { "matchedFields", match.matched_fields },
        { "matchedTokens", match.matched_tokens },
    };
}

std::optional<bool> ParseBooleanOption(
    const RawOption& option,
    std::string& error)
{
    if (option.negated)
    {
        return false;
    }
    if (!option.value)
    {
        return true;
    }
    if (*option.value == "true" || *option.value == "1")
    {
        return true;
    }
    if (*option.value == "false" || *option.value == "0")
    {
        return false;
    }
    error =
        "--" + option.name + " must be true or false.";
    return std::nullopt;
}

int RunLocalCapabilities(
    const CapabilityCatalog& catalog,
    const Options& options,
    std::ostream& output,
    std::ostream& error)
{
    std::string query;
    std::string operation;
    std::string domain;
    std::string kind;
    std::string output_kind;
    std::string risk;
    std::string detail = "summary";
    std::optional<bool> read_only;
    std::optional<bool> destructive;
    std::optional<bool> expensive;
    std::size_t offset = 0;
    std::size_t limit = 25;

    const auto fail = [&](const std::string& message)
    {
        return PrintFailure(
            {
                false,
                json(),
                "invalid_arguments",
                message,
            },
            options.json_output,
            kExitUsage,
            output,
            error);
    };
    std::set<std::string> assigned;
    for (const RawOption& option : options.raw_options)
    {
        if (!assigned.insert(option.name).second)
        {
            return fail(
                "Capabilities option --" + option.name
                + " cannot be repeated.");
        }
        const auto require_value =
            [&]() -> std::optional<std::string>
        {
            if (!option.value || option.negated)
            {
                return std::nullopt;
            }
            return *option.value;
        };
        if (option.name == "available-only")
        {
            return fail(
                "--available-only requires --live-schema because "
                "availability is Editor-specific.");
        }
        if (option.name == "read-only"
            || option.name == "destructive"
            || option.name == "expensive")
        {
            std::string parse_error;
            const auto value =
                ParseBooleanOption(option, parse_error);
            if (!value)
            {
                return fail(parse_error);
            }
            if (option.name == "read-only")
            {
                read_only = *value;
            }
            else if (option.name == "destructive")
            {
                destructive = *value;
            }
            else
            {
                expensive = *value;
            }
            continue;
        }
        const auto value = require_value();
        if (!value)
        {
            return fail(
                "--" + option.name + " requires a value.");
        }
        if (option.name == "query")
        {
            query = *value;
        }
        else if (option.name == "operation")
        {
            operation = *value;
        }
        else if (option.name == "domain")
        {
            domain = *value;
        }
        else if (option.name == "kind")
        {
            kind = *value;
        }
        else if (option.name == "output-kind")
        {
            output_kind = *value;
        }
        else if (option.name == "risk")
        {
            risk = *value;
        }
        else if (option.name == "detail")
        {
            if (*value != "summary" && *value != "full")
            {
                return fail("--detail must be summary or full.");
            }
            detail = *value;
        }
        else if (option.name == "offset"
            || option.name == "limit")
        {
            std::uint64_t parsed = 0;
            const auto result = std::from_chars(
                value->data(),
                value->data() + value->size(),
                parsed);
            if (result.ec != std::errc{}
                || result.ptr != value->data() + value->size()
                || parsed
                    > std::numeric_limits<std::size_t>::max()
                || (option.name == "limit"
                    && (parsed == 0 || parsed > 100)))
            {
                return fail(
                    option.name == "limit"
                        ? "--limit must be between 1 and 100."
                        : "--offset must be a non-negative integer.");
            }
            if (option.name == "offset")
            {
                offset = static_cast<std::size_t>(parsed);
            }
            else
            {
                limit = static_cast<std::size_t>(parsed);
            }
        }
        else
        {
            return fail(
                "Unknown capabilities option --"
                + option.name + ".");
        }
    }

    struct RankedCapability
    {
        const json* descriptor = nullptr;
        std::optional<ue::workflow::CapabilitySearchMatch> match;
    };
    std::vector<RankedCapability> matches;
    for (const auto& [id, descriptor] : catalog.Descriptors())
    {
        const json traits =
            descriptor.value("traits", json::object());
        const json output_descriptor =
            descriptor.value("output", json::object());
        if ((!operation.empty() && id != operation)
            || (!domain.empty()
                && descriptor.value("domain", std::string{})
                    != domain)
            || (!kind.empty()
                && descriptor.value("kind", std::string{})
                    != kind)
            || (!output_kind.empty()
                && output_descriptor.value("kind", std::string{})
                    != output_kind)
            || (!risk.empty()
                && DescriptorRisk(descriptor) != risk)
            || (read_only
                && traits.value("readOnly", false) != *read_only)
            || (destructive
                && traits.value("destructive", false) != *destructive)
            || (expensive
                && traits.value("expensive", false) != *expensive)
            )
        {
            continue;
        }
        std::optional<ue::workflow::CapabilitySearchMatch> search_match;
        if (!query.empty())
        {
            search_match = ue::workflow::MatchCapabilitySearch(
                query,
                CapabilitySearchDocument(descriptor));
            if (!search_match)
            {
                continue;
            }
        }
        matches.push_back({
            &descriptor,
            std::move(search_match),
        });
    }
    std::sort(
        matches.begin(),
        matches.end(),
        [&query](
            const RankedCapability& left,
            const RankedCapability& right)
        {
            if (!query.empty()
                && left.match->score != right.match->score)
            {
                return left.match->score > right.match->score;
            }
            return left.descriptor->value("id", std::string{})
                < right.descriptor->value("id", std::string{});
        });

    json capabilities = json::array();
    const std::size_t end = offset < matches.size()
        ? offset + std::min(limit, matches.size() - offset)
        : matches.size();
    if (offset < matches.size())
    {
        for (std::size_t index = offset; index < end; ++index)
        {
            json projected =
                detail == "full"
                    ? *matches[index].descriptor
                    : LocalCapabilitySummary(
                        *matches[index].descriptor);
            if (matches[index].match)
            {
                projected["match"] =
                    CapabilitySearchMatchJson(
                        *matches[index].match);
            }
            capabilities.push_back(std::move(projected));
        }
    }
    ParsedEnvelope response{
        true,
        {
            { "ok", true },
            { "data", {
                { "capabilities", std::move(capabilities) },
                { "total", matches.size() },
                { "offset", offset },
                { "limit", limit },
                { "hasMore", end < matches.size() },
                { "detail", detail },
                { "source", "local" },
                { "capabilityRoot",
                    catalog.Root().generic_string() },
            } },
        },
        {},
        {},
    };
    return PrintSuccess(
        "capabilities",
        response,
        options.json_output,
        output);
}

bool ArrayContainsString(
    const json& values,
    const std::string& expected)
{
    return values.is_array()
        && std::any_of(
            values.begin(),
            values.end(),
            [&](const json& value)
            {
                return value.is_string()
                    && value.get_ref<const std::string&>() == expected;
            });
}

json RecipeOperations(const json& recipe)
{
    std::set<std::string> unique;
    for (const auto& step :
         recipe.value("steps", json::array()))
    {
        for (const auto& operation :
             step.value("operations", json::array()))
        {
            if (operation.is_string())
            {
                unique.insert(operation.get<std::string>());
            }
        }
    }
    return json(unique);
}

json LocalRecipeSummary(const json& recipe)
{
    return {
        { "id", recipe.value("id", std::string{}) },
        { "title", recipe.value("title", std::string{}) },
        { "description",
            recipe.value("description", std::string{}) },
        { "risk", recipe.value("risk", std::string{}) },
        { "operations", RecipeOperations(recipe) },
        { "result", recipe.value("result", json::object()) },
    };
}

json LocalSkillSummary(const json& skill)
{
    json recipes = json::array();
    for (const auto& recipe :
         skill.value("recipes", json::array()))
    {
        recipes.push_back(LocalRecipeSummary(recipe));
    }
    return {
        { "id", skill.value("id", std::string{}) },
        { "version", skill.value("version", std::string{}) },
        { "title", skill.value("title", std::string{}) },
        { "description",
            skill.value("description", std::string{}) },
        { "domains", skill.value("domains", json::array()) },
        { "risk", skill.value("risk", std::string{}) },
        { "triggers", skill.value("triggers", json::array()) },
        { "entrypoint",
            skill.value("entrypoint", std::string{}) },
        { "requirements",
            skill.value("requirements", json::object()) },
        { "recipes", std::move(recipes) },
    };
}

bool SkillMatchesQuery(
    const json& skill,
    const std::string& query)
{
    if (query.empty())
    {
        return true;
    }
    std::string searchable =
        skill.value("id", std::string{}) + " "
        + skill.value("title", std::string{}) + " "
        + skill.value("description", std::string{});
    for (const auto& trigger :
         skill.value("triggers", json::array()))
    {
        if (trigger.is_string())
        {
            searchable += " " + trigger.get<std::string>();
        }
    }
    for (const auto& recipe :
         skill.value("recipes", json::array()))
    {
        searchable += " "
            + recipe.value("id", std::string{}) + " "
            + recipe.value("title", std::string{}) + " "
            + recipe.value("description", std::string{});
    }
    searchable = Lower(searchable);

    std::istringstream tokens(query);
    std::string token;
    while (tokens >> token)
    {
        while (!token.empty()
            && static_cast<unsigned char>(token.front()) < 0x80
            && !std::isalnum(
                static_cast<unsigned char>(token.front()))
            && token.front() != '.'
            && token.front() != '_'
            && token.front() != '-')
        {
            token.erase(token.begin());
        }
        while (!token.empty()
            && static_cast<unsigned char>(token.back()) < 0x80
            && !std::isalnum(
                static_cast<unsigned char>(token.back()))
            && token.back() != '.'
            && token.back() != '_'
            && token.back() != '-')
        {
            token.pop_back();
        }
        if (!token.empty()
            && searchable.find(token) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

int RunLocalSkills(
    const SkillCatalog& catalog,
    const CapabilityCatalog& capabilities,
    const Options& options,
    std::ostream& output,
    std::ostream& error)
{
    const auto fail = [&](const std::string& message)
    {
        return PrintFailure(
            {
                false,
                json(),
                "invalid_arguments",
                message,
            },
            options.json_output,
            kExitUsage,
            output,
            error);
    };
    if (options.live_schema)
    {
        return fail(
            "ue skills is local-only; remove --live-schema.");
    }

    std::string query;
    std::string name;
    std::string recipe;
    std::string domain;
    std::string operation;
    std::string risk;
    std::string detail = "summary";
    std::size_t offset = 0;
    std::size_t limit = 25;
    std::set<std::string> assigned;
    for (const RawOption& option : options.raw_options)
    {
        if (!assigned.insert(option.name).second)
        {
            return fail(
                "Skills option --" + option.name
                + " cannot be repeated.");
        }
        if (!option.value || option.negated)
        {
            return fail(
                "--" + option.name + " requires a value.");
        }
        const std::string& value = *option.value;
        if (option.name == "query")
        {
            query = Lower(value);
        }
        else if (option.name == "name")
        {
            name = value;
        }
        else if (option.name == "recipe")
        {
            recipe = value;
        }
        else if (option.name == "domain")
        {
            domain = value;
        }
        else if (option.name == "operation")
        {
            operation = value;
        }
        else if (option.name == "risk")
        {
            risk = value;
        }
        else if (option.name == "detail")
        {
            if (value != "summary" && value != "full")
            {
                return fail("--detail must be summary or full.");
            }
            detail = value;
        }
        else if (option.name == "offset"
            || option.name == "limit")
        {
            std::uint64_t parsed = 0;
            const auto result = std::from_chars(
                value.data(),
                value.data() + value.size(),
                parsed);
            if (result.ec != std::errc{}
                || result.ptr != value.data() + value.size()
                || parsed
                    > std::numeric_limits<std::size_t>::max()
                || (option.name == "limit"
                    && (parsed == 0 || parsed > 100)))
            {
                return fail(
                    option.name == "limit"
                        ? "--limit must be between 1 and 100."
                        : "--offset must be a non-negative integer.");
            }
            if (option.name == "offset")
            {
                offset = static_cast<std::size_t>(parsed);
            }
            else
            {
                limit = static_cast<std::size_t>(parsed);
            }
        }
        else
        {
            return fail(
                "Unknown skills option --" + option.name + ".");
        }
    }

    std::vector<const json*> matches;
    for (const auto& [id, skill] : catalog.Descriptors())
    {
        bool recipe_match = recipe.empty();
        bool operation_match = operation.empty();
        bool risk_match =
            risk.empty()
            || skill.value("risk", std::string{}) == risk;
        for (const auto& item :
             skill.value("recipes", json::array()))
        {
            recipe_match = recipe_match
                || item.value("id", std::string{}) == recipe;
            risk_match = risk_match
                || item.value("risk", std::string{}) == risk;
            if (!operation_match)
            {
                for (const auto& step :
                     item.value("steps", json::array()))
                {
                    if (ArrayContainsString(
                            step.value(
                                "operations",
                                json::array()),
                            operation))
                    {
                        operation_match = true;
                        break;
                    }
                }
            }
        }
        if ((!name.empty() && id != name)
            || (!domain.empty()
                && !ArrayContainsString(
                    skill.value("domains", json::array()),
                    domain))
            || !recipe_match
            || !operation_match
            || !risk_match
            || !SkillMatchesQuery(skill, query))
        {
            continue;
        }
        matches.push_back(&skill);
    }

    json skills = json::array();
    const std::size_t end = offset < matches.size()
        ? offset + std::min(limit, matches.size() - offset)
        : matches.size();
    if (offset < matches.size())
    {
        for (std::size_t index = offset; index < end; ++index)
        {
            skills.push_back(
                detail == "full"
                    ? *matches[index]
                    : LocalSkillSummary(*matches[index]));
        }
    }
    ParsedEnvelope response{
        true,
        {
            { "ok", true },
            { "data", {
                { "skills", std::move(skills) },
                { "total", matches.size() },
                { "offset", offset },
                { "limit", limit },
                { "hasMore", end < matches.size() },
                { "detail", detail },
                { "source", "local" },
                { "skillRoot",
                    catalog.Root().generic_string() },
                { "capabilityRoot",
                    capabilities.Root().generic_string() },
            } },
        },
        {},
        {},
    };
    return PrintSuccess(
        "skills",
        response,
        options.json_output,
        output);
}

} // namespace

std::string CamelToKebab(const std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 4);
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        const unsigned char character =
            static_cast<unsigned char>(value[index]);
        if (std::isupper(character))
        {
            if (!result.empty() && result.back() != '-')
            {
                result.push_back('-');
            }
            result.push_back(
                static_cast<char>(std::tolower(character)));
        }
        else
        {
            result.push_back(static_cast<char>(character));
        }
    }
    return result;
}

ConversionResult ConvertParameters(
    const json& capability_descriptor,
    const std::vector<RawOption>& options,
    const bool confirm_write)
{
    ConversionResult result;
    const json schema =
        capability_descriptor.value("inputSchema", json::object());
    const json properties =
        schema.value("properties", json::object());
    if (!schema.is_object() || !properties.is_object())
    {
        result.code = "invalid_capability_schema";
        result.message =
            "Capability inputSchema must contain object properties.";
        return result;
    }

    std::map<std::string, std::string> names;
    for (auto iterator = properties.begin();
         iterator != properties.end();
         ++iterator)
    {
        names[iterator.key()] = iterator.key();
        names[CamelToKebab(iterator.key())] = iterator.key();
    }
    result.schema_accepts_request_id =
        properties.contains("requestId");

    std::set<std::string> assigned;
    for (const RawOption& option : options)
    {
        const auto name = names.find(option.name);
        if (name == names.end() || name->second == "requestId")
        {
            result.code = "unknown_parameter";
            result.message =
                "Unknown parameter '--" + option.name + "'.";
            return result;
        }
        const std::string& property_name = name->second;
        const json& property_schema = properties[property_name];
        const bool is_array =
            property_schema.value("type", std::string{}) == "array";
        if (!is_array && assigned.contains(property_name))
        {
            result.code = "duplicate_parameter";
            result.message =
                "Parameter '--" + CamelToKebab(property_name)
                + "' cannot be repeated.";
            return result;
        }
        if (option.negated)
        {
            if (property_schema.value("type", std::string{})
                != "boolean")
            {
                result.code = "invalid_parameter";
                result.message =
                    "'--no-" + option.name
                    + "' is only valid for boolean parameters.";
                return result;
            }
            result.params[property_name] = false;
            assigned.insert(property_name);
            continue;
        }
        if (!option.value)
        {
            if (property_schema.value("type", std::string{})
                    == "boolean"
                || (property_schema.contains("const")
                    && property_schema["const"] == true))
            {
                result.params[property_name] = true;
                assigned.insert(property_name);
                continue;
            }
            result.code = "parameter_value_required";
            result.message =
                "Parameter '--" + CamelToKebab(property_name)
                + "' requires a value.";
            return result;
        }

        std::string conversion_error;
        if (is_array)
        {
            const auto converted = ConvertArrayValue(
                property_schema,
                *option.value,
                conversion_error);
            if (!converted)
            {
                result.code = "invalid_parameter";
                result.message =
                    "Parameter '--" + CamelToKebab(property_name)
                    + "': " + conversion_error;
                return result;
            }
            if (!result.params.contains(property_name))
            {
                result.params[property_name] = json::array();
            }
            if (converted->is_array())
            {
                for (const auto& item : *converted)
                {
                    result.params[property_name].push_back(item);
                }
            }
            else
            {
                result.params[property_name].push_back(*converted);
            }
        }
        else
        {
            const auto converted = ConvertScalar(
                property_schema,
                *option.value,
                conversion_error);
            if (!converted)
            {
                result.code = "invalid_parameter";
                result.message =
                    "Parameter '--" + CamelToKebab(property_name)
                    + "': " + conversion_error;
                return result;
            }
            result.params[property_name] = *converted;
        }
        assigned.insert(property_name);
    }

    if (confirm_write)
    {
        if (!properties.contains("confirmWrite"))
        {
            result.code = "confirm_write_unsupported";
            result.message =
                "--confirm-write is not declared by this capability.";
            return result;
        }
        result.params["confirmWrite"] = true;
        assigned.insert("confirmWrite");
    }

    for (const auto& required :
         schema.value("required", json::array()))
    {
        if (!required.is_string())
        {
            continue;
        }
        const std::string property = required.get<std::string>();
        if (property == "requestId")
        {
            continue;
        }
        if (!result.params.contains(property))
        {
            result.code = "required_parameter_missing";
            result.message =
                "Missing required parameter '--"
                + CamelToKebab(property) + "'.";
            return result;
        }
    }
    result.ok = true;
    return result;
}

std::string FormatSuccessSummary(
    const std::string_view capability,
    const json& envelope,
    const std::size_t max_bytes)
{
    const json* data = &envelope;
    if (envelope.is_object())
    {
        const auto data_field = envelope.find("data");
        if (data_field != envelope.end())
        {
            data = &*data_field;
        }
    }
    std::vector<SummaryCandidate> candidates;
    bool omitted_payload = false;
    if (data->is_object())
    {
        CollectSummary(
            *data,
            {},
            0,
            candidates,
            omitted_payload);
    }
    else
    {
        candidates.push_back({
            "value",
            ShortValue(*data),
            0,
        });
    }
    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const SummaryCandidate& left, const SummaryCandidate& right)
        {
            return left.priority != right.priority
                ? left.priority < right.priority
                : left.path < right.path;
        });

    std::string result = "OK " + std::string(capability);
    std::size_t included = 0;
    bool omitted = omitted_payload;
    for (const SummaryCandidate& candidate : candidates)
    {
        if (included >= 8)
        {
            omitted = true;
            break;
        }
        const std::string addition =
            " " + candidate.path + "=" + candidate.value;
        constexpr std::string_view suffix =
            " ... (--json for full result)";
        if (result.size() + addition.size() + suffix.size()
            > max_bytes)
        {
            omitted = true;
            break;
        }
        result += addition;
        ++included;
    }
    if (included < candidates.size())
    {
        omitted = true;
    }
    if (omitted)
    {
        result += " ... (--json for full result)";
    }
    if (result.size() > max_bytes)
    {
        result.resize(max_bytes);
    }
    return result;
}


namespace
{

int PrintVersion(
    const bool json_output,
    std::ostream& output)
{
    if (json_output)
    {
        output << json({
            { "ok", true },
            { "product", "UE short-operation CLI" },
            { "executable", "ue" },
            { "version", UE_CLI_VERSION },
            { "apiVersion", "v1" },
            { "transport", "loopback-http" },
            { "defaultSchemaSource", "local" },
            { "persistentMode", "shell" },
        }).dump() << "\n";
    }
    else
    {
        output << "ue " << UE_CLI_VERSION << " (API v1)\n";
    }
    return 0;
}

std::optional<CapabilityCatalog> LoadLocalCatalog(
    const Options& options,
    const std::filesystem::path& executable,
    const std::string& capability_hint,
    std::ostream& output,
    std::ostream& error,
    int& exit_code)
{
    std::string resolution_error;
    std::vector<std::filesystem::path> checked;
    const auto root = ResolveCapabilityRoot(
        executable,
        options.capability_root,
        &checked,
        resolution_error);
    if (!root)
    {
        exit_code = PrintFailure(
            {
                false,
                json(),
                "capability_catalog_unavailable",
                resolution_error,
            },
            options.json_output,
            kExitUnavailable,
            output,
            error);
        return std::nullopt;
    }
    std::string load_error;
    auto catalog = capability_hint.empty()
        ? CapabilityCatalog::Load(*root, load_error)
        : CapabilityCatalog::LoadForCapability(
            *root,
            capability_hint,
            load_error);
    if (!catalog)
    {
        exit_code = PrintFailure(
            {
                false,
                json(),
                "capability_catalog_invalid",
                load_error,
            },
            options.json_output,
            kExitUnavailable,
            output,
            error);
        return std::nullopt;
    }
    return catalog;
}

std::optional<SkillCatalog> LoadLocalSkillCatalog(
    const Options& options,
    const std::filesystem::path& executable,
    const CapabilityCatalog& capabilities,
    std::ostream& output,
    std::ostream& error,
    int& exit_code)
{
    std::string resolution_error;
    std::vector<std::filesystem::path> checked;
    const auto root = ResolveSkillRoot(
        executable,
        options.skill_root,
        &checked,
        resolution_error);
    if (!root)
    {
        exit_code = PrintFailure(
            {
                false,
                json(),
                "skill_catalog_unavailable",
                resolution_error,
            },
            options.json_output,
            kExitUnavailable,
            output,
            error);
        return std::nullopt;
    }
    std::string load_error;
    auto catalog = SkillCatalog::Load(
        *root,
        capabilities,
        load_error);
    if (!catalog)
    {
        exit_code = PrintFailure(
            {
                false,
                json(),
                "skill_catalog_invalid",
                load_error,
            },
            options.json_output,
            kExitUnavailable,
            output,
            error);
        return std::nullopt;
    }
    return catalog;
}

std::optional<json> ResolveDescriptor(
    const std::string& capability,
    const Options& options,
    const CapabilityCatalog* catalog,
    ue::api::Client& client,
    std::ostream& output,
    std::ostream& error,
    int& exit_code)
{
    if (!options.live_schema)
    {
        const json* descriptor =
            catalog ? catalog->Find(capability) : nullptr;
        if (!descriptor)
        {
            exit_code = PrintFailure(
                {
                    false,
                    json(),
                    "capability_not_found",
                    "Local capability catalog does not contain '"
                        + capability + "'.",
                },
                options.json_output,
                kExitUsage,
                output,
                error);
            return std::nullopt;
        }
        return *descriptor;
    }

    const ParsedEnvelope descriptor_response =
        ParseEnvelope(client.Get(CapabilityPath(capability)));
    if (!descriptor_response.ok)
    {
        exit_code = PrintFailure(
            descriptor_response,
            options.json_output,
            descriptor_response.code == "editor_unreachable"
                ? kExitUnavailable
                : kExitUsage,
            output,
            error);
        return std::nullopt;
    }
    const auto descriptor =
        ExtractCapability(descriptor_response);
    if (!descriptor)
    {
        exit_code = PrintFailure(
            {
                false,
                descriptor_response.value,
                "invalid_editor_response",
                "Exact capability query did not return one descriptor.",
            },
            options.json_output,
            kExitExecution,
            output,
            error);
        return std::nullopt;
    }
    if (!descriptor->value("available", true))
    {
        exit_code = PrintFailure(
            {
                false,
                json({
                    { "ok", false },
                    { "error", {
                        { "code", "capability_unavailable" },
                        { "message",
                            "Capability is unavailable in this Editor." },
                        { "details",
                            descriptor->value(
                                "availabilityReasons",
                                json::array()) },
                    } },
                }),
                "capability_unavailable",
                "Capability is unavailable in this Editor.",
            },
            options.json_output,
            kExitUnavailable,
            output,
            error);
        return std::nullopt;
    }
    return descriptor;
}

int ExecuteOptions(
    const Options& options,
    const CapabilityCatalog* catalog,
    const SkillCatalog* skill_catalog,
    ue::api::Client& client,
    std::istream& input,
    std::ostream& output,
    std::ostream& error)
{
    if (options.help)
    {
        PrintCommandHelp(options, output);
        return 0;
    }
    if (options.command.empty()
        || options.command == "--help"
        || (options.command == "help"
            && options.help_capability.empty()))
    {
        PrintGeneralHelp(output);
        return 0;
    }
    if (options.command == "--version"
        || options.command == "version")
    {
        return PrintVersion(options.json_output, output);
    }
    if (options.command == "shell")
    {
        return PrintFailure(
            {
                false,
                json(),
                "nested_shell_unsupported",
                "A shell session is already running.",
            },
            options.json_output,
            kExitUsage,
            output,
            error);
    }
    if ((options.params_json || options.params_file)
        && (options.command == "status"
            || options.command == "capabilities"
            || options.command == "help"))
    {
        return PrintFailure(
            {
                false,
                json(),
                "params_option_misplaced",
                "--params and --params-file are only valid for a dotted "
                "capability command.",
            },
            options.json_output,
            kExitUsage,
            output,
            error);
    }
    if (options.command == "status")
    {
        const ParsedEnvelope response =
            ParseEnvelope(client.Get("/api/health"));
        if (!response.ok)
        {
            return PrintFailure(
                response,
                options.json_output,
                response.code == "editor_unreachable"
                    ? kExitUnavailable
                    : kExitExecution,
                output,
                error);
        }
        return PrintSuccess(
            "status",
            response,
            options.json_output,
            output);
    }
    if (options.command == "capabilities")
    {
        if (options.live_schema)
        {
            return RunLiveCapabilities(
                client,
                options,
                output,
                error);
        }
        if (!catalog)
        {
            return PrintFailure(
                {
                    false,
                    json(),
                    "capability_catalog_unavailable",
                    "Local capability catalog was not loaded.",
                },
                options.json_output,
                kExitUnavailable,
                output,
                error);
        }
        return RunLocalCapabilities(
            *catalog,
            options,
            output,
            error);
    }
    if (options.command == "skills")
    {
        if (!skill_catalog || !catalog)
        {
            return PrintFailure(
                {
                    false,
                    json(),
                    "skill_catalog_unavailable",
                    "The local Agent Skill catalog was not loaded.",
                },
                options.json_output,
                kExitUnavailable,
                output,
                error);
        }
        return RunLocalSkills(
            *skill_catalog,
            *catalog,
            options,
            output,
            error);
    }

    const std::string capability =
        options.command == "help"
            ? options.help_capability
            : options.command;
    if (capability.empty() || capability.find('.') == std::string::npos)
    {
        return PrintFailure(
            {
                false,
                json(),
                "invalid_capability",
                "A dotted capability ID is required.",
            },
            options.json_output,
            kExitUsage,
            output,
            error);
    }

    int descriptor_exit = kExitExecution;
    const auto descriptor = ResolveDescriptor(
        capability,
        options,
        catalog,
        client,
        output,
        error,
        descriptor_exit);
    if (!descriptor)
    {
        return descriptor_exit;
    }
    if (options.command == "help" || options.help)
    {
        if (options.json_output)
        {
            json envelope = {
                { "ok", true },
                { "data", *descriptor },
                { "meta", {
                    { "schemaSource",
                        options.live_schema ? "editor" : "local" },
                } },
            };
            if (!options.live_schema && catalog)
            {
                envelope["meta"]["capabilityRoot"] =
                    catalog->Root().generic_string();
            }
            output << envelope.dump() << "\n";
        }
        else
        {
            output
                << "Schema source: "
                << (options.live_schema ? "Editor" : "local manifest")
                << "\n"
                << CapabilityHelp(*descriptor);
        }
        return 0;
    }

    if ((options.params_json || options.params_file)
        && !options.raw_options.empty())
    {
        return PrintFailure(
            {
                false,
                json(),
                "parameter_sources_conflict",
                "--params/--params-file cannot be combined with "
                "schema-derived --field options.",
            },
            options.json_output,
            kExitUsage,
            output,
            error);
    }

    ConversionResult conversion;
    if (options.params_json || options.params_file)
    {
        std::string read_error_code;
        std::string read_error_message;
        const auto params_text = ReadParameterText(
            options,
            input,
            read_error_code,
            read_error_message);
        if (!params_text)
        {
            return PrintFailure(
                {
                    false,
                    json(),
                    read_error_code.empty()
                        ? "params_invalid"
                        : read_error_code,
                    read_error_message.empty()
                        ? "Params JSON could not be read."
                        : read_error_message,
                },
                options.json_output,
                kExitUsage,
                output,
                error);
        }
        const json params =
            json::parse(*params_text, nullptr, false, true);
        if (params.is_discarded() || !params.is_object())
        {
            return PrintFailure(
                {
                    false,
                    json(),
                    "params_invalid",
                    "--params/--params-file must contain one JSON object.",
                },
                options.json_output,
                kExitUsage,
                output,
                error);
        }
        conversion = ConvertParameterObject(
            *descriptor,
            params,
            options.confirm_write);
    }
    else
    {
        conversion = ConvertParameters(
            *descriptor,
            options.raw_options,
            options.confirm_write);
    }
    if (!conversion.ok)
    {
        return PrintFailure(
            {
                false,
                json(),
                conversion.code,
                conversion.message,
            },
            options.json_output,
            kExitUsage,
            output,
            error);
    }
    std::string request_id = options.request_id;
    if (request_id.empty()
        && conversion.schema_accepts_request_id)
    {
        request_id = GenerateRequestId();
    }
    json request = {
        { "capability", capability },
        { "params", conversion.params },
    };
    if (!request_id.empty())
    {
        request["requestId"] = request_id;
    }
    ParsedEnvelope response = ParseEnvelope(
        client.Post("/api/execute", request.dump()));
    if (!response.ok)
    {
        return PrintFailure(
            response,
            options.json_output,
            response.code == "editor_unreachable"
                ? kExitUnavailable
                : kExitExecution,
            output,
            error);
    }
    if (!options.output_path.empty())
    {
        response = ExportPayload(
            client,
            capability,
            conversion.params,
            request_id,
            std::move(response),
            options.output_path);
        if (!response.ok)
        {
            return PrintFailure(
                response,
                options.json_output,
                kExitExecution,
                output,
                error);
        }
    }
    return PrintSuccess(
        capability,
        response,
        options.json_output,
        output);
}

bool Tokenize(
    std::string_view line,
    std::vector<std::string>& tokens,
    std::string& error)
{
    constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";
    if (line.starts_with(utf8_bom))
    {
        line.remove_prefix(utf8_bom.size());
    }
    std::string current;
    bool quoted = false;
    char quote = '\0';
    for (std::size_t index = 0; index < line.size(); ++index)
    {
        const char character = line[index];
        if (quoted)
        {
            if (character == quote)
            {
                quoted = false;
            }
            else if (character == '\\'
                && index + 1 < line.size()
                && line[index + 1] == quote)
            {
                current.push_back(line[++index]);
            }
            else
            {
                current.push_back(character);
            }
        }
        else if (character == '"' || character == '\'')
        {
            quoted = true;
            quote = character;
        }
        else if (std::isspace(
                     static_cast<unsigned char>(character)))
        {
            if (!current.empty())
            {
                tokens.push_back(std::move(current));
                current.clear();
            }
        }
        else
        {
            current.push_back(character);
        }
    }
    if (quoted)
    {
        error = "Unterminated quoted value.";
        return false;
    }
    if (!current.empty())
    {
        tokens.push_back(std::move(current));
    }
    return true;
}

int RunShell(
    const Options& base,
    const CapabilityCatalog* catalog,
    const SkillCatalog* skill_catalog,
    ue::api::Client& client,
    std::istream& input,
    std::ostream& output,
    std::ostream& error)
{
    if (!base.raw_options.empty()
        || !base.request_id.empty()
        || base.params_json
        || base.params_file
        || !base.output_path.empty()
        || base.confirm_write)
    {
        return PrintFailure(
            {
                false,
                json(),
                "invalid_shell_options",
                "ue shell accepts only global connection, catalog, "
                "--json, and --live-schema options.",
            },
            base.json_output,
            kExitUsage,
            output,
            error);
    }
    error
        << "UE short-operation shell ("
        << (base.live_schema ? "live schema" : "local schema")
        << "). Type 'help' or 'exit'.\n";
    std::string line;
    for (;;)
    {
        error << "ue> " << std::flush;
        if (!std::getline(input, line))
        {
            return 0;
        }
        std::vector<std::string> tokens;
        std::string token_error;
        if (!Tokenize(line, tokens, token_error))
        {
            (void)PrintFailure(
                {
                    false,
                    json(),
                    "invalid_arguments",
                    token_error,
                },
                base.json_output,
                kExitUsage,
                output,
                error);
            continue;
        }
        if (tokens.empty())
        {
            continue;
        }
        if (tokens[0] == "exit" || tokens[0] == "quit")
        {
            return 0;
        }

        Options command;
        std::string parse_error;
        if (!ParseArguments(tokens, command, parse_error))
        {
            (void)PrintFailure(
                {
                    false,
                    json(),
                    "invalid_arguments",
                    parse_error,
                },
                base.json_output
                    || std::find(
                           tokens.begin(),
                           tokens.end(),
                           "--json") != tokens.end(),
                kExitUsage,
                output,
                error);
            continue;
        }
        if (command.endpoint_explicit
            || command.timeout_explicit
            || !command.capability_root.empty()
            || !command.skill_root.empty())
        {
            (void)PrintFailure(
                {
                    false,
                    json(),
                    "shell_session_option_locked",
                    "Endpoint, timeout, capability root, and skill root "
                    "are fixed for the current shell session.",
                },
                base.json_output || command.json_output,
                kExitUsage,
                output,
                error);
            continue;
        }
        command.endpoint = base.endpoint;
        command.timeout_ms = base.timeout_ms;
        command.capability_root = base.capability_root;
        command.skill_root = base.skill_root;
        // Agent Skill discovery is intentionally local-only. A shell-wide
        // live-schema policy applies to API discovery and execution, but must
        // not disable the local "skills" directory command. An explicit
        // "skills --live-schema" remains an error in RunLocalSkills.
        command.live_schema =
            command.command == "skills"
                ? command.live_schema
                : (base.live_schema || command.live_schema);
        command.json_output =
            base.json_output || command.json_output;
        (void)ExecuteOptions(
            command,
            catalog,
            skill_catalog,
            client,
            input,
            output,
            error);
    }
}

} // namespace

int Run(
    const std::vector<std::string>& arguments,
    const std::filesystem::path& executable,
    std::istream& input,
    std::ostream& output,
    std::ostream& error)
{
    Options options;
    std::string parse_error;
    if (!ParseArguments(arguments, options, parse_error))
    {
        const bool requested_json =
            std::find(arguments.begin(), arguments.end(), "--json")
            != arguments.end();
        return PrintFailure(
            {
                false,
                json(),
                "invalid_arguments",
                parse_error,
            },
            requested_json,
            kExitUsage,
            output,
            error);
    }
    if (options.command.empty()
        || options.command == "--help"
        || (options.command == "help"
            && options.help_capability.empty()))
    {
        PrintGeneralHelp(output);
        return 0;
    }
    if (options.help)
    {
        PrintCommandHelp(options, output);
        return 0;
    }
    if (options.command == "--version"
        || options.command == "version")
    {
        return PrintVersion(options.json_output, output);
    }

    std::string capability_hint;
    if (options.command == "help")
    {
        capability_hint = options.help_capability;
    }
    else if (options.command != "shell"
        && options.command != "capabilities"
        && options.command != "skills"
        && options.command.find('.') != std::string::npos)
    {
        capability_hint = options.command;
    }

    std::optional<CapabilityCatalog> catalog;
    const bool requires_local_catalog =
        options.command == "shell"
        || (!options.live_schema
            && options.command != "status"
            && (options.command == "capabilities"
            || options.command == "skills"
            || !capability_hint.empty()));
    if (requires_local_catalog)
    {
        int load_exit = kExitUnavailable;
        catalog = LoadLocalCatalog(
            options,
            executable,
            capability_hint,
            output,
            error,
            load_exit);
        if (!catalog)
        {
            return load_exit;
        }
    }

    std::optional<SkillCatalog> skill_catalog;
    const bool requires_skill_catalog =
        options.command == "skills"
        || (options.command == "shell"
            && !options.skill_root.empty());
    if (requires_skill_catalog)
    {
        if (options.command == "skills" && options.live_schema)
        {
            return PrintFailure(
                {
                    false,
                    json(),
                    "invalid_arguments",
                    "ue skills is local-only; remove --live-schema.",
                },
                options.json_output,
                kExitUsage,
                output,
                error);
        }
        if (!catalog)
        {
            return PrintFailure(
                {
                    false,
                    json(),
                    "capability_catalog_unavailable",
                    "The capability catalog is required to validate "
                    "Agent Skill operations.",
                },
                options.json_output,
                kExitUnavailable,
                output,
                error);
        }
        int load_exit = kExitUnavailable;
        skill_catalog = LoadLocalSkillCatalog(
            options,
            executable,
            *catalog,
            output,
            error,
            load_exit);
        if (!skill_catalog)
        {
            return load_exit;
        }
    }
    else if (options.command == "shell" && catalog)
    {
        std::string skill_resolution_error;
        const auto skill_root = ResolveSkillRoot(
            executable,
            {},
            nullptr,
            skill_resolution_error);
        if (skill_root)
        {
            std::string skill_load_error;
            skill_catalog = SkillCatalog::Load(
                *skill_root,
                *catalog,
                skill_load_error);
        }
    }

    ue::api::Client client(
        options.endpoint,
        options.timeout_ms);
    const std::string invocation_id = ue::api::NewInvocationId();
    client.ConfigureBestEffortCliSession({
        .name = "ue",
        .version = UE_CLI_VERSION,
        .command = options.command,
        .invocation_id = invocation_id,
        .instance_id = invocation_id,
        .process_id = ue::api::CurrentProcessId(),
    });
    if (options.command == "shell")
    {
        return RunShell(
            options,
            catalog ? &*catalog : nullptr,
            skill_catalog ? &*skill_catalog : nullptr,
            client,
            input,
            output,
            error);
    }
    return ExecuteOptions(
        options,
        catalog ? &*catalog : nullptr,
        skill_catalog ? &*skill_catalog : nullptr,
        client,
        input,
        output,
        error);
}

int Run(
    const std::vector<std::string>& arguments,
    std::ostream& output,
    std::ostream& error)
{
    return Run(
        arguments,
        {},
        std::cin,
        output,
        error);
}

} // namespace ue::command
