#include "render/ShaderOverrideRegistry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "Framework.hpp"
#include "render/D3D12Diagnostics.hpp"
#include "render/ShaderCompiler.hpp"
#include "utility/String.hpp"

using json = nlohmann::json;

namespace {
constexpr size_t MAX_RECENT_EVENTS = 64;
constexpr auto AUTO_RELOAD_INTERVAL = std::chrono::milliseconds(1000);
constexpr auto IDLE_AUTO_RELOAD_INTERVAL = std::chrono::milliseconds(30000);
constexpr uint64_t MAX_PSO_BIND_CONTEXT_AGE_FRAMES = 2;
constexpr size_t MAX_PSO_USAGE_ENTRIES = 8;
constexpr size_t MAX_PSO_USAGE_RECORDS = 32;
constexpr size_t MAX_TRACKED_D3D12_PIPELINE_STATES = 4096;
constexpr size_t MAX_DISTINCT_D3D12_PAIRS = 4096;
constexpr size_t MAX_D3D12_PSO_AGGREGATES = 4096;

std::string backend_to_string(render::ShaderOverrideRegistry::Backend backend) {
    switch (backend) {
    case render::ShaderOverrideRegistry::Backend::D3D11:
        return "dx11";
    case render::ShaderOverrideRegistry::Backend::D3D12:
        return "dx12";
    default:
        return "unknown";
    }
}

std::string stage_to_string(render::ShaderOverrideRegistry::Stage stage) {
    switch (stage) {
    case render::ShaderOverrideRegistry::Stage::Vertex:
        return "vs";
    case render::ShaderOverrideRegistry::Stage::Pixel:
        return "ps";
    default:
        return "unknown";
    }
}

std::string format_pointer_to_hex(uintptr_t pointer) {
    std::ostringstream ss{};
    ss << "0x" << std::hex << std::uppercase << pointer;
    return ss.str();
}

std::string join_target_names(const std::vector<render::D3D12Diagnostics::BoundTargetInfo>& targets) {
    std::ostringstream ss{};
    for (size_t i = 0; i < targets.size(); ++i) {
        if (i > 0) {
            ss << " | ";
        }
        ss << (targets[i].name.empty() ? format_pointer_to_hex(targets[i].handle) : targets[i].name);
    }
    return ss.str();
}

std::string join_target_keys(const std::vector<render::D3D12Diagnostics::BoundTargetInfo>& targets) {
    std::ostringstream ss{};
    for (size_t i = 0; i < targets.size(); ++i) {
        if (i > 0) {
            ss << ";";
        }
        ss << format_pointer_to_hex(targets[i].handle);
    }
    return ss.str();
}

std::optional<render::ShaderOverrideRegistry::Backend> parse_backend(std::string_view value) {
    if (_stricmp(value.data(), "dx11") == 0) {
        return render::ShaderOverrideRegistry::Backend::D3D11;
    }

    if (_stricmp(value.data(), "dx12") == 0) {
        return render::ShaderOverrideRegistry::Backend::D3D12;
    }

    return std::nullopt;
}

std::optional<render::ShaderOverrideRegistry::Stage> parse_stage(std::string_view value) {
    if (_stricmp(value.data(), "vs") == 0 || _stricmp(value.data(), "vertex") == 0) {
        return render::ShaderOverrideRegistry::Stage::Vertex;
    }

    if (_stricmp(value.data(), "ps") == 0 || _stricmp(value.data(), "pixel") == 0) {
        return render::ShaderOverrideRegistry::Stage::Pixel;
    }

    return std::nullopt;
}

std::optional<render::ShaderCompilerBackend> parse_compiler(std::string_view value) {
    if (_stricmp(value.data(), "auto") == 0) {
        return render::ShaderCompilerBackend::Auto;
    }

    if (_stricmp(value.data(), "dxc") == 0) {
        return render::ShaderCompilerBackend::Dxc;
    }

    if (_stricmp(value.data(), "fxc") == 0) {
        return render::ShaderCompilerBackend::Fxc;
    }

    return std::nullopt;
}

std::string normalize_hash(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), value.end());

    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value.erase(0, 2);
    }

    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    return value;
}

std::string default_profile(render::ShaderOverrideRegistry::Backend backend, render::ShaderOverrideRegistry::Stage stage) {
    if (backend == render::ShaderOverrideRegistry::Backend::D3D12) {
        return stage == render::ShaderOverrideRegistry::Stage::Vertex ? "vs_6_0" : "ps_6_0";
    }

    return stage == render::ShaderOverrideRegistry::Stage::Vertex ? "vs_5_0" : "ps_5_0";
}

std::string compiler_to_string(render::ShaderCompilerBackend compiler) {
    switch (compiler) {
    case render::ShaderCompilerBackend::Dxc:
        return "dxc";
    case render::ShaderCompilerBackend::Fxc:
        return "fxc";
    case render::ShaderCompilerBackend::Auto:
    default:
        return "auto";
    }
}

thread_local bool g_inside_d3d12_override_pipeline_creation = false;

class ScopedD3D12OverridePipelineCreation {
public:
    ScopedD3D12OverridePipelineCreation() {
        g_inside_d3d12_override_pipeline_creation = true;
    }

    ~ScopedD3D12OverridePipelineCreation() {
        g_inside_d3d12_override_pipeline_creation = false;
    }
};

std::vector<uint8_t> copy_shader_bytecode_blob(const D3D12_SHADER_BYTECODE& shader) {
    if (shader.pShaderBytecode == nullptr || shader.BytecodeLength == 0) {
        return {};
    }

    const auto* bytes = static_cast<const uint8_t*>(shader.pShaderBytecode);
    return std::vector<uint8_t>{bytes, bytes + shader.BytecodeLength};
}

D3D12_SHADER_BYTECODE make_shader_bytecode_blob(const std::vector<uint8_t>& shader) {
    D3D12_SHADER_BYTECODE out{};
    out.pShaderBytecode = shader.empty() ? nullptr : shader.data();
    out.BytecodeLength = shader.size();
    return out;
}

std::string format_hresult(HRESULT hr) {
    std::ostringstream ss{};
    ss << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr);
    return ss.str();
}

constexpr size_t INVALID_STREAM_OFFSET = static_cast<size_t>(-1);

template <typename T>
struct alignas(void*) PipelineStateStreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T payload;
};

template <typename T>
constexpr size_t stream_payload_offset() {
    return offsetof(PipelineStateStreamSubobject<T>, payload);
}

template <typename T>
constexpr size_t stream_subobject_size() {
    return sizeof(PipelineStateStreamSubobject<T>);
}

template <typename T>
bool read_stream_payload(const uint8_t* stream_bytes, size_t stream_size, size_t subobject_offset, T& out) {
    const auto payload_offset = subobject_offset + stream_payload_offset<T>();
    const auto payload_end = subobject_offset + stream_subobject_size<T>();

    if (payload_offset > stream_size || payload_end > stream_size) {
        return false;
    }

    std::memcpy(&out, stream_bytes + payload_offset, sizeof(T));
    return true;
}

template <typename T>
bool write_stream_payload(std::vector<uint8_t>& stream_bytes, size_t subobject_offset, const T& value) {
    const auto payload_offset = subobject_offset + stream_payload_offset<T>();
    const auto payload_end = subobject_offset + stream_subobject_size<T>();

    if (payload_offset > stream_bytes.size() || payload_end > stream_bytes.size()) {
        return false;
    }

    std::memcpy(stream_bytes.data() + payload_offset, &value, sizeof(T));
    return true;
}

size_t get_pipeline_stream_subobject_size(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
        return stream_subobject_size<ID3D12RootSignature*>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        return stream_subobject_size<D3D12_SHADER_BYTECODE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
        return stream_subobject_size<D3D12_STREAM_OUTPUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
        return stream_subobject_size<D3D12_BLEND_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
        return stream_subobject_size<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
        return stream_subobject_size<D3D12_RASTERIZER_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
        return stream_subobject_size<D3D12_DEPTH_STENCIL_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
        return stream_subobject_size<D3D12_INPUT_LAYOUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
        return stream_subobject_size<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        return stream_subobject_size<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
        return stream_subobject_size<D3D12_RT_FORMAT_ARRAY>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        return stream_subobject_size<DXGI_FORMAT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
        return stream_subobject_size<DXGI_SAMPLE_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
        return stream_subobject_size<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
        return stream_subobject_size<D3D12_CACHED_PIPELINE_STATE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        return stream_subobject_size<D3D12_PIPELINE_STATE_FLAGS>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
        return stream_subobject_size<D3D12_DEPTH_STENCIL_DESC1>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
        return stream_subobject_size<D3D12_VIEW_INSTANCING_DESC>();
    default:
        return 0;
    }
}

bool same_bound_shader_info(
    const render::ShaderOverrideRegistry::BoundShaderInfo& lhs,
    const render::ShaderOverrideRegistry::BoundShaderInfo& rhs
) {
    return lhs.known == rhs.known &&
           lhs.backend == rhs.backend &&
           lhs.stage == rhs.stage &&
           lhs.original_pointer == rhs.original_pointer &&
           lhs.bound_pointer == rhs.bound_pointer &&
           lhs.hash == rhs.hash &&
           lhs.override_active == rhs.override_active &&
           lhs.override_name == rhs.override_name &&
           lhs.note == rhs.note;
}

