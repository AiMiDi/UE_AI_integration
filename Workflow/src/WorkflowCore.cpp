#include "UEWorkflowCore/WorkflowCore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace ue::workflow
{

namespace
{

using json = nlohmann::json;

constexpr std::string_view kDslVersion = "1.0";
constexpr std::string_view kPlannerVersion = "1.0";
constexpr std::string_view kDslVersionV2 = "2.0";
constexpr std::string_view kPlannerVersionV2 = "2.0";
constexpr std::size_t kMaxV2Scopes = 16;
constexpr std::size_t kMaxV2Operations = 256;

class Sha256
{
public:
    void update(const std::uint8_t* data, std::size_t length)
    {
        for (std::size_t index = 0; index < length; ++index)
        {
            block_[block_length_++] = data[index];
            if (block_length_ == 64)
            {
                transform();
                bit_length_ += 512;
                block_length_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finish()
    {
        std::size_t index = block_length_;
        block_[index++] = 0x80;
        if (index > 56)
        {
            while (index < 64)
            {
                block_[index++] = 0;
            }
            transform();
            index = 0;
        }
        while (index < 56)
        {
            block_[index++] = 0;
        }

        bit_length_ += static_cast<std::uint64_t>(block_length_) * 8;
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            block_[index++] = static_cast<std::uint8_t>((bit_length_ >> shift) & 0xff);
        }
        transform();

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t word = 0; word < state_.size(); ++word)
        {
            digest[word * 4] = static_cast<std::uint8_t>((state_[word] >> 24) & 0xff);
            digest[word * 4 + 1] = static_cast<std::uint8_t>((state_[word] >> 16) & 0xff);
            digest[word * 4 + 2] = static_cast<std::uint8_t>((state_[word] >> 8) & 0xff);
            digest[word * 4 + 3] = static_cast<std::uint8_t>(state_[word] & 0xff);
        }
        return digest;
    }

private:
    static constexpr std::array<std::uint32_t, 64> constants_ = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    static std::uint32_t rotate_right(std::uint32_t value, std::uint32_t count)
    {
        return (value >> count) | (value << (32 - count));
    }

    void transform()
    {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index)
        {
            const std::size_t offset = index * 4;
            words[index] =
                (static_cast<std::uint32_t>(block_[offset]) << 24) |
                (static_cast<std::uint32_t>(block_[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(block_[offset + 2]) << 8) |
                static_cast<std::uint32_t>(block_[offset + 3]);
        }
        for (std::size_t index = 16; index < 64; ++index)
        {
            const std::uint32_t s0 =
                rotate_right(words[index - 15], 7) ^
                rotate_right(words[index - 15], 18) ^
                (words[index - 15] >> 3);
            const std::uint32_t s1 =
                rotate_right(words[index - 2], 17) ^
                rotate_right(words[index - 2], 19) ^
                (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
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
            const std::uint32_t s1 =
                rotate_right(e, 6) ^
                rotate_right(e, 11) ^
                rotate_right(e, 25);
            const std::uint32_t choice = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + s1 + choice + constants_[index] + words[index];
            const std::uint32_t s0 =
                rotate_right(a, 2) ^
                rotate_right(a, 13) ^
                rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
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

    std::array<std::uint8_t, 64> block_{};
    std::size_t block_length_ = 0;
    std::uint64_t bit_length_ = 0;
    std::array<std::uint32_t, 8> state_ = {
        0x6a09e667,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19,
    };
};

std::string sha256(std::string_view text)
{
    Sha256 hasher;
    hasher.update(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    const auto digest = hasher.finish();
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(71);
    for (const auto byte : digest)
    {
        result.push_back(hex[(byte >> 4) & 0x0f]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

std::optional<std::string> read_text_file(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

std::optional<json> read_json_file(
    const std::filesystem::path& path,
    std::vector<Diagnostic>& diagnostics,
    std::string_view phase)
{
    const auto text = read_text_file(path);
    if (!text)
    {
        diagnostics.push_back(Diagnostic{
            Severity::Error,
            std::string(phase),
            "contract_file_unreadable",
            path.generic_string(),
            "Could not read the JSON contract file.",
            {},
            "Check --contract-root and --capability-root.",
        });
        return std::nullopt;
    }

    auto value = json::parse(*text, nullptr, false, true);
    if (value.is_discarded())
    {
        diagnostics.push_back(Diagnostic{
            Severity::Error,
            std::string(phase),
            "contract_json_invalid",
            path.generic_string(),
            "Contract file is not valid UTF-8 JSON.",
            {},
            "Fix or restore the contract file.",
        });
        return std::nullopt;
    }
    return value;
}

json diagnostic_to_json(const Diagnostic& diagnostic)
{
    json value = {
        { "severity", SeverityName(diagnostic.severity) },
        { "phase", diagnostic.phase },
        { "code", diagnostic.code },
        { "path", diagnostic.path },
        { "message", diagnostic.message },
    };
    if (!diagnostic.operation_id.empty())
    {
        value["opId"] = diagnostic.operation_id;
    }
    if (!diagnostic.next_action.empty())
    {
        value["nextAction"] = diagnostic.next_action;
    }
    return value;
}

json diagnostics_to_json(const std::vector<Diagnostic>& diagnostics)
{
    json values = json::array();
    for (const auto& diagnostic : diagnostics)
    {
        values.push_back(diagnostic_to_json(diagnostic));
    }
    return values;
}

void add_diagnostic(
    std::vector<Diagnostic>& diagnostics,
    std::string phase,
    std::string code,
    std::string path,
    std::string message,
    std::string operation_id = {},
    std::string next_action = {},
    Severity severity = Severity::Error)
{
    diagnostics.push_back(Diagnostic{
        severity,
        std::move(phase),
        std::move(code),
        std::move(path),
        std::move(message),
        std::move(operation_id),
        std::move(next_action),
    });
}

bool has_errors(const std::vector<Diagnostic>& diagnostics)
{
    return std::any_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == Severity::Error;
        });
}

Result make_result(json payload, std::vector<Diagnostic> diagnostics, int failure_exit_code)
{
    const bool ok = !has_errors(diagnostics);
    payload["ok"] = ok;
    payload["diagnostics"] = diagnostics_to_json(diagnostics);
    return Result{
        ok,
        ok ? 0 : failure_exit_code,
        payload.dump(),
        std::move(diagnostics),
    };
}

json merge_json(json base, const json& overlay)
{
    if (!base.is_object() || !overlay.is_object())
    {
        return overlay;
    }
    for (const auto& [key, value] : overlay.items())
    {
        const auto existing = base.find(key);
        if (existing != base.end() && existing->is_object() && value.is_object())
        {
            *existing = merge_json(*existing, value);
        }
        else
        {
            base[key] = value;
        }
    }
    return base;
}

std::vector<std::string> string_array(const json* value)
{
    std::vector<std::string> result;
    if (!value || !value->is_array())
    {
        return result;
    }
    for (const auto& item : *value)
    {
        if (item.is_string())
        {
            result.push_back(item.get<std::string>());
        }
    }
    return result;
}

std::string pointer_escape(std::string_view value)
{
    std::string result;
    for (const char c : value)
    {
        if (c == '~')
        {
            result += "~0";
        }
        else if (c == '/')
        {
            result += "~1";
        }
        else
        {
            result.push_back(c);
        }
    }
    return result;
}

std::optional<std::vector<std::string>> parse_json_pointer(std::string_view pointer)
{
    if (pointer.empty())
    {
        return std::vector<std::string>{};
    }
    if (pointer.front() != '/')
    {
        return std::nullopt;
    }

    std::vector<std::string> tokens;
    std::size_t start = 1;
    while (start <= pointer.size())
    {
        const auto slash = pointer.find('/', start);
        const auto raw = pointer.substr(
            start,
            slash == std::string_view::npos ? pointer.size() - start : slash - start);
        std::string token;
        for (std::size_t index = 0; index < raw.size(); ++index)
        {
            if (raw[index] != '~')
            {
                token.push_back(raw[index]);
                continue;
            }
            if (index + 1 >= raw.size())
            {
                return std::nullopt;
            }
            const char escaped = raw[++index];
            if (escaped == '0')
            {
                token.push_back('~');
            }
            else if (escaped == '1')
            {
                token.push_back('/');
            }
            else
            {
                return std::nullopt;
            }
        }
        tokens.push_back(std::move(token));
        if (slash == std::string_view::npos)
        {
            break;
        }
        start = slash + 1;
    }
    return tokens;
}

std::string make_json_pointer(const std::vector<std::string>& tokens)
{
    std::string pointer;
    for (const auto& token : tokens)
    {
        pointer += "/";
        pointer += pointer_escape(token);
    }
    return pointer;
}

const json* json_at_tokens(const json& root, const std::vector<std::string>& tokens)
{
    const json* value = &root;
    for (const auto& token : tokens)
    {
        if (value->is_object())
        {
            const auto it = value->find(token);
            if (it == value->end())
            {
                return nullptr;
            }
            value = &*it;
            continue;
        }
        if (value->is_array())
        {
            std::size_t index = 0;
            const auto parsed = std::from_chars(
                token.data(),
                token.data() + token.size(),
                index);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != token.data() + token.size() ||
                index >= value->size())
            {
                return nullptr;
            }
            value = &(*value)[index];
            continue;
        }
        return nullptr;
    }
    return value;
}

const json* schema_at_tokens(const json& root, const std::vector<std::string>& tokens)
{
    const json* schema = &root;
    for (const auto& token : tokens)
    {
        if (!schema->is_object())
        {
            return nullptr;
        }
        const auto type = schema->value("type", std::string{});
        if (type == "object" || schema->contains("properties"))
        {
            const auto properties = schema->find("properties");
            if (properties != schema->end() && properties->is_object())
            {
                const auto property = properties->find(token);
                if (property != properties->end())
                {
                    schema = &*property;
                    continue;
                }
            }
            const auto additional = schema->find("additionalProperties");
            if (additional != schema->end() && additional->is_object())
            {
                schema = &*additional;
                continue;
            }
            return nullptr;
        }
        if (type == "array" || schema->contains("items"))
        {
            const auto items = schema->find("items");
            if (items == schema->end() || !items->is_object())
            {
                return nullptr;
            }
            if (token.empty() ||
                !std::all_of(token.begin(), token.end(), [](char c) { return c >= '0' && c <= '9'; }))
            {
                return nullptr;
            }
            schema = &*items;
            continue;
        }
        return nullptr;
    }
    return schema;
}

std::string schema_type(const json* schema)
{
    return schema && schema->is_object()
        ? schema->value("type", std::string{})
        : std::string{};
}

bool types_compatible(const json* source, const json* destination)
{
    const auto source_type = schema_type(source);
    const auto destination_type = schema_type(destination);
    if (source_type.empty() || destination_type.empty() || source_type == destination_type)
    {
        return true;
    }
    return source_type == "integer" && destination_type == "number";
}

std::optional<std::string> graph_read_back_target(const json& operation)
{
    if (!operation.is_object() ||
        !operation.contains("params") ||
        !operation["params"].is_object())
    {
        return std::nullopt;
    }
    const auto type = operation.value("type", std::string{});
    const auto& params = operation["params"];
    const char* field = nullptr;
    if (type == "blueprint.graph.create")
    {
        const auto graph_type = params.value("graphType", std::string{});
        if (graph_type != "function" && graph_type != "macro")
        {
            return std::nullopt;
        }
        field = "graphName";
    }
    else if (type == "blueprint.node.add")
    {
        field = "graph";
    }
    else if (type == "blueprint.function.parameter.add")
    {
        field = "functionName";
    }
    else
    {
        return std::nullopt;
    }

    const auto value = params.find(field);
    if (value == params.end() || !value->is_string() ||
        value->get_ref<const std::string&>().empty())
    {
        return std::nullopt;
    }
    return value->get<std::string>();
}

bool valid_operation_id(std::string_view value)
{
    if (value.empty() ||
        !((value.front() >= 'A' && value.front() <= 'Z') ||
          (value.front() >= 'a' && value.front() <= 'z')))
    {
        return false;
    }
    return std::all_of(
        value.begin() + 1,
        value.end(),
        [](char c) {
            return (c >= 'A' && c <= 'Z') ||
                (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '_' ||
                c == '-';
        });
}

bool digest_pattern(std::string_view value)
{
    if (value.size() != 71 || !value.starts_with("sha256:"))
    {
        return false;
    }
    return std::all_of(
        value.begin() + 7,
        value.end(),
        [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}

bool lightweight_pattern_match(std::string_view value, std::string_view pattern)
{
    if (pattern == "^/")
    {
        return value.starts_with("/");
    }
    if (pattern == "^/Game/")
    {
        return value.starts_with("/Game/");
    }
    if (pattern == "^[A-Za-z][A-Za-z0-9_-]*$")
    {
        return valid_operation_id(value);
    }
    if (pattern == "^sha256:[0-9a-f]{64}$")
    {
        return digest_pattern(value);
    }
    // Capability contracts currently use simple anchored patterns. Unknown
    // regular expressions are intentionally left to the UE runtime validator.
    return true;
}

bool matches_type(const json& value, std::string_view type)
{
    if (type == "object")
    {
        return value.is_object();
    }
    if (type == "array")
    {
        return value.is_array();
    }
    if (type == "string")
    {
        return value.is_string();
    }
    if (type == "number")
    {
        return value.is_number();
    }
    if (type == "integer")
    {
        return value.is_number_integer() || value.is_number_unsigned();
    }
    if (type == "boolean")
    {
        return value.is_boolean();
    }
    if (type == "null")
    {
        return value.is_null();
    }
    return true;
}

void validate_schema(
    const json& value,
    const json& schema,
    const json& root_schema,
    const std::string& path,
    std::vector<Diagnostic>& diagnostics,
    const std::set<std::string>* satisfied_root_properties = nullptr,
    int depth = 0)
{
    if (!schema.is_object() || depth > 96)
    {
        return;
    }

    if (const auto ref = schema.find("$ref"); ref != schema.end() && ref->is_string())
    {
        const auto reference = ref->get<std::string>();
        if (!reference.starts_with("#"))
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "external_schema_ref_unsupported",
                path,
                "Only local JSON Schema references are supported by the offline validator.");
            return;
        }
        const auto tokens = parse_json_pointer(std::string_view(reference).substr(1));
        const auto* resolved = tokens ? json_at_tokens(root_schema, *tokens) : nullptr;
        if (!resolved)
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "schema_ref_unresolved",
                path,
                "A local JSON Schema reference could not be resolved.");
            return;
        }
        validate_schema(
            value,
            *resolved,
            root_schema,
            path,
            diagnostics,
            satisfied_root_properties,
            depth + 1);
        return;
    }

    if (const auto constant = schema.find("const");
        constant != schema.end() && value != *constant)
    {
        add_diagnostic(
            diagnostics,
            "schema",
            "const_mismatch",
            path,
            "Value does not match the required constant.");
        return;
    }

    if (const auto enumeration = schema.find("enum");
        enumeration != schema.end() && enumeration->is_array() &&
        std::find(enumeration->begin(), enumeration->end(), value) == enumeration->end())
    {
        add_diagnostic(
            diagnostics,
            "schema",
            "enum_mismatch",
            path,
            "Value is not one of the allowed enum values.");
        return;
    }

    if (const auto type = schema.find("type"); type != schema.end())
    {
        bool type_ok = true;
        if (type->is_string())
        {
            type_ok = matches_type(value, type->get<std::string>());
        }
        else if (type->is_array())
        {
            type_ok = std::any_of(
                type->begin(),
                type->end(),
                [&value](const json& candidate) {
                    return candidate.is_string() &&
                        matches_type(value, candidate.get<std::string>());
                });
        }
        if (!type_ok)
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "type_mismatch",
                path,
                "JSON value has the wrong type.");
            return;
        }
    }

    auto branch_matches = [&](const json& branch) {
        std::vector<Diagnostic> branch_diagnostics;
        validate_schema(
            value,
            branch,
            root_schema,
            path,
            branch_diagnostics,
            satisfied_root_properties,
            depth + 1);
        return !has_errors(branch_diagnostics);
    };

    if (const auto any_of = schema.find("anyOf"); any_of != schema.end() && any_of->is_array())
    {
        if (!std::any_of(any_of->begin(), any_of->end(), branch_matches))
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "any_of_not_satisfied",
                path,
                "Value does not satisfy any allowed JSON Schema branch.");
        }
    }

    if (const auto one_of = schema.find("oneOf"); one_of != schema.end() && one_of->is_array())
    {
        const auto count = std::count_if(one_of->begin(), one_of->end(), branch_matches);
        if (count != 1)
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "one_of_not_satisfied",
                path,
                "Value must satisfy exactly one JSON Schema branch.");
        }
    }

    if (value.is_object())
    {
        if (const auto required = schema.find("required");
            required != schema.end() && required->is_array())
        {
            for (const auto& item : *required)
            {
                if (!item.is_string())
                {
                    continue;
                }
                const auto name = item.get<std::string>();
                const bool satisfied =
                    value.contains(name) ||
                    (satisfied_root_properties &&
                     satisfied_root_properties->contains(name));
                if (!satisfied)
                {
                    add_diagnostic(
                        diagnostics,
                        "schema",
                        "required_property_missing",
                        path + "/" + pointer_escape(name),
                        "Required property is missing.");
                }
            }
        }

        const auto properties = schema.find("properties");
        const auto additional = schema.find("additionalProperties");
        for (const auto& [name, child] : value.items())
        {
            const json* child_schema = nullptr;
            if (properties != schema.end() && properties->is_object())
            {
                const auto property = properties->find(name);
                if (property != properties->end())
                {
                    child_schema = &*property;
                }
            }
            if (child_schema)
            {
                validate_schema(
                    child,
                    *child_schema,
                    root_schema,
                    path + "/" + pointer_escape(name),
                    diagnostics,
                    nullptr,
                    depth + 1);
            }
            else if (additional != schema.end() && additional->is_boolean() && !additional->get<bool>())
            {
                add_diagnostic(
                    diagnostics,
                    "schema",
                    "additional_property_forbidden",
                    path + "/" + pointer_escape(name),
                    "JSON Schema does not allow this property.");
            }
            else if (additional != schema.end() && additional->is_object())
            {
                validate_schema(
                    child,
                    *additional,
                    root_schema,
                    path + "/" + pointer_escape(name),
                    diagnostics,
                    nullptr,
                    depth + 1);
            }
        }

        if (const auto property_names = schema.find("propertyNames");
            property_names != schema.end() && property_names->is_object())
        {
            for (const auto& [name, ignored] : value.items())
            {
                (void)ignored;
                validate_schema(
                    json(name),
                    *property_names,
                    root_schema,
                    path + "/" + pointer_escape(name),
                    diagnostics,
                    nullptr,
                    depth + 1);
            }
        }
    }

    if (value.is_array())
    {
        if (const auto minimum = schema.find("minItems");
            minimum != schema.end() && minimum->is_number_unsigned() &&
            value.size() < minimum->get<std::size_t>())
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "array_too_short",
                path,
                "Array contains fewer items than the schema minimum.");
        }
        if (const auto maximum = schema.find("maxItems");
            maximum != schema.end() && maximum->is_number_unsigned() &&
            value.size() > maximum->get<std::size_t>())
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "array_too_long",
                path,
                "Array contains more items than the schema maximum.");
        }
        if (schema.value("uniqueItems", false))
        {
            std::set<std::string> seen;
            for (const auto& item : value)
            {
                if (!seen.insert(item.dump()).second)
                {
                    add_diagnostic(
                        diagnostics,
                        "schema",
                        "array_items_not_unique",
                        path,
                        "Array items must be unique.");
                    break;
                }
            }
        }
        if (const auto items = schema.find("items"); items != schema.end() && items->is_object())
        {
            for (std::size_t index = 0; index < value.size(); ++index)
            {
                validate_schema(
                    value[index],
                    *items,
                    root_schema,
                    path + "/" + std::to_string(index),
                    diagnostics,
                    nullptr,
                    depth + 1);
            }
        }
    }

    if (value.is_string())
    {
        const auto text = value.get<std::string>();
        if (const auto minimum = schema.find("minLength");
            minimum != schema.end() && minimum->is_number_unsigned() &&
            text.size() < minimum->get<std::size_t>())
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "string_too_short",
                path,
                "String is shorter than the schema minimum.");
        }
        if (const auto maximum = schema.find("maxLength");
            maximum != schema.end() && maximum->is_number_unsigned() &&
            text.size() > maximum->get<std::size_t>())
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "string_too_long",
                path,
                "String is longer than the schema maximum.");
        }
        if (const auto pattern = schema.find("pattern");
            pattern != schema.end() && pattern->is_string() &&
            !lightweight_pattern_match(text, pattern->get<std::string>()))
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "pattern_mismatch",
                path,
                "String does not match the required pattern.");
        }
    }

    if (value.is_number())
    {
        const auto number = value.get<double>();
        if (const auto minimum = schema.find("minimum");
            minimum != schema.end() && minimum->is_number() &&
            number < minimum->get<double>())
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "number_below_minimum",
                path,
                "Number is below the schema minimum.");
        }
        if (const auto maximum = schema.find("maximum");
            maximum != schema.end() && maximum->is_number() &&
            number > maximum->get<double>())
        {
            add_diagnostic(
                diagnostics,
                "schema",
                "number_above_maximum",
                path,
                "Number is above the schema maximum.");
        }
    }
}

