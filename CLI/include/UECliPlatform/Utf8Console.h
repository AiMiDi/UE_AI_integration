#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace ue::cli
{

struct Utf8TextResult
{
    bool ok = false;
    std::string text;
    std::string code;
    std::string message;
};

[[nodiscard]] Utf8TextResult DecodeTextToUtf8(
    std::string_view bytes);
[[nodiscard]] Utf8TextResult ReadTextToUtf8(
    std::istream& input);
[[nodiscard]] std::filesystem::path Utf8Path(
    std::string_view value);
void InitializeUtf8Console();

[[nodiscard]] std::vector<std::string> Utf8Arguments(
    int argc,
    char** argv);

#if defined(_WIN32)
[[nodiscard]] std::vector<std::string> Utf8Arguments(
    int argc,
    wchar_t** argv);
#endif

} // namespace ue::cli
