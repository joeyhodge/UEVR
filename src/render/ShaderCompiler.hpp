#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace render {
enum class ShaderCompilerBackend : uint8_t {
    Auto,
    Dxc,
    Fxc,
};

struct ShaderCompileRequest {
    std::filesystem::path source_path{};
    std::string entry_point{"main"};
    std::string profile{};
    ShaderCompilerBackend preferred_backend{ShaderCompilerBackend::Auto};
    bool warnings_as_errors{true};
    bool strict_mode{true};
    bool debug_info{
#if defined(_DEBUG)
        true
#else
        false
#endif
    };
    bool strip_reflection{true};
    bool strip_debug{
#if defined(_DEBUG)
        false
#else
        true
#endif
    };
};

struct ShaderCompileResult {
    bool succeeded{};
    std::string compiler{};
    std::string notes{};
    std::string error{};
    std::vector<uint8_t> bytecode{};
};

ShaderCompileResult compile_shader_file(const ShaderCompileRequest& request);
} // namespace render