void find_forbidden_interpolation(
    const json& value,
    const std::string& path,
    std::vector<Diagnostic>& diagnostics)
{
    if (value.is_string() && value.get_ref<const std::string&>().find("${") != std::string::npos)
    {
        add_diagnostic(
            diagnostics,
            "workflow",
            "string_interpolation_forbidden",
            path,
            "UE Workflow v1 uses typed bindings; '${...}' string interpolation is not allowed.");
        return;
    }
    if (value.is_array())
    {
        for (std::size_t index = 0; index < value.size(); ++index)
        {
            find_forbidden_interpolation(
                value[index],
                path + "/" + std::to_string(index),
                diagnostics);
        }
    }
    else if (value.is_object())
    {
        for (const auto& [key, child] : value.items())
        {
            find_forbidden_interpolation(
                child,
                path + "/" + pointer_escape(key),
                diagnostics);
        }
    }
}

int risk_rank(std::string_view risk)
{
    if (risk == "readOnly")
    {
        return 0;
    }
    if (risk == "safeWrite")
    {
        return 1;
    }
    if (risk == "confirmWrite")
    {
        return 2;
    }
    return 3;
}

struct Capability
{
    std::string id;
    std::string domain;
    std::string description;
    json raw = json::object();
    json input_schema = json::object();
    json output_schema = json::object();
    json dsl = json::object();
};

struct NormalizedOperation
{
    json operation = json::object();
    const Capability* capability = nullptr;
    std::vector<std::string> dependencies;
};

struct ValidationData
{
    json normalized_workflow = json::object();
    std::vector<NormalizedOperation> operations;
    std::vector<Diagnostic> diagnostics;
};

std::string capability_alias(const Capability& capability, std::string_view name)
{
    const auto aliases = capability.dsl.find("parameterAliases");
    if (aliases == capability.dsl.end() || !aliases->is_object())
    {
        return std::string(name);
    }
    const auto alias = aliases->find(name);
    return alias != aliases->end() && alias->is_string()
        ? alias->get<std::string>()
        : std::string(name);
}

json normalize_params(
    const json& params,
    const Capability& capability,
    const std::string& path,
    const std::string& operation_id,
    std::vector<Diagnostic>& diagnostics)
{
    json normalized = params;
    const auto aliases = capability.dsl.find("parameterAliases");
    if (aliases == capability.dsl.end() || !aliases->is_object() || !normalized.is_object())
    {
        return normalized;
    }
    for (const auto& [alias, canonical_value] : aliases->items())
    {
        if (!canonical_value.is_string() || !normalized.contains(alias))
        {
            continue;
        }
        const auto canonical = canonical_value.get<std::string>();
        if (normalized.contains(canonical))
        {
            add_diagnostic(
                diagnostics,
                "normalize",
                "parameter_alias_conflict",
                path + "/" + pointer_escape(alias),
                "Both a workflow parameter alias and its canonical capability parameter were provided.",
                operation_id);
            continue;
        }
        normalized[canonical] = normalized[alias];
        normalized.erase(alias);
    }
    return normalized;
}

} // namespace

