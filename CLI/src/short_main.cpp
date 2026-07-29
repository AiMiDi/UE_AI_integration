#include "UECommandCli/CommandCli.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    std::vector<std::string> arguments;
    arguments.reserve(
        static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index)
    {
        arguments.emplace_back(argv[index]);
    }
    std::error_code error;
    const auto executable =
        std::filesystem::absolute(argv[0], error);
    return ue::command::Run(
        arguments,
        error ? std::filesystem::path(argv[0]) : executable,
        std::cin,
        std::cout,
        std::cerr);
}
