#include "UETraceWorker/TraceWorkerClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#ifndef UE_CLI_SOURCE_ROOT
#define UE_CLI_SOURCE_ROOT ""
#endif
#ifndef UE_TRACE_CLIENT_VERSION
#define UE_TRACE_CLIENT_VERSION "1.0.0"
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ue::trace
{
namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxFrameBytes = 4U * 1024U * 1024U;
constexpr std::uint32_t kStartupTimeoutMs = 10000;

std::string GenericUtf8(const std::filesystem::path& path);

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

std::string MergeTraceRoots(const std::filesystem::path& canonical_parent)
{
    const std::string parent = GenericUtf8(canonical_parent);
    if (const auto inherited = Environment("UEAI_TRACE_ROOTS"))
    {
        return *inherited + ";" + parent;
    }
    return parent;
}

#if defined(_WIN32)
std::optional<std::vector<wchar_t>> ChildEnvironmentWithTraceRoot(
    const std::filesystem::path& canonical_parent)
{
    LPWCH block = GetEnvironmentStringsW();
    if (!block)
    {
        return std::nullopt;
    }
    std::vector<std::wstring> entries;
    constexpr std::wstring_view key = L"UEAI_TRACE_ROOTS";
    bool replaced = false;
    for (const wchar_t* current = block; *current;
         current += std::wcslen(current) + 1)
    {
        std::wstring entry(current);
        const std::size_t separator = entry.find(L'=');
        if (separator != 0
            && separator != std::wstring::npos
            && separator == key.size()
            && CompareStringOrdinal(
                   entry.data(), static_cast<int>(separator),
                   key.data(), static_cast<int>(key.size()), TRUE)
                == CSTR_EQUAL)
        {
            entries.emplace_back(key);
            entries.back() += L"=";
            const std::wstring inherited = entry.substr(separator + 1);
            if (!inherited.empty())
            {
                entries.back() += inherited;
                entries.back() += L";";
            }
            entries.back() += canonical_parent.wstring();
            replaced = true;
        }
        else
        {
            entries.push_back(std::move(entry));
        }
    }
    FreeEnvironmentStringsW(block);
    if (!replaced)
    {
        entries.emplace_back(key);
        entries.back() += L"=";
        entries.back() += canonical_parent.wstring();
    }
    std::sort(
        entries.begin(), entries.end(),
        [](const std::wstring& left, const std::wstring& right)
        {
            return _wcsicmp(left.c_str(), right.c_str()) < 0;
        });
    std::size_t size = 1;
    for (const std::wstring& entry : entries)
    {
        size += entry.size() + 1;
    }
    std::vector<wchar_t> result;
    result.reserve(size);
    for (const std::wstring& entry : entries)
    {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}
#endif

std::string PlatformDirectory()
{
#if defined(_WIN32)
    return "Win64";
#elif defined(__APPLE__)
    return "Mac";
#else
    return "Linux";
#endif
}

std::string WorkerName()
{
#if defined(_WIN32)
    return "UEAITraceWorker.exe";
#else
    return "UEAITraceWorker";
#endif
}

bool LooksLikeEngineVersion(const std::string& value)
{
    return !value.empty()
        && std::isdigit(static_cast<unsigned char>(value.front())) != 0
        && value.find('.') != std::string::npos;
}

std::string InferEngineVersion(const std::filesystem::path& worker)
{
    const auto parent = GenericUtf8(worker.parent_path().filename());
    return LooksLikeEngineVersion(parent) ? parent : "unknown";
}

std::optional<std::filesystem::path> NormalizeExecutable(
    const std::filesystem::path& candidate)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(candidate, error) || error)
    {
        return std::nullopt;
    }
    auto normalized = std::filesystem::weakly_canonical(candidate, error);
    return error
        ? std::optional<std::filesystem::path>(candidate)
        : std::optional<std::filesystem::path>(normalized);
}

std::optional<std::pair<int, int>> ParseMajorMinor(
    const std::string& value)
{
    const auto separator = value.find('.');
    if (separator == std::string::npos || separator == 0)
    {
        return std::nullopt;
    }
    const auto end = value.find_first_not_of("0123456789", separator + 1);
    const auto major_text = value.substr(0, separator);
    const auto minor_text = value.substr(
        separator + 1,
        (end == std::string::npos ? value.size() : end) - separator - 1);
    if (minor_text.empty()
        || major_text.find_first_not_of("0123456789") != std::string::npos
        || minor_text.find_first_not_of("0123456789") != std::string::npos)
    {
        return std::nullopt;
    }
    try
    {
        return std::pair{ std::stoi(major_text), std::stoi(minor_text) };
    }
    catch (...)
    {
        return std::nullopt;
    }
}

struct EngineDirectoryResolution
{
    std::optional<std::filesystem::path> path;
    std::string source;
    std::string error;
};