class Engine::Impl
{
public:
    Result load(const LoadOptions& options)
    {
        loaded_ = false;
        capabilities_.clear();
        contract_files_.clear();
        contract_set_digest_.clear();
        contract_root_ = options.contract_root;
        capability_roots_.clear();
        std::vector<Diagnostic> diagnostics;

        if (contract_root_.empty())
        {
            add_diagnostic(
                diagnostics,
                "contracts",
                "contract_root_required",
                "/contractRoot",
                "A UE Workflow contract root is required.");
            return make_load_result(std::move(diagnostics));
        }

        contract_set_ = load_contract_file("contract-set.v1.json", diagnostics);
        if (has_errors(diagnostics))
        {
            return make_load_result(std::move(diagnostics));
        }

        const auto contract_file_entries = contract_set_.find("files");
        if (contract_file_entries == contract_set_.end() ||
            !contract_file_entries->is_array())
        {
            add_diagnostic(
                diagnostics,
                "contracts",
                "contract_files_missing",
                "/files",
                "contract-set.v1.json must declare an array of contract files.");
        }
        else
        {
            for (std::size_t index = 0;
                 index < contract_file_entries->size();
                 ++index)
            {
                const auto& item = (*contract_file_entries)[index];
                if (!item.is_string())
                {
                    add_diagnostic(
                        diagnostics,
                        "contracts",
                        "contract_file_invalid",
                        "/files/" + std::to_string(index),
                        "Every contract file entry must be a relative path string.");
                    continue;
                }

                const std::filesystem::path relative = item.get<std::string>();
                bool escapes_root = relative.empty() || relative.is_absolute();
                for (const auto& component : relative)
                {
                    escapes_root = escapes_root || component == "..";
                }
                if (escapes_root)
                {
                    add_diagnostic(
                        diagnostics,
                        "contracts",
                        "contract_file_outside_root",
                        "/files/" + std::to_string(index),
                        "Contract files must remain inside the configured contract root.");
                    continue;
                }

                const auto key = relative.lexically_normal().generic_string();
                if (contract_files_.contains(key))
                {
                    add_diagnostic(
                        diagnostics,
                        "contracts",
                        "contract_file_duplicate",
                        "/files/" + std::to_string(index),
                        "A contract file is listed more than once: " + key);
                    continue;
                }
                const auto value =
                    read_json_file(contract_root_ / relative, diagnostics, "contracts");
                if (!value)
                {
                    continue;
                }
                if (!value->is_object())
                {
                    add_diagnostic(
                        diagnostics,
                        "contracts",
                        "contract_file_not_object",
                        key,
                        "Contract documents must be JSON objects.");
                    continue;
                }
                contract_files_[key] = *value;
            }
        }

        auto assign_contract = [&](std::string_view field, json& target) {
            const auto reference = contract_set_.find(std::string(field));
            if (reference == contract_set_.end() || !reference->is_string())
            {
                add_diagnostic(
                    diagnostics,
                    "contracts",
                    "contract_reference_missing",
                    "/" + std::string(field),
                    "The contract set must reference a declared contract file.");
                return;
            }
            const auto key =
                std::filesystem::path(reference->get<std::string>())
                    .lexically_normal()
                    .generic_string();
            const auto value = contract_files_.find(key);
            if (value == contract_files_.end())
            {
                add_diagnostic(
                    diagnostics,
                    "contracts",
                    "contract_reference_not_declared",
                    "/" + std::string(field),
                    "Referenced contract file is not present in contract-set.files: " + key);
                return;
            }
            target = *value;
        };

        assign_contract("workflowSchema", workflow_schema_);
        assign_contract("planSchema", plan_schema_);
        assign_contract("resultSchema", result_schema_);
        assign_contract("receiptSchema", receipt_schema_);
        assign_contract("admission", admission_);
        if (has_errors(diagnostics))
        {
            return make_load_result(std::move(diagnostics));
        }

        workflow_schema_v2_ =
            load_contract_file("schema.v2.json", diagnostics);
        plan_schema_v2_ =
            load_contract_file("plan.schema.v2.json", diagnostics);
        if (!workflow_schema_v2_.is_object() || workflow_schema_v2_.empty())
        {
            add_diagnostic(
                diagnostics,
                "contracts",
                "workflow_v2_schema_missing",
                "/schema.v2.json",
                "UE Workflow DSL v2 requires schema.v2.json.");
        }
        if (!plan_schema_v2_.is_object() || plan_schema_v2_.empty())
        {
            add_diagnostic(
                diagnostics,
                "contracts",
                "workflow_v2_plan_schema_missing",
                "/plan.schema.v2.json",
                "UE Workflow DSL v2 requires plan.schema.v2.json.");
        }
        if (has_errors(diagnostics))
        {
            return make_load_result(std::move(diagnostics));
        }

        std::vector<std::filesystem::path> requested_roots = options.capability_roots;
        if (const auto roots = contract_set_.find("capabilityRoots");
            roots != contract_set_.end() && roots->is_array())
        {
            for (const auto& item : *roots)
            {
                if (item.is_string())
                {
                    requested_roots.push_back(
                        contract_root_ / std::filesystem::path(item.get<std::string>()));
                }
            }
        }

        std::set<std::string> seen_roots;
        for (const auto& requested : requested_roots)
        {
            std::error_code error;
            const auto absolute = std::filesystem::absolute(requested, error);
            if (error || !std::filesystem::is_directory(absolute, error))
            {
                continue;
            }
            const auto normalized = absolute.lexically_normal();
            if (seen_roots.insert(normalized.generic_string()).second)
            {
                capability_roots_.push_back(normalized);
            }
        }
        if (capability_roots_.empty())
        {
            add_diagnostic(
                diagnostics,
                "contracts",
                "capability_root_not_found",
                "/capabilityRoots",
                "No capability manifest directory could be resolved.",
                {},
                "Pass --capability-root pointing at Resources/Capabilities.");
            return make_load_result(std::move(diagnostics));
        }

        const auto admission_operations = admission_.find("operations");
        std::set<std::string> seen_manifest_paths;
        json digest_capabilities = json::array();
        for (const auto& root : capability_roots_)
        {
            std::error_code iteration_error;
            std::vector<std::filesystem::path> capability_manifest_files;
            for (std::filesystem::directory_iterator it(root, iteration_error), end;
                 !iteration_error && it != end;
                 it.increment(iteration_error))
            {
                if (it->is_regular_file() && it->path().extension() == ".json")
                {
                    capability_manifest_files.push_back(it->path());
                }
            }
            std::sort(
                capability_manifest_files.begin(),
                capability_manifest_files.end());
            for (const auto& file : capability_manifest_files)
            {
                const auto key = file.lexically_normal().generic_string();
                if (!seen_manifest_paths.insert(key).second)
                {
                    continue;
                }
                const auto manifest = read_json_file(file, diagnostics, "capabilities");
                if (!manifest)
                {
                    continue;
                }
                const auto entries = manifest->find("capabilities");
                if (entries == manifest->end() || !entries->is_array())
                {
                    add_diagnostic(
                        diagnostics,
                        "capabilities",
                        "capabilities_array_missing",
                        file.generic_string(),
                        "Capability manifest must contain a capabilities array.");
                    continue;
                }
                for (const auto& entry : *entries)
                {
                    if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string())
                    {
                        add_diagnostic(
                            diagnostics,
                            "capabilities",
                            "capability_id_missing",
                            file.generic_string(),
                            "Capability entry must contain a string id.");
                        continue;
                    }
                    const auto id = entry["id"].get<std::string>();
                    if (capabilities_.contains(id))
                    {
                        add_diagnostic(
                            diagnostics,
                            "capabilities",
                            "duplicate_capability",
                            file.generic_string(),
                            "Capability id is declared more than once: " + id);
                        continue;
                    }

                    Capability capability;
                    capability.id = id;
                    capability.domain = entry.value("domain", std::string{});
                    capability.description = entry.value("description", std::string{});
                    capability.raw = entry;
                    if (const auto input = entry.find("inputSchema");
                        input != entry.end() && input->is_object())
                    {
                        capability.input_schema = *input;
                    }

                    json overlay = json::object();
                    if (admission_operations != admission_.end() &&
                        admission_operations->is_object())
                    {
                        const auto configured = admission_operations->find(id);
                        if (configured != admission_operations->end() && configured->is_object())
                        {
                            overlay = *configured;
                        }
                    }
                    if (const auto dsl = entry.find("dsl"); dsl != entry.end() && dsl->is_object())
                    {
                        capability.dsl = merge_json(std::move(overlay), *dsl);
                    }
                    else
                    {
                        capability.dsl = std::move(overlay);
                    }
                    if (!capability.dsl.contains("admission"))
                    {
                        capability.dsl["admission"] = "none";
                    }
                    if (const auto output = capability.dsl.find("outputSchema");
                        output != capability.dsl.end() && output->is_object())
                    {
                        capability.output_schema = *output;
                    }
                    else if (const auto capability_output = entry.find("output");
                             capability_output != entry.end() && capability_output->is_object())
                    {
                        const auto schema = capability_output->find("schema");
                        if (schema != capability_output->end() && schema->is_object())
                        {
                            capability.output_schema = *schema;
                        }
                    }

                    const auto admission =
                        capability.dsl.value("admission", std::string{ "none" });
                    if (admission == "editStep"
                        || admission == "finalizer"
                        || admission == "observeOnly")
                    {
                        json digest_entry = {
                            { "id", capability.id },
                            { "domain", capability.domain },
                            { "kind", entry.value("kind", std::string{}) },
                            { "inputSchema", capability.input_schema },
                            { "dsl", capability.dsl },
                        };
                        if (const auto output = entry.find("output");
                            output != entry.end())
                        {
                            digest_entry["output"] = *output;
                        }
                        if (const auto traits = entry.find("traits");
                            traits != entry.end())
                        {
                            digest_entry["traits"] = *traits;
                        }
                        if (const auto requirements = entry.find("requires");
                            requirements != entry.end())
                        {
                            digest_entry["requires"] = *requirements;
                        }
                        digest_capabilities.push_back(std::move(digest_entry));
                    }
                    capabilities_.emplace(id, std::move(capability));
                }
            }
        }

        std::sort(
            digest_capabilities.begin(),
            digest_capabilities.end(),
            [](const json& left, const json& right) {
                return left.value("id", std::string{}) < right.value("id", std::string{});
            });

