#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "render/D3D12Diagnostics.hpp"
#include "render/FrameResourceInspector.hpp"
#include "render/ShaderOverrideRegistry.hpp"

namespace render {
struct RenderAnalysisExportInput {
    std::string profile_name{};
    std::string backend{};
    uint64_t frame{};
    std::vector<FrameResourceInspector::ResourceInfo> resources{};
    D3D12Diagnostics::Snapshot d3d12{};
    ShaderOverrideRegistry::Snapshot shaders{};
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