bool same_d3d12_pipeline_pair(
    const render::ShaderOverrideRegistry::D3D12PipelinePairInfo& lhs,
    const render::ShaderOverrideRegistry::D3D12PipelinePairInfo& rhs
) {
    return lhs.original_pipeline_state == rhs.original_pipeline_state &&
           lhs.bound_pipeline_state == rhs.bound_pipeline_state &&
           lhs.pipeline_stream == rhs.pipeline_stream &&
           lhs.tracking_note == rhs.tracking_note &&
           same_bound_shader_info(lhs.vertex_shader, rhs.vertex_shader) &&
           same_bound_shader_info(lhs.pixel_shader, rhs.pixel_shader);
}
} // namespace

namespace render {
void ShaderOverrideRegistry::OwnedD3D12GraphicsPipelineStateDesc::refresh_views() {
    desc.pRootSignature = root_signature.Get();
    desc.VS = make_shader_bytecode_blob(vertex_shader);
    desc.PS = make_shader_bytecode_blob(pixel_shader);
    desc.DS = make_shader_bytecode_blob(domain_shader);
    desc.HS = make_shader_bytecode_blob(hull_shader);
    desc.GS = make_shader_bytecode_blob(geometry_shader);

    for (size_t i = 0; i < input_elements.size() && i < input_semantic_names.size(); ++i) {
        input_elements[i].SemanticName = input_semantic_names[i].c_str();
    }

    desc.InputLayout.pInputElementDescs = input_elements.empty() ? nullptr : input_elements.data();
    desc.InputLayout.NumElements = static_cast<UINT>(input_elements.size());

    for (size_t i = 0; i < stream_output_declarations.size() && i < stream_output_semantic_names.size(); ++i) {
        stream_output_declarations[i].SemanticName = stream_output_semantic_names[i].c_str();
    }

    desc.StreamOutput.pSODeclaration = stream_output_declarations.empty() ? nullptr : stream_output_declarations.data();
    desc.StreamOutput.NumEntries = static_cast<UINT>(stream_output_declarations.size());
    desc.StreamOutput.pBufferStrides = stream_output_strides.empty() ? nullptr : stream_output_strides.data();
    desc.StreamOutput.NumStrides = static_cast<UINT>(stream_output_strides.size());
    desc.CachedPSO = {};
}

void ShaderOverrideRegistry::OwnedD3D12PipelineStateStream::refresh_views() {
    desc.SizeInBytes = stream_bytes.size();
    desc.pPipelineStateSubobjectStream = stream_bytes.empty() ? nullptr : stream_bytes.data();

    if (root_signature_offset != INVALID_STREAM_OFFSET) {
        auto root_signature_ptr = root_signature.Get();
        write_stream_payload<ID3D12RootSignature*>(stream_bytes, root_signature_offset, root_signature_ptr);
    }

    if (vertex_shader_offset != INVALID_STREAM_OFFSET) {
        write_stream_payload<D3D12_SHADER_BYTECODE>(stream_bytes, vertex_shader_offset, make_shader_bytecode_blob(vertex_shader));
    }

    if (pixel_shader_offset != INVALID_STREAM_OFFSET) {
        write_stream_payload<D3D12_SHADER_BYTECODE>(stream_bytes, pixel_shader_offset, make_shader_bytecode_blob(pixel_shader));
    }

    if (domain_shader_offset != INVALID_STREAM_OFFSET) {
        write_stream_payload<D3D12_SHADER_BYTECODE>(stream_bytes, domain_shader_offset, make_shader_bytecode_blob(domain_shader));
    }

    if (hull_shader_offset != INVALID_STREAM_OFFSET) {
        write_stream_payload<D3D12_SHADER_BYTECODE>(stream_bytes, hull_shader_offset, make_shader_bytecode_blob(hull_shader));
    }

    if (geometry_shader_offset != INVALID_STREAM_OFFSET) {
        write_stream_payload<D3D12_SHADER_BYTECODE>(stream_bytes, geometry_shader_offset, make_shader_bytecode_blob(geometry_shader));
    }

    if (compute_shader_offset != INVALID_STREAM_OFFSET) {
        write_stream_payload<D3D12_SHADER_BYTECODE>(stream_bytes, compute_shader_offset, make_shader_bytecode_blob(compute_shader));
    }

    if (amplification_shader_offset != INVALID_STREAM_OFFSET) {
        write_stream_payload<D3D12_SHADER_BYTECODE>(stream_bytes, amplification_shader_offset, make_shader_bytecode_blob(amplification_shader));
    }

    if (mesh_shader_offset != INVALID_STREAM_OFFSET) {
        write_stream_payload<D3D12_SHADER_BYTECODE>(stream_bytes, mesh_shader_offset, make_shader_bytecode_blob(mesh_shader));
    }

    if (input_layout_offset != INVALID_STREAM_OFFSET) {
        for (size_t i = 0; i < input_elements.size() && i < input_semantic_names.size(); ++i) {
            input_elements[i].SemanticName = input_semantic_names[i].c_str();
        }

        D3D12_INPUT_LAYOUT_DESC input_layout{};
        input_layout.pInputElementDescs = input_elements.empty() ? nullptr : input_elements.data();
        input_layout.NumElements = static_cast<UINT>(input_elements.size());
        write_stream_payload<D3D12_INPUT_LAYOUT_DESC>(stream_bytes, input_layout_offset, input_layout);
    }

    if (stream_output_offset != INVALID_STREAM_OFFSET) {
        for (size_t i = 0; i < stream_output_declarations.size() && i < stream_output_semantic_names.size(); ++i) {
            stream_output_declarations[i].SemanticName = stream_output_semantic_names[i].c_str();
        }

        D3D12_STREAM_OUTPUT_DESC stream_output{};
        stream_output.pSODeclaration = stream_output_declarations.empty() ? nullptr : stream_output_declarations.data();
        stream_output.NumEntries = static_cast<UINT>(stream_output_declarations.size());
        stream_output.pBufferStrides = stream_output_strides.empty() ? nullptr : stream_output_strides.data();
        stream_output.NumStrides = static_cast<UINT>(stream_output_strides.size());

        D3D12_STREAM_OUTPUT_DESC existing{};
        if (read_stream_payload<D3D12_STREAM_OUTPUT_DESC>(stream_bytes.data(), stream_bytes.size(), stream_output_offset, existing)) {
            stream_output.RasterizedStream = existing.RasterizedStream;
        }

        write_stream_payload<D3D12_STREAM_OUTPUT_DESC>(stream_bytes, stream_output_offset, stream_output);
    }

    if (cached_pso_offset != INVALID_STREAM_OFFSET) {
        const D3D12_CACHED_PIPELINE_STATE cached_pso{};
        write_stream_payload<D3D12_CACHED_PIPELINE_STATE>(stream_bytes, cached_pso_offset, cached_pso);
    }

    if (view_instancing_offset != INVALID_STREAM_OFFSET) {
        D3D12_VIEW_INSTANCING_DESC view_instancing{};
        if (read_stream_payload<D3D12_VIEW_INSTANCING_DESC>(stream_bytes.data(), stream_bytes.size(), view_instancing_offset, view_instancing)) {
            view_instancing.ViewInstanceCount = static_cast<UINT>(view_instance_locations.size());
            view_instancing.pViewInstanceLocations = view_instance_locations.empty() ? nullptr : view_instance_locations.data();
            write_stream_payload<D3D12_VIEW_INSTANCING_DESC>(stream_bytes, view_instancing_offset, view_instancing);
        }
    }
}

bool ShaderOverrideRegistry::copy_pipeline_state_stream(
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
    ShaderOverrideRegistry::OwnedD3D12PipelineStateStream& out,
    std::string& error_out
) {
    if (desc == nullptr || desc->pPipelineStateSubobjectStream == nullptr || desc->SizeInBytes == 0) {
        error_out = "Pipeline stream descriptor was empty";
        return false;
    }

    out = {};
    const auto* source_bytes = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
    out.stream_bytes.assign(source_bytes, source_bytes + desc->SizeInBytes);

    size_t offset = 0;
    while (offset < out.stream_bytes.size()) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};
        if (offset + sizeof(type) > out.stream_bytes.size()) {
            error_out = "Pipeline stream ended before subobject type";
            return false;
        }

        std::memcpy(&type, out.stream_bytes.data() + offset, sizeof(type));
        const auto subobject_size = get_pipeline_stream_subobject_size(type);

        if (subobject_size == 0 || offset + subobject_size > out.stream_bytes.size()) {
            std::ostringstream ss{};
            ss << "Unsupported or truncated pipeline stream subobject type " << static_cast<uint32_t>(type);
            error_out = ss.str();
            return false;
        }

        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
            ID3D12RootSignature* root_signature{};
            if (!read_stream_payload<ID3D12RootSignature*>(out.stream_bytes.data(), out.stream_bytes.size(), offset, root_signature)) {
                error_out = "Failed to read pipeline stream root signature";
                return false;
            }

            out.root_signature = root_signature;
            out.root_signature_offset = offset;
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
            D3D12_SHADER_BYTECODE shader{};
            if (!read_stream_payload<D3D12_SHADER_BYTECODE>(out.stream_bytes.data(), out.stream_bytes.size(), offset, shader)) {
                error_out = "Failed to read pipeline stream shader bytecode";
                return false;
            }

            auto* target = &out.vertex_shader;
            auto* target_offset = &out.vertex_shader_offset;

            switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
                target = &out.vertex_shader;
                target_offset = &out.vertex_shader_offset;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
                target = &out.pixel_shader;
                target_offset = &out.pixel_shader_offset;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
                target = &out.domain_shader;
                target_offset = &out.domain_shader_offset;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
                target = &out.hull_shader;
                target_offset = &out.hull_shader_offset;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
                target = &out.geometry_shader;
                target_offset = &out.geometry_shader_offset;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
                target = &out.compute_shader;
                target_offset = &out.compute_shader_offset;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
                target = &out.amplification_shader;
                target_offset = &out.amplification_shader_offset;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
                target = &out.mesh_shader;
                target_offset = &out.mesh_shader_offset;
                break;
            default:
                break;
            }

            *target = copy_shader_bytecode_blob(shader);
            *target_offset = offset;
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
            D3D12_INPUT_LAYOUT_DESC input_layout{};
            if (!read_stream_payload<D3D12_INPUT_LAYOUT_DESC>(out.stream_bytes.data(), out.stream_bytes.size(), offset, input_layout)) {
                error_out = "Failed to read pipeline stream input layout";
                return false;
            }

            out.input_layout_offset = offset;
            out.input_semantic_names.clear();
            out.input_elements.clear();

            if (input_layout.pInputElementDescs != nullptr && input_layout.NumElements > 0) {
                out.input_semantic_names.reserve(input_layout.NumElements);
                out.input_elements.reserve(input_layout.NumElements);

                for (UINT i = 0; i < input_layout.NumElements; ++i) {
                    auto element = input_layout.pInputElementDescs[i];
                    out.input_semantic_names.emplace_back(element.SemanticName != nullptr ? element.SemanticName : "");
                    out.input_elements.emplace_back(element);
                }
            }

            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
            D3D12_STREAM_OUTPUT_DESC stream_output{};
            if (!read_stream_payload<D3D12_STREAM_OUTPUT_DESC>(out.stream_bytes.data(), out.stream_bytes.size(), offset, stream_output)) {
                error_out = "Failed to read pipeline stream output";
                return false;
            }

            out.stream_output_offset = offset;
            out.stream_output_semantic_names.clear();
            out.stream_output_declarations.clear();
            out.stream_output_strides.clear();

            if (stream_output.pSODeclaration != nullptr && stream_output.NumEntries > 0) {
                out.stream_output_semantic_names.reserve(stream_output.NumEntries);
                out.stream_output_declarations.reserve(stream_output.NumEntries);

                for (UINT i = 0; i < stream_output.NumEntries; ++i) {
                    auto declaration = stream_output.pSODeclaration[i];
                    out.stream_output_semantic_names.emplace_back(declaration.SemanticName != nullptr ? declaration.SemanticName : "");
                    out.stream_output_declarations.emplace_back(declaration);
                }
            }

            if (stream_output.pBufferStrides != nullptr && stream_output.NumStrides > 0) {
                out.stream_output_strides.assign(stream_output.pBufferStrides, stream_output.pBufferStrides + stream_output.NumStrides);
            }

            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
            out.cached_pso_offset = offset;
            break;
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
            D3D12_VIEW_INSTANCING_DESC view_instancing{};
            if (!read_stream_payload<D3D12_VIEW_INSTANCING_DESC>(out.stream_bytes.data(), out.stream_bytes.size(), offset, view_instancing)) {
                error_out = "Failed to read pipeline stream view instancing";
                return false;
            }

            out.view_instancing_offset = offset;
            out.view_instance_locations.clear();

            if (view_instancing.pViewInstanceLocations != nullptr && view_instancing.ViewInstanceCount > 0) {
                out.view_instance_locations.assign(
                    view_instancing.pViewInstanceLocations,
                    view_instancing.pViewInstanceLocations + view_instancing.ViewInstanceCount
                );
            }

            break;
        }
        default:
            break;
        }

        offset += subobject_size;
    }

    out.refresh_views();
    return true;
}

ShaderOverrideRegistry& ShaderOverrideRegistry::get() {
    static ShaderOverrideRegistry instance{};
    return instance;
}

void ShaderOverrideRegistry::on_present(Framework&) {
    const auto now = std::chrono::steady_clock::now();
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    m_frame.fetch_add(1, std::memory_order_relaxed);

    const auto full_tracking_active = should_track_d3d11_shaders() || should_track_d3d12_pipelines();
    const auto reload_interval = full_tracking_active ? AUTO_RELOAD_INTERVAL : IDLE_AUTO_RELOAD_INTERVAL;
    const auto reload_interval_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(reload_interval).count();
    const auto last_scan_ns = m_last_scan_time_ns.load(std::memory_order_relaxed);

    if (!m_force_reload.load(std::memory_order_acquire) &&
        last_scan_ns != 0 &&
        now_ns - last_scan_ns < reload_interval_ns)
    {
        return;
    }

    std::scoped_lock _{m_mutex};
    const auto force_reload = m_force_reload.exchange(false, std::memory_order_acq_rel);
    const auto locked_last_scan_ns = m_last_scan_time_ns.load(std::memory_order_relaxed);

    if (!force_reload &&
        locked_last_scan_ns != 0 &&
        now_ns - locked_last_scan_ns < reload_interval_ns)
    {
        return;
    }

    m_last_scan_time_ns.store(now_ns, std::memory_order_relaxed);
    scan_override_directories();
}

void ShaderOverrideRegistry::set_inspector_tracking_enabled(bool enabled) {
    m_inspector_tracking_enabled.store(enabled, std::memory_order_relaxed);
}

bool ShaderOverrideRegistry::should_track_d3d11_shaders() const {
    return m_has_active_d3d11_overrides.load(std::memory_order_relaxed) ||
        m_inspector_tracking_enabled.load(std::memory_order_relaxed);
}

bool ShaderOverrideRegistry::should_track_d3d12_pipelines() const {
    return m_has_active_d3d12_overrides.load(std::memory_order_relaxed) ||
        m_inspector_tracking_enabled.load(std::memory_order_relaxed) ||
        m_capture_next_d3d12_change_hot_path.load(std::memory_order_relaxed);
}

bool ShaderOverrideRegistry::should_collect_d3d12_inspector_data() const {
    return m_inspector_tracking_enabled.load(std::memory_order_relaxed) ||
        m_capture_next_d3d12_change_hot_path.load(std::memory_order_relaxed);
}

void ShaderOverrideRegistry::request_reload() {
    m_force_reload.store(true, std::memory_order_release);
}

void ShaderOverrideRegistry::request_capture_next_d3d12_change() {
    std::scoped_lock _{m_mutex};
    m_capture_next_d3d12_change = true;
    m_capture_next_d3d12_change_hot_path.store(true, std::memory_order_relaxed);
}

void ShaderOverrideRegistry::clear_captured_d3d12_change() {
    std::scoped_lock _{m_mutex};
    m_capture_next_d3d12_change = false;
    m_capture_next_d3d12_change_hot_path.store(false, std::memory_order_relaxed);
    m_captured_d3d12_pair.reset();
}

bool ShaderOverrideRegistry::export_d3d12_pairs_json(std::filesystem::path& out_path, std::string& error_out) {
    std::scoped_lock _{m_mutex};

    try {
        out_path = make_d3d12_pair_export_path("json");
        std::filesystem::create_directories(out_path.parent_path());

        json root{};
        root["frame"] = m_frame.load(std::memory_order_relaxed);
        root["total_samples"] = m_total_d3d12_pair_samples;
        root["distinct_pairs"] = json::array();

        std::vector<D3D12PipelinePairInfo> pairs = m_distinct_d3d12_pairs;
        std::stable_sort(pairs.begin(), pairs.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.hit_count != rhs.hit_count) {
                return lhs.hit_count > rhs.hit_count;
            }

            return lhs.last_seen_frame > rhs.last_seen_frame;
        });

        for (const auto& pair : pairs) {
            json entry{};
            entry["first_seen_frame"] = pair.first_seen_frame;
            entry["last_seen_frame"] = pair.last_seen_frame;
            entry["hit_count"] = pair.hit_count;
            entry["sample_share"] = m_total_d3d12_pair_samples > 0
                ? static_cast<double>(pair.hit_count) / static_cast<double>(m_total_d3d12_pair_samples)
                : 0.0;
            entry["original_pipeline_state"] = format_pointer_to_hex(pair.original_pipeline_state);
            entry["bound_pipeline_state"] = format_pointer_to_hex(pair.bound_pipeline_state);
            entry["pipeline_stream"] = pair.pipeline_stream;
            entry["tracking_note"] = pair.tracking_note;
            entry["vertex_shader"] = {
                {"hash", pair.vertex_shader.hash},
                {"known", pair.vertex_shader.known},
                {"override_active", pair.vertex_shader.override_active},
                {"override_name", pair.vertex_shader.override_name},
                {"note", pair.vertex_shader.note}
            };
            entry["pixel_shader"] = {
                {"hash", pair.pixel_shader.hash},
                {"known", pair.pixel_shader.known},
                {"override_active", pair.pixel_shader.override_active},
                {"override_name", pair.pixel_shader.override_name},
                {"note", pair.pixel_shader.note}
            };

            root["distinct_pairs"].push_back(std::move(entry));
        }

        std::ofstream file{out_path, std::ios::binary | std::ios::trunc};
        file << root.dump(2);
        file.close();

        std::ostringstream ss{};
        ss << "Exported DX12 pair analysis to " << out_path.string();
        push_event(ss.str());
        return true;
    } catch (const std::exception& e) {
        error_out = e.what();
        return false;
    }
}

