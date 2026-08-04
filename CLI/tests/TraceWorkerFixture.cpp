#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <process.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace
{
constexpr std::size_t MaxFrameBytes = 4U * 1024U * 1024U;
bool GStdioOneShot = false;
bool GNoLog = false;
bool GNoDefaultLog = false;
bool GSaveToUserDir = false;
std::string GEngineDirectory;

std::array<unsigned char, 4> EncodeLength(const std::size_t size)
{
    const auto value = static_cast<std::uint32_t>(size);
    return {
        static_cast<unsigned char>(value & 0xffU),
        static_cast<unsigned char>((value >> 8U) & 0xffU),
        static_cast<unsigned char>((value >> 16U) & 0xffU),
        static_cast<unsigned char>((value >> 24U) & 0xffU),
    };
}

std::uint32_t DecodeLength(const std::array<unsigned char, 4>& bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t ProcessId()
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::string EnvironmentOr(
    const char* name,
    const char* fallback)
{
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

const char* ResidentTransport()
{
#if defined(_WIN32)
    return "named-pipe";
#else
    return "unix-socket";
#endif
}

bool IsInsideConfiguredTraceRoot(const std::string& requested)
{
    std::error_code error;
    const auto file = std::filesystem::canonical(requested, error);
    if (error)
    {
        return false;
    }
    std::istringstream roots(EnvironmentOr("UEAI_TRACE_ROOTS", ""));
    std::string root_text;
    while (std::getline(roots, root_text, ';'))
    {
        if (root_text.empty())
        {
            continue;
        }
        const auto root = std::filesystem::canonical(root_text, error);
        if (error)
        {
            error.clear();
            continue;
        }
        const auto relative = std::filesystem::relative(file, root, error);
        if (!error && !relative.empty()
            && *relative.begin() != "..")
        {
            return true;
        }
        error.clear();
    }
    return false;
}

nlohmann::json Handle(const nlohmann::json& request)
{
    if (!request.is_object()
        || request.value("schema", "")
            != "ue.trace-worker-request.v1")
    {
        return {
            { "schema", "ue.trace-worker-response.v1" },
            { "ok", false },
            { "error", {
                { "code", "invalid_request" },
                { "message", "Invalid request." },
            } },
        };
    }
    const std::string action = request.value("action", "");
    nlohmann::json data;
    if (action == "handshake")
    {
#if defined(_WIN32)
        const auto insights = std::filesystem::path(GEngineDirectory)
            / "Binaries" / "Win64" / "UnrealInsights.exe";
#elif defined(__APPLE__)
        const auto insights = std::filesystem::path(GEngineDirectory)
            / "Binaries" / "Mac" / "UnrealInsights.app" / "Contents"
            / "MacOS" / "UnrealInsights";
#else
        const auto insights = std::filesystem::path(GEngineDirectory)
            / "Binaries" / "Linux" / "UnrealInsights";
#endif
        const bool insights_available =
            EnvironmentOr("FAKE_TRACE_INSIGHTS_AVAILABLE", "true")
                == std::string("true")
            && !GEngineDirectory.empty();
        const std::string default_insights_path =
            insights_available ? insights.string() : std::string();
        const std::string insights_path = EnvironmentOr(
            "FAKE_TRACE_INSIGHTS_PATH",
            default_insights_path.c_str());
        data = {
            { "schema", "ue.trace-worker-handshake.v1" },
            { "protocolVersion", 1 },
            { "workerVersion", "0.9.0" },
            { "engineVersion", "5.3.2-fixture" },
            { "contractBound", true },
            { "contractDigest", EnvironmentOr(
                "FAKE_TRACE_CONTRACT_DIGEST",
                "sha256:1111111111111111111111111111111111111111111111111111111111111111") },
            { "providerSchemaDigest", EnvironmentOr(
                "FAKE_TRACE_PROVIDER_DIGEST",
                "sha256:2222222222222222222222222222222222222222222222222222222222222222") },
            { "maximumResidentSessions", GStdioOneShot ? 1 : 2 },
            { "residentSessionKind", "connection" },
            { "maximumConcurrentConnections", GStdioOneShot ? 1 : 2 },
            { "maximumConcurrentAnalyses", 1 },
            { "analysisSessionCacheCapacity", 2 },
            { "analysisSessionPolicy", "sha256-lru" },
            { "unrealInsightsAvailable", insights_available },
            { "unrealInsightsPath", insights_path },
            { "unrealInsightsEngineDir", GEngineDirectory },
            { "unrealInsightsEngineVersion", EnvironmentOr(
                "FAKE_TRACE_INSIGHTS_ENGINE_VERSION", "5.3") },
            { "unrealInsightsSource", "commandLine" },
            { "unrealInsightsUnavailableReason",
                insights_available ? "" : "fixture unavailable" },
            { "noLog", GNoLog },
            { "noDefaultLog", GNoDefaultLog },
            { "saveToUserDir", GSaveToUserDir },
            { "transport", GStdioOneShot ? "stdio-one-shot" : ResidentTransport() },
            { "endpoint", GStdioOneShot ? "" : "fixture-resident" },
            { "storeRoot", "fixture-store" },
            { "serverPid", ProcessId() },
        };
    }
    else if (action == "execute")
    {
        const std::string capability = request.value("capability", "");
        const nlohmann::json params = request.value(
            "params", nlohmann::json::object());
        if (capability == "production.trace.import"
            && params.value("copyMode", "copy") == "reference"
            && !IsInsideConfiguredTraceRoot(
                params.value("tracePath", "")))
        {
            return {
                { "schema", "ue.trace-worker-response.v1" },
                { "ok", false },
                { "error", {
                    { "code", "trace_path_not_allowed" },
                    { "message", "Reference import is outside persistent roots." },
                } },
                { "meta", {
                    { "requestId", request.value("requestId", "") },
                    { "workerVersion", "0.9.0" },
                    { "engineVersion", "5.3.2-fixture" },
                    { "backend", "localTrace" },
                } },
            };
        }
        if (capability == "production.trace.target.list")
        {
            data = {
                { "schema", "ue.trace-target-list.v1" },
                { "targets", nlohmann::json::array({
                    {
                        { "kind", "development" },
                        { "available", true },
                        { "launchProfileIds", nlohmann::json::array({
                            "projectDevelopment",
                        }) },
                    },
                }) },
                { "launchProfiles", nlohmann::json::array({
                    {
                        { "id", "projectDevelopment" },
                        { "displayName", "Project Development" },
                        { "executableKind", "projectDevelopment" },
                        { "available", true },
                    },
                }) },
                { "backend", "localTrace" },
                { "traceRoots", EnvironmentOr("UEAI_TRACE_ROOTS", "") },
                { "serverPid", ProcessId() },
            };
        }
        else
        {
            data = {
                { "capability", capability },
                { "params", request.value(
                    "params",
                    nlohmann::json::object()) },
                { "backend", "localTrace" },
                { "traceRoots", EnvironmentOr("UEAI_TRACE_ROOTS", "") },
                { "serverPid", ProcessId() },
            };
        }
    }
    else
    {
        return {
            { "schema", "ue.trace-worker-response.v1" },
            { "ok", false },
            { "error", {
                { "code", "invalid_action" },
                { "message", "Unsupported action." },
            } },
        };
    }
    return {
        { "schema", "ue.trace-worker-response.v1" },
        { "ok", true },
        { "data", std::move(data) },
        { "meta", {
            { "requestId", request.value("requestId", "") },
            { "workerVersion", "0.9.0" },
            { "engineVersion", "5.3.2-fixture" },
            { "backend", "localTrace" },
        } },
    };
}

#if defined(_WIN32)
bool ReadExact(HANDLE pipe, unsigned char* data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size)
    {
        DWORD read = 0;
        if (!ReadFile(
                pipe,
                data + offset,
                static_cast<DWORD>(size - offset),
                &read,
                nullptr)
            || read == 0)
        {
            return false;
        }
        offset += read;
    }
    return true;
}

bool WriteExact(HANDLE pipe, const unsigned char* data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size)
    {
        DWORD written = 0;
        if (!WriteFile(
                pipe,
                data + offset,
                static_cast<DWORD>(size - offset),
                &written,
                nullptr)
            || written == 0)
        {
            return false;
        }
        offset += written;
    }
    return true;
}
#else
bool ReadExact(int socket_fd, unsigned char* data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size)
    {
        const ssize_t read = ::recv(socket_fd, data + offset, size - offset, 0);
        if (read <= 0)
        {
            return false;
        }
        offset += static_cast<std::size_t>(read);
    }
    return true;
}