        json digest_input = {
            { "contractSet", contract_set_ },
            { "contractFiles", contract_files_ },
            { "capabilities", std::move(digest_capabilities) },
        };
        contract_set_digest_ = sha256(digest_input.dump());
        v2_contract_set_digest_ = sha256(json({
            { "schema", "ue.workflow-contract-set.v2" },
            { "baseContractSetDigest", contract_set_digest_ },
            { "workflowSchema", workflow_schema_v2_ },
            { "planSchema", plan_schema_v2_ },
        }).dump());
        loaded_ = !has_errors(diagnostics);
        return make_load_result(std::move(diagnostics));
    }

    Result validate(std::string_view workflow_json) const
    {
        if (!loaded_)
        {
            return not_loaded_result();
        }
        const auto input = json::parse(workflow_json, nullptr, false, true);
        const bool is_v2 =
            input.is_object() && input.value("dslVersion", std::string{}) == kDslVersionV2;
        auto validation = is_v2
            ? validate_v2_internal(workflow_json)
            : validate_internal(workflow_json);
        json payload = {
            { "schema", "ue.workflow-validation.v1" },
            { "contractSet", is_v2 ? contract_summary_v2() : contract_summary() },
            { "validationScope", {
                { "dsl", "ue.workflow" },
                { "dslVersion", is_v2 ? kDslVersionV2 : kDslVersion },
                { "workflowKind", "assetEdit" },
                { "capabilityCount", capabilities_.size() },
                { "composableOperationCount", composable_count() },
            } },
        };
        if (!validation.normalized_workflow.empty())
        {
            payload["normalizedWorkflow"] = std::move(validation.normalized_workflow);
        }
        return make_result(std::move(payload), std::move(validation.diagnostics), 2);
    }

    Result validate_contract_document(
        std::string_view document_json,
        const json& schema,
        std::string_view document_kind) const
    {
        if (!loaded_)
        {
            return not_loaded_result();
        }
        std::vector<Diagnostic> diagnostics;
        auto document = json::parse(document_json, nullptr, false, true);
        if (document.is_discarded())
        {
            add_diagnostic(
                diagnostics,
                "json",
                "invalid_json",
                "",
                std::string(document_kind) + " must be valid UTF-8 JSON.");
        }
        else
        {
            validate_schema(document, schema, schema, "", diagnostics);
        }
        json payload = {
            { "schema", "ue.workflow-contract-validation.v1" },
            { "documentKind", document_kind },
            { "contractSet", contract_summary() },
        };
        return make_result(std::move(payload), std::move(diagnostics), 2);
    }

    Result validate_result(std::string_view result_json) const
    {
        return validate_contract_document(result_json, result_schema_, "result");
    }

    Result validate_receipt(std::string_view receipt_json) const
    {
        return validate_contract_document(receipt_json, receipt_schema_, "receipt");
    }

    Result plan(std::string_view workflow_json) const
    {
        if (!loaded_)
        {
            return not_loaded_result();
        }
        const auto input = json::parse(workflow_json, nullptr, false, true);
        if (input.is_object() &&
            input.value("dslVersion", std::string{}) == kDslVersionV2)
        {
            return plan_v2(workflow_json);
        }
        auto validation = validate_internal(workflow_json);
        if (has_errors(validation.diagnostics))
        {
            json payload = {
                { "schema", "ue.workflow-plan.v1" },
                { "plannerVersion", kPlannerVersion },
                { "contractSetDigest", contract_set_digest_ },
            };
            return make_result(std::move(payload), std::move(validation.diagnostics), 2);
        }

        const auto& workflow = validation.normalized_workflow;
        const auto& scope = workflow["scope"];
        const auto scope_kind = scope.value("kind", std::string{});
        const auto scope_asset = scope.value("asset", std::string{});
        const bool create_if_missing = scope.value("createIfMissing", false);
        const auto scope_config_it = admission_["scopeKinds"].find(scope_kind);
        const json scope_config =
            scope_config_it != admission_["scopeKinds"].end() && scope_config_it->is_object()
            ? *scope_config_it
            : json::object();

        json initializers = json::array();
        std::string initializer_operation_type;
        if (const auto initializer = scope_config.find("initializer");
            initializer != scope_config.end() && initializer->is_object())
        {
            initializer_operation_type =
                initializer->value("operationType", std::string{});
        }
        std::size_t explicit_initializer_count = 0;
        std::size_t explicit_initializer_index = 0;
        for (std::size_t index = 0; index < validation.operations.size(); ++index)
        {
            if (validation.operations[index].operation.value("type", std::string{}) ==
                initializer_operation_type)
            {
                ++explicit_initializer_count;
                explicit_initializer_index = index;
            }
        }
        if (explicit_initializer_count > 1)
        {
            add_diagnostic(
                validation.diagnostics,
                "plan",
                "scope_initializer_duplicate",
                "/operations",
                "The scope initializer capability may appear at most once.");
        }
        if (explicit_initializer_count == 1 && explicit_initializer_index != 0)
        {
            add_diagnostic(
                validation.diagnostics,
                "plan",
                "scope_initializer_must_be_first",
                "/operations/" + std::to_string(explicit_initializer_index),
                "An explicit scope initializer must be the first authored operation.");
        }

        const bool automatic_initializer =
            create_if_missing && explicit_initializer_count == 0;
        if (automatic_initializer)
        {
            const auto initializer = scope_config.find("initializer");
            if (initializer == scope_config.end() || !initializer->is_object())
            {
                add_diagnostic(
                    validation.diagnostics,
                    "plan",
                    "scope_initializer_missing",
                    "/scope/createIfMissing",
                    "No deterministic initializer is registered for this scope kind.");
            }
            else
            {
                const auto operation_type = initializer->value("operationType", std::string{});
                const auto capability = capabilities_.find(operation_type);
                if (capability == capabilities_.end())
                {
                    add_diagnostic(
                        validation.diagnostics,
                        "plan",
                        "scope_initializer_unavailable",
                        "/scope/createIfMissing",
                        "The registered scope initializer capability is unavailable.");
                }
                else
                {
                    initializers.push_back({
                        { "id", "$initializer.create" },
                        { "kind", "createIfMissing" },
                        { "automatic", true },
                        { "operationType", operation_type },
                        { "params", initializer->value("params", json::object()) },
                        { "scopeParameters", capability->second.dsl.value("scopeParameters", json::array()) },
                        { "risk", capability->second.dsl.value("risk", "safeWrite") },
                        { "dependsOn", json::array() },
                    });
                }
            }
        }

        json planned_operations = json::array();
        std::string overall_risk = "readOnly";
        for (std::size_t planned_index = 0;
             planned_index < validation.operations.size();
             ++planned_index)
        {
            const auto& normalized = validation.operations[planned_index];
            json operation = normalized.operation;
            std::vector<std::string> dependencies = normalized.dependencies;
            if (automatic_initializer &&
                std::find(dependencies.begin(), dependencies.end(), "$initializer.create") ==
                    dependencies.end())
            {
                dependencies.insert(dependencies.begin(), "$initializer.create");
            }
            operation["dependsOn"] = dependencies;
            operation["automatic"] = false;
            operation["scopeParameters"] =
                normalized.capability->dsl.value("scopeParameters", json::array());
            operation["transactionDomain"] =
                normalized.capability->dsl.value("transactionDomain", "asset");
            operation["deferCompile"] =
                normalized.capability->dsl.value("deferCompile", true);
            const auto risk = normalized.capability->dsl.value("risk", "safeWrite");
            operation["risk"] = risk;
            if (explicit_initializer_count == 1 &&
                planned_index == explicit_initializer_index)
            {
                operation["kind"] = "scopeInitializer";
            }
            if (risk_rank(risk) > risk_rank(overall_risk))
            {
                overall_risk = risk;
            }
            planned_operations.push_back(std::move(operation));
        }
        for (const auto& initializer : initializers)
        {
            const auto risk = initializer.value("risk", "safeWrite");
            if (risk_rank(risk) > risk_rank(overall_risk))
            {
                overall_risk = risk;
            }
        }

        std::vector<std::string> authored_ids;
        for (const auto& operation : validation.operations)
        {
            authored_ids.push_back(operation.operation.value("id", std::string{}));
        }

        json finalizers = json::array();
        std::vector<std::string> post_compile_dependencies = authored_ids;
        std::vector<std::string> read_back_finalizer_ids;
        std::map<std::string, std::size_t> grouped_read_back_indices;
        const auto verify = workflow.value("verify", json::object());
        const auto compile = scope_config.find("compile");
        if (compile != scope_config.end() && compile->is_object())
        {
            finalizers.push_back({
                { "id", "$finalizer.compile" },
                { "kind", "compile" },
                { "automatic", true },
                { "operationType", compile->value("operationType", std::string{}) },
                { "dependsOn", authored_ids },
            });
            post_compile_dependencies = { "$finalizer.compile" };
        }
        else
        {
            add_diagnostic(
                validation.diagnostics,
                "plan",
                "compile_finalizer_unavailable",
                "/scope/kind",
                "The selected scope has no registered compile finalizer.");
        }

        const auto read_back_config = scope_config.find("readBack");
        for (const auto& read_back : verify.value("readBack", json::array()))
        {
            if (!read_back.is_string())
            {
                continue;
            }
            const auto key = read_back.get<std::string>();
            const json* entry = nullptr;
            if (read_back_config != scope_config.end() && read_back_config->is_object())
            {
                const auto found = read_back_config->find(key);
                if (found != read_back_config->end() && found->is_object())
                {
                    entry = &*found;
                }
            }
            if (!entry)
            {
                add_diagnostic(
                    validation.diagnostics,
                    "plan",
                    "read_back_unknown",
                    "/verify/readBack",
                    "Read-back key is not supported by this scope: " + key);
                continue;
            }
            if (key == "graphs")
            {
                std::set<std::string> graph_names;
                for (const auto& operation : validation.operations)
                {
                    if (const auto target =
                            graph_read_back_target(operation.operation))
                    {
                        graph_names.insert(*target);
                    }
                }
                if (graph_names.empty())
                {
                    add_diagnostic(
                        validation.diagnostics,
                        "plan",
                    "graph_read_back_target_missing",
                    "/verify/readBack",
                    "Graph read-back requires an authored operation with a deterministic Blueprint graph target.");
                    continue;
                }
                std::size_t graph_index = 0;
                for (const auto& graph_name : graph_names)
                {
                    const auto finalizer_id =
                        "$finalizer.readBack.graphs." + std::to_string(graph_index++);
                    finalizers.push_back({
                        { "id", finalizer_id },
                        { "kind", "readBack" },
                        { "readBackKey", key },
                        { "automatic", true },
                        { "operationType", entry->value("operationType", std::string{}) },
                        { "params", { { "graph", graph_name } } },
                        { "dependsOn", post_compile_dependencies },
                    });
                    read_back_finalizer_ids.push_back(finalizer_id);
                }
                continue;
            }
            const auto finalizer_id = "$finalizer.readBack." + key;
            const auto operation_type = entry->value("operationType", std::string{});
            const auto params = entry->value("params", json::object());
            const auto group_key = operation_type + "\n" + params.dump();
            if (const auto grouped = grouped_read_back_indices.find(group_key);
                grouped != grouped_read_back_indices.end())
            {
                auto& existing = finalizers.at(grouped->second);
                if (!existing.contains("readBackKeys"))
                {
                    existing["readBackKeys"] = json::array({
                        existing.value("readBackKey", std::string{}),
                    });
                    existing.erase("readBackKey");
                }
                existing["readBackKeys"].push_back(key);
                continue;
            }

            json finalizer = {
                {"id", finalizer_id},
                {"kind", "readBack"},
                {"readBackKey", key},
                {"automatic", true},
                {"operationType", operation_type},
                {"dependsOn", post_compile_dependencies},
            };
            if (!params.empty())
            {
                finalizer["params"] = params;
            }
            grouped_read_back_indices.emplace(group_key, finalizers.size());
            finalizers.push_back(std::move(finalizer));
            read_back_finalizer_ids.push_back(finalizer_id);
        }
        if (read_back_finalizer_ids.empty())
        {
            add_diagnostic(
                validation.diagnostics,
                "plan",
                "read_back_finalizer_required",
                "/verify/readBack",
                "UE Workflow v1 requires at least one deterministic structural read-back finalizer.");
        }
        const auto& diff_dependencies =
            read_back_finalizer_ids.empty()
            ? post_compile_dependencies
            : read_back_finalizer_ids;
        finalizers.push_back({
            { "id", "$finalizer.diff" },
            { "kind", "diff" },
            { "automatic", true },
            { "dependsOn", diff_dependencies },
        });

        if (has_errors(validation.diagnostics))
        {
            json payload = {
                { "schema", "ue.workflow-plan.v1" },
                { "plannerVersion", kPlannerVersion },
                { "contractSetDigest", contract_set_digest_ },
            };
            return make_result(std::move(payload), std::move(validation.diagnostics), 2);
        }

        json digest_input = {
            { "schema", "ue.workflow-plan-digest.v1" },
            { "plannerVersion", kPlannerVersion },
            { "contractSetDigest", contract_set_digest_ },
            { "normalizedWorkflow", workflow },
            { "initializers", initializers },
            { "operations", planned_operations },
            { "finalizers", finalizers },
        };
        const auto plan_digest = sha256(digest_input.dump());
        const bool approval_required = risk_rank(overall_risk) >= risk_rank("safeWrite");
        const bool confirm_write = risk_rank(overall_risk) >= risk_rank("confirmWrite");

        json plan_payload = {
            { "schema", "ue.workflow-plan.v1" },
            { "plannerVersion", kPlannerVersion },
            { "contractSetDigest", contract_set_digest_ },
            { "normalizedWorkflow", workflow },
            { "initializers", std::move(initializers) },
            { "operations", std::move(planned_operations) },
            { "finalizers", std::move(finalizers) },
            { "risk", {
                { "overall", overall_risk },
            } },
            { "approval", {
                { "required", approval_required },
                { "confirmWriteRequired", confirm_write },
                { "planDigest", plan_digest },
            } },
            { "expectedDiff", {
                { "kind", "assetDiff" },
                { "scopeKind", scope_kind },
                { "asset", scope_asset },
            } },
            { "planDigest", plan_digest },
        };
        validate_schema(
            plan_payload,
            plan_schema_,
            plan_schema_,
            "",
            validation.diagnostics);
        return make_result(std::move(plan_payload), std::move(validation.diagnostics), 2);
    }

    Result capabilities(const CapabilityQuery& query) const
    {
        if (!loaded_)
        {
            return not_loaded_result();
        }

        std::vector<Diagnostic> diagnostics;
        if (query.limit == 0 || query.limit > 100)
        {
            add_diagnostic(
                diagnostics,
                "capabilities",
                "capability_limit_invalid",
                "/limit",
                "Capability query limit must be between 1 and 100.");
            return make_result({
                { "schema", "ue.workflow-capabilities.v1" },
            }, std::move(diagnostics), 2);
        }
        if (query.available_only)
        {
            add_diagnostic(
                diagnostics,
                "capabilities",
                "capability_availability_requires_connection",
                "/availableOnly",
                "Capability availability requires a connection to the Unreal Editor.");
            return make_result({
                { "schema", "ue.workflow-capabilities.v1" },
            }, std::move(diagnostics), 2);
        }
        const std::set<std::string> valid_domains = {
            "ai",
            "animation",
            "blueprint",
            "content",
            "production",
            "scene",
        };
        const std::set<std::string> valid_kinds = {
            "command",
            "query",
            "validation",
        };
        const std::set<std::string> valid_output_kinds = {
            "image",
            "json",
        };
        const std::set<std::string> valid_risks = {
            "confirmWrite",
            "notOpen",
            "readOnly",
            "safeWrite",
        };
        auto validate_filter = [&diagnostics](
            const std::string& value,
            const std::set<std::string>& allowed,
            std::string_view field,
            std::string_view code) {
            if (!value.empty() && !allowed.contains(value))
            {
                add_diagnostic(
                    diagnostics,
                    "capabilities",
                    std::string(code),
                    "/" + std::string(field),
                    "Capability query contains an unsupported " +
                        std::string(field) + " value.");
            }
        };
        validate_filter(
            query.domain,
            valid_domains,
            "domain",
            "capability_domain_invalid");
        validate_filter(
            query.kind,
            valid_kinds,
            "kind",
            "capability_kind_invalid");
        validate_filter(
            query.output_kind,
            valid_output_kinds,
            "outputKind",
            "capability_output_kind_invalid");
        validate_filter(
            query.risk,
            valid_risks,
            "risk",
            "capability_risk_invalid");
        if (has_errors(diagnostics))
        {
            return make_result({
                { "schema", "ue.workflow-capabilities.v1" },
            }, std::move(diagnostics), 2);
        }

        auto lowercase = [](std::string value) {
            std::transform(
                value.begin(),
                value.end(),
                value.begin(),
                [](const unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            return value;
        };
        const auto search = lowercase(query.query);
        std::vector<const Capability*> matches;
        for (const auto& [id, capability] : capabilities_)
        {
            if (!query.operation.empty() && id != query.operation)
            {
                continue;
            }
            if (!query.domain.empty() && capability.domain != query.domain)
            {
                continue;
            }
            if (!query.kind.empty() &&
                capability.raw.value("kind", std::string{}) != query.kind)
            {
                continue;
            }
            const auto output_kind =
                capability.raw.value("output", json::object())
                    .value("kind", std::string{});
            if (!query.output_kind.empty() && output_kind != query.output_kind)
            {
                continue;
            }
            const auto descriptor_dsl =
                capability.raw.value("dsl", json::object());
            const auto descriptor_risk =
                descriptor_dsl.value("risk", std::string{});
            if (!query.risk.empty() && descriptor_risk != query.risk)
            {
                continue;
            }
            const auto traits =
                capability.raw.value("traits", json::object());
            auto trait_matches = [&traits](
                std::string_view name,
                const std::optional<bool>& expected) {
                return !expected.has_value() ||
                    traits.value(std::string(name), false) == *expected;
            };
            if (!trait_matches("readOnly", query.read_only) ||
                !trait_matches("destructive", query.destructive) ||
                !trait_matches("expensive", query.expensive))
            {
                continue;
            }
            if (!search.empty())
            {
                const auto haystack =
                    lowercase(id + "\n" + capability.description);
                if (haystack.find(search) == std::string::npos)
                {
                    continue;
                }
            }
            matches.push_back(&capability);
        }

        if (!query.operation.empty() && matches.empty())
        {
            add_diagnostic(
                diagnostics,
                "capabilities",
                "operation_unknown",
                "/operation",
                "Capability is not present in the loaded manifests.");
        }

        const std::size_t total = matches.size();
        const std::size_t effective_offset =
            query.operation.empty() ? query.offset : 0;
        const std::size_t effective_limit =
            query.operation.empty() ? query.limit : 1;
        const std::size_t page_end =
            effective_offset >= total
            ? total
            : std::min(effective_offset + effective_limit, total);
        const bool full =
            !query.operation.empty() || query.detail == CapabilityDetail::Full;
        json page = json::array();
        for (std::size_t index = effective_offset;
             index < page_end;
             ++index)
        {
            const Capability& capability = *matches[index];
            if (full)
            {
                json descriptor = capability.raw;
                descriptor["dsl"] = capability.dsl;
                descriptor["available"] = nullptr;
                descriptor["availabilityReasons"] =
                    json::array({ "editor_connection_required" });
                page.push_back(std::move(descriptor));
                continue;
            }
            json summary = {
                { "id", capability.id },
                { "domain", capability.domain },
                { "kind", capability.raw.value("kind", std::string{}) },
                { "description", capability.description },
                { "traits", capability.raw.value("traits", json::object()) },
                { "output", capability.raw.value("output", json::object()) },
                { "available", nullptr },
                { "availabilityReasons",
                    json::array({ "editor_connection_required" }) },
            };
            if (const auto requirements_it =
                    capability.raw.find("requires");
                requirements_it != capability.raw.end())
            {
                summary["requires"] = *requirements_it;
            }
            const auto descriptor_dsl =
                capability.raw.value("dsl", json::object());
            const auto descriptor_risk =
                descriptor_dsl.value("risk", std::string{});
            if (!descriptor_risk.empty())
            {
                summary["risk"] = descriptor_risk;
            }
            page.push_back(std::move(summary));
        }

        return make_result({
            { "schema", "ue.workflow-capabilities.v1" },
            { "capabilities", std::move(page) },
            { "total", total },
            { "offset", effective_offset },
            { "limit", effective_limit },
            { "hasMore",
                effective_offset < total && page_end < total },
            { "detail", full ? "full" : "summary" },
        }, std::move(diagnostics), 2);
    }

    Result help(std::string_view scope_kind) const
    {
        if (!loaded_)
        {
            return not_loaded_result();
        }
        std::vector<Diagnostic> diagnostics;
        json operations = json::array();
        for (const auto& [id, capability] : capabilities_)
        {
            const auto admission = capability.dsl.value("admission", "none");
            if (admission != "editStep")
            {
                continue;
            }
            const auto scopes = string_array(
                capability.dsl.contains("scopeKinds") ? &capability.dsl["scopeKinds"] : nullptr);
            if (!scope_kind.empty() &&
                std::find(scopes.begin(), scopes.end(), scope_kind) == scopes.end())
            {
                continue;
            }
            operations.push_back({
                { "type", id },
                { "description", capability.description },
                { "admission", admission },
                { "scopeKinds", scopes },
                { "risk", capability.dsl.value("risk", "safeWrite") },
                { "inputSchema", capability.input_schema },
                { "outputSchema", capability.output_schema },
            });
        }
        if (!scope_kind.empty() &&
            scope_kind != "blueprint" &&
            scope_kind != "widgetBlueprint" &&
            scope_kind != "widget" &&
            scope_kind != "material")
        {
            add_diagnostic(
                diagnostics,
                "help",
                "scope_unknown",
                "/scope",
                "Unknown composable scope. Use blueprint, widget, or material.");
        }
        return make_result({
            { "schema", "ue.workflow-help.v1" },
            { "product", "UE Workflow DSL" },
            { "dsl", "ue.workflow" },
            { "scope", std::string(scope_kind) },
            { "operations", std::move(operations) },
            { "contractSet", contract_summary() },
        }, std::move(diagnostics), 2);
    }

    Result explain(std::string_view operation_type) const
    {
        if (!loaded_)
        {
            return not_loaded_result();
        }
        std::vector<Diagnostic> diagnostics;
        const auto capability = capabilities_.find(std::string(operation_type));
        if (capability == capabilities_.end())
        {
            add_diagnostic(
                diagnostics,
                "help",
                "operation_unknown",
                "/operation",
                "Capability is not present in the loaded manifests.");
            return make_result({
                { "schema", "ue.workflow-operation-help.v1" },
                { "type", operation_type },
            }, std::move(diagnostics), 2);
        }
        return make_result({
            { "schema", "ue.workflow-operation-help.v1" },
            { "type", capability->first },
            { "domain", capability->second.domain },
            { "description", capability->second.description },
            { "inputSchema", capability->second.input_schema },
            { "outputSchema", capability->second.output_schema },
            { "dsl", capability->second.dsl },
        }, std::move(diagnostics), 2);
    }

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] std::size_t capability_count() const noexcept { return capabilities_.size(); }
    [[nodiscard]] std::size_t composable_count() const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            capabilities_.begin(),
            capabilities_.end(),
            [](const auto& item) {
                return item.second.dsl.value("admission", "none") == "editStep";
            }));
    }
    [[nodiscard]] const std::string& digest() const noexcept { return contract_set_digest_; }