bool ShaderOverrideRegistry::export_d3d12_pairs_csv(std::filesystem::path& out_path, std::string& error_out) {
    std::scoped_lock _{m_mutex};

    try {
        out_path = make_d3d12_pair_export_path("csv");
        std::filesystem::create_directories(out_path.parent_path());

        std::vector<D3D12PipelinePairInfo> pairs = m_distinct_d3d12_pairs;
        std::stable_sort(pairs.begin(), pairs.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.hit_count != rhs.hit_count) {
                return lhs.hit_count > rhs.hit_count;
            }

            return lhs.last_seen_frame > rhs.last_seen_frame;
        });

        std::ofstream file{out_path, std::ios::binary | std::ios::trunc};
        file << "first_seen_frame,last_seen_frame,hit_count,sample_share,original_pso,bound_pso,pipeline_stream,tracking_note,vs_hash,ps_hash,vs_override,ps_override,vs_note,ps_note\n";

        auto csv_escape = [](std::string_view value) {
            std::string out{value};
            size_t pos = 0;
            while ((pos = out.find('"', pos)) != std::string::npos) {
                out.insert(pos, 1, '"');
                pos += 2;
            }

            if (out.find_first_of(",\"\n\r") != std::string::npos) {
                out.insert(out.begin(), '"');
                out.push_back('"');
            }

            return out;
        };

        for (const auto& pair : pairs) {
            const auto share = m_total_d3d12_pair_samples > 0
                ? static_cast<double>(pair.hit_count) / static_cast<double>(m_total_d3d12_pair_samples)
                : 0.0;
            file << pair.first_seen_frame << ','
                 << pair.last_seen_frame << ','
                 << pair.hit_count << ','
                 << std::fixed << std::setprecision(6) << share << ','
                 << csv_escape(format_pointer_to_hex(pair.original_pipeline_state)) << ','
                 << csv_escape(format_pointer_to_hex(pair.bound_pipeline_state)) << ','
                 << (pair.pipeline_stream ? "yes" : "no") << ','
                 << csv_escape(pair.tracking_note) << ','
                 << csv_escape(pair.vertex_shader.hash) << ','
                 << csv_escape(pair.pixel_shader.hash) << ','
                 << csv_escape(pair.vertex_shader.override_name) << ','
                 << csv_escape(pair.pixel_shader.override_name) << ','
                 << csv_escape(pair.vertex_shader.note) << ','
                 << csv_escape(pair.pixel_shader.note) << '\n';
        }
        file.close();

        std::ostringstream ss{};
        ss << "Exported DX12 pair analysis to " << out_path.string();
        push_event(ss.str());
        return true;
    } catch (const std::exception& e) {
        error_out = e.what();
        return false;
    }
}

ShaderOverrideRegistry::Snapshot ShaderOverrideRegistry::snapshot(bool include_live_d3d12_tracking) const {
    std::scoped_lock _{m_mutex};

    Snapshot out{};
    out.frame = m_frame.load(std::memory_order_relaxed);
    out.global_override_dir = global_override_dir().string();
    out.profile_override_dir = profile_override_dir().string();
    out.bound_vertex_shader = m_bound_vertex_shader;
    out.bound_pixel_shader = m_bound_pixel_shader;
    out.capture_next_d3d12_change_armed = m_capture_next_d3d12_change;
    out.captured_d3d12_pair = m_captured_d3d12_pair;
    out.recent_events = m_recent_events;

    if (include_live_d3d12_tracking) {
        out.current_d3d12_pair = m_last_d3d12_pair;
        out.total_d3d12_pair_samples = m_total_d3d12_pair_samples;
        out.distinct_d3d12_pairs = m_distinct_d3d12_pairs;
        out.total_d3d12_pso_samples = m_total_d3d12_pso_samples;
        out.d3d12_pso_aggregates.reserve(m_d3d12_pso_aggregates.size());
    }

    for (const auto& [_, aggregate] : m_d3d12_pso_aggregates) {
        if (!include_live_d3d12_tracking) {
            break;
        }
        D3D12PsoAggregateInfo info{};
        info.total_samples = aggregate.total_samples;
        info.sample_share = m_total_d3d12_pso_samples > 0
            ? static_cast<double>(aggregate.total_samples) / static_cast<double>(m_total_d3d12_pso_samples)
            : 0.0;
        info.bind_count_with_known_targets = aggregate.bind_count_with_known_targets;
        info.first_seen_frame = aggregate.first_seen_frame;
        info.last_seen_frame = aggregate.last_seen_frame;
        info.original_pso = aggregate.original_pso;
        info.last_bound_pso = aggregate.last_bound_pso;
        info.pipeline_stream = aggregate.pipeline_stream;
        info.tracking_note = aggregate.tracking_note;
        info.vs_hash = aggregate.vs_hash;
        info.ps_hash = aggregate.ps_hash;
        info.vs_override = aggregate.vs_override;
        info.ps_override = aggregate.ps_override;

        std::vector<PsoRenderUsageInfo> usages{};
        usages.reserve(aggregate.usage_by_key.size());
        for (const auto& [usage_key, usage] : aggregate.usage_by_key) {
            (void)usage_key;
            PsoRenderUsageInfo usage_info{};
            usage_info.render_target_name = usage.render_target_name;
            usage_info.depth_target_name = usage.depth_target_name;
            usage_info.render_target_key = usage.render_target_key;
            usage_info.depth_target_key = usage.depth_target_key;
            usage_info.hit_count = usage.hit_count;
            usage_info.share = aggregate.bind_count_with_known_targets > 0
                ? static_cast<double>(usage.hit_count) / static_cast<double>(aggregate.bind_count_with_known_targets)
                : 0.0;
            usages.emplace_back(std::move(usage_info));
        }

        std::stable_sort(usages.begin(), usages.end(), [](const auto& lhs, const auto& rhs) {
            if (lhs.hit_count != rhs.hit_count) {
                return lhs.hit_count > rhs.hit_count;
            }

            return lhs.render_target_name < rhs.render_target_name;
        });

        if (usages.size() > MAX_PSO_USAGE_ENTRIES) {
            usages.resize(MAX_PSO_USAGE_ENTRIES);
        }

        info.likely_targets = std::move(usages);
        out.d3d12_pso_aggregates.emplace_back(std::move(info));
    }

    std::stable_sort(out.d3d12_pso_aggregates.begin(), out.d3d12_pso_aggregates.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.total_samples != rhs.total_samples) {
            return lhs.total_samples > rhs.total_samples;
        }

        return lhs.last_seen_frame > rhs.last_seen_frame;
    });

    out.overrides.reserve(m_overrides.size());
    for (const auto& [_, entry] : m_overrides) {
        OverrideEntryInfo info{};
        info.key = entry.key;
        info.name = entry.name;
        info.backend = entry.backend;
        info.stage = entry.stage;
        info.target_hash = entry.target_hash;
        info.manifest_path = entry.manifest_path.string();
        info.source_path = entry.source_path.string();
        info.entry_point = entry.entry_point;
        info.profile = entry.profile;
        info.enabled = entry.enabled;
        info.compiled = entry.compiled;
        info.apply_supported = entry.apply_supported;
        info.from_profile_dir = entry.from_profile_dir;
        info.generation = entry.generation;
        info.status = entry.status;
        info.compiler = entry.compiler;
        info.last_error = entry.last_error;
        out.overrides.emplace_back(std::move(info));
    }

    std::sort(out.overrides.begin(), out.overrides.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.backend != rhs.backend) {
            return lhs.backend < rhs.backend;
        }
        if (lhs.stage != rhs.stage) {
            return lhs.stage < rhs.stage;
        }
        return lhs.target_hash < rhs.target_hash;
    });

    return out;
}

void ShaderOverrideRegistry::set_d3d11_create_callbacks(CreateVertexShaderFn create_vs, CreatePixelShaderFn create_ps) {
    std::scoped_lock _{m_mutex};
    m_create_vertex_shader = create_vs;
    m_create_pixel_shader = create_ps;
}

void ShaderOverrideRegistry::register_d3d11_shader_creation(Stage stage, ID3D11Device* device, IUnknown* shader, const void* bytecode, size_t bytecode_size) {
    if (!should_track_d3d11_shaders()) {
        return;
    }

    if (shader == nullptr || bytecode == nullptr || bytecode_size == 0) {
        return;
    }

    std::scoped_lock _{m_mutex};

    const auto shader_ptr = reinterpret_cast<uintptr_t>(shader);
    auto& record = m_d3d11_shader_records[shader_ptr];
    record.stage = stage;
    record.shader_pointer = shader_ptr;
    record.device_pointer = reinterpret_cast<uintptr_t>(device);
    record.hash = hash_shader_bytecode(bytecode, bytecode_size);
    if (record.first_seen_frame == 0) {
        record.first_seen_frame = m_frame.load(std::memory_order_relaxed);
    }
    record.last_seen_frame = m_frame.load(std::memory_order_relaxed);
    ++record.seen_count;

    update_d3d11_override_shader(record, device);
}