bool WriteExact(int socket_fd, const unsigned char* data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size)
    {
        const ssize_t written = ::send(
            socket_fd, data + offset, size - offset, 0);
        if (written <= 0)
        {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}
#endif

template <typename HandleType>
bool ServeConnection(const HandleType connection)
{
    std::array<unsigned char, 4> header{};
    if (!ReadExact(connection, header.data(), header.size()))
    {
        return false;
    }
    const std::uint32_t size = DecodeLength(header);
    if (size == 0 || size > MaxFrameBytes)
    {
        return false;
    }
    std::string payload(size, '\0');
    if (!ReadExact(
            connection,
            reinterpret_cast<unsigned char*>(payload.data()),
            payload.size()))
    {
        return false;
    }
    const auto request = nlohmann::json::parse(
        payload, nullptr, false, true);
    const std::string response = Handle(request).dump();
    const auto response_header = EncodeLength(response.size());
    return WriteExact(
            connection, response_header.data(), response_header.size())
        && WriteExact(
            connection,
            reinterpret_cast<const unsigned char*>(response.data()),
            response.size());
}

int Serve(const std::string& endpoint)
{
#if defined(_WIN32)
    const std::wstring wide(endpoint.begin(), endpoint.end());
    auto idle_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    for (;;)
    {
        HANDLE pipe = CreateNamedPipeW(
            wide.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
            PIPE_UNLIMITED_INSTANCES,
            static_cast<DWORD>(MaxFrameBytes + 4),
            static_cast<DWORD>(MaxFrameBytes + 4),
            0,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            return 5;
        }
        bool connected = false;
        while (std::chrono::steady_clock::now() < idle_deadline)
        {
            if (ConnectNamedPipe(pipe, nullptr))
            {
                connected = true;
                break;
            }
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED)
            {
                connected = true;
                break;
            }
            if (error != ERROR_PIPE_LISTENING)
            {
                CloseHandle(pipe);
                return 6;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!connected)
        {
            CloseHandle(pipe);
            return 0;
        }
        DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
        (void)SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
        (void)ServeConnection(pipe);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        idle_deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(2);
    }
#else
    sockaddr_un address{};
    if (endpoint.size() >= sizeof(address.sun_path))
    {
        return 5;
    }
    const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0)
    {
        return 5;
    }
    (void)::unlink(endpoint.c_str());
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    if (::bind(
            server,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0
        || ::listen(server, 8) != 0)
    {
        ::close(server);
        return 6;
    }
    for (;;)
    {
        pollfd descriptor{};
        descriptor.fd = server;
        descriptor.events = POLLIN;
        if (::poll(&descriptor, 1, 2000) <= 0)
        {
            break;
        }
        const int connection = ::accept(server, nullptr, nullptr);
        if (connection >= 0)
        {
            (void)ServeConnection(connection);
            ::close(connection);
        }
    }
    ::close(server);
    (void)::unlink(endpoint.c_str());
    return 0;
#endif
}
}

