#include "UECommandCli/CommandCli.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace
{

std::filesystem::path NormalizeExecutablePath(
    const std::filesystem::path& candidate)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(candidate, error);
    if (!error)
    {
        return normalized;
    }
    error.clear();
    normalized = std::filesystem::absolute(candidate, error);
    return error ? candidate : normalized;
}

std::filesystem::path CurrentExecutablePath(const char* argument_zero)
{
#if defined(_WIN32)
    std::vector<wchar_t> buffer(1024);
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length > 0
            && length < static_cast<DWORD>(buffer.size()))
        {
            return NormalizeExecutablePath(
                std::filesystem::path(
                    std::wstring(buffer.data(), length)));
        }
        if (buffer.size() >= 32768)
        {
            break;
        }
        buffer.resize(std::min<std::size_t>(
            buffer.size() * 2,
            32768));
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size > 0)
    {
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        {
            return NormalizeExecutablePath(
                std::filesystem::path(buffer.data()));
        }
    }
#else
    std::vector<char> buffer(1024);
    for (;;)
    {
        const auto length = readlink(
            "/proc/self/exe",
            buffer.data(),
            buffer.size());
        if (length >= 0
            && static_cast<std::size_t>(length) < buffer.size())
        {
            return NormalizeExecutablePath(
                std::filesystem::path(
                    std::string(buffer.data(), length)));
        }
        if (buffer.size() >= 65536)
        {
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
    return NormalizeExecutablePath(
        argument_zero
            ? std::filesystem::path(argument_zero)
            : std::filesystem::path{});
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(
        static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }
    return ue::command::Run(
        arguments,
        CurrentExecutablePath(argc > 0 ? argv[0] : nullptr),
        std::cin,
        std::cout,
        std::cerr);
}