ID3D11VertexShader* ShaderOverrideRegistry::resolve_d3d11_vertex_shader(ID3D11Device* device, ID3D11VertexShader* shader) {
    if (!should_track_d3d11_shaders()) {
        return shader;
    }

    std::scoped_lock _{m_mutex};

    if (shader == nullptr) {
        return nullptr;
    }

    auto it = m_d3d11_shader_records.find(reinterpret_cast<uintptr_t>(shader));
    if (it == m_d3d11_shader_records.end()) {
        return shader;
    }

    auto& record = it->second;
    update_d3d11_override_shader(record, device);

    if (record.override_active && record.override_shader != nullptr) {
        return static_cast<ID3D11VertexShader*>(record.override_shader.Get());
    }

    return shader;
}

ID3D11PixelShader* ShaderOverrideRegistry::resolve_d3d11_pixel_shader(ID3D11Device* device, ID3D11PixelShader* shader) {
    if (!should_track_d3d11_shaders()) {
        return shader;
    }

    std::scoped_lock _{m_mutex};

    if (shader == nullptr) {
        return nullptr;
    }

    auto it = m_d3d11_shader_records.find(reinterpret_cast<uintptr_t>(shader));
    if (it == m_d3d11_shader_records.end()) {
        return shader;
    }

    auto& record = it->second;
    update_d3d11_override_shader(record, device);

    if (record.override_active && record.override_shader != nullptr) {
        return static_cast<ID3D11PixelShader*>(record.override_shader.Get());
    }

    return shader;
}

void ShaderOverrideRegistry::note_d3d11_shader_bound(Stage stage, IUnknown* original_shader, IUnknown* bound_shader) {
    if (!should_track_d3d11_shaders()) {
        return;
    }

    std::scoped_lock _{m_mutex};

    BoundShaderInfo info{};
    info.backend = Backend::D3D11;
    info.stage = stage;
    info.original_pointer = reinterpret_cast<uintptr_t>(original_shader);
    info.bound_pointer = reinterpret_cast<uintptr_t>(bound_shader);
    info.last_bound_frame = m_frame.load(std::memory_order_relaxed);

    if (original_shader == nullptr) {
        info.known = false;
        info.note = "null";
    } else if (const auto it = m_d3d11_shader_records.find(reinterpret_cast<uintptr_t>(original_shader)); it != m_d3d11_shader_records.end()) {
        const auto& record = it->second;
        info.known = true;
        info.hash = record.hash;
        info.override_active = record.override_active;
        info.override_name = record.override_name;
        if (!record.override_active) {
            info.note = "original";
        }
    } else {
        info.known = false;
        info.note = "hash unavailable (created before hook or unsupported stage)";
    }

    if (stage == Stage::Vertex) {
        m_bound_vertex_shader = std::move(info);
    } else {
        m_bound_pixel_shader = std::move(info);
    }
}

void ShaderOverrideRegistry::register_d3d12_graphics_pipeline_state_creation(
    ID3D12Device* device,
    ID3D12PipelineState* pipeline_state,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc
) {
    if (!should_track_d3d12_pipelines()) {
        return;
    }

    if (device == nullptr || pipeline_state == nullptr || desc == nullptr) {
        return;
    }

    if (g_inside_d3d12_override_pipeline_creation) {
        return;
    }

    std::scoped_lock _{m_mutex};

    const auto pipeline_state_key = reinterpret_cast<uintptr_t>(pipeline_state);
    auto record_it = m_d3d12_graphics_pso_records.find(pipeline_state_key);
    if (record_it == m_d3d12_graphics_pso_records.end()) {
        if (m_d3d12_graphics_pso_records.size() >= MAX_TRACKED_D3D12_PIPELINE_STATES &&
            !m_has_active_d3d12_overrides.load(std::memory_order_relaxed)) {
            return;
        }
        record_it = m_d3d12_graphics_pso_records.try_emplace(pipeline_state_key).first;
    }
    auto& record = record_it->second;
    record.pipeline_state_pointer = reinterpret_cast<uintptr_t>(pipeline_state);
    record.device = device;
    record.is_pipeline_stream = false;
    record.tracking_note.clear();
    record.owned_stream = {};
    record.owned_desc.desc = *desc;
    record.owned_desc.root_signature = desc->pRootSignature;
    record.owned_desc.vertex_shader = copy_shader_bytecode_blob(desc->VS);
    record.owned_desc.pixel_shader = copy_shader_bytecode_blob(desc->PS);
    record.owned_desc.domain_shader = copy_shader_bytecode_blob(desc->DS);
    record.owned_desc.hull_shader = copy_shader_bytecode_blob(desc->HS);
    record.owned_desc.geometry_shader = copy_shader_bytecode_blob(desc->GS);
    record.owned_desc.input_semantic_names.clear();
    record.owned_desc.input_elements.clear();
    record.owned_desc.stream_output_semantic_names.clear();
    record.owned_desc.stream_output_declarations.clear();
    record.owned_desc.stream_output_strides.clear();

    if (desc->InputLayout.pInputElementDescs != nullptr && desc->InputLayout.NumElements > 0) {
        record.owned_desc.input_semantic_names.reserve(desc->InputLayout.NumElements);
        record.owned_desc.input_elements.reserve(desc->InputLayout.NumElements);

        for (UINT i = 0; i < desc->InputLayout.NumElements; ++i) {
            auto element = desc->InputLayout.pInputElementDescs[i];
            record.owned_desc.input_semantic_names.emplace_back(element.SemanticName != nullptr ? element.SemanticName : "");
            record.owned_desc.input_elements.emplace_back(element);
        }
    }

    if (desc->StreamOutput.pSODeclaration != nullptr && desc->StreamOutput.NumEntries > 0) {
        record.owned_desc.stream_output_semantic_names.reserve(desc->StreamOutput.NumEntries);
        record.owned_desc.stream_output_declarations.reserve(desc->StreamOutput.NumEntries);

        for (UINT i = 0; i < desc->StreamOutput.NumEntries; ++i) {
            auto declaration = desc->StreamOutput.pSODeclaration[i];
            record.owned_desc.stream_output_semantic_names.emplace_back(declaration.SemanticName != nullptr ? declaration.SemanticName : "");
            record.owned_desc.stream_output_declarations.emplace_back(declaration);
        }
    }

    if (desc->StreamOutput.pBufferStrides != nullptr && desc->StreamOutput.NumStrides > 0) {
        record.owned_desc.stream_output_strides.assign(
            desc->StreamOutput.pBufferStrides,
            desc->StreamOutput.pBufferStrides + desc->StreamOutput.NumStrides
        );
    }

    record.owned_desc.refresh_views();
    record.vertex_hash = hash_shader_bytecode(desc->VS.pShaderBytecode, desc->VS.BytecodeLength);
    record.pixel_hash = hash_shader_bytecode(desc->PS.pShaderBytecode, desc->PS.BytecodeLength);
    record.last_seen_frame = m_frame.load(std::memory_order_relaxed);
    if (record.first_seen_frame == 0) {
        record.first_seen_frame = m_frame.load(std::memory_order_relaxed);
        record.applied_override_revision = (std::numeric_limits<uint64_t>::max)();
    }
    ++record.seen_count;

    update_d3d12_override_pipeline_state(record);
}

void ShaderOverrideRegistry::register_d3d12_pipeline_state_stream_creation(
    ID3D12Device* device,
    ID3D12PipelineState* pipeline_state,
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc
) {
    if (!should_track_d3d12_pipelines()) {
        return;
    }

    if (device == nullptr || pipeline_state == nullptr || desc == nullptr) {
        return;
    }

    if (g_inside_d3d12_override_pipeline_creation) {
        return;
    }

    std::scoped_lock _{m_mutex};

    const auto pipeline_state_key = reinterpret_cast<uintptr_t>(pipeline_state);
    auto record_it = m_d3d12_graphics_pso_records.find(pipeline_state_key);
    if (record_it == m_d3d12_graphics_pso_records.end()) {
        if (m_d3d12_graphics_pso_records.size() >= MAX_TRACKED_D3D12_PIPELINE_STATES &&
            !m_has_active_d3d12_overrides.load(std::memory_order_relaxed)) {
            return;
        }
        record_it = m_d3d12_graphics_pso_records.try_emplace(pipeline_state_key).first;
    }
    auto& record = record_it->second;
    record.pipeline_state_pointer = reinterpret_cast<uintptr_t>(pipeline_state);
    record.device = device;
    record.is_pipeline_stream = true;
    record.tracking_note.clear();
    record.last_error.clear();
    record.override_pipeline_state.Reset();
    record.owned_desc = {};
    record.owned_stream = {};

    std::string stream_error{};
    if (!copy_pipeline_state_stream(desc, record.owned_stream, stream_error)) {
        record.tracking_note = "pipeline-stream pso not tracked";
        record.last_error = stream_error;
        std::ostringstream ss{};
        ss << "Failed to track DX12 pipeline-stream PSO 0x" << std::hex << std::uppercase << record.pipeline_state_pointer << ": " << stream_error;
        push_event(ss.str());
        spdlog::warn("[ShaderOverrideRegistry] {}", ss.str());
    }

    record.vertex_hash = hash_shader_bytecode(record.owned_stream.vertex_shader.data(), record.owned_stream.vertex_shader.size());
    record.pixel_hash = hash_shader_bytecode(record.owned_stream.pixel_shader.data(), record.owned_stream.pixel_shader.size());
    record.last_seen_frame = m_frame.load(std::memory_order_relaxed);

    if (record.first_seen_frame == 0) {
        record.first_seen_frame = m_frame.load(std::memory_order_relaxed);
        record.applied_override_revision = (std::numeric_limits<uint64_t>::max)();
    }

    ++record.seen_count;

    if (record.tracking_note.empty()) {
        if (!record.owned_stream.compute_shader.empty()) {
            record.tracking_note = "pipeline-stream compute pso not supported";
        } else if (!record.owned_stream.amplification_shader.empty() || !record.owned_stream.mesh_shader.empty()) {
            record.tracking_note = "pipeline-stream mesh pso not supported";
        } else if (record.vertex_hash.empty() && record.pixel_hash.empty()) {
            record.tracking_note = "pipeline-stream pso has no vertex/pixel shader";
        }
    }

    update_d3d12_override_pipeline_state(record);
}

