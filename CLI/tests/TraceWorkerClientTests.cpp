#include "UETraceWorker/TraceWorkerClient.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
void Require(const bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

void SetWorker(const std::filesystem::path& path)
{
#if defined(_WIN32)
    _wputenv_s(L"UEAI_TRACE_WORKER", path.c_str());
#else
    setenv("UEAI_TRACE_WORKER", path.c_str(), 1);
#endif
}

void ClearWorker()
{
#if defined(_WIN32)
    _wputenv_s(L"UEAI_TRACE_WORKER", L"");
#else
    unsetenv("UEAI_TRACE_WORKER");
#endif
}

void SetEngineVersion(const std::string& version)
{
#if defined(_WIN32)
    _putenv_s("UE_ENGINE_VERSION", version.c_str());
#else
    setenv("UE_ENGINE_VERSION", version.c_str(), 1);
#endif
}

void SetEngineRoot(const std::filesystem::path& path)
{
#if defined(_WIN32)
    _wputenv_s(L"UEAI_ENGINE_ROOT", path.c_str());
#else
    setenv("UEAI_ENGINE_ROOT", path.c_str(), 1);
#endif
}

void ClearEngineRoot()
{
#if defined(_WIN32)
    _wputenv_s(L"UEAI_ENGINE_ROOT", L"");
#else
    unsetenv("UEAI_ENGINE_ROOT");
#endif
}

void SetTraceRoots(const std::filesystem::path& path)
{
#if defined(_WIN32)
    _wputenv_s(L"UEAI_TRACE_ROOTS", path.c_str());
#else
    setenv("UEAI_TRACE_ROOTS", path.c_str(), 1);
#endif
}

void ClearTraceRoots()
{
#if defined(_WIN32)
    _wputenv_s(L"UEAI_TRACE_ROOTS", L"");
#else
    unsetenv("UEAI_TRACE_ROOTS");
#endif
}
}

