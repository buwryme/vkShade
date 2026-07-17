#include "reshade_parser.hpp"
#include "config_serializer.hpp"
#include "logger.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    void printUsage(const char* argv0)
    {
        std::cerr << "Usage: " << argv0 << " <shader.fx> [--effect-name NAME] [--include DIR ...]\n";
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printUsage(argv[0]);
        return 1;
    }

    std::string shaderPath;
    std::string effectName;
    std::vector<std::string> includePaths;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--effect-name")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for --effect-name\n";
                return 1;
            }
            effectName = argv[++i];
            continue;
        }

        if (arg == "--include")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "Missing value for --include\n";
                return 1;
            }
            includePaths.emplace_back(argv[++i]);
            continue;
        }

        if (shaderPath.empty())
        {
            shaderPath = arg;
            continue;
        }

        std::cerr << "Unexpected argument: " << arg << "\n";
        printUsage(argv[0]);
        return 1;
    }

    if (shaderPath.empty())
    {
        printUsage(argv[0]);
        return 1;
    }

    if (effectName.empty())
        effectName = std::filesystem::path(shaderPath).stem().string();

    vkShade::ShaderTestResult result;
    if (includePaths.empty())
    {
        result = vkShade::testShaderCompilation(effectName, shaderPath);
    }
    else
    {
        result = vkShade::testShaderCompilation(effectName, shaderPath, includePaths);
    }

    std::cout << "effect: " << result.effectName << "\n";
    std::cout << "path: " << result.filePath << "\n";
    std::cout << "success: " << (result.success ? "true" : "false") << "\n";
    std::cout << "usesDepth: " << (result.usesDepth ? "true" : "false") << "\n";
    if (!result.errorMessage.empty())
        std::cout << "message: " << result.errorMessage << "\n";

    return result.success ? 0 : 2;
}