ID3D12PipelineState* ShaderOverrideRegistry::resolve_d3d12_pipeline_state(ID3D12PipelineState* pipeline_state) {
    if (!m_has_active_d3d12_overrides.load(std::memory_order_relaxed)) {
        return pipeline_state;
    }

    std::scoped_lock _{m_mutex};

    if (pipeline_state == nullptr) {
        return nullptr;
    }

    const auto it = m_d3d12_graphics_pso_records.find(reinterpret_cast<uintptr_t>(pipeline_state));
    if (it == m_d3d12_graphics_pso_records.end()) {
        return pipeline_state;
    }

    auto& record = it->second;
    update_d3d12_override_pipeline_state(record);

    if (record.override_active && record.override_pipeline_state != nullptr) {
        return record.override_pipeline_state.Get();
    }

    return pipeline_state;
}

void ShaderOverrideRegistry::note_d3d12_pipeline_state_bound(ID3D12PipelineState* original_pipeline_state, ID3D12PipelineState* bound_pipeline_state) {
    if (!should_collect_d3d12_inspector_data()) {
        return;
    }

    std::scoped_lock _{m_mutex};

    auto fill_info = [this, original_pipeline_state, bound_pipeline_state](Stage stage) {
        BoundShaderInfo info{};
        info.backend = Backend::D3D12;
        info.stage = stage;
        info.original_pointer = reinterpret_cast<uintptr_t>(original_pipeline_state);
        info.bound_pointer = reinterpret_cast<uintptr_t>(bound_pipeline_state);
        info.last_bound_frame = m_frame.load(std::memory_order_relaxed);

        if (original_pipeline_state == nullptr) {
            info.note = "null pso";
            return info;
        }

        const auto it = m_d3d12_graphics_pso_records.find(reinterpret_cast<uintptr_t>(original_pipeline_state));
        if (it == m_d3d12_graphics_pso_records.end()) {
            info.note = "untracked pso (created before injection)";
            return info;
        }

        const auto& record = it->second;
        if (!record.tracking_note.empty()) {
            info.note = record.tracking_note;
            return info;
        }

        const auto& hash = stage == Stage::Vertex ? record.vertex_hash : record.pixel_hash;
        const auto& override_name = stage == Stage::Vertex ? record.vertex_override_name : record.pixel_override_name;

        if (hash.empty()) {
            info.note = stage == Stage::Vertex ? "no vertex shader bytecode" : "no pixel shader bytecode";
            return info;
        }

        info.known = true;
        info.hash = hash;
        info.override_active = !override_name.empty();
        info.override_name = override_name;

        if (bound_pipeline_state != nullptr && bound_pipeline_state != original_pipeline_state) {
            info.note = "replacement pso";
        } else {
            info.note = "original pso";
        }

        return info;
    };

    m_bound_vertex_shader = fill_info(Stage::Vertex);
    m_bound_pixel_shader = fill_info(Stage::Pixel);

    D3D12PipelinePairInfo pair{};
    pair.frame = m_frame.load(std::memory_order_relaxed);
    pair.original_pipeline_state = reinterpret_cast<uintptr_t>(original_pipeline_state);
    pair.bound_pipeline_state = reinterpret_cast<uintptr_t>(bound_pipeline_state);
    pair.vertex_shader = m_bound_vertex_shader;
    pair.pixel_shader = m_bound_pixel_shader;

    if (original_pipeline_state != nullptr) {
        if (const auto it = m_d3d12_graphics_pso_records.find(reinterpret_cast<uintptr_t>(original_pipeline_state)); it != m_d3d12_graphics_pso_records.end()) {
            pair.pipeline_stream = it->second.is_pipeline_stream;
            pair.tracking_note = it->second.tracking_note;
        } else {
            pair.tracking_note = "created before injection";
        }
    } else {
        pair.tracking_note = "null pso";
    }

    record_d3d12_pso_sample(pair);
    record_d3d12_pipeline_pair(pair);
}

void ShaderOverrideRegistry::scan_override_directories() {
    std::unordered_map<std::string, OverrideEntry> discovered_entries{};

    scan_single_directory(global_override_dir(), false, discovered_entries);
    scan_single_directory(profile_override_dir(), true, discovered_entries);

    std::unordered_map<std::string, std::filesystem::path> discovered_paths{};
    discovered_paths.reserve(discovered_entries.size());

    for (auto& [key, entry] : discovered_entries) {
        discovered_paths.emplace(key, entry.manifest_path);
        auto existing = m_overrides.find(key);

        if (existing == m_overrides.end()) {
            compile_or_refresh_entry(entry);
            m_overrides[key] = std::move(entry);
            continue;
        }

        auto& current = existing->second;
        const bool changed =
            current.manifest_path != entry.manifest_path ||
            current.source_path != entry.source_path ||
            current.enabled != entry.enabled ||
            current.entry_point != entry.entry_point ||
            current.profile != entry.profile ||
            current.name != entry.name ||
            current.manifest_write_time != entry.manifest_write_time ||
            current.source_write_time != entry.source_write_time ||
            current.from_profile_dir != entry.from_profile_dir;

        entry.generation = current.generation;
        entry.compiled_bytecode = current.compiled_bytecode;
        entry.compiled = current.compiled;
        entry.status = current.status;
        entry.last_error = current.last_error;

        if (changed) {
            compile_or_refresh_entry(entry);
        }

        current = std::move(entry);
    }

    remove_deleted_entries(discovered_paths);
    refresh_active_override_flags_locked();
}

void ShaderOverrideRegistry::scan_single_directory(
    const std::filesystem::path& dir,
    bool from_profile_dir,
    std::unordered_map<std::string, OverrideEntry>& discovered_entries)
{
    std::error_code ec{};
    std::filesystem::create_directories(dir, ec);

    if (ec || !std::filesystem::exists(dir)) {
        return;
    }

    for (const auto& file : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }

        if (!file.is_regular_file()) {
            continue;
        }

        if (file.path().extension() != ".json") {
            continue;
        }

        auto parsed = parse_manifest(file.path(), from_profile_dir);
        if (!parsed.has_value()) {
            continue;
        }

        auto& entry = parsed.value();
        auto existing = discovered_entries.find(entry.key);

        // Profile entries intentionally replace global entries with the same key.
        if (existing == discovered_entries.end() || from_profile_dir || !existing->second.from_profile_dir) {
            discovered_entries[entry.key] = std::move(entry);
        }
    }
}

void ShaderOverrideRegistry::remove_deleted_entries(const std::unordered_map<std::string, std::filesystem::path>& discovered_entries) {
    std::vector<std::string> dead_keys{};

    for (const auto& [key, entry] : m_overrides) {
        if (!discovered_entries.contains(key)) {
            dead_keys.emplace_back(key);
        }
    }

    for (const auto& key : dead_keys) {
        push_event("Removed shader override " + key);
        m_overrides.erase(key);
        ++m_override_revision;
    }
}

void ShaderOverrideRegistry::compile_or_refresh_entry(OverrideEntry& entry) {
    if (!entry.enabled) {
        entry.status = "Disabled";
        entry.last_error.clear();
        ++m_override_revision;
        return;
    }

    std::string error{};
    if (compile_entry(entry, error)) {
        ++entry.generation;
        entry.compiled = true;
        entry.last_error.clear();
        ++m_override_revision;
        push_event("Compiled shader override " + entry.key + " with " + entry.compiler);
    } else {
        entry.compiled = !entry.compiled_bytecode.empty();
        entry.status = "Compile failed";
        entry.last_error = error;
        const auto compiler_name = entry.compiler.empty() ? compiler_to_string(entry.preferred_compiler) : entry.compiler;
        push_event("Failed to compile shader override " + entry.key + " with " + compiler_name);
        spdlog::error("[ShaderOverrideRegistry] Failed to compile {}: {}", entry.key, error);
    }
}

