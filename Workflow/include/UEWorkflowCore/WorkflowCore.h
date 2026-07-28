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

    [[nodiscard]] bool IsLoaded() const noexcept;
    [[nodiscard]] std::size_t CapabilityCount() const noexcept;
    [[nodiscard]] std::size_t ComposableOperationCount() const noexcept;
    [[nodiscard]] const std::string& ContractSetDigest() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view PlannerVersion() noexcept;
[[nodiscard]] std::string_view DslVersion() noexcept;
[[nodiscard]] const char* SeverityName(Severity severity) noexcept;
[[nodiscard]] std::optional<std::string> Sha256File(
    const std::filesystem::path& path);

} // namespace ue::workflow