int main(int argc, char** argv)
{
    try
    {
    Require(argc == 2, "fixture path is required");
    SetWorker(std::filesystem::absolute(argv[1]));
    const auto inherited_root = std::filesystem::temp_directory_path()
        / "ue-trace-worker-client-inherited-root";
    const auto import_root = std::filesystem::temp_directory_path()
        / "ue-trace-worker-client-explicit-import";
    std::filesystem::create_directories(inherited_root);
    std::filesystem::create_directories(import_root);
    const auto engine_root = std::filesystem::temp_directory_path()
        / "ue-trace-worker-client-engine-root";
    const auto engine_directory = engine_root / "Engine";
    std::filesystem::create_directories(engine_directory / "Build");
    std::ofstream(engine_directory / "Build" / "Build.version")
        << R"({"MajorVersion":5,"MinorVersion":3,"PatchVersion":0})";
#if defined(_WIN32)
    const auto insights = engine_directory / "Binaries" / "Win64"
        / "UnrealInsights.exe";
#elif defined(__APPLE__)
    const auto insights = engine_directory / "Binaries" / "Mac"
        / "UnrealInsights.app" / "Contents" / "MacOS" / "UnrealInsights";
#else
    const auto insights = engine_directory / "Binaries" / "Linux"
        / "UnrealInsights";
#endif
    std::filesystem::create_directories(insights.parent_path());
    std::ofstream(insights).put('\0');
    SetEngineRoot(engine_root);
    SetEngineVersion("5.3");
    SetTraceRoots(std::filesystem::canonical(inherited_root));
    ue::trace::WorkerClient client({}, 5000);
    Require(client.Location().path.has_value(), client.Location().error);

    const auto handshake = client.Invoke(
        R"({"schema":"ue.trace-worker-request.v1","action":"handshake","requestId":"test"})");
    Require(handshake.launched, handshake.error);
    Require(!handshake.timed_out, "fixture timed out");
    Require(handshake.exit_code == 0, handshake.diagnostics);
    const auto response = nlohmann::json::parse(handshake.response);
    Require(response.value("ok", false), "handshake envelope failed");
    Require(
        response["data"]["protocolVersion"].get<int>() == 1,
        "protocol version did not match v1");
    Require(
        response["data"]["noLog"].get<bool>(),
        "CLI did not suppress Worker project logs");
    Require(
        response["data"]["noDefaultLog"].get<bool>(),
        "CLI did not disable the Worker default file output device");
    Require(
        response["data"]["saveToUserDir"].get<bool>(),
        "CLI did not redirect incidental Program state outside the install tree");
    Require(
        std::filesystem::equivalent(
            response["data"]["unrealInsightsEngineDir"].get<std::string>(),
            engine_directory),
        "CLI did not pass the validated Engine directory to the Worker");
    const auto service_pid = response["data"]["serverPid"].get<std::uint64_t>();

    const auto execute = client.Invoke(
        R"({"schema":"ue.trace-worker-request.v1","action":"execute","requestId":"test-2","capability":"production.trace.provider.list","params":{"tracePath":"sample.utrace","backend":"local"}})");
    Require(execute.exit_code == 0, execute.error);
    const auto execute_response = nlohmann::json::parse(execute.response);
    Require(
        execute_response["data"]["backend"] == "localTrace",
        "execute did not use the local Trace backend");
    Require(
        execute_response["data"]["serverPid"].get<std::uint64_t>()
            == service_pid,
        "requests did not reuse the resident Trace Worker service");
    Require(
        execute_response["data"]["traceRoots"].get<std::string>()
            == inherited_root.string(),
        "resident Worker did not inherit the configured roots exactly once");

    const auto one_shot = client.InvokeOneShot(
        R"({"schema":"ue.trace-worker-request.v1","action":"execute","requestId":"one-shot","capability":"production.trace.provider.list","params":{}})");
    Require(one_shot.exit_code == 0, one_shot.error);
    const auto one_shot_response = nlohmann::json::parse(one_shot.response);
    Require(
        one_shot_response["data"]["traceRoots"].get<std::string>()
            == inherited_root.string(),
        "ordinary one-shot Worker request received an unexpected root grant");

    const auto explicit_import = client.InvokeTraceImport(
        R"({"schema":"ue.trace-worker-request.v1","action":"execute","requestId":"import","capability":"production.trace.import","params":{"tracePath":"fixture.utrace"}})",
        std::filesystem::canonical(import_root));
    Require(explicit_import.exit_code == 0, explicit_import.error);
    const auto import_response =
        nlohmann::json::parse(explicit_import.response);
    const std::string expected_roots = inherited_root.string()
        + ";" + import_root.string();
    Require(
        import_response["data"]["traceRoots"].get<std::string>()
            == expected_roots,
        "explicit import did not merge the canonical root in the child only");
    Require(
        import_response["data"]["serverPid"].get<std::uint64_t>()
            != service_pid,
        "explicit import reused the resident Worker instead of one-shot stdio");

    const auto resident_after_import = client.Invoke(
        R"({"schema":"ue.trace-worker-request.v1","action":"execute","requestId":"resident-after","capability":"production.trace.provider.list","params":{}})");
    const auto resident_after_response =
        nlohmann::json::parse(resident_after_import.response);
    Require(
        resident_after_response["data"]["serverPid"].get<std::uint64_t>()
            == service_pid
            && resident_after_response["data"]["traceRoots"].get<std::string>()
                == inherited_root.string(),
        "one-shot import leaked its temporary root into the resident Worker");

    ue::trace::WorkerClient stdio_client(
        {},
        5000,
        ue::trace::WorkerTransport::Stdio);
    const auto diagnostic = stdio_client.Invoke(
        R"({"schema":"ue.trace-worker-request.v1","action":"handshake","requestId":"stdio-test"})");
    Require(
        diagnostic.exit_code == 0,
        "explicit stdio diagnostic fallback failed");

    ClearWorker();
    SetEngineVersion("5.3");
    const auto root = std::filesystem::temp_directory_path()
        / ("ue-trace-worker-layout-" + std::to_string(service_pid));
#if defined(_WIN32)
    const auto packaged_worker = root / "Tools" / "Trace" / "Win64"
        / "5.3" / "UEAITraceWorker.exe";
#elif defined(__APPLE__)
    const auto packaged_worker = root / "Tools" / "Trace" / "Mac"
        / "5.3" / "UEAITraceWorker";
#else
    const auto packaged_worker = root / "Tools" / "Trace" / "Linux"
        / "5.3" / "UEAITraceWorker";
#endif
    std::filesystem::create_directories(packaged_worker.parent_path());
    std::ofstream(packaged_worker).put('\0');
    const auto packaged = ue::trace::ResolveWorker(
        root / "CLI" / "bin" / "ue");
    Require(
        packaged.path.has_value()
            && std::filesystem::equivalent(
                *packaged.path, packaged_worker),
        "packaged Tools/Trace Worker layout was not resolved");
    ue::trace::WorkerLocation different_engine = packaged;
    different_engine.engine_directory = engine_directory / "Different";
    Require(
        ue::trace::ResolveServiceEndpoint(packaged)
            != ue::trace::ResolveServiceEndpoint(different_engine),
        "Trace Worker endpoint did not include the Engine directory");
    std::filesystem::remove_all(root);

    const auto source_root = std::filesystem::temp_directory_path()
        / ("ue-trace-worker-source-layout-" + std::to_string(service_pid));
#if defined(_WIN32)
    const auto source_worker = source_root / "Programs" / "UEAITraceWorker"
        / "Binaries" / "Win64" / "UEAITraceWorker.exe";
    const auto staged_worker = source_root / "Intermediate"
        / "TraceWorkerStage" / "Tools" / "Trace" / "Win64"
        / "5.3" / "UEAITraceWorker.exe";
#elif defined(__APPLE__)
    const auto source_worker = source_root / "Programs" / "UEAITraceWorker"
        / "Binaries" / "Mac" / "UEAITraceWorker";
    const auto staged_worker = source_root / "Intermediate"
        / "TraceWorkerStage" / "Tools" / "Trace" / "Mac"
        / "5.3" / "UEAITraceWorker";
#else
    const auto source_worker = source_root / "Programs" / "UEAITraceWorker"
        / "Binaries" / "Linux" / "UEAITraceWorker";
    const auto staged_worker = source_root / "Intermediate"
        / "TraceWorkerStage" / "Tools" / "Trace" / "Linux"
        / "5.3" / "UEAITraceWorker";
#endif
    std::filesystem::create_directories(source_worker.parent_path());
    std::filesystem::create_directories(staged_worker.parent_path());
    std::ofstream(source_worker).put('\0');
    std::ofstream(staged_worker).put('\0');
    const auto staged_preferred = ue::trace::ResolveWorker({}, source_root);
    Require(
        staged_preferred.path.has_value()
            && std::filesystem::equivalent(
                *staged_preferred.path, staged_worker),
        "versioned staged Worker did not take precedence over Programs/Binaries");
    std::filesystem::remove(staged_worker);
    const auto source_fallback = ue::trace::ResolveWorker({}, source_root);
    Require(
        source_fallback.path.has_value()
            && std::filesystem::equivalent(
                *source_fallback.path, source_worker),
        "Programs/Binaries Worker was not retained as the source fallback");
    std::filesystem::remove_all(source_root);
    ClearTraceRoots();
    ClearEngineRoot();
    std::filesystem::remove_all(inherited_root);
    std::filesystem::remove_all(import_root);
    std::filesystem::remove_all(engine_root);
    return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: unexpected exception: " << error.what() << "\n";
        return 1;
    }
}