std::optional<ShaderOverrideRegistry::OverrideEntry> ShaderOverrideRegistry::parse_manifest(const std::filesystem::path& manifest_path, bool from_profile_dir) {
    try {
        std::ifstream file{manifest_path};
        if (!file) {
            return std::nullopt;
        }

        const auto manifest = json::parse(file);
        const auto backend_value = manifest.at("backend").get<std::string>();
        const auto stage_value = manifest.at("stage").get<std::string>();
        const auto hash_value = normalize_hash(manifest.at("target_hash").get<std::string>());

        const auto backend = parse_backend(backend_value);
        const auto stage = parse_stage(stage_value);

        if (!backend.has_value() || !stage.has_value() || hash_value.empty()) {
            push_event("Skipped invalid shader override manifest " + manifest_path.string());
            return std::nullopt;
        }

        OverrideEntry entry{};
        entry.backend = *backend;
        entry.stage = *stage;
        entry.target_hash = hash_value;
        entry.key = make_override_key(*backend, *stage, hash_value);
        entry.name = manifest.value("name", manifest_path.stem().string());
        entry.manifest_path = manifest_path;
        entry.enabled = manifest.value("enabled", true);
        entry.entry_point = manifest.value("entry_point", "main");
        entry.profile = manifest.value("profile", default_profile(*backend, *stage));
        entry.preferred_compiler = ShaderCompilerBackend::Auto;
        entry.from_profile_dir = from_profile_dir;
        entry.apply_supported = true;

        if (manifest.contains("compiler")) {
            const auto compiler_value = manifest.at("compiler").get<std::string>();
            if (const auto compiler = parse_compiler(compiler_value); compiler.has_value()) {
                entry.preferred_compiler = *compiler;
            }
        }

        auto source_value = manifest.at("source").get<std::string>();
        std::filesystem::path source_path = source_value;
        if (source_path.is_relative()) {
            source_path = manifest_path.parent_path() / source_path;
        }
        entry.source_path = source_path.lexically_normal();

        std::error_code ec{};
        entry.manifest_write_time = std::filesystem::last_write_time(entry.manifest_path, ec);
        ec.clear();
        entry.source_write_time = std::filesystem::exists(entry.source_path, ec)
            ? std::filesystem::last_write_time(entry.source_path, ec)
            : std::filesystem::file_time_type{};

        return entry;
    } catch (const std::exception& e) {
        push_event("Failed to parse shader override manifest " + manifest_path.string());
        spdlog::error("[ShaderOverrideRegistry] Failed to parse {}: {}", manifest_path.string(), e.what());
        return std::nullopt;
    }
}

bool ShaderOverrideRegistry::compile_entry(OverrideEntry& entry, std::string& error_out) {
    if (!std::filesystem::exists(entry.source_path)) {
        error_out = "Source file does not exist: " + entry.source_path.string();
        return false;
    }

    if (entry.backend == Backend::D3D11 && entry.profile.find("_6_") != std::string::npos) {
        error_out = "DX11 live overrides require DXBC-compatible shader models (use vs_5_0/ps_5_0 or compiler=fxc)";
        return false;
    }

    ShaderCompileRequest request{};
    request.source_path = entry.source_path;
    request.entry_point = entry.entry_point;
    request.profile = entry.profile;
    request.preferred_backend = entry.preferred_compiler;

    if (entry.backend == Backend::D3D12 && request.preferred_backend == ShaderCompilerBackend::Auto) {
        request.preferred_backend = ShaderCompilerBackend::Dxc;
    }

    entry.compiler = compiler_to_string(request.preferred_backend);
    const auto result = compile_shader_file(request);
    if (!result.compiler.empty()) {
        entry.compiler = result.compiler;
    }

    if (!result.succeeded) {
        error_out = result.error;
        if (!result.notes.empty()) {
            error_out += "\n";
            error_out += result.notes;
        }
        return false;
    }

    entry.compiled_bytecode = result.bytecode;
    entry.status = "Compiled (" + entry.compiler + ")";
    if (!result.notes.empty()) {
        entry.last_error = result.notes;
    }

    return true;
}

void ShaderOverrideRegistry::push_event(std::string message) {
    if (m_recent_events.size() >= MAX_RECENT_EVENTS) {
        m_recent_events.erase(m_recent_events.begin());
    }

    m_recent_events.emplace_back(std::move(message));
}

void ShaderOverrideRegistry::refresh_active_override_flags_locked() {
    bool has_d3d11 = false;
    bool has_d3d12 = false;

    for (const auto& [_, entry] : m_overrides) {
        if (!entry.enabled || !entry.compiled || !entry.apply_supported) {
            continue;
        }

        if (entry.backend == Backend::D3D11) {
            has_d3d11 = true;
        } else if (entry.backend == Backend::D3D12) {
            has_d3d12 = true;
        }
    }

    m_has_active_d3d11_overrides.store(has_d3d11, std::memory_order_relaxed);
    m_has_active_d3d12_overrides.store(has_d3d12, std::memory_order_relaxed);
}

void ShaderOverrideRegistry::record_d3d12_pipeline_pair(const D3D12PipelinePairInfo& info) {
    ++m_total_d3d12_pair_samples;

    auto pair = info;
    const auto pair_key = make_d3d12_pair_key(pair);
    const auto pair_changed = !m_last_d3d12_pair.has_value() || !same_d3d12_pipeline_pair(*m_last_d3d12_pair, info);

    if (const auto index_it = m_distinct_d3d12_pair_indices.find(pair_key); index_it != m_distinct_d3d12_pair_indices.end()) {
        auto& aggregate = m_distinct_d3d12_pairs[index_it->second];
        aggregate.frame = info.frame;
        aggregate.last_seen_frame = info.frame;
        ++aggregate.hit_count;
        pair.first_seen_frame = aggregate.first_seen_frame;
        pair.last_seen_frame = aggregate.last_seen_frame;
        pair.hit_count = aggregate.hit_count;
        aggregate.vertex_shader = pair.vertex_shader;
        aggregate.pixel_shader = pair.pixel_shader;
        aggregate.tracking_note = pair.tracking_note;
        aggregate.bound_pipeline_state = pair.bound_pipeline_state;
    } else if (m_distinct_d3d12_pairs.size() < MAX_DISTINCT_D3D12_PAIRS) {
        pair.first_seen_frame = info.frame;
        pair.last_seen_frame = info.frame;
        pair.hit_count = 1;
        m_distinct_d3d12_pair_indices.emplace(pair_key, m_distinct_d3d12_pairs.size());
        m_distinct_d3d12_pairs.emplace_back(pair);
    }

    m_last_d3d12_pair = pair;

    if (pair_changed && m_capture_next_d3d12_change) {
        m_captured_d3d12_pair = pair;
        m_capture_next_d3d12_change = false;
        m_capture_next_d3d12_change_hot_path.store(false, std::memory_order_relaxed);

        std::ostringstream ss{};
        ss << "Captured DX12 shader change at frame " << pair.frame;
        if (pair.original_pipeline_state != 0) {
            ss << " PSO=0x" << std::hex << std::uppercase << pair.original_pipeline_state;
        }
        push_event(ss.str());
    }
}

void ShaderOverrideRegistry::record_d3d12_pso_sample(const D3D12PipelinePairInfo& info) {
    ++m_total_d3d12_pso_samples;

    const auto pso_key = make_d3d12_pso_key(info);
    auto aggregate_it = m_d3d12_pso_aggregates.find(pso_key);
    if (aggregate_it == m_d3d12_pso_aggregates.end()) {
        if (m_d3d12_pso_aggregates.size() >= MAX_D3D12_PSO_AGGREGATES) {
            return;
        }
        aggregate_it = m_d3d12_pso_aggregates.try_emplace(pso_key).first;
    }
    auto& aggregate = aggregate_it->second;
    if (aggregate.first_seen_frame == 0) {
        aggregate.first_seen_frame = info.frame;
        aggregate.original_pso = info.original_pipeline_state;
        aggregate.pipeline_stream = info.pipeline_stream;
        aggregate.tracking_note = info.tracking_note;
        aggregate.vs_hash = info.vertex_shader.hash;
        aggregate.ps_hash = info.pixel_shader.hash;
    }

    aggregate.last_seen_frame = info.frame;
    aggregate.last_bound_pso = info.bound_pipeline_state;
    aggregate.pipeline_stream = info.pipeline_stream;
    aggregate.tracking_note = info.tracking_note;
    aggregate.vs_hash = info.vertex_shader.hash;
    aggregate.ps_hash = info.pixel_shader.hash;
    aggregate.vs_override = info.vertex_shader.override_active ? info.vertex_shader.override_name : "";
    aggregate.ps_override = info.pixel_shader.override_active ? info.pixel_shader.override_name : "";
    ++aggregate.total_samples;

    const auto bind_context = D3D12Diagnostics::get().current_bind_context();
    if (!bind_context.has_value() || bind_context->frame > info.frame || (info.frame - bind_context->frame) > MAX_PSO_BIND_CONTEXT_AGE_FRAMES) {
        return;
    }

    const auto render_target_name = join_target_names(bind_context->render_targets);
    const auto render_target_key = join_target_keys(bind_context->render_targets);
    const auto depth_target_name = bind_context->depth_target.has_value()
        ? bind_context->depth_target->name
        : std::string{};
    const auto depth_target_key = bind_context->depth_target.has_value()
        ? format_pointer_to_hex(bind_context->depth_target->handle)
        : std::string{};

    if (render_target_name.empty() && depth_target_name.empty()) {
        return;
    }

    std::ostringstream usage_key{};
    usage_key << render_target_key << '|' << depth_target_key;

    const auto usage_key_string = usage_key.str();
    auto usage_it = aggregate.usage_by_key.find(usage_key_string);
    if (usage_it == aggregate.usage_by_key.end()) {
        if (aggregate.usage_by_key.size() >= MAX_PSO_USAGE_RECORDS) {
            return;
        }
        usage_it = aggregate.usage_by_key.try_emplace(usage_key_string).first;
    }
    auto& usage = usage_it->second;
    if (usage.hit_count == 0) {
        usage.render_target_name = render_target_name;
        usage.depth_target_name = depth_target_name;
        usage.render_target_key = render_target_key;
        usage.depth_target_key = depth_target_key;
    }

    ++usage.hit_count;
    ++aggregate.bind_count_with_known_targets;
}

