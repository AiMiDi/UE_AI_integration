#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ue::trace
{

inline constexpr std::string_view kProtocol =
    "ue.trace-worker-request.v1";

enum class WorkerTransport
{
    ResidentService,
    Stdio,
};

struct WorkerLocation
{
    std::optional<std::filesystem::path> path;
    std::optional<std::filesystem::path> engine_directory;
    std::string source;
    std::string engine_version;
    std::string engine_directory_source;
    std::string engine_directory_error;
    std::vector<std::filesystem::path> checked;
    std::string error;
};

struct WorkerResult
{
    bool launched = false;
    bool timed_out = false;
    int exit_code = -1;
    std::string response;
    std::string diagnostics;
    std::string code;
    std::string error;
};

WorkerLocation ResolveWorker(
    const std::filesystem::path& cli_executable,
    const std::filesystem::path& source_root_override = {});

/** Derives the current-user local IPC endpoint for a resolved Worker. */
std::string ResolveServiceEndpoint(const WorkerLocation& location);

class WorkerClient
{
public:
    explicit WorkerClient(
        std::filesystem::path cli_executable,
        std::uint32_t timeout_ms = 300000,
        WorkerTransport transport = WorkerTransport::ResidentService);

    const WorkerLocation& Location() const;
    WorkerTransport Transport() const;
    WorkerResult Invoke(std::string_view request_json) const;

    /** Executes one request in an isolated Worker process with no root grant. */
    WorkerResult InvokeOneShot(std::string_view request_json) const;

    /**
     * Executes one explicit trace import in an isolated Worker process. The
     * canonical parent is added to UEAI_TRACE_ROOTS in the child environment
     * only; the CLI process and resident Worker service are never widened.
     */
    WorkerResult InvokeTraceImport(
        std::string_view request_json,
        const std::filesystem::path& canonical_trace_parent) const;

private:
    WorkerResult InvokeService(std::string_view request_json) const;
    WorkerResult InvokeStdio(
        std::string_view request_json,
        const std::optional<std::filesystem::path>& trace_import_root) const;

    WorkerLocation location_;
    std::uint32_t timeout_ms_;
    WorkerTransport transport_;
};

} // namespace ue::trace