std::optional<int> ReadJsonIntegerField(
    const std::string& contents,
    const std::string& field)
{
    const std::regex pattern(
        "\\\"" + field + "\\\"\\s*:\\s*(-?[0-9]+)");
    std::smatch match;
    if (!std::regex_search(contents, match, pattern))
    {
        return std::nullopt;
    }
    try
    {
        return std::stoi(match[1].str());
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<std::string> ReadJsonStringField(
    const std::string& contents,
    const std::string& field)
{
    const std::regex pattern(
        "\\\"" + field + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch match;
    return std::regex_search(contents, match, pattern)
        ? std::optional<std::string>(match[1].str())
        : std::nullopt;
}

EngineDirectoryResolution ValidateEngineDirectory(
    std::filesystem::path candidate,
    const std::string& source,
    const std::string& expected_version)
{
    EngineDirectoryResolution result;
    result.source = source;
    std::error_code error;
    candidate = std::filesystem::absolute(candidate, error).lexically_normal();
    if (error)
    {
        result.error = source + " Engine path could not be normalized.";
        return result;
    }
    if (candidate.filename() != "Engine"
        && std::filesystem::is_directory(candidate / "Engine", error)
        && !error)
    {
        candidate /= "Engine";
    }
    error.clear();
    const auto canonical = std::filesystem::weakly_canonical(candidate, error);
    if (!error)
    {
        candidate = canonical;
    }
    const auto build_version_path = candidate / "Build" / "Build.version";
    std::ifstream stream(build_version_path, std::ios::binary);
    const std::string build_version(
        (std::istreambuf_iterator<char>(stream)),
        std::istreambuf_iterator<char>());
    const auto major_value = ReadJsonIntegerField(
        build_version, "MajorVersion");
    const auto minor_value = ReadJsonIntegerField(
        build_version, "MinorVersion");
    if (!stream.is_open() || !major_value || !minor_value)
    {
        result.error = source
            + " has no readable Engine/Build/Build.version.";
        return result;
    }
    const auto expected = ParseMajorMinor(expected_version);
    if (!expected)
    {
        result.error = "The selected Trace Worker Engine version is ambiguous.";
        return result;
    }
    const int major = *major_value;
    const int minor = *minor_value;
    if (major != expected->first || minor != expected->second)
    {
        result.error = source + " Engine version "
            + std::to_string(major) + "." + std::to_string(minor)
            + " does not match Worker " + expected_version + ".";
        return result;
    }
    result.path = candidate;
    return result;
}

#if defined(_WIN32)
std::optional<std::filesystem::path> QueryEngineAssociation(
    const std::string& association)
{
    if (association.empty() || association.size() > 64)
    {
        return std::nullopt;
    }
    std::wstring key;
    std::wstring value;
    HKEY hive = nullptr;
    const auto wide_association = std::filesystem::path(association).wstring();
    if (ParseMajorMinor(association))
    {
        if (association.find_first_not_of("0123456789.") != std::string::npos)
        {
            return std::nullopt;
        }
        hive = HKEY_LOCAL_MACHINE;
        key = L"SOFTWARE\\EpicGames\\Unreal Engine\\" + wide_association;
        value = L"InstalledDirectory";
    }
    else
    {
        if (association.front() != '{' || association.back() != '}'
            || association.find_first_not_of(
                "{}-0123456789abcdefABCDEF") != std::string::npos)
        {
            return std::nullopt;
        }
        hive = HKEY_CURRENT_USER;
        key = L"SOFTWARE\\Epic Games\\Unreal Engine\\Builds";
        value = wide_association;
    }
    DWORD bytes = 0;
    if (RegGetValueW(
            hive, key.c_str(), value.c_str(), RRF_RT_REG_SZ,
            nullptr, nullptr, &bytes) != ERROR_SUCCESS
        || bytes < sizeof(wchar_t))
    {
        return std::nullopt;
    }
    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(
            hive, key.c_str(), value.c_str(), RRF_RT_REG_SZ,
            nullptr, buffer.data(), &bytes) != ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    buffer.resize(std::wcslen(buffer.c_str()));
    return buffer.empty()
        ? std::nullopt
        : std::optional<std::filesystem::path>(buffer);
}
#endif

EngineDirectoryResolution ResolveEngineDirectory(
    const WorkerLocation& location)
{
    if (const auto configured = Environment("UEAI_ENGINE_ROOT"))
    {
        return ValidateEngineDirectory(
            *configured, "UEAI_ENGINE_ROOT", location.engine_version);
    }
    if (const auto configured = Environment("UE_ENGINE_ROOT"))
    {
        return ValidateEngineDirectory(
            *configured, "UE_ENGINE_ROOT", location.engine_version);
    }
    if (!location.path)
    {
        return {};
    }
    std::filesystem::path directory = location.path->parent_path();
    for (int depth = 0; depth < 12 && !directory.empty(); ++depth)
    {
        std::vector<std::filesystem::path> projects;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(
                 directory, error))
        {
            if (error)
            {
                break;
            }
            if (entry.is_regular_file(error)
                && !error
                && entry.path().extension() == ".uproject")
            {
                projects.push_back(entry.path());
            }
            error.clear();
        }
        if (projects.size() > 1)
        {
            return {
                std::nullopt,
                "projectAssociation",
                "Multiple .uproject files were found beside the installed Worker.",
            };
        }
        if (projects.size() == 1)
        {
            std::ifstream stream(projects.front(), std::ios::binary);
            const std::string project(
                (std::istreambuf_iterator<char>(stream)),
                std::istreambuf_iterator<char>());
            if (!stream.is_open() || project.size() > 1024U * 1024U)
            {
                return {
                    std::nullopt,
                    "projectAssociation",
                    "The installed project's .uproject could not be parsed.",
                };
            }
            const std::string association =
                ReadJsonStringField(project, "EngineAssociation")
                    .value_or("");
            if (association.empty())
            {
                return {};
            }
#if defined(_WIN32)
            const auto associated = QueryEngineAssociation(association);
            if (!associated)
            {
                return {
                    std::nullopt,
                    "projectAssociation",
                    "The project's exact EngineAssociation is not registered.",
                };
            }
            return ValidateEngineDirectory(
                *associated, "projectAssociation", location.engine_version);
#else
            return {
                std::nullopt,
                "projectAssociation",
                "Project EngineAssociation discovery is unavailable; set UEAI_ENGINE_ROOT.",
            };
#endif
        }
        const auto parent = directory.parent_path();
        if (parent == directory)
        {
            break;
        }
        directory = parent;
    }
    return {};
}

WorkerLocation FinalizeWorkerLocation(WorkerLocation location)
{
    if (location.path)
    {
        const EngineDirectoryResolution engine =
            ResolveEngineDirectory(location);
        location.engine_directory = engine.path;
        location.engine_directory_source = engine.source;
        location.engine_directory_error = engine.error;
    }
    return location;
}

void AddChecked(
    WorkerLocation& location,
    const std::filesystem::path& candidate)
{
    std::error_code error;
    auto absolute = std::filesystem::absolute(candidate, error);
    const auto normalized = (error ? candidate : absolute).lexically_normal();
    if (std::find(
            location.checked.begin(),
            location.checked.end(),
            normalized) == location.checked.end())
    {
        location.checked.push_back(normalized);
    }
}

std::vector<std::filesystem::path> InstalledWorkers(
    WorkerLocation& location,
    const std::filesystem::path& root)
{
    std::vector<std::filesystem::path> matches;
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error)
    {
        return matches;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, error))
    {
        if (error)
        {
            break;
        }
        if (!entry.is_directory(error) || error)
        {
            error.clear();
            continue;
        }
        const auto candidate = entry.path() / WorkerName();
        AddChecked(location, candidate);
        if (const auto found = NormalizeExecutable(candidate))
        {
            matches.push_back(*found);
        }
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

std::string GenericUtf8(const std::filesystem::path& path)
{
    const auto bytes = path.generic_u8string();
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size());
}

std::string UserIdentity()
{
#if defined(_WIN32)
    auto identity = Environment("USERNAME").value_or("unknown-user");
    std::transform(
        identity.begin(), identity.end(), identity.begin(),
        [](const unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
    return identity;
#else
    return std::to_string(static_cast<unsigned long long>(::getuid()));
#endif
}

std::uint64_t Fnv1a64(const std::string_view value)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value)
    {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string EndpointToken(
    const std::filesystem::path& worker,
    const std::string_view engine_version,
    const std::optional<std::filesystem::path>& engine_directory)
{
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(worker, error);
    std::string identity = GenericUtf8(error ? worker : canonical);
#if defined(_WIN32)
    std::transform(
        identity.begin(), identity.end(), identity.begin(),
        [](const unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
#endif
    identity += "|";
    identity += engine_version;
    identity += "|";
    std::string engine_identity = engine_directory
        ? GenericUtf8(*engine_directory)
        : std::string("unresolved-engine-dir");
#if defined(_WIN32)
    std::transform(
        engine_identity.begin(), engine_identity.end(), engine_identity.begin(),
        [](const unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });
#endif
    identity += engine_identity;
    identity += "|";
    identity += UE_TRACE_CLIENT_VERSION;
    identity += "|";
    identity += UserIdentity();
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16)
           << Fnv1a64(identity);
    return output.str();
}

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

Clock::time_point DeadlineAfter(const std::uint32_t timeout_ms)
{
    return Clock::now() + std::chrono::milliseconds(timeout_ms);
}

std::uint32_t RemainingMilliseconds(const Clock::time_point deadline)
{
    const auto now = Clock::now();
    if (now >= deadline)
    {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now).count();
    return static_cast<std::uint32_t>(std::max<std::int64_t>(1, remaining));
}

std::string EngineDirectoryArgument(const WorkerLocation& location)
{
    return location.engine_directory
        ? "-EngineDir=" + GenericUtf8(*location.engine_directory)
        : std::string();
}

#if defined(_WIN32)

enum class TransferStatus
{
    Ok,
    Timeout,
    Broken,
};

std::string WindowsError(const DWORD code)
{
    return "Windows error " + std::to_string(code);
}

TransferStatus TransferExact(
    const HANDLE pipe,
    unsigned char* bytes,
    const std::size_t size,
    const bool write,
    const Clock::time_point deadline,
    DWORD& out_error)
{
    std::size_t offset = 0;
    while (offset < size)
    {
        const std::uint32_t remaining = RemainingMilliseconds(deadline);
        if (remaining == 0)
        {
            return TransferStatus::Timeout;
        }
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent)
        {
            out_error = GetLastError();
            return TransferStatus::Broken;
        }
        DWORD transferred = 0;
        const DWORD count = static_cast<DWORD>(std::min<std::size_t>(
            size - offset, 1U << 20U));
        const BOOL started = write
            ? WriteFile(
                pipe, bytes + offset, count, &transferred, &operation)
            : ReadFile(
                pipe, bytes + offset, count, &transferred, &operation);
        DWORD operation_error = started ? ERROR_SUCCESS : GetLastError();
        if (!started && operation_error == ERROR_IO_PENDING)
        {
            const DWORD wait = WaitForSingleObject(operation.hEvent, remaining);
            if (wait == WAIT_TIMEOUT)
            {
                CancelIoEx(pipe, &operation);
                WaitForSingleObject(operation.hEvent, INFINITE);
                CloseHandle(operation.hEvent);
                return TransferStatus::Timeout;
            }
            if (wait != WAIT_OBJECT_0
                || !GetOverlappedResult(
                    pipe, &operation, &transferred, FALSE))
            {
                operation_error = GetLastError();
            }
            else
            {
                operation_error = ERROR_SUCCESS;
            }
        }
        CloseHandle(operation.hEvent);
        if (operation_error != ERROR_SUCCESS
            && operation_error != ERROR_MORE_DATA)
        {
            out_error = operation_error;
            return TransferStatus::Broken;
        }
        if (transferred == 0)
        {
            out_error = ERROR_BROKEN_PIPE;
            return TransferStatus::Broken;
        }
        offset += transferred;
    }
    return TransferStatus::Ok;
}

HANDLE OpenPipe(const std::string& endpoint, DWORD& out_error)
{
    const std::wstring wide(endpoint.begin(), endpoint.end());
    HANDLE pipe = CreateFileW(
        wide.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    out_error = pipe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    return pipe;
}

HANDLE SpawnService(
    const WorkerLocation& location,
    const std::string& endpoint,
    std::string& out_error)
{
    const std::filesystem::path& worker = *location.path;
    const std::wstring wide_endpoint(endpoint.begin(), endpoint.end());
    std::wstring command = L"\"" + worker.wstring()
        + L"\" --serve --endpoint=\"" + wide_endpoint
        + L"\" --idleSeconds=600 --maxSessions=2 -NoLog -NoDefaultLog"
        + L" -SaveToUserDir";
    if (location.engine_directory)
    {
        command += L" -EngineDir=\"";
        command += location.engine_directory->wstring();
        command += L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        worker.c_str(),
        command.data(),
        nullptr,
        nullptr,
        FALSE,
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &startup,
        &process);
    if (!created)
    {
        out_error = WindowsError(GetLastError());
        return nullptr;
    }
    CloseHandle(process.hThread);
    return process.hProcess;
}

void ReadHandle(const HANDLE handle, std::string& destination)
{
    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0)
    {
        if (destination.size() < kMaxFrameBytes)
        {
            destination.append(
                buffer,
                std::min<std::size_t>(
                    read,
                    kMaxFrameBytes - destination.size()));
        }
    }
    CloseHandle(handle);
}

#else

enum class TransferStatus
{
    Ok,
    Timeout,
    Broken,
};

int ConnectSocket(const std::string& endpoint, int& out_error)
{
    sockaddr_un address{};
    if (endpoint.size() >= sizeof(address.sun_path))
    {
        out_error = ENAMETOOLONG;
        return -1;
    }
    const int socket_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        out_error = errno;
        return -1;
    }
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    int enabled = 1;
    (void)::setsockopt(
        socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    if (::connect(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0)
    {
        out_error = errno;
        ::close(socket_fd);
        return -1;
    }
    out_error = 0;
    return socket_fd;
}

TransferStatus TransferExact(
    const int socket_fd,
    unsigned char* bytes,
    const std::size_t size,
    const bool write,
    const Clock::time_point deadline,
    int& out_error)
{
    std::size_t offset = 0;
    while (offset < size)
    {
        const std::uint32_t remaining = RemainingMilliseconds(deadline);
        if (remaining == 0)
        {
            return TransferStatus::Timeout;
        }
        pollfd descriptor{};
        descriptor.fd = socket_fd;
        descriptor.events = write ? POLLOUT : POLLIN;
        const int ready = ::poll(
            &descriptor,
            1,
            static_cast<int>(remaining));
        if (ready == 0)
        {
            return TransferStatus::Timeout;
        }
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            out_error = errno;
            return TransferStatus::Broken;
        }
        ssize_t transferred = 0;
        if (write)
        {
#if defined(MSG_NOSIGNAL)
            transferred = ::send(
                socket_fd,
                bytes + offset,
                size - offset,
                MSG_NOSIGNAL);
#else
            transferred = ::send(
                socket_fd, bytes + offset, size - offset, 0);
#endif
        }
        else
        {
            transferred = ::recv(
                socket_fd, bytes + offset, size - offset, 0);
        }
        if (transferred < 0 && (errno == EINTR || errno == EAGAIN))
        {
            continue;
        }
        if (transferred <= 0)
        {
            out_error = transferred == 0 ? ECONNRESET : errno;
            return TransferStatus::Broken;
        }
        offset += static_cast<std::size_t>(transferred);
    }
    return TransferStatus::Ok;
}

bool SpawnService(
    const WorkerLocation& location,
    const std::string& endpoint,
    std::string& out_error)
{
    const std::filesystem::path& worker = *location.path;
    const pid_t first = ::fork();
    if (first < 0)
    {
        out_error = std::strerror(errno);
        return false;
    }
    if (first == 0)
    {
        const pid_t service = ::fork();
        if (service < 0)
        {
            _exit(126);
        }
        if (service > 0)
        {
            _exit(0);
        }
        (void)::setsid();
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0)
        {
            (void)::dup2(null_fd, STDIN_FILENO);
            (void)::dup2(null_fd, STDOUT_FILENO);
            (void)::dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
            {
                ::close(null_fd);
            }
        }
        const std::string endpoint_argument = "--endpoint=" + endpoint;
        const std::string engine_argument =
            EngineDirectoryArgument(location);
        if (engine_argument.empty())
        {
            ::execl(
                worker.c_str(), worker.c_str(),
                "--serve", endpoint_argument.c_str(),
                "--idleSeconds=600", "--maxSessions=2", "-NoLog",
                "-NoDefaultLog", "-SaveToUserDir",
                static_cast<char*>(nullptr));
        }
        else
        {
            ::execl(
                worker.c_str(), worker.c_str(),
                "--serve", endpoint_argument.c_str(),
                "--idleSeconds=600", "--maxSessions=2", "-NoLog",
                "-NoDefaultLog", "-SaveToUserDir", engine_argument.c_str(),
                static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    int status = 0;
    if (::waitpid(first, &status, 0) < 0
        || !WIFEXITED(status)
        || WEXITSTATUS(status) != 0)
    {
        out_error = "Trace Worker service bootstrap failed.";
        return false;
    }
    return true;
}

void ReadDescriptor(const int descriptor, std::string& destination)
{
    char buffer[8192];
    for (;;)
    {
        const ssize_t read = ::read(descriptor, buffer, sizeof(buffer));
        if (read <= 0)
        {
            break;
        }
        if (destination.size() < kMaxFrameBytes)
        {
            destination.append(
                buffer,
                std::min<std::size_t>(
                    static_cast<std::size_t>(read),
                    kMaxFrameBytes - destination.size()));
        }
    }
    ::close(descriptor);
}

#endif

} // namespace

WorkerLocation ResolveWorker(
    const std::filesystem::path& cli_executable,
    const std::filesystem::path& source_root_override)
{
    WorkerLocation location;
    if (const auto configured = Environment("UEAI_TRACE_WORKER"))
    {
        AddChecked(location, *configured);
        location.path = NormalizeExecutable(*configured);
        if (location.path)
        {
            location.source = "environment";
            const auto requested = Environment("UE_ENGINE_VERSION");
            const auto legacy = Environment("UE_VERSION");
            location.engine_version = requested
                ? *requested
                : legacy ? *legacy : InferEngineVersion(*location.path);
        }
        else
        {
            location.error =
                "UEAI_TRACE_WORKER does not name a readable executable.";
        }
        return FinalizeWorkerLocation(std::move(location));
    }

    std::vector<std::filesystem::path> roots;
    if (!cli_executable.empty())
    {
        roots.push_back(
            cli_executable.parent_path()
            / ".." / ".." / "Tools" / "Trace"
            / PlatformDirectory());
    }
    const std::filesystem::path source_root = !source_root_override.empty()
        ? source_root_override
        : std::filesystem::path(UE_CLI_SOURCE_ROOT);
    if (!source_root.empty())
    {
        roots.push_back(
            source_root
            / "Tools" / "Trace" / PlatformDirectory());
    }
    const auto engine_version = Environment("UE_ENGINE_VERSION");
    const auto legacy_version = Environment("UE_VERSION");
    const auto version = engine_version ? engine_version : legacy_version;
    std::set<std::filesystem::path> matches;
    for (const auto& root : roots)
    {
        if (version)
        {
            const auto candidate = root / *version / WorkerName();
            AddChecked(location, candidate);
            if (const auto found = NormalizeExecutable(candidate))
            {
                matches.insert(*found);
            }
        }
        else
        {
            for (const auto& found : InstalledWorkers(location, root))
            {
                matches.insert(found);
            }
        }
    }
    if (matches.size() == 1)
    {
        location.path = *matches.begin();
        location.source = "installed";
        location.engine_version = version
            ? *version
            : InferEngineVersion(*location.path);
        return FinalizeWorkerLocation(std::move(location));
    }
    if (matches.size() > 1)
    {
        location.error =
            "Multiple engine-specific Trace Workers are installed; set "
            "UE_ENGINE_VERSION.";
        return FinalizeWorkerLocation(std::move(location));
    }

    if (!source_root.empty())
    {
        // build_trace_worker.ps1 stages source-tree builds here for local
        // validation. Prefer its engine-versioned output over the unversioned
        // Programs/Binaries location, which may contain a stale build from a
        // different Engine checkout.
        const auto staged_root =
            source_root / "Intermediate" / "TraceWorkerStage" / "Tools"
            / "Trace" / PlatformDirectory();
        if (version)
        {
            const auto candidate = staged_root / *version / WorkerName();
            AddChecked(location, candidate);
            if (const auto found = NormalizeExecutable(candidate))
            {
                location.path = *found;
                location.source = "source";
                location.engine_version = *version;
                return FinalizeWorkerLocation(std::move(location));
            }
        }
        else
        {
            const auto staged = InstalledWorkers(location, staged_root);
            if (staged.size() == 1)
            {
                location.path = staged.front();
                location.source = "source";
                location.engine_version = InferEngineVersion(staged.front());
                return FinalizeWorkerLocation(std::move(location));
            }
            if (staged.size() > 1)
            {
                location.error =
                    "Multiple source-built Trace Workers were found; set "
                    "UE_ENGINE_VERSION.";
                return FinalizeWorkerLocation(std::move(location));
            }
        }

        // This path is intentionally a last-resort source-tree fallback. It
        // has no version component, so the handshake must still prove that it
        // matches UE_ENGINE_VERSION before any local execution is accepted.
        const auto program_candidate =
            source_root / "Programs" / "UEAITraceWorker" / "Binaries"
            / PlatformDirectory() / WorkerName();
        AddChecked(location, program_candidate);
        if (const auto found = NormalizeExecutable(program_candidate))
        {
            location.path = *found;
            location.source = "source";
            location.engine_version = version ? *version : "unknown";
            return FinalizeWorkerLocation(std::move(location));
        }
        for (const char* build : {
                 "build", "build-workflow", "build-workflow-final" })
        {
            const auto candidate =
                source_root / build / "Tools" / "Trace" / WorkerName();
            AddChecked(location, candidate);
            if (const auto found = NormalizeExecutable(candidate))
            {
                location.path = *found;
                location.source = "source";
                location.engine_version = version ? *version : "unknown";
                return FinalizeWorkerLocation(std::move(location));
            }
        }
    }
    location.error =
        "UEAITraceWorker was not found. Set UEAI_TRACE_WORKER or install "
        "the worker for this Engine version.";
    return FinalizeWorkerLocation(std::move(location));
}

std::string ResolveServiceEndpoint(const WorkerLocation& location)
{
    if (!location.path)
    {
        return {};
    }
    const std::string token = EndpointToken(
        *location.path,
        location.engine_version.empty()
            ? std::string_view("unknown")
            : std::string_view(location.engine_version),
        location.engine_directory);
#if defined(_WIN32)
    return R"(\\.\pipe\UEAITraceWorker-)" + token;
#else
    std::filesystem::path directory = Environment("XDG_RUNTIME_DIR")
        ? std::filesystem::path(*Environment("XDG_RUNTIME_DIR"))
        : Environment("TMPDIR")
            ? std::filesystem::path(*Environment("TMPDIR"))
            : std::filesystem::path("/tmp");
    return GenericUtf8(
        directory / ("ueai-trace-worker-" + UserIdentity()
            + "-" + token + ".sock"));
#endif
}

WorkerClient::WorkerClient(
    std::filesystem::path cli_executable,
    const std::uint32_t timeout_ms,
    const WorkerTransport transport)
    : location_(ResolveWorker(cli_executable)),
      timeout_ms_(timeout_ms),
      transport_(transport)
{
}

const WorkerLocation& WorkerClient::Location() const
{
    return location_;
}

WorkerTransport WorkerClient::Transport() const
{
    return transport_;
}

WorkerResult WorkerClient::Invoke(const std::string_view request_json) const
{
    return transport_ == WorkerTransport::Stdio
        ? InvokeStdio(request_json, std::nullopt)
        : InvokeService(request_json);
}

WorkerResult WorkerClient::InvokeOneShot(
    const std::string_view request_json) const
{
    return InvokeStdio(request_json, std::nullopt);
}

WorkerResult WorkerClient::InvokeTraceImport(
    const std::string_view request_json,
    const std::filesystem::path& canonical_trace_parent) const
{
    std::error_code error;
    const auto final_parent =
        std::filesystem::canonical(canonical_trace_parent, error);
    if (error
        || !std::filesystem::is_directory(final_parent, error)
        || error)
    {
        WorkerResult result;
        result.code = "trace_import_path_invalid";
        result.error =
            "The one-shot Trace import root is not a canonical directory.";
        return result;
    }
    return InvokeStdio(request_json, final_parent);
}

WorkerResult WorkerClient::InvokeService(
    const std::string_view request_json) const
{
    WorkerResult result;
    if (!location_.path)
    {
        result.code = "trace_worker_unavailable";
        result.error = location_.error;
        return result;
    }
    if (request_json.empty() || request_json.size() > kMaxFrameBytes)
    {
        result.code = "trace_worker_request_too_large";
        result.error = "Trace Worker request must be between 1 byte and 4 MiB.";
        return result;
    }
    const std::string endpoint = ResolveServiceEndpoint(location_);
    if (endpoint.empty())
    {
        result.code = "trace_worker_unavailable";
        result.error = "The Trace Worker service endpoint could not be derived.";
        return result;
    }
    const auto request_deadline = DeadlineAfter(timeout_ms_);
    const auto startup_deadline = std::min(
        request_deadline,
        DeadlineAfter(kStartupTimeoutMs));

#if defined(_WIN32)
    DWORD pipe_error = ERROR_SUCCESS;
    HANDLE pipe = OpenPipe(endpoint, pipe_error);
    HANDLE spawned_process = nullptr;
    if (pipe == INVALID_HANDLE_VALUE && pipe_error == ERROR_FILE_NOT_FOUND)
    {
        std::string spawn_error;
        spawned_process = SpawnService(
            location_, endpoint, spawn_error);
        if (!spawned_process)
        {
            result.code = "trace_worker_unavailable";
            result.error = spawn_error;
            return result;
        }
    }
    while (pipe == INVALID_HANDLE_VALUE
        && (pipe_error == ERROR_FILE_NOT_FOUND
            || pipe_error == ERROR_PIPE_BUSY)
        && Clock::now() < startup_deadline)
    {
        if (spawned_process
            && WaitForSingleObject(spawned_process, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(spawned_process);
            result.code = "trace_worker_crashed";
            result.error = "Trace Worker exited before its service endpoint became ready.";
            return result;
        }
        const std::wstring wide(endpoint.begin(), endpoint.end());
        (void)WaitNamedPipeW(wide.c_str(), 50);
        pipe = OpenPipe(endpoint, pipe_error);
    }
    if (spawned_process)
    {
        CloseHandle(spawned_process);
    }
    if (pipe == INVALID_HANDLE_VALUE)
    {
        result.code =
            Clock::now() >= startup_deadline
                ? "trace_worker_service_start_timeout"
                : "trace_worker_unavailable";
        result.error =
            "Trace Worker service endpoint is unavailable: "
            + WindowsError(pipe_error);
        return result;
    }
    result.launched = true;
    const auto length = EncodeLength(request_json.size());
    DWORD transfer_error = ERROR_SUCCESS;
    auto write_header = TransferExact(
        pipe,
        const_cast<unsigned char*>(length.data()),
        length.size(),
        true,
        request_deadline,
        transfer_error);
    auto write_body = write_header == TransferStatus::Ok
        ? TransferExact(
            pipe,
            reinterpret_cast<unsigned char*>(
                const_cast<char*>(request_json.data())),
            request_json.size(),
            true,
            request_deadline,
            transfer_error)
        : write_header;
    std::array<unsigned char, 4> response_length{};
    auto read_header = write_body == TransferStatus::Ok
        ? TransferExact(
            pipe,
            response_length.data(),
            response_length.size(),
            false,
            request_deadline,
            transfer_error)
        : write_body;
    if (read_header == TransferStatus::Ok)
    {
        const std::uint32_t response_size = DecodeLength(response_length);
        if (response_size == 0 || response_size > kMaxFrameBytes)
        {
            CloseHandle(pipe);
            result.code = "trace_worker_invalid_response";
            result.error = "Trace Worker returned an invalid frame length.";
            return result;
        }
        result.response.resize(response_size);
        read_header = TransferExact(
            pipe,
            reinterpret_cast<unsigned char*>(result.response.data()),
            result.response.size(),
            false,
            request_deadline,
            transfer_error);
    }
    CloseHandle(pipe);
    if (read_header == TransferStatus::Timeout)
    {
        result.timed_out = true;
        result.code = "trace_worker_timeout";
        result.error = "Trace Worker exceeded the configured timeout.";
        return result;
    }
    if (read_header != TransferStatus::Ok)
    {
        result.code = "trace_worker_crashed";
        result.error =
            "Trace Worker service connection closed unexpectedly: "
            + WindowsError(transfer_error);
        return result;
    }
#else
    int socket_error = 0;
    int socket_fd = ConnectSocket(endpoint, socket_error);
    if (socket_fd < 0
        && (socket_error == ENOENT || socket_error == ECONNREFUSED))
    {
        std::string spawn_error;
        if (!SpawnService(location_, endpoint, spawn_error))
        {
            result.code = "trace_worker_unavailable";
            result.error = spawn_error;
            return result;
        }
    }
    while (socket_fd < 0
        && (socket_error == ENOENT || socket_error == ECONNREFUSED)
        && Clock::now() < startup_deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        socket_fd = ConnectSocket(endpoint, socket_error);
    }
    if (socket_fd < 0)
    {
        result.code =
            Clock::now() >= startup_deadline
                ? "trace_worker_service_start_timeout"
                : "trace_worker_unavailable";
        result.error =
            "Trace Worker service endpoint is unavailable: "
            + std::string(std::strerror(socket_error));
        return result;
    }
    result.launched = true;
    const auto length = EncodeLength(request_json.size());
    int transfer_error = 0;
    auto write_header = TransferExact(
        socket_fd,
        const_cast<unsigned char*>(length.data()),
        length.size(),
        true,
        request_deadline,
        transfer_error);
    auto write_body = write_header == TransferStatus::Ok
        ? TransferExact(
            socket_fd,
            reinterpret_cast<unsigned char*>(
                const_cast<char*>(request_json.data())),
            request_json.size(),
            true,
            request_deadline,
            transfer_error)
        : write_header;
    std::array<unsigned char, 4> response_length{};
    auto read_header = write_body == TransferStatus::Ok
        ? TransferExact(
            socket_fd,
            response_length.data(),
            response_length.size(),
            false,
            request_deadline,
            transfer_error)
        : write_body;
    if (read_header == TransferStatus::Ok)
    {
        const std::uint32_t response_size = DecodeLength(response_length);
        if (response_size == 0 || response_size > kMaxFrameBytes)
        {
            ::close(socket_fd);
            result.code = "trace_worker_invalid_response";
            result.error = "Trace Worker returned an invalid frame length.";
            return result;
        }
        result.response.resize(response_size);
        read_header = TransferExact(
            socket_fd,
            reinterpret_cast<unsigned char*>(result.response.data()),
            result.response.size(),
            false,
            request_deadline,
            transfer_error);
    }
    ::close(socket_fd);
    if (read_header == TransferStatus::Timeout)
    {
        result.timed_out = true;
        result.code = "trace_worker_timeout";
        result.error = "Trace Worker exceeded the configured timeout.";
        return result;
    }
    if (read_header != TransferStatus::Ok)
    {
        result.code = "trace_worker_crashed";
        result.error =
            "Trace Worker service connection closed unexpectedly: "
            + std::string(std::strerror(transfer_error));
        return result;
    }
#endif
    result.exit_code = 0;
    return result;
}

WorkerResult WorkerClient::InvokeStdio(
    const std::string_view request_json,
    const std::optional<std::filesystem::path>& trace_import_root) const
{
    WorkerResult result;
    if (!location_.path)
    {
        result.code = "trace_worker_unavailable";
        result.error = location_.error;
        return result;
    }
    if (request_json.empty() || request_json.size() > kMaxFrameBytes)
    {
        result.code = "trace_worker_request_too_large";
        result.error = "Trace Worker request must be between 1 byte and 4 MiB.";
        return result;
    }
    std::string request(request_json);
    request.push_back('\n');

#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{
        sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    if (!CreatePipe(&stdin_read, &stdin_write, &attributes, 0)
        || !CreatePipe(&stdout_read, &stdout_write, &attributes, 0)
        || !CreatePipe(&stderr_read, &stderr_write, &attributes, 0))
    {
        for (HANDLE handle : {
                 stdin_read, stdin_write, stdout_read,
                 stdout_write, stderr_read, stderr_write })
        {
            if (handle)
            {
                CloseHandle(handle);
            }
        }
        result.code = "trace_worker_unavailable";
        result.error = WindowsError(GetLastError());
        return result;
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + location_.path->wstring()
        + L"\" --stdio -NoLog -NoDefaultLog -SaveToUserDir";
    if (location_.engine_directory)
    {
        command += L" -EngineDir=\"";
        command += location_.engine_directory->wstring();
        command += L"\"";
    }
    std::optional<std::vector<wchar_t>> child_environment;
    if (trace_import_root)
    {
        child_environment =
            ChildEnvironmentWithTraceRoot(*trace_import_root);
        if (!child_environment)
        {
            CloseHandle(stdin_read);
            CloseHandle(stdin_write);
            CloseHandle(stdout_read);
            CloseHandle(stdout_write);
            CloseHandle(stderr_read);
            CloseHandle(stderr_write);
            result.code = "trace_worker_unavailable";
            result.error =
                "The one-shot Trace Worker environment could not be built.";
            return result;
        }
    }
    const BOOL created = CreateProcessW(
        location_.path->c_str(),
        command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | (child_environment
            ? CREATE_UNICODE_ENVIRONMENT : 0),
        child_environment
            ? static_cast<void*>(child_environment->data())
            : nullptr,
        nullptr,
        &startup,
        &process);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (!created)
    {
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        result.code = "trace_worker_unavailable";
        result.error = WindowsError(GetLastError());
        return result;
    }
    result.launched = true;
    std::thread stdout_thread(ReadHandle, stdout_read, std::ref(result.response));
    std::thread stderr_thread(
        ReadHandle, stderr_read, std::ref(result.diagnostics));
    DWORD written = 0;
    std::size_t offset = 0;
    while (offset < request.size()
        && WriteFile(
            stdin_write,
            request.data() + offset,
            static_cast<DWORD>(std::min<std::size_t>(
                request.size() - offset,
                1U << 20U)),
            &written,
            nullptr)
        && written > 0)
    {
        offset += written;
    }
    CloseHandle(stdin_write);
    const DWORD wait = WaitForSingleObject(process.hProcess, timeout_ms_);
    if (wait == WAIT_TIMEOUT || wait == WAIT_FAILED)
    {
        result.timed_out = wait == WAIT_TIMEOUT;
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    result.exit_code = static_cast<int>(exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    stdout_thread.join();
    stderr_thread.join();
#else
    int input_pipe[2]{ -1, -1 };
    int output_pipe[2]{ -1, -1 };
    int error_pipe[2]{ -1, -1 };
    if (::pipe(input_pipe) != 0
        || ::pipe(output_pipe) != 0
        || ::pipe(error_pipe) != 0)
    {
        for (const int descriptor : {
                 input_pipe[0], input_pipe[1], output_pipe[0],
                 output_pipe[1], error_pipe[0], error_pipe[1] })
        {
            if (descriptor >= 0)
            {
                ::close(descriptor);
            }
        }
        result.code = "trace_worker_unavailable";
        result.error = std::strerror(errno);
        return result;
    }
    const pid_t process = ::fork();
    if (process == 0)
    {
        ::dup2(input_pipe[0], STDIN_FILENO);
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::dup2(error_pipe[1], STDERR_FILENO);
        ::close(input_pipe[0]); ::close(input_pipe[1]);
        ::close(output_pipe[0]); ::close(output_pipe[1]);
        ::close(error_pipe[0]); ::close(error_pipe[1]);
        if (trace_import_root)
        {
            const std::string roots = MergeTraceRoots(*trace_import_root);
            if (::setenv("UEAI_TRACE_ROOTS", roots.c_str(), 1) != 0)
            {
                _exit(126);
            }
        }
        const std::string engine_argument =
            EngineDirectoryArgument(location_);
        if (engine_argument.empty())
        {
            ::execl(
                location_.path->c_str(), location_.path->c_str(),
                "--stdio", "-NoLog", "-NoDefaultLog", "-SaveToUserDir",
                static_cast<char*>(nullptr));
        }
        else
        {
            ::execl(
                location_.path->c_str(), location_.path->c_str(),
                "--stdio", "-NoLog", "-NoDefaultLog", "-SaveToUserDir",
                engine_argument.c_str(),
                static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    ::close(input_pipe[0]);
    ::close(output_pipe[1]);
    ::close(error_pipe[1]);
    if (process < 0)
    {
        ::close(input_pipe[1]);
        ::close(output_pipe[0]);
        ::close(error_pipe[0]);
        result.code = "trace_worker_unavailable";
        result.error = std::strerror(errno);
        return result;
    }
    result.launched = true;
    std::thread stdout_thread(
        ReadDescriptor, output_pipe[0], std::ref(result.response));
    std::thread stderr_thread(
        ReadDescriptor, error_pipe[0], std::ref(result.diagnostics));
    std::size_t offset = 0;
    while (offset < request.size())
    {
        const ssize_t written = ::write(
            input_pipe[1], request.data() + offset, request.size() - offset);
        if (written <= 0)
        {
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    ::close(input_pipe[1]);
    const auto deadline = DeadlineAfter(timeout_ms_);
    int status = 0;
    while (::waitpid(process, &status, WNOHANG) == 0)
    {
        if (Clock::now() >= deadline)
        {
            result.timed_out = true;
            ::kill(process, SIGKILL);
            ::waitpid(process, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    stdout_thread.join();
    stderr_thread.join();
#endif

    if (result.timed_out)
    {
        result.code = "trace_worker_timeout";
        result.error = "Trace Worker exceeded the configured timeout.";
    }
    else if (result.response.size() >= kMaxFrameBytes)
    {
        result.code = "trace_worker_output_too_large";
        result.error = "Trace Worker response exceeded 4 MiB.";
    }
    return result;
}

} // namespace ue::trace