std::string ShaderOverrideRegistry::make_d3d12_pair_key(const D3D12PipelinePairInfo& info) const {
    char buffer[48]{};
    sprintf_s(
        buffer,
        "%016llX:%016llX",
        static_cast<unsigned long long>(info.original_pipeline_state),
        static_cast<unsigned long long>(info.bound_pipeline_state)
    );
    return buffer;
}

std::string ShaderOverrideRegistry::make_d3d12_pso_key(const D3D12PipelinePairInfo& info) const {
    char buffer[24]{};
    sprintf_s(buffer, "%016llX", static_cast<unsigned long long>(info.original_pipeline_state));
    return buffer;
}

std::filesystem::path ShaderOverrideRegistry::make_d3d12_pair_export_path(const char* extension) const {
    const auto export_dir = Framework::get_persistent_dir("render_inspector");
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_s(&local_time, &now_time);

    std::ostringstream file_name{};
    file_name << "dx12_shader_pairs_" << std::put_time(&local_time, "%Y%m%d_%H%M%S") << '.' << extension;
    return export_dir / file_name.str();
}

void ShaderOverrideRegistry::update_d3d11_override_shader(D3D11ShaderRecord& record, ID3D11Device* device) {
    const auto key = make_override_key(Backend::D3D11, record.stage, record.hash);
    const auto override_it = m_overrides.find(key);

    if (override_it == m_overrides.end() || !override_it->second.enabled || !override_it->second.compiled) {
        record.override_active = false;
        record.override_name.clear();
        record.override_shader.Reset();
        record.override_generation = 0;
        return;
    }

    auto& entry = override_it->second;
    if (record.override_generation == entry.generation && record.override_shader != nullptr) {
        record.override_active = true;
        record.override_name = entry.name;
        return;
    }

    if (device == nullptr) {
        record.override_active = false;
        record.override_name.clear();
        record.override_shader.Reset();
        return;
    }

    HRESULT hr = E_FAIL;
    Microsoft::WRL::ComPtr<ID3D11DeviceChild> new_shader{};

    if (record.stage == Stage::Vertex) {
        if (m_create_vertex_shader == nullptr) {
            record.override_active = false;
            record.override_name.clear();
            record.override_shader.Reset();
            return;
        }

        ID3D11VertexShader* created = nullptr;
        hr = m_create_vertex_shader(device, entry.compiled_bytecode.data(), entry.compiled_bytecode.size(), nullptr, &created);
        if (SUCCEEDED(hr) && created != nullptr) {
            new_shader.Attach(created);
        }
    } else {
        if (m_create_pixel_shader == nullptr) {
            record.override_active = false;
            record.override_name.clear();
            record.override_shader.Reset();
            return;
        }

        ID3D11PixelShader* created = nullptr;
        hr = m_create_pixel_shader(device, entry.compiled_bytecode.data(), entry.compiled_bytecode.size(), nullptr, &created);
        if (SUCCEEDED(hr) && created != nullptr) {
            new_shader.Attach(created);
        }
    }

    if (FAILED(hr) || new_shader == nullptr) {
        std::ostringstream ss{};
        ss << "Failed to create D3D11 override shader for " << key << " (HRESULT 0x" << std::hex << std::uppercase << static_cast<uint32_t>(hr) << ")";
        push_event(ss.str());
        record.override_active = false;
        record.override_name.clear();
        record.override_shader.Reset();
        return;
    }

    record.override_generation = entry.generation;
    record.override_shader = new_shader;
    record.override_active = true;
    record.override_name = entry.name;
}

void ShaderOverrideRegistry::update_d3d12_override_pipeline_state(D3D12GraphicsPsoRecord& record) {
    const auto vertex_key = record.vertex_hash.empty() ? std::string{} : make_override_key(Backend::D3D12, Stage::Vertex, record.vertex_hash);
    const auto pixel_key = record.pixel_hash.empty() ? std::string{} : make_override_key(Backend::D3D12, Stage::Pixel, record.pixel_hash);

    OverrideEntry* vertex_entry = nullptr;
    OverrideEntry* pixel_entry = nullptr;

    if (!vertex_key.empty()) {
        if (const auto it = m_overrides.find(vertex_key); it != m_overrides.end() && it->second.enabled && it->second.compiled) {
            vertex_entry = &it->second;
        }
    }

    if (!pixel_key.empty()) {
        if (const auto it = m_overrides.find(pixel_key); it != m_overrides.end() && it->second.enabled && it->second.compiled) {
            pixel_entry = &it->second;
        }
    }

    if (record.applied_override_revision == m_override_revision) {
        return;
    }

    record.applied_override_revision = m_override_revision;
    record.override_active = false;
    record.vertex_override_name.clear();
    record.pixel_override_name.clear();
    record.last_error.clear();
    record.override_pipeline_state.Reset();

    if (record.device == nullptr) {
        record.last_error = "Device unavailable";
        return;
    }

    if (vertex_entry == nullptr && pixel_entry == nullptr) {
        return;
    }

    if (vertex_entry != nullptr) {
        record.vertex_override_name = vertex_entry->name;
    }

    if (pixel_entry != nullptr) {
        record.pixel_override_name = pixel_entry->name;
    }

    Microsoft::WRL::ComPtr<ID3D12PipelineState> replacement_pso{};
    HRESULT hr = E_FAIL;

    if (record.is_pipeline_stream) {
        if (record.owned_stream.empty()) {
            record.last_error = record.tracking_note.empty() ? "pipeline-stream pso not tracked" : record.tracking_note;
            return;
        }

        auto replacement_stream = record.owned_stream;

        if (vertex_entry != nullptr) {
            replacement_stream.vertex_shader = vertex_entry->compiled_bytecode;
        }

        if (pixel_entry != nullptr) {
            replacement_stream.pixel_shader = pixel_entry->compiled_bytecode;
        }

        replacement_stream.refresh_views();

        Microsoft::WRL::ComPtr<ID3D12Device2> device2{};
        hr = record.device->QueryInterface(IID_PPV_ARGS(&device2));

        if (FAILED(hr) || device2 == nullptr) {
            record.last_error = "ID3D12Device2 unavailable for pipeline-stream override";
            record.vertex_override_name.clear();
            record.pixel_override_name.clear();
            return;
        }

        ScopedD3D12OverridePipelineCreation scoped_creation{};
        hr = device2->CreatePipelineState(&replacement_stream.desc, IID_PPV_ARGS(&replacement_pso));
    } else {
        auto replacement_desc = record.owned_desc;

        if (vertex_entry != nullptr) {
            replacement_desc.vertex_shader = vertex_entry->compiled_bytecode;
        }

        if (pixel_entry != nullptr) {
            replacement_desc.pixel_shader = pixel_entry->compiled_bytecode;
        }

        replacement_desc.refresh_views();

        ScopedD3D12OverridePipelineCreation scoped_creation{};
        hr = record.device->CreateGraphicsPipelineState(&replacement_desc.desc, IID_PPV_ARGS(&replacement_pso));
    }

    if (FAILED(hr) || replacement_pso == nullptr) {
        std::ostringstream ss{};
        ss << "Failed to create DX12 override PSO";
        if (!record.vertex_override_name.empty()) {
            ss << " VS=" << record.vertex_override_name;
        }
        if (!record.pixel_override_name.empty()) {
            ss << " PS=" << record.pixel_override_name;
        }
        ss << " (" << format_hresult(hr) << ")";

        record.last_error = ss.str();
        push_event(ss.str());
        spdlog::error("[ShaderOverrideRegistry] {}", ss.str());
        record.vertex_override_name.clear();
        record.pixel_override_name.clear();
        return;
    }

    record.override_pipeline_state = replacement_pso;
    record.override_active = true;

    std::ostringstream ss{};
    ss << "Created DX12 override PSO for 0x" << std::hex << std::uppercase << record.pipeline_state_pointer;
    if (!record.vertex_override_name.empty()) {
        ss << " VS=" << record.vertex_override_name;
    }
    if (!record.pixel_override_name.empty()) {
        ss << " PS=" << record.pixel_override_name;
    }
    push_event(ss.str());
}

std::string ShaderOverrideRegistry::make_override_key(Backend backend, Stage stage, std::string_view target_hash) const {
    return backend_to_string(backend) + ":" + stage_to_string(stage) + ":" + normalize_hash(std::string{target_hash});
}

std::string ShaderOverrideRegistry::hash_shader_bytecode(const void* bytecode, size_t bytecode_size) const {
    if (bytecode == nullptr || bytecode_size == 0) {
        return {};
    }

    constexpr uint64_t fnv_offset = 1469598103934665603ull;
    constexpr uint64_t fnv_prime = 1099511628211ull;

    uint64_t hash = fnv_offset;
    const auto* bytes = static_cast<const uint8_t*>(bytecode);

    for (size_t i = 0; i < bytecode_size; ++i) {
        hash ^= bytes[i];
        hash *= fnv_prime;
    }

    std::ostringstream ss{};
    ss << std::hex << std::setfill('0') << std::setw(16) << std::nouppercase << hash;
    return normalize_hash(ss.str());
}

std::filesystem::path ShaderOverrideRegistry::global_override_dir() const {
    return Framework::get_persistent_dir().parent_path() / "shader_overrides";
}

std::filesystem::path ShaderOverrideRegistry::profile_override_dir() const {
    return Framework::get_persistent_dir("shader_overrides");
}
} // namespace render