int main(int argc, char** argv)
{
    bool stdio = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument(argv[index]);
        stdio = stdio || argument == "--stdio";
        GNoLog = GNoLog || argument == "-NoLog" || argument == "--NoLog";
        GNoDefaultLog = GNoDefaultLog
            || argument == "-NoDefaultLog"
            || argument == "--NoDefaultLog";
        GSaveToUserDir = GSaveToUserDir
            || argument == "-SaveToUserDir"
            || argument == "--SaveToUserDir";
        if (argument.starts_with("-EngineDir="))
        {
            GEngineDirectory = argument.substr(std::strlen("-EngineDir="));
        }
        else if (argument.starts_with("--EngineDir="))
        {
            GEngineDirectory = argument.substr(std::strlen("--EngineDir="));
        }
    }
    if (stdio)
    {
        GStdioOneShot = true;
        std::string input;
        std::getline(std::cin, input);
        const auto request = nlohmann::json::parse(
            input, nullptr, false, true);
        std::cout << Handle(request).dump();
        return request.is_object() ? 0 : 3;
    }
    std::string endpoint;
    bool serve = false;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument(argv[index]);
        if (argument == "--serve")
        {
            serve = true;
        }
        else if (argument.starts_with("--endpoint="))
        {
            endpoint = argument.substr(std::strlen("--endpoint="));
        }
    }
    return serve && !endpoint.empty() ? Serve(endpoint) : 2;
}