private:
    json load_contract_file(
        const std::filesystem::path& relative,
        std::vector<Diagnostic>& diagnostics) const
    {
        const auto value = read_json_file(contract_root_ / relative, diagnostics, "contracts");
        return value ? *value : json::object();
    }

    Result make_load_result(std::vector<Diagnostic> diagnostics) const
    {
        return make_result({
            { "schema", "ue.workflow-contract-load.v1" },
            { "contractRoot", contract_root_.generic_string() },
            { "capabilityRoots", [&]() {
                json roots = json::array();
                for (const auto& root : capability_roots_)
                {
                    roots.push_back(root.generic_string());
                }
                return roots;
            }() },
            { "capabilityCount", capabilities_.size() },
            { "composableOperationCount", composable_count() },
            { "contractSetDigest", contract_set_digest_ },
        }, std::move(diagnostics), 4);
    }

    Result not_loaded_result() const
    {
        std::vector<Diagnostic> diagnostics;
        add_diagnostic(
            diagnostics,
            "contracts",
            "contracts_not_loaded",
            "/contracts",
            "UE Workflow contracts have not been loaded.",
            {},
            "Call Engine::Load or check the CLI contract paths.");
        return make_result({
            { "schema", "ue.workflow-error.v1" },
        }, std::move(diagnostics), 4);
    }

    json contract_summary() const
    {
        return {
            { "schema", "ue.workflow-contract-set.v1" },
            { "contractApiVersion", contract_set_.value("contractApiVersion", "1") },
            { "digest", contract_set_digest_ },
            { "capabilityCount", capabilities_.size() },
            { "composableOperationCount", composable_count() },
        };
    }

    json contract_summary_v2() const
    {
        return {
            { "schema", "ue.workflow-contract-set.v2" },
            { "contractApiVersion", "2" },
            { "baseContractSetDigest", contract_set_digest_ },
            { "digest", v2_contract_set_digest_ },
            { "capabilityCount", capabilities_.size() },
            { "composableOperationCount", composable_count() },
        };
    }

    ValidationData validate_internal(std::string_view workflow_json) const
    {
        ValidationData result;
        auto workflow = json::parse(workflow_json, nullptr, false, true);
        if (workflow.is_discarded())
        {
            add_diagnostic(
                result.diagnostics,
                "json",
                "invalid_json",
                "",
                "Workflow input must be valid UTF-8 JSON.");
            return result;
        }

        validate_schema(
            workflow,
            workflow_schema_,
            workflow_schema_,
            "",
            result.diagnostics);
        find_forbidden_interpolation(workflow, "", result.diagnostics);
        if (!workflow.is_object())
        {
            return result;
        }

        const bool default_read_back =
            !workflow.contains("verify") ||
            !workflow["verify"].is_object() ||
            !workflow["verify"].contains("readBack") ||
            (workflow["verify"]["readBack"].is_array() &&
             workflow["verify"]["readBack"].empty());
        json normalized = workflow;
        if (!normalized.contains("persistence"))
        {
            normalized["persistence"] = "dirtyOnly";
        }
        if (normalized.contains("scope") && normalized["scope"].is_object() &&
            !normalized["scope"].contains("createIfMissing"))
        {
            normalized["scope"]["createIfMissing"] = false;
        }

        const auto scope_kind =
            normalized.contains("scope") && normalized["scope"].is_object()
            ? normalized["scope"].value("kind", std::string{})
            : std::string{};
        const json* scope_config = nullptr;
        if (admission_.contains("scopeKinds") && admission_["scopeKinds"].is_object())
        {
            const auto found_scope = admission_["scopeKinds"].find(scope_kind);
            if (found_scope != admission_["scopeKinds"].end() && found_scope->is_object())
            {
                scope_config = &*found_scope;
            }
        }
        std::set<std::string> forbidden_scope_parameters;
        if (scope_config &&
            scope_config->contains("forbiddenParameters"))
        {
            for (const auto& parameter : string_array(&(*scope_config)["forbiddenParameters"]))
            {
                forbidden_scope_parameters.insert(parameter);
            }
        }

        if (!normalized.contains("verify"))
        {
            normalized["verify"] = json::object();
        }
        if (normalized["verify"].is_object())
        {
            // UE Workflow v1 finalization is invariant: authored verification
            // fields may select read-back detail, but cannot disable compile or
            // all structural read-back. Canonicalizing compatibility-shaped
            // inputs keeps approval digests deterministic.
            normalized["verify"]["compile"] = true;
            if (!normalized["verify"].contains("readBack") ||
                (normalized["verify"]["readBack"].is_array() &&
                 normalized["verify"]["readBack"].empty()))
            {
                normalized["verify"]["readBack"] =
                    scope_config && scope_config->contains("defaultReadBack")
                    ? (*scope_config)["defaultReadBack"]
                    : json::array();
            }
        }

        if (!normalized.contains("operations") || !normalized["operations"].is_array())
        {
            result.normalized_workflow = std::move(normalized);
            return result;
        }

        std::map<std::string, std::size_t, std::less<>> operation_indices;
        std::set<std::string> transaction_domains;
        json normalized_operations = json::array();

        for (std::size_t index = 0; index < normalized["operations"].size(); ++index)
        {
            const auto& authored = normalized["operations"][index];
            const auto base_path = "/operations/" + std::to_string(index);
            if (!authored.is_object())
            {
                continue;
            }
            const auto id = authored.value("id", std::string{});
            const auto type = authored.value("type", std::string{});
            if (!id.empty())
            {
                if (operation_indices.contains(id))
                {
                    add_diagnostic(
                        result.diagnostics,
                        "workflow",
                        "duplicate_operation_id",
                        base_path + "/id",
                        "Operation ids must be unique.",
                        id);
                }
                else
                {
                    operation_indices.emplace(id, index);
                }
            }

            const auto capability = capabilities_.find(type);
            if (capability == capabilities_.end())
            {
                add_diagnostic(
                    result.diagnostics,
                    "workflow",
                    "unknown_operation",
                    base_path + "/type",
                    "Operation is not present in the loaded capability manifests.",
                    id);
                normalized_operations.push_back(authored);
                continue;
            }
            const auto admission = capability->second.dsl.value("admission", "none");
            if (admission != "editStep")
            {
                add_diagnostic(
                    result.diagnostics,
                    "workflow",
                    "operation_not_composable",
                    base_path + "/type",
                    "Only operations with dsl.admission=editStep may appear in UE Workflow v1.",
                    id,
                    "Use the domain MCP tool or ue <capability> for non-composable capabilities.");
            }

            const auto scopes = string_array(
                capability->second.dsl.contains("scopeKinds")
                ? &capability->second.dsl["scopeKinds"]
                : nullptr);
            if (std::find(scopes.begin(), scopes.end(), scope_kind) == scopes.end())
            {
                add_diagnostic(
                    result.diagnostics,
                    "workflow",
                    "operation_scope_mismatch",
                    base_path + "/type",
                    "Operation does not support the workflow scope kind.",
                    id);
            }
            transaction_domains.insert(
                capability->second.dsl.value("transactionDomain", std::string{}));

            json normalized_operation = authored;
            normalized_operation["params"] = normalize_params(
                authored.value("params", json::object()),
                capability->second,
                base_path + "/params",
                id,
                result.diagnostics);
            std::set<std::string> operation_scope_parameters;
            for (const auto& parameter : string_array(
                     capability->second.dsl.contains("scopeParameters")
                     ? &capability->second.dsl["scopeParameters"]
                     : nullptr))
            {
                operation_scope_parameters.insert(parameter);
                if (normalized_operation["params"].contains(parameter))
                {
                    add_diagnostic(
                        result.diagnostics,
                        "scope",
                        "scope_parameter_authored",
                        base_path + "/params/" + pointer_escape(parameter),
                        "Scope-bound capability parameters are injected from scope.asset and may not be authored.",
                        id);
                }
            }
            for (const auto& parameter : forbidden_scope_parameters)
            {
                if (normalized_operation["params"].contains(parameter))
                {
                    add_diagnostic(
                        result.diagnostics,
                        "scope",
                        "scope_parameter_forbidden",
                        base_path + "/params/" + pointer_escape(parameter),
                        "This parameter would escape the workflow's single primary asset scope.",
                        id);
                }
            }

            std::vector<std::string> dependencies;
            if (const auto depends_on = authored.find("dependsOn");
                depends_on != authored.end() && depends_on->is_array())
            {
                for (const auto& dependency : *depends_on)
                {
                    if (!dependency.is_string())
                    {
                        continue;
                    }
                    const auto dependency_id = dependency.get<std::string>();
                    const auto found = operation_indices.find(dependency_id);
                    if (found == operation_indices.end() || found->second >= index)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "dependency",
                            "dependency_not_prior",
                            base_path + "/dependsOn",
                            "Dependencies must reference an earlier operation.",
                            id);
                    }
                    if (std::find(dependencies.begin(), dependencies.end(), dependency_id) ==
                        dependencies.end())
                    {
                        dependencies.push_back(dependency_id);
                    }
                }
            }

            std::set<std::string> satisfied_parameters;
            for (const auto& parameter : operation_scope_parameters)
            {
                satisfied_parameters.insert(parameter);
            }

            json normalized_bindings = json::object();
            if (const auto bindings = authored.find("bindings");
                bindings != authored.end() && bindings->is_object())
            {
                for (const auto& [destination_text, binding] : bindings->items())
                {
                    if (!binding.is_object() ||
                        !binding.contains("from") || !binding["from"].is_string() ||
                        !binding.contains("path") || !binding["path"].is_string())
                    {
                        continue;
                    }

                    auto destination_tokens = parse_json_pointer(destination_text);
                    if (!destination_tokens || destination_tokens->empty())
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_invalid",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "Binding destination must be a non-empty RFC 6901 JSON Pointer.",
                            id);
                        continue;
                    }

                    const bool explicit_params = destination_tokens->front() == "params";
                    std::vector<std::string> capability_tokens = *destination_tokens;
                    if (explicit_params)
                    {
                        capability_tokens.erase(capability_tokens.begin());
                    }
                    if (capability_tokens.empty())
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_invalid",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "Binding destination may not replace the entire params object.",
                            id);
                        continue;
                    }
                    if (capability_tokens.size() != 1)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_depth_unsupported",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "UE Workflow v1 binding destinations must target one top-level capability parameter.",
                            id,
                            "Use /params/<field> or the shorthand /<field>; source pointers may remain nested.");
                        continue;
                    }

                    capability_tokens.front() =
                        capability_alias(capability->second, capability_tokens.front());
                    if (operation_scope_parameters.contains(capability_tokens.front()) ||
                        forbidden_scope_parameters.contains(capability_tokens.front()))
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "scope",
                            "scope_binding_forbidden",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "Bindings may not override scope-injected or scope-forbidden capability parameters.",
                            id);
                        continue;
                    }
                    std::vector<std::string> normalized_destination_tokens = capability_tokens;
                    normalized_destination_tokens.insert(
                        normalized_destination_tokens.begin(),
                        "params");
                    const auto normalized_destination =
                        make_json_pointer(normalized_destination_tokens);
                    const auto* destination_schema =
                        schema_at_tokens(capability->second.input_schema, capability_tokens);
                    if (!destination_schema)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_unknown",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "Binding destination is not declared by the capability input schema.",
                            id);
                        continue;
                    }

                    const auto source_id = binding["from"].get<std::string>();
                    const auto source_index = operation_indices.find(source_id);
                    if (source_index == operation_indices.end() || source_index->second >= index)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_source_not_prior",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "Binding source must reference an earlier operation.",
                            id);
                        continue;
                    }
                    const auto source_operation = std::find_if(
                        result.operations.begin(),
                        result.operations.end(),
                        [&source_id](const NormalizedOperation& candidate) {
                            return candidate.operation.value("id", std::string{}) == source_id;
                        });
                    if (source_operation == result.operations.end())
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_source_unavailable",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "Binding source operation did not pass operation validation.",
                            id);
                        continue;
                    }
                    const auto source_path = binding["path"].get<std::string>();
                    const auto source_tokens = parse_json_pointer(source_path);
                    if (!source_tokens || source_tokens->empty())
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_source_path_invalid",
                            base_path + "/bindings/" + pointer_escape(destination_text) + "/path",
                            "Binding source path must be a non-empty RFC 6901 JSON Pointer.",
                            id);
                        continue;
                    }
                    const auto* source_schema =
                        schema_at_tokens(source_operation->capability->output_schema, *source_tokens);
                    if (!source_schema)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_source_untyped",
                            base_path + "/bindings/" + pointer_escape(destination_text) + "/path",
                            "Binding source path is not declared by the source operation output schema.",
                            id);
                        continue;
                    }
                    if (!types_compatible(source_schema, destination_schema))
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_type_mismatch",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "Binding source and destination JSON Schema types are incompatible.",
                            id);
                        continue;
                    }
                    if (normalized_operation["params"].contains(capability_tokens.front()))
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_conflict",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "Binding destination conflicts with an explicitly supplied parameter.",
                            id);
                    }
                    if (normalized_bindings.contains(normalized_destination))
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_duplicate",
                            base_path + "/bindings/" + pointer_escape(destination_text),
                            "Multiple bindings normalize to the same destination.",
                            id);
                    }
                    else
                    {
                        normalized_bindings[normalized_destination] = {
                            { "from", source_id },
                            { "path", make_json_pointer(*source_tokens) },
                        };
                    }
                    satisfied_parameters.insert(capability_tokens.front());
                    if (std::find(dependencies.begin(), dependencies.end(), source_id) ==
                        dependencies.end())
                    {
                        dependencies.push_back(source_id);
                    }
                }
            }

            if (!normalized_bindings.empty())
            {
                normalized_operation["bindings"] = std::move(normalized_bindings);
            }
            else
            {
                normalized_operation.erase("bindings");
            }
            normalized_operation["dependsOn"] = dependencies;
            validate_schema(
                normalized_operation["params"],
                capability->second.input_schema,
                capability->second.input_schema,
                base_path + "/params",
                result.diagnostics,
                &satisfied_parameters);

            result.operations.push_back(NormalizedOperation{
                normalized_operation,
                &capability->second,
                dependencies,
            });
            normalized_operations.push_back(std::move(normalized_operation));
        }

        if (default_read_back &&
            (scope_kind == "blueprint" || scope_kind == "widgetBlueprint"))
        {
            std::set<std::string> graph_names;
            for (const auto& operation : result.operations)
            {
                if (const auto target =
                        graph_read_back_target(operation.operation))
                {
                    graph_names.insert(*target);
                }
            }
            if (!graph_names.empty())
            {
                auto& read_back = normalized["verify"]["readBack"];
                if (std::find(read_back.begin(), read_back.end(), json("graphs")) == read_back.end())
                {
                    read_back.push_back("graphs");
                }
            }
        }

        if (transaction_domains.size() > 1 ||
            (!transaction_domains.empty() && *transaction_domains.begin() != "asset"))
        {
            add_diagnostic(
                result.diagnostics,
                "workflow",
                "transaction_domain_mismatch",
                "/operations",
                "All UE Workflow v1 operations must share the asset transaction domain.");
        }

        normalized["operations"] = std::move(normalized_operations);
        result.normalized_workflow = std::move(normalized);
        return result;
    }

    ValidationData validate_v2_internal(std::string_view workflow_json) const
    {
        ValidationData result;
        auto workflow = json::parse(workflow_json, nullptr, false, true);
        if (workflow.is_discarded())
        {
            add_diagnostic(
                result.diagnostics,
                "json",
                "invalid_json",
                "",
                "Workflow input must be valid UTF-8 JSON.");
            return result;
        }

        validate_schema(
            workflow,
            workflow_schema_v2_,
            workflow_schema_v2_,
            "",
            result.diagnostics);
        find_forbidden_interpolation(workflow, "", result.diagnostics);
        if (!workflow.is_object())
        {
            return result;
        }

        json normalized = workflow;
        if (!normalized.contains("persistence"))
        {
            normalized["persistence"] = "dirtyOnly";
        }
        if (!normalized.contains("scopes") || !normalized["scopes"].is_object())
        {
            result.normalized_workflow = std::move(normalized);
            return result;
        }
        if (normalized["scopes"].empty())
        {
            add_diagnostic(
                result.diagnostics,
                "scope",
                "scope_set_empty",
                "/scopes",
                "UE Workflow v2 requires at least one named scope.");
        }
        if (normalized["scopes"].size() > kMaxV2Scopes)
        {
            add_diagnostic(
                result.diagnostics,
                "scope",
                "scope_limit_exceeded",
                "/scopes",
                "UE Workflow v2 supports at most 16 named scopes.");
        }

        std::set<std::string> seen_assets;
        for (auto& [scope_id, scope] : normalized["scopes"].items())
        {
            const auto scope_path = "/scopes/" + pointer_escape(scope_id);
            if (!scope.is_object())
            {
                continue;
            }
            if (!scope.contains("createIfMissing"))
            {
                scope["createIfMissing"] = false;
            }
            const auto asset = scope.value("asset", std::string{});
            if (!asset.empty() && !seen_assets.insert(asset).second)
            {
                add_diagnostic(
                    result.diagnostics,
                    "scope",
                    "duplicate_scope_asset",
                    scope_path + "/asset",
                    "Each named scope must target a distinct asset.");
            }

            const auto scope_kind = scope.value("kind", std::string{});
            const json* scope_config = nullptr;
            if (admission_.contains("scopeKinds") &&
                admission_["scopeKinds"].is_object())
            {
                const auto found =
                    admission_["scopeKinds"].find(scope_kind);
                if (found != admission_["scopeKinds"].end() &&
                    found->is_object())
                {
                    scope_config = &*found;
                }
            }
            if (!scope_config)
            {
                add_diagnostic(
                    result.diagnostics,
                    "scope",
                    "scope_kind_unknown",
                    scope_path + "/kind",
                    "Named scope kind is not registered for UE Workflow.");
                continue;
            }

            if (!scope.contains("verify") || !scope["verify"].is_object())
            {
                scope["verify"] = json::object();
            }
            scope["verify"]["compile"] = true;
            if (!scope["verify"].contains("readBack") ||
                !scope["verify"]["readBack"].is_array() ||
                scope["verify"]["readBack"].empty())
            {
                scope["verify"]["readBack"] =
                    scope_config->value("defaultReadBack", json::array());
            }
        }

        if (!normalized.contains("operations") ||
            !normalized["operations"].is_array())
        {
            result.normalized_workflow = std::move(normalized);
            return result;
        }
        if (normalized["operations"].size() > kMaxV2Operations)
        {
            add_diagnostic(
                result.diagnostics,
                "workflow",
                "operation_limit_exceeded",
                "/operations",
                "UE Workflow v2 supports at most 256 operations.");
        }

        const std::size_t operation_count = normalized["operations"].size();
        std::map<std::string, std::size_t, std::less<>> authored_indices;
        std::vector<const Capability*> operation_capabilities(
            operation_count,
            nullptr);
        for (std::size_t index = 0; index < operation_count; ++index)
        {
            const auto& operation = normalized["operations"][index];
            if (!operation.is_object())
            {
                continue;
            }
            const auto id = operation.value("id", std::string{});
            if (!id.empty() && !authored_indices.emplace(id, index).second)
            {
                add_diagnostic(
                    result.diagnostics,
                    "workflow",
                    "duplicate_operation_id",
                    "/operations/" + std::to_string(index) + "/id",
                    "Operation ids must be unique.",
                    id);
            }
            const auto capability =
                capabilities_.find(operation.value("type", std::string{}));
            if (capability != capabilities_.end())
            {
                operation_capabilities[index] = &capability->second;
            }
        }

        std::vector<NormalizedOperation> candidates;
        candidates.reserve(operation_count);
        std::set<std::string> transaction_domains;
        std::set<std::string> all_forbidden_scope_parameters;
        if (admission_.contains("scopeKinds") &&
            admission_["scopeKinds"].is_object())
        {
            for (const auto& [ignored, config] :
                 admission_["scopeKinds"].items())
            {
                (void)ignored;
                if (!config.is_object() ||
                    !config.contains("forbiddenParameters"))
                {
                    continue;
                }
                for (const auto& parameter :
                     string_array(&config["forbiddenParameters"]))
                {
                    all_forbidden_scope_parameters.insert(parameter);
                }
            }
        }

        for (std::size_t index = 0; index < operation_count; ++index)
        {
            const auto& authored = normalized["operations"][index];
            const auto base_path = "/operations/" + std::to_string(index);
            if (!authored.is_object())
            {
                continue;
            }
            const auto id = authored.value("id", std::string{});
            const auto type = authored.value("type", std::string{});
            const auto scope_id = authored.value("scope", std::string{});
            const Capability* capability = operation_capabilities[index];
            if (!capability)
            {
                add_diagnostic(
                    result.diagnostics,
                    "workflow",
                    "unknown_operation",
                    base_path + "/type",
                    "Operation is not present in the loaded capability manifests.",
                    id);
                continue;
            }
            if (capability->dsl.value("admission", "none") != "editStep")
            {
                add_diagnostic(
                    result.diagnostics,
                    "workflow",
                    "operation_not_composable",
                    base_path + "/type",
                    "Only operations with dsl.admission=editStep may appear in UE Workflow v2.",
                    id);
            }

            const auto scope = normalized["scopes"].find(scope_id);
            const auto scope_kind =
                scope != normalized["scopes"].end() && scope->is_object()
                ? scope->value("kind", std::string{})
                : std::string{};
            if (scope == normalized["scopes"].end())
            {
                add_diagnostic(
                    result.diagnostics,
                    "scope",
                    "operation_scope_unknown",
                    base_path + "/scope",
                    "Operation references an unknown named scope.",
                    id);
            }
            const auto supported_scopes = string_array(
                capability->dsl.contains("scopeKinds")
                ? &capability->dsl["scopeKinds"]
                : nullptr);
            if (!scope_kind.empty() &&
                std::find(
                    supported_scopes.begin(),
                    supported_scopes.end(),
                    scope_kind) == supported_scopes.end())
            {
                add_diagnostic(
                    result.diagnostics,
                    "scope",
                    "operation_scope_mismatch",
                    base_path + "/scope",
                    "Operation does not support the referenced scope kind.",
                    id);
            }

            transaction_domains.insert(
                capability->dsl.value("transactionDomain", std::string{}));
            json normalized_operation = authored;
            normalized_operation["params"] = normalize_params(
                authored.value("params", json::object()),
                *capability,
                base_path + "/params",
                id,
                result.diagnostics);

            std::set<std::string> scope_parameters;
            for (const auto& parameter : string_array(
                     capability->dsl.contains("scopeParameters")
                     ? &capability->dsl["scopeParameters"]
                     : nullptr))
            {
                scope_parameters.insert(parameter);
                if (normalized_operation["params"].contains(parameter))
                {
                    add_diagnostic(
                        result.diagnostics,
                        "scope",
                        "scope_parameter_authored",
                        base_path + "/params/" + pointer_escape(parameter),
                        "Scope-bound capability parameters are injected from the referenced named scope.",
                        id);
                }
            }
            for (const auto& parameter : all_forbidden_scope_parameters)
            {
                if (normalized_operation["params"].contains(parameter))
                {
                    add_diagnostic(
                        result.diagnostics,
                        "scope",
                        "scope_parameter_forbidden",
                        base_path + "/params/" + pointer_escape(parameter),
                        "This parameter would escape the operation's named asset scope.",
                        id);
                }
            }

            std::vector<std::string> dependencies;
            std::set<std::string> dependency_set;
            if (const auto depends_on = authored.find("dependsOn");
                depends_on != authored.end() && depends_on->is_array())
            {
                for (const auto& dependency : *depends_on)
                {
                    if (!dependency.is_string())
                    {
                        continue;
                    }
                    const auto dependency_id = dependency.get<std::string>();
                    if (!authored_indices.contains(dependency_id))
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "dependency",
                            "dependency_unknown",
                            base_path + "/dependsOn",
                            "Dependency references an unknown operation.",
                            id);
                    }
                    else if (dependency_id == id)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "dependency",
                            "dependency_self_reference",
                            base_path + "/dependsOn",
                            "Operation may not depend on itself.",
                            id);
                    }
                    else if (dependency_set.insert(dependency_id).second)
                    {
                        dependencies.push_back(dependency_id);
                    }
                }
            }

            std::set<std::string> satisfied_parameters = scope_parameters;
            json normalized_bindings = json::object();
            if (const auto bindings = authored.find("bindings");
                bindings != authored.end() && bindings->is_object())
            {
                for (const auto& [destination_text, binding] :
                     bindings->items())
                {
                    if (!binding.is_object() ||
                        !binding.contains("from") ||
                        !binding["from"].is_string() ||
                        !binding.contains("path") ||
                        !binding["path"].is_string())
                    {
                        continue;
                    }
                    auto destination_tokens =
                        parse_json_pointer(destination_text);
                    if (!destination_tokens || destination_tokens->empty())
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_invalid",
                            base_path + "/bindings/" +
                                pointer_escape(destination_text),
                            "Binding destination must be a non-empty RFC 6901 JSON Pointer.",
                            id);
                        continue;
                    }
                    if (destination_tokens->front() == "params")
                    {
                        destination_tokens->erase(destination_tokens->begin());
                    }
                    if (destination_tokens->size() != 1)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_depth_unsupported",
                            base_path + "/bindings/" +
                                pointer_escape(destination_text),
                            "Workflow binding destinations must target one top-level capability parameter.",
                            id);
                        continue;
                    }
                    destination_tokens->front() =
                        capability_alias(*capability, destination_tokens->front());
                    const auto destination_parameter =
                        destination_tokens->front();
                    if (scope_parameters.contains(destination_parameter) ||
                        all_forbidden_scope_parameters.contains(
                            destination_parameter))
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "scope",
                            "scope_binding_forbidden",
                            base_path + "/bindings/" +
                                pointer_escape(destination_text),
                            "Bindings may not override scope-injected or scope-forbidden parameters.",
                            id);
                        continue;
                    }
                    const auto* destination_schema = schema_at_tokens(
                        capability->input_schema,
                        *destination_tokens);
                    if (!destination_schema)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_unknown",
                            base_path + "/bindings/" +
                                pointer_escape(destination_text),
                            "Binding destination is not declared by the capability input schema.",
                            id);
                        continue;
                    }

                    const auto source_id =
                        binding["from"].get<std::string>();
                    const auto source_index =
                        authored_indices.find(source_id);
                    if (source_index == authored_indices.end() ||
                        source_id == id)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_source_unknown",
                            base_path + "/bindings/" +
                                pointer_escape(destination_text),
                            "Binding source must reference another workflow operation.",
                            id);
                        continue;
                    }
                    const Capability* source_capability =
                        operation_capabilities[source_index->second];
                    const auto source_tokens =
                        parse_json_pointer(binding["path"].get<std::string>());
                    const auto* source_schema =
                        source_capability && source_tokens &&
                            !source_tokens->empty()
                        ? schema_at_tokens(
                            source_capability->output_schema,
                            *source_tokens)
                        : nullptr;
                    if (!source_schema)
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_source_untyped",
                            base_path + "/bindings/" +
                                pointer_escape(destination_text) + "/path",
                            "Binding source path is not declared by the source operation output schema.",
                            id);
                        continue;
                    }
                    if (!types_compatible(source_schema, destination_schema))
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_type_mismatch",
                            base_path + "/bindings/" +
                                pointer_escape(destination_text),
                            "Binding source and destination JSON Schema types are incompatible.",
                            id);
                        continue;
                    }
                    if (normalized_operation["params"].contains(
                            destination_parameter))
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_conflict",
                            base_path + "/bindings/" +
                                pointer_escape(destination_text),
                            "Binding destination conflicts with an explicitly supplied parameter.",
                            id);
                    }
                    std::vector<std::string> normalized_tokens = {
                        "params",
                        destination_parameter,
                    };
                    const auto normalized_destination =
                        make_json_pointer(normalized_tokens);
                    if (normalized_bindings.contains(normalized_destination))
                    {
                        add_diagnostic(
                            result.diagnostics,
                            "binding",
                            "binding_destination_duplicate",
                            base_path + "/bindings/" +
                                pointer_escape(destination_text),
                            "Multiple bindings normalize to the same destination.",
                            id);
                    }
                    else
                    {
                        normalized_bindings[normalized_destination] = {
                            { "from", source_id },
                            { "path", make_json_pointer(*source_tokens) },
                        };
                    }
                    satisfied_parameters.insert(destination_parameter);
                    if (dependency_set.insert(source_id).second)
                    {
                        dependencies.push_back(source_id);
                    }
                }
            }
            if (normalized_bindings.empty())
            {
                normalized_operation.erase("bindings");
            }
            else
            {
                normalized_operation["bindings"] =
                    std::move(normalized_bindings);
            }
            normalized_operation["dependsOn"] = dependencies;
            validate_schema(
                normalized_operation["params"],
                capability->input_schema,
                capability->input_schema,
                base_path + "/params",
                result.diagnostics,
                &satisfied_parameters);
            candidates.push_back({
                std::move(normalized_operation),
                capability,
                std::move(dependencies),
            });
        }

        if (transaction_domains.size() > 1 ||
            (!transaction_domains.empty() &&
             *transaction_domains.begin() != "asset"))
        {
            add_diagnostic(
                result.diagnostics,
                "workflow",
                "transaction_domain_mismatch",
                "/operations",
                "All UE Workflow v2 operations must use the asset transaction domain.");
        }

        std::map<std::string, std::size_t, std::less<>> candidate_indices;
        for (std::size_t index = 0; index < candidates.size(); ++index)
        {
            candidate_indices.emplace(
                candidates[index].operation.value("id", std::string{}),
                index);
        }
        std::vector<int> indegree(candidates.size(), 0);
        std::vector<std::vector<std::size_t>> outgoing(candidates.size());
        for (std::size_t index = 0; index < candidates.size(); ++index)
        {
            for (const auto& dependency : candidates[index].dependencies)
            {
                const auto found = candidate_indices.find(dependency);
                if (found == candidate_indices.end())
                {
                    continue;
                }
                ++indegree[index];
                outgoing[found->second].push_back(index);
            }
        }
        std::vector<bool> emitted(candidates.size(), false);
        std::vector<NormalizedOperation> ordered;
        ordered.reserve(candidates.size());
        while (ordered.size() < candidates.size())
        {
            std::optional<std::size_t> next;
            for (std::size_t index = 0; index < candidates.size(); ++index)
            {
                if (!emitted[index] && indegree[index] == 0)
                {
                    next = index;
                    break;
                }
            }
            if (!next)
            {
                add_diagnostic(
                    result.diagnostics,
                    "dependency",
                    "dependency_cycle",
                    "/operations",
                    "Operation dependency and binding edges contain a cycle.");
                break;
            }
            emitted[*next] = true;
            ordered.push_back(candidates[*next]);
            for (const auto target : outgoing[*next])
            {
                --indegree[target];
            }
        }
        if (ordered.size() == candidates.size())
        {
            result.operations = std::move(ordered);
        }
        else
        {
            result.operations = std::move(candidates);
        }
        json normalized_operations = json::array();
        for (const auto& operation : result.operations)
        {
            normalized_operations.push_back(operation.operation);
        }
        normalized["operations"] = std::move(normalized_operations);
        result.normalized_workflow = std::move(normalized);
        return result;
    }

    Result plan_v2(std::string_view workflow_json) const
    {
        auto validation = validate_v2_internal(workflow_json);
        if (has_errors(validation.diagnostics))
        {
            return make_result({
                { "schema", "ue.workflow-plan.v2" },
                { "plannerVersion", kPlannerVersionV2 },
                { "contractSetDigest", v2_contract_set_digest_ },
            }, std::move(validation.diagnostics), 2);
        }

        const auto& workflow = validation.normalized_workflow;
        struct ScopePlanData
        {
            std::string id;
            std::string kind;
            std::string asset;
            json scope = json::object();
            json config = json::object();
        };
        std::vector<ScopePlanData> scopes;
        for (const auto& [scope_id, scope] : workflow["scopes"].items())
        {
            const auto kind = scope.value("kind", std::string{});
            const auto config = admission_["scopeKinds"].find(kind);
            scopes.push_back({
                scope_id,
                kind,
                scope.value("asset", std::string{}),
                scope,
                config != admission_["scopeKinds"].end() &&
                        config->is_object()
                    ? *config
                    : json::object(),
            });
        }
        std::sort(
            scopes.begin(),
            scopes.end(),
            [](const ScopePlanData& left, const ScopePlanData& right) {
                return std::tie(left.asset, left.id) <
                    std::tie(right.asset, right.id);
            });

        json asset_set = json::array();
        json initializers = json::array();
        std::map<std::string, std::string, std::less<>> initializer_ids;
        std::string overall_risk = "readOnly";
        for (const auto& scope : scopes)
        {
            asset_set.push_back({
                { "scopeId", scope.id },
                { "kind", scope.kind },
                { "asset", scope.asset },
                { "createIfMissing", scope.scope.value("createIfMissing", false) },
            });

            const auto initializer_config =
                scope.config.find("initializer");
            const auto initializer_type =
                initializer_config != scope.config.end() &&
                    initializer_config->is_object()
                ? initializer_config->value(
                    "operationType",
                    std::string{})
                : std::string{};
            std::size_t explicit_count = 0;
            std::size_t first_scope_operation = validation.operations.size();
            std::size_t explicit_index = validation.operations.size();
            for (std::size_t index = 0;
                 index < validation.operations.size();
                 ++index)
            {
                const auto& operation =
                    validation.operations[index].operation;
                if (operation.value("scope", std::string{}) != scope.id)
                {
                    continue;
                }
                first_scope_operation =
                    std::min(first_scope_operation, index);
                if (operation.value("type", std::string{}) ==
                    initializer_type)
                {
                    ++explicit_count;
                    explicit_index = index;
                }
            }
            if (explicit_count > 1)
            {
                add_diagnostic(
                    validation.diagnostics,
                    "plan",
                    "scope_initializer_duplicate",
                    "/scopes/" + pointer_escape(scope.id),
                    "A named scope initializer may appear at most once.");
            }
            if (explicit_count == 1 &&
                explicit_index != first_scope_operation)
            {
                add_diagnostic(
                    validation.diagnostics,
                    "plan",
                    "scope_initializer_must_be_first",
                    "/operations/" + std::to_string(explicit_index),
                    "An explicit initializer must be the first operation for its named scope.");
            }

            if (scope.scope.value("createIfMissing", false) &&
                explicit_count == 0)
            {
                if (initializer_type.empty())
                {
                    add_diagnostic(
                        validation.diagnostics,
                        "plan",
                        "scope_initializer_missing",
                        "/scopes/" + pointer_escape(scope.id) +
                            "/createIfMissing",
                        "No deterministic initializer is registered for this scope kind.");
                    continue;
                }
                const auto capability =
                    capabilities_.find(initializer_type);
                if (capability == capabilities_.end())
                {
                    add_diagnostic(
                        validation.diagnostics,
                        "plan",
                        "scope_initializer_unavailable",
                        "/scopes/" + pointer_escape(scope.id) +
                            "/createIfMissing",
                        "The registered initializer capability is unavailable.");
                    continue;
                }
                const auto initializer_id =
                    "$initializer." + scope.id + ".create";
                initializer_ids[scope.id] = initializer_id;
                const auto risk =
                    capability->second.dsl.value("risk", "safeWrite");
                initializers.push_back({
                    { "id", initializer_id },
                    { "scope", scope.id },
                    { "kind", "createIfMissing" },
                    { "automatic", true },
                    { "operationType", initializer_type },
                    { "params", initializer_config->value("params", json::object()) },
                    { "scopeParameters", capability->second.dsl.value("scopeParameters", json::array()) },
                    { "risk", risk },
                    { "dependsOn", json::array() },
                });
                if (risk_rank(risk) > risk_rank(overall_risk))
                {
                    overall_risk = risk;
                }
            }
        }

        json planned_operations = json::array();
        std::map<std::string, std::vector<std::string>, std::less<>>
            scope_operation_ids;
        for (const auto& normalized : validation.operations)
        {
            json operation = normalized.operation;
            const auto scope_id =
                operation.value("scope", std::string{});
            auto dependencies = normalized.dependencies;
            if (const auto initializer = initializer_ids.find(scope_id);
                initializer != initializer_ids.end() &&
                std::find(
                    dependencies.begin(),
                    dependencies.end(),
                    initializer->second) == dependencies.end())
            {
                dependencies.insert(
                    dependencies.begin(),
                    initializer->second);
            }
            operation["dependsOn"] = dependencies;
            operation["automatic"] = false;
            operation["scopeParameters"] =
                normalized.capability->dsl.value(
                    "scopeParameters",
                    json::array());
            operation["transactionDomain"] =
                normalized.capability->dsl.value(
                    "transactionDomain",
                    "asset");
            operation["deferCompile"] =
                normalized.capability->dsl.value("deferCompile", true);
            const auto risk =
                normalized.capability->dsl.value("risk", "safeWrite");
            operation["risk"] = risk;

            const auto& scope_data = *std::find_if(
                scopes.begin(),
                scopes.end(),
                [&scope_id](const ScopePlanData& candidate) {
                    return candidate.id == scope_id;
                });
            const auto initializer_config =
                scope_data.config.find("initializer");
            if (initializer_config != scope_data.config.end() &&
                initializer_config->is_object() &&
                operation.value("type", std::string{}) ==
                    initializer_config->value(
                        "operationType",
                        std::string{}))
            {
                operation["kind"] = "scopeInitializer";
            }
            scope_operation_ids[scope_id].push_back(
                operation.value("id", std::string{}));
            if (risk_rank(risk) > risk_rank(overall_risk))
            {
                overall_risk = risk;
            }
            planned_operations.push_back(std::move(operation));
        }

        json finalizers = json::array();
        for (const auto& scope : scopes)
        {
            std::vector<std::string> authored_ids =
                scope_operation_ids[scope.id];
            if (const auto initializer = initializer_ids.find(scope.id);
                authored_ids.empty() && initializer != initializer_ids.end())
            {
                authored_ids.push_back(initializer->second);
            }
            const auto compile_config = scope.config.find("compile");
            std::vector<std::string> post_compile_dependencies =
                authored_ids;
            if (compile_config != scope.config.end() &&
                compile_config->is_object())
            {
                const auto compile_id =
                    "$finalizer." + scope.id + ".compile";
                finalizers.push_back({
                    { "id", compile_id },
                    { "scope", scope.id },
                    { "kind", "compile" },
                    { "automatic", true },
                    { "operationType", compile_config->value("operationType", std::string{}) },
                    { "dependsOn", authored_ids },
                });
                post_compile_dependencies = { compile_id };
            }
            else
            {
                add_diagnostic(
                    validation.diagnostics,
                    "plan",
                    "compile_finalizer_unavailable",
                    "/scopes/" + pointer_escape(scope.id) + "/kind",
                    "Named scope has no registered compile finalizer.");
            }

            std::vector<std::string> read_back_ids;
            std::map<std::string, std::size_t> grouped_indices;
            const auto read_back_config =
                scope.config.find("readBack");
            const auto verify =
                scope.scope.value("verify", json::object());
            for (const auto& read_back :
                 verify.value("readBack", json::array()))
            {
                if (!read_back.is_string())
                {
                    continue;
                }
                const auto key = read_back.get<std::string>();
                const json* entry = nullptr;
                if (read_back_config != scope.config.end() &&
                    read_back_config->is_object())
                {
                    const auto found = read_back_config->find(key);
                    if (found != read_back_config->end() &&
                        found->is_object())
                    {
                        entry = &*found;
                    }
                }
                if (!entry)
                {
                    add_diagnostic(
                        validation.diagnostics,
                        "plan",
                        "read_back_unknown",
                        "/scopes/" + pointer_escape(scope.id) +
                            "/verify/readBack",
                        "Read-back key is not supported by this scope: " +
                            key);
                    continue;
                }

                if (key == "graphs")
                {
                    std::set<std::string> graph_names;
                    for (const auto& operation : validation.operations)
                    {
                        if (operation.operation.value(
                                "scope",
                                std::string{}) != scope.id)
                        {
                            continue;
                        }
                        if (const auto target =
                                graph_read_back_target(
                                    operation.operation))
                        {
                            graph_names.insert(*target);
                        }
                    }
                    std::size_t graph_index = 0;
                    for (const auto& graph_name : graph_names)
                    {
                        const auto finalizer_id =
                            "$finalizer." + scope.id +
                            ".readBack.graphs." +
                            std::to_string(graph_index++);
                        finalizers.push_back({
                            { "id", finalizer_id },
                            { "scope", scope.id },
                            { "kind", "readBack" },
                            { "readBackKey", key },
                            { "automatic", true },
                            { "operationType", entry->value("operationType", std::string{}) },
                            { "params", { { "graph", graph_name } } },
                            { "dependsOn", post_compile_dependencies },
                        });
                        read_back_ids.push_back(finalizer_id);
                    }
                    if (graph_names.empty())
                    {
                        add_diagnostic(
                            validation.diagnostics,
                            "plan",
                            "graph_read_back_target_missing",
                            "/scopes/" + pointer_escape(scope.id) +
                                "/verify/readBack",
                            "Graph read-back requires a deterministic graph target in the same scope.");
                    }
                    continue;
                }

                const auto operation_type =
                    entry->value("operationType", std::string{});
                const auto params =
                    entry->value("params", json::object());
                const auto group_key =
                    operation_type + "\n" + params.dump();
                if (const auto grouped =
                        grouped_indices.find(group_key);
                    grouped != grouped_indices.end())
                {
                    auto& existing = finalizers.at(grouped->second);
                    if (!existing.contains("readBackKeys"))
                    {
                        existing["readBackKeys"] = json::array({
                            existing.value(
                                "readBackKey",
                                std::string{}),
                        });
                        existing.erase("readBackKey");
                    }
                    existing["readBackKeys"].push_back(key);
                    continue;
                }
                const auto finalizer_id =
                    "$finalizer." + scope.id + ".readBack." + key;
                json finalizer = {
                    { "id", finalizer_id },
                    { "scope", scope.id },
                    { "kind", "readBack" },
                    { "readBackKey", key },
                    { "automatic", true },
                    { "operationType", operation_type },
                    { "dependsOn", post_compile_dependencies },
                };
                if (!params.empty())
                {
                    finalizer["params"] = params;
                }
                grouped_indices.emplace(
                    group_key,
                    finalizers.size());
                finalizers.push_back(std::move(finalizer));
                read_back_ids.push_back(finalizer_id);
            }
            if (read_back_ids.empty())
            {
                add_diagnostic(
                    validation.diagnostics,
                    "plan",
                    "read_back_finalizer_required",
                    "/scopes/" + pointer_escape(scope.id) +
                        "/verify/readBack",
                    "Each named scope requires at least one deterministic structural read-back.");
            }
            finalizers.push_back({
                { "id", "$finalizer." + scope.id + ".diff" },
                { "scope", scope.id },
                { "kind", "diff" },
                { "automatic", true },
                { "dependsOn", read_back_ids.empty() ? post_compile_dependencies : read_back_ids },
            });
        }

        if (has_errors(validation.diagnostics))
        {
            return make_result({
                { "schema", "ue.workflow-plan.v2" },
                { "plannerVersion", kPlannerVersionV2 },
                { "contractSetDigest", v2_contract_set_digest_ },
            }, std::move(validation.diagnostics), 2);
        }

        json digest_input = {
            { "schema", "ue.workflow-plan-digest.v2" },
            { "plannerVersion", kPlannerVersionV2 },
            { "contractSetDigest", v2_contract_set_digest_ },
            { "normalizedWorkflow", workflow },
            { "assetSet", asset_set },
            { "initializers", initializers },
            { "operations", planned_operations },
            { "finalizers", finalizers },
        };
        const auto plan_digest = sha256(digest_input.dump());
        const bool approval_required =
            risk_rank(overall_risk) >= risk_rank("safeWrite");
        const bool confirm_write =
            risk_rank(overall_risk) >= risk_rank("confirmWrite");
        json payload = {
            { "schema", "ue.workflow-plan.v2" },
            { "plannerVersion", kPlannerVersionV2 },
            { "contractSetDigest", v2_contract_set_digest_ },
            { "normalizedWorkflow", workflow },
            { "assetSet", std::move(asset_set) },
            { "initializers", std::move(initializers) },
            { "operations", std::move(planned_operations) },
            { "finalizers", std::move(finalizers) },
            { "risk", { { "overall", overall_risk } } },
            { "approval", {
                { "required", approval_required },
                { "confirmWriteRequired", confirm_write },
                { "planDigest", plan_digest },
            } },
            { "expectedDiff", { { "kind", "assetSetDiff" } } },
            { "planDigest", plan_digest },
        };
        validate_schema(
            payload,
            plan_schema_v2_,
            plan_schema_v2_,
            "",
            validation.diagnostics);
        return make_result(
            std::move(payload),
            std::move(validation.diagnostics),
            2);
    }

    bool loaded_ = false;
    std::filesystem::path contract_root_;
    std::vector<std::filesystem::path> capability_roots_;
    json contract_set_ = json::object();
    json contract_files_ = json::object();
    json workflow_schema_ = json::object();
    json workflow_schema_v2_ = json::object();
    json plan_schema_ = json::object();
    json plan_schema_v2_ = json::object();
    json result_schema_ = json::object();
    json receipt_schema_ = json::object();
    json admission_ = json::object();
    std::map<std::string, Capability, std::less<>> capabilities_;
    std::string contract_set_digest_;
    std::string v2_contract_set_digest_;
};

