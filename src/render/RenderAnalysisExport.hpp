#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "render/D3D12Diagnostics.hpp"
#include "render/FrameResourceInspector.hpp"
#include "render/ShaderOverrideRegistry.hpp"

namespace render {
struct PreInjectionShaderRegistryCoverage {
    bool helper_available{};
    uint64_t captured_records{};
    uint64_t imported_records{};
    uint64_t observed_psos{};
    uint64_t matched_observed_psos{};
    uint64_t unmatched_observed_psos{};
    uint64_t matched_samples{};
    uint64_t unmatched_samples{};
    uint64_t dropped_records{};
    uint64_t dropped_shader_bytes{};
};

struct RenderAnalysisExportInput {
    std::string profile_name{};
    std::string backend{};
    uint64_t frame{};
    std::vector<FrameResourceInspector::ResourceInfo> resources{};
    D3D12Diagnostics::Snapshot d3d12{};
    ShaderOverrideRegistry::Snapshot shaders{};
    PreInjectionShaderRegistryCoverage preinjection_registry{};
};

struct RenderAnalysisExportResult {
    bool succeeded{};
    std::filesystem::path bundle_dir{};
    std::vector<std::filesystem::path> files{};
    std::string error{};
};

class RenderAnalysisExport {
public:
    static RenderAnalysisExportResult export_bundle(const RenderAnalysisExportInput& input);
};
} // namespace render
