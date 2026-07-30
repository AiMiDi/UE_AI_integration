#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ue::workflow
{

enum class Severity
{
    Info,
    Warning,
    Error,
};

struct Diagnostic
{
    Severity severity = Severity::Error;
    std::string phase;
    std::string code;
    std::string path;
    std::string message;
    std::string operation_id;
    std::string next_action;
};

struct LoadOptions
{
    std::filesystem::path contract_root;
    std::vector<std::filesystem::path> capability_roots;
};

struct Result
{
    bool ok = false;
    int exit_code = 2;
    std::string json;
    std::vector<Diagnostic> diagnostics;
};

enum class CapabilityDetail
{
    Summary,
    Full,
};

struct CapabilityQuery
{
    std::string query;
    std::string operation;
    std::string domain;
    std::string kind;
    std::string output_kind;
    std::string risk;
    std::optional<bool> read_only;
    std::optional<bool> destructive;
    std::optional<bool> expensive;
    bool available_only = false;
    std::size_t offset = 0;
    std::size_t limit = 25;
    CapabilityDetail detail = CapabilityDetail::Summary;
};

struct CapabilitySearchDocument
{
    std::string id;
    std::string title;
    std::string description;
    std::vector<std::string> keywords;
    std::vector<std::string> aliases;
};

struct CapabilitySearchMatch
{
    int score = 0;
    std::vector<std::string> matched_fields;
    std::vector<std::string> matched_tokens;
};

class Engine
{
public:
    Engine();
    ~Engine();

    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Result Load(const LoadOptions& options);
    Result ValidateJson(std::string_view workflow_json) const;
    Result PlanJson(std::string_view workflow_json) const;
    Result ValidateResultJson(std::string_view result_json) const;
    Result ValidateReceiptJson(std::string_view receipt_json) const;
    Result HelpJson(std::string_view scope_kind = {}) const;
    Result ExplainOperation(std::string_view operation_type) const;
    Result CapabilitiesJson(const CapabilityQuery& query = {}) const;

    [[nodiscard]] bool IsLoaded() const noexcept;
    [[nodiscard]] std::size_t CapabilityCount() const noexcept;
    [[nodiscard]] std::size_t ComposableOperationCount() const noexcept;
    /** Compatibility digest for the UE Workflow DSL 1.0 contract surface. */
    [[nodiscard]] const std::string& ContractSetDigest() const noexcept;
    /** Digest for the multi-scope UE Workflow DSL 2.0 contract surface. */
    [[nodiscard]] const std::string& ContractSetDigestV2() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view PlannerVersion() noexcept;
[[nodiscard]] std::string_view DslVersion() noexcept;
[[nodiscard]] const char* SeverityName(Severity severity) noexcept;
[[nodiscard]] std::optional<std::string> Sha256File(
    const std::filesystem::path& path);
[[nodiscard]] std::optional<std::string> CanonicalizeJsonText(
    std::string_view json_text);
[[nodiscard]] std::optional<std::string> CanonicalJsonSha256(
    std::string_view json_text);
/**
 * Match a capability using the shared v1 discovery contract.
 *
 * Query and document text are split on ASCII punctuation/whitespace and
 * camelCase boundaries. Query tokens use AND semantics. Non-ASCII UTF-8 text
 * is preserved byte-for-byte so Chinese titles and aliases remain searchable.
 */
[[nodiscard]] std::optional<CapabilitySearchMatch> MatchCapabilitySearch(
    std::string_view query,
    const CapabilitySearchDocument& document);

} // namespace ue::workflow