Engine::Engine()
    : impl_(std::make_unique<Impl>())
{
}

Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

Result Engine::Load(const LoadOptions& options)
{
    return impl_->load(options);
}

Result Engine::ValidateJson(std::string_view workflow_json) const
{
    return impl_->validate(workflow_json);
}

Result Engine::PlanJson(std::string_view workflow_json) const
{
    return impl_->plan(workflow_json);
}

Result Engine::ValidateResultJson(std::string_view result_json) const
{
    return impl_->validate_result(result_json);
}

Result Engine::ValidateReceiptJson(std::string_view receipt_json) const
{
    return impl_->validate_receipt(receipt_json);
}

Result Engine::HelpJson(std::string_view scope_kind) const
{
    return impl_->help(scope_kind == "widget" ? "widgetBlueprint" : scope_kind);
}

Result Engine::ExplainOperation(std::string_view operation_type) const
{
    return impl_->explain(operation_type);
}

Result Engine::CapabilitiesJson(const CapabilityQuery& query) const
{
    return impl_->capabilities(query);
}

bool Engine::IsLoaded() const noexcept
{
    return impl_->loaded();
}

std::size_t Engine::CapabilityCount() const noexcept
{
    return impl_->capability_count();
}

std::size_t Engine::ComposableOperationCount() const noexcept
{
    return impl_->composable_count();
}

