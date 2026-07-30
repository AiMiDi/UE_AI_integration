#include "UECliPlatform/Utf8Console.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <streambuf>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace ue::cli
{
namespace
{

Utf8TextResult EncodingError(std::string message)
{
    return {
        false,
        {},
        "invalid_text_encoding",
        std::move(message),
    };
}

bool AppendCodePoint(
    const std::uint32_t code_point,
    std::string& output)
{
    if (code_point <= 0x7f)
    {
        output.push_back(static_cast<char>(code_point));
        return true;
    }
    if (code_point <= 0x7ff)
    {
        output.push_back(static_cast<char>(
            0xc0u | (code_point >> 6u)));
        output.push_back(static_cast<char>(
            0x80u | (code_point & 0x3fu)));
        return true;
    }
    if (code_point >= 0xd800 && code_point <= 0xdfff)
    {
        return false;
    }
    if (code_point <= 0xffff)
    {
        output.push_back(static_cast<char>(
            0xe0u | (code_point >> 12u)));
        output.push_back(static_cast<char>(
            0x80u | ((code_point >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(
            0x80u | (code_point & 0x3fu)));
        return true;
    }
    if (code_point > 0x10ffff)
    {
        return false;
    }
    output.push_back(static_cast<char>(
        0xf0u | (code_point >> 18u)));
    output.push_back(static_cast<char>(
        0x80u | ((code_point >> 12u) & 0x3fu)));
    output.push_back(static_cast<char>(
        0x80u | ((code_point >> 6u) & 0x3fu)));
    output.push_back(static_cast<char>(
        0x80u | (code_point & 0x3fu)));
    return true;
}

bool IsValidUtf8(const std::string_view text)
{
    for (std::size_t index = 0; index < text.size();)
    {
        const auto first =
            static_cast<std::uint8_t>(text[index++]);
        if (first <= 0x7f)
        {
            continue;
        }
        int continuation_count = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf)
        {
            continuation_count = 1;
            code_point = first & 0x1fu;
            minimum = 0x80;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            continuation_count = 2;
            code_point = first & 0x0fu;
            minimum = 0x800;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            continuation_count = 3;
            code_point = first & 0x07u;
            minimum = 0x10000;
        }
        else
        {
            return false;
        }
        if (index + continuation_count > text.size())
        {
            return false;
        }
        for (int offset = 0; offset < continuation_count; ++offset)
        {
            const auto continuation =
                static_cast<std::uint8_t>(text[index++]);
            if ((continuation & 0xc0u) != 0x80u)
            {
                return false;
            }
            code_point =
                (code_point << 6u) | (continuation & 0x3fu);
        }
        if (code_point < minimum
            || code_point > 0x10ffff
            || (code_point >= 0xd800 && code_point <= 0xdfff))
        {
            return false;
        }
    }
    return true;
}

Utf8TextResult DecodeUtf16(
    const std::string_view bytes,
    const bool little_endian)
{
    if ((bytes.size() % 2u) != 0u)
    {
        return EncodingError(
            "UTF-16 input contains a truncated code unit.");
    }
    std::string output;
    output.reserve(bytes.size());
    const auto read_unit =
        [bytes, little_endian](const std::size_t index)
    {
        const auto first =
            static_cast<std::uint8_t>(bytes[index]);
        const auto second =
            static_cast<std::uint8_t>(bytes[index + 1]);
        return static_cast<std::uint16_t>(
            little_endian
                ? first | (static_cast<std::uint16_t>(second) << 8u)
                : (static_cast<std::uint16_t>(first) << 8u) | second);
    };
    for (std::size_t index = 0; index < bytes.size(); index += 2)
    {
        const std::uint16_t first = read_unit(index);
        std::uint32_t code_point = first;
        if (first >= 0xd800 && first <= 0xdbff)
        {
            if (index + 3 >= bytes.size())
            {
                return EncodingError(
                    "UTF-16 input contains a truncated surrogate pair.");
            }
            const std::uint16_t second = read_unit(index + 2);
            if (second < 0xdc00 || second > 0xdfff)
            {
                return EncodingError(
                    "UTF-16 input contains an invalid surrogate pair.");
            }
            code_point =
                0x10000u
                + ((static_cast<std::uint32_t>(first) - 0xd800u)
                    << 10u)
                + (static_cast<std::uint32_t>(second) - 0xdc00u);
            index += 2;
        }
        else if (first >= 0xdc00 && first <= 0xdfff)
        {
            return EncodingError(
                "UTF-16 input contains an unpaired low surrogate.");
        }
        if (!AppendCodePoint(code_point, output))
        {
            return EncodingError(
                "UTF-16 input contains an invalid Unicode scalar.");
        }
    }
    return {true, std::move(output), {}, {}};
}

#if defined(_WIN32)
std::string WideToUtf8(const std::wstring_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length,
            nullptr,
            nullptr)
        != length)
    {
        return {};
    }
    return result;
}

std::wstring Utf8ToWide(const std::string_view value)
{
    if (value.empty())
    {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (length <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length)
        != length)
    {
        return {};
    }
    return result;
}

class WindowsConsoleBuffer final : public std::streambuf
{
public:
    explicit WindowsConsoleBuffer(HANDLE handle)
        : handle_(handle)
    {
    }

protected:
    std::streamsize xsputn(
        const char* text,
        const std::streamsize count) override
    {
        pending_.append(text, static_cast<std::size_t>(count));
        if (pending_.find('\n') != std::string::npos)
        {
            (void)sync();
        }
        return count;
    }

    int overflow(const int character) override
    {
        if (character == traits_type::eof())
        {
            return sync() == 0
                ? traits_type::not_eof(character)
                : traits_type::eof();
        }
        pending_.push_back(static_cast<char>(character));
        if (character == '\n')
        {
            (void)sync();
        }
        return traits_type::not_eof(character);
    }

    int sync() override
    {
        if (pending_.empty())
        {
            return 0;
        }
        const std::wstring wide = Utf8ToWide(pending_);
        if (wide.empty() && !pending_.empty())
        {
            return -1;
        }
        DWORD written = 0;
        const bool ok = WriteConsoleW(
            handle_,
            wide.data(),
            static_cast<DWORD>(wide.size()),
            &written,
            nullptr) != FALSE
            && static_cast<std::size_t>(written) == wide.size();
        if (ok)
        {
            pending_.clear();
        }
        return ok ? 0 : -1;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::string pending_;
};

bool IsConsole(const DWORD standard_handle)
{
    const HANDLE handle = GetStdHandle(standard_handle);
    DWORD mode = 0;
    return handle != INVALID_HANDLE_VALUE
        && handle != nullptr
        && GetConsoleMode(handle, &mode) != FALSE;
}
#endif

} // namespace

Utf8TextResult DecodeTextToUtf8(const std::string_view bytes)
{
    if (bytes.size() >= 4
        && ((static_cast<std::uint8_t>(bytes[0]) == 0xff
                && static_cast<std::uint8_t>(bytes[1]) == 0xfe
                && static_cast<std::uint8_t>(bytes[2]) == 0x00
                && static_cast<std::uint8_t>(bytes[3]) == 0x00)
            || (static_cast<std::uint8_t>(bytes[0]) == 0x00
                && static_cast<std::uint8_t>(bytes[1]) == 0x00
                && static_cast<std::uint8_t>(bytes[2]) == 0xfe
                && static_cast<std::uint8_t>(bytes[3]) == 0xff)))
    {
        return EncodingError(
            "UTF-32 input is not supported; use UTF-8 or BOM-marked UTF-16.");
    }
    if (bytes.size() >= 3
        && static_cast<std::uint8_t>(bytes[0]) == 0xef
        && static_cast<std::uint8_t>(bytes[1]) == 0xbb
        && static_cast<std::uint8_t>(bytes[2]) == 0xbf)
    {
        const std::string_view utf8 = bytes.substr(3);
        return IsValidUtf8(utf8)
            ? Utf8TextResult{true, std::string(utf8), {}, {}}
            : EncodingError(
                "Input after the UTF-8 BOM is not valid UTF-8.");
    }
    if (bytes.size() >= 2
        && static_cast<std::uint8_t>(bytes[0]) == 0xff
        && static_cast<std::uint8_t>(bytes[1]) == 0xfe)
    {
        return DecodeUtf16(bytes.substr(2), true);
    }
    if (bytes.size() >= 2
        && static_cast<std::uint8_t>(bytes[0]) == 0xfe
        && static_cast<std::uint8_t>(bytes[1]) == 0xff)
    {
        return DecodeUtf16(bytes.substr(2), false);
    }
    return IsValidUtf8(bytes)
        ? Utf8TextResult{true, std::string(bytes), {}, {}}
        : EncodingError(
            "Input must be valid UTF-8, UTF-8 with BOM, or BOM-marked UTF-16.");
}

Utf8TextResult ReadTextToUtf8(std::istream& input)
{
    const std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    return DecodeTextToUtf8(bytes);
}

std::filesystem::path Utf8Path(const std::string_view value)
{
#if defined(_WIN32)
    return std::filesystem::path(Utf8ToWide(value));
#else
    return std::filesystem::path(value);
#endif
}

void InitializeUtf8Console()
{
#if defined(_WIN32)
    if (IsConsole(STD_INPUT_HANDLE))
    {
        SetConsoleCP(CP_UTF8);
    }
    else
    {
        (void)_setmode(_fileno(stdin), _O_BINARY);
    }
    if (IsConsole(STD_OUTPUT_HANDLE))
    {
        SetConsoleOutputCP(CP_UTF8);
        static auto* output_buffer =
            new WindowsConsoleBuffer(GetStdHandle(STD_OUTPUT_HANDLE));
        std::cout.rdbuf(output_buffer);
    }
    else
    {
        (void)_setmode(_fileno(stdout), _O_BINARY);
    }
    if (IsConsole(STD_ERROR_HANDLE))
    {
        SetConsoleOutputCP(CP_UTF8);
        static auto* error_buffer =
            new WindowsConsoleBuffer(GetStdHandle(STD_ERROR_HANDLE));
        std::cerr.rdbuf(error_buffer);
    }
    else
    {
        (void)_setmode(_fileno(stderr), _O_BINARY);
    }
#endif
}

std::vector<std::string> Utf8Arguments(
    const int argc,
    char** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(
        static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int index = 0; index < argc; ++index)
    {
        arguments.emplace_back(argv[index] ? argv[index] : "");
    }
    return arguments;
}

#if defined(_WIN32)
std::vector<std::string> Utf8Arguments(
    const int argc,
    wchar_t** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(
        static_cast<std::size_t>(argc > 0 ? argc : 0));
    for (int index = 0; index < argc; ++index)
    {
        arguments.push_back(
            argv[index] ? WideToUtf8(argv[index]) : std::string{});
    }
    return arguments;
}
#endif

} // namespace ue::cli