const std::string& Engine::ContractSetDigest() const noexcept
{
    return impl_->digest();
}

std::string_view PlannerVersion() noexcept
{
    return kPlannerVersion;
}

std::string_view DslVersion() noexcept
{
    return kDslVersion;
}

const char* SeverityName(Severity severity) noexcept
{
    switch (severity)
    {
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
    default:
        return "error";
    }
}

std::optional<std::string> Sha256File(
    const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }

    Sha256 hasher;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (stream)
    {
        stream.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = stream.gcount();
        if (count > 0)
        {
            hasher.update(
                buffer.data(),
                static_cast<std::size_t>(count));
        }
    }
    if (!stream.eof())
    {
        return std::nullopt;
    }

    const auto digest = hasher.finish();
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(71);
    for (const auto byte : digest)
    {
        result.push_back(hex[(byte >> 4) & 0x0f]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

std::optional<std::string> CanonicalizeJsonText(
    std::string_view json_text)
{
    const auto value = json::parse(json_text, nullptr, false, true);
    if (value.is_discarded())
    {
        return std::nullopt;
    }

    const auto unicode_scalars = [](std::string_view text)
    {
        std::vector<std::uint32_t> scalars;
        for (std::size_t index = 0; index < text.size();)
        {
            const auto first =
                static_cast<std::uint8_t>(text[index++]);
            std::uint32_t scalar = first;
            std::size_t continuation_count = 0;
            if ((first & 0xe0u) == 0xc0u)
            {
                scalar = first & 0x1fu;
                continuation_count = 1;
            }
            else if ((first & 0xf0u) == 0xe0u)
            {
                scalar = first & 0x0fu;
                continuation_count = 2;
            }
            else if ((first & 0xf8u) == 0xf0u)
            {
                scalar = first & 0x07u;
                continuation_count = 3;
            }
            for (std::size_t offset = 0;
                 offset < continuation_count && index < text.size();
                 ++offset)
            {
                scalar =
                    (scalar << 6u) |
                    (static_cast<std::uint8_t>(text[index++]) & 0x3fu);
            }
            scalars.push_back(scalar);
        }
        return scalars;
    };
    const auto scalar_less =
        [&unicode_scalars](
            const std::string& left,
            const std::string& right)
    {
        const auto left_scalars = unicode_scalars(left);
        const auto right_scalars = unicode_scalars(right);
        return std::lexicographical_compare(
            left_scalars.begin(),
            left_scalars.end(),
            right_scalars.begin(),
            right_scalars.end());
    };
    const auto canonical_number = [](double number)
    {
        if (number == 0.0)
        {
            return std::string("0");
        }
        constexpr double max_exact_integer = 9007199254740991.0;
        if (std::trunc(number) == number &&
            std::abs(number) <= max_exact_integer)
        {
            std::array<char, 64> integer_buffer{};
            std::snprintf(
                integer_buffer.data(),
                integer_buffer.size(),
                "%.0f",
                number);
            return std::string(integer_buffer.data());
        }

        std::array<char, 64> scientific_buffer{};
        std::snprintf(
            scientific_buffer.data(),
            scientific_buffer.size(),
            "%.17e",
            number);
        std::string scientific(scientific_buffer.data());
        const auto exponent_pos = scientific.find('e');
        const int exponent = std::stoi(
            scientific.substr(exponent_pos + 1));
        return scientific.substr(0, exponent_pos) +
            "e" + (exponent >= 0 ? "+" : "") +
            std::to_string(exponent);
    };
    std::function<std::string(const json&)> canonicalize;
    canonicalize = [&](const json& current) -> std::string
    {
        if (current.is_null())
        {
            return "null";
        }
        if (current.is_boolean())
        {
            return current.get<bool>() ? "true" : "false";
        }
        if (current.is_number())
        {
            return canonical_number(current.get<double>());
        }
        if (current.is_string())
        {
            return json(current.get<std::string>()).dump();
        }
        if (current.is_array())
        {
            std::string result = "[";
            for (std::size_t index = 0; index < current.size(); ++index)
            {
                if (index > 0)
                {
                    result += ",";
                }
                result += canonicalize(current[index]);
            }
            return result + "]";
        }

        std::vector<std::string> keys;
        keys.reserve(current.size());
        for (const auto& [key, ignored] : current.items())
        {
            (void)ignored;
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end(), scalar_less);
        std::string result = "{";
        for (std::size_t index = 0; index < keys.size(); ++index)
        {
            if (index > 0)
            {
                result += ",";
            }
            result += json(keys[index]).dump();
            result += ":";
            result += canonicalize(current.at(keys[index]));
        }
        return result + "}";
    };
    return canonicalize(value);
}

std::optional<std::string> CanonicalJsonSha256(
    std::string_view json_text)
{
    const auto canonical = CanonicalizeJsonText(json_text);
    return canonical
        ? std::optional<std::string>(sha256(*canonical))
        : std::nullopt;
}

} // namespace ue::workflow
