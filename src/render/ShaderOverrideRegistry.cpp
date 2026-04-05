#include "render/ShaderOverrideRegistry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "Framework.hpp"
#include "render/ShaderCompiler.hpp"
#include "utility/String.hpp"

using json = nlohmann::json;

namespace {
constexpr size_t MAX_RECENT_EVENTS = 64;
constexpr auto AUTO_RELOAD_INTERVAL = std::chrono::milliseconds(1000);

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

ShaderOverrideRegistry& ShaderOverrideRegistry::get() {
    static ShaderOverrideRegistry instance{};
    return instance;
}

void ShaderOverrideRegistry::on_present(Framework&) {
    std::scoped_lock _{m_mutex};
    ++m_frame;

    const auto now = std::chrono::steady_clock::now();
    if (m_force_reload || m_last_scan_time.time_since_epoch().count() == 0 || (now - m_last_scan_time) >= AUTO_RELOAD_INTERVAL) {
        m_force_reload = false;
        m_last_scan_time = now;
        scan_override_directories();
    }
}

void ShaderOverrideRegistry::request_reload() {
    std::scoped_lock _{m_mutex};
    m_force_reload = true;
}

ShaderOverrideRegistry::Snapshot ShaderOverrideRegistry::snapshot() const {
    std::scoped_lock _{m_mutex};

    Snapshot out{};
    out.frame = m_frame;
    out.global_override_dir = global_override_dir().string();
    out.profile_override_dir = profile_override_dir().string();
    out.bound_vertex_shader = m_bound_vertex_shader;
    out.bound_pixel_shader = m_bound_pixel_shader;
    out.recent_events = m_recent_events;

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
        record.first_seen_frame = m_frame;
    }
    record.last_seen_frame = m_frame;
    ++record.seen_count;

    update_d3d11_override_shader(record, device);
}

ID3D11VertexShader* ShaderOverrideRegistry::resolve_d3d11_vertex_shader(ID3D11Device* device, ID3D11VertexShader* shader) {
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
    std::scoped_lock _{m_mutex};

    BoundShaderInfo info{};
    info.backend = Backend::D3D11;
    info.stage = stage;
    info.original_pointer = reinterpret_cast<uintptr_t>(original_shader);
    info.bound_pointer = reinterpret_cast<uintptr_t>(bound_shader);
    info.last_bound_frame = m_frame;

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
    if (device == nullptr || pipeline_state == nullptr || desc == nullptr) {
        return;
    }

    if (g_inside_d3d12_override_pipeline_creation) {
        return;
    }

    std::scoped_lock _{m_mutex};

    auto& record = m_d3d12_graphics_pso_records[reinterpret_cast<uintptr_t>(pipeline_state)];
    record.pipeline_state_pointer = reinterpret_cast<uintptr_t>(pipeline_state);
    record.device = device;
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
    record.last_seen_frame = m_frame;
    if (record.first_seen_frame == 0) {
        record.first_seen_frame = m_frame;
        record.applied_override_revision = (std::numeric_limits<uint64_t>::max)();
    }
    ++record.seen_count;

    update_d3d12_override_pipeline_state(record);
}

ID3D12PipelineState* ShaderOverrideRegistry::resolve_d3d12_pipeline_state(ID3D12PipelineState* pipeline_state) {
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
    std::scoped_lock _{m_mutex};

    auto fill_info = [this, original_pipeline_state, bound_pipeline_state](Stage stage) {
        BoundShaderInfo info{};
        info.backend = Backend::D3D12;
        info.stage = stage;
        info.original_pointer = reinterpret_cast<uintptr_t>(original_pipeline_state);
        info.bound_pointer = reinterpret_cast<uintptr_t>(bound_pipeline_state);
        info.last_bound_frame = m_frame;

        if (original_pipeline_state == nullptr) {
            info.note = "null pso";
            return info;
        }

        const auto it = m_d3d12_graphics_pso_records.find(reinterpret_cast<uintptr_t>(original_pipeline_state));
        if (it == m_d3d12_graphics_pso_records.end()) {
            info.note = "untracked pso (created before hook or unsupported pipeline)";
            return info;
        }

        const auto& record = it->second;
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
}

void ShaderOverrideRegistry::scan_override_directories() {
    std::unordered_map<std::string, std::filesystem::path> discovered_entries{};

    scan_single_directory(global_override_dir(), false);
    for (const auto& [key, entry] : m_overrides) {
        discovered_entries[key] = entry.manifest_path;
    }

    scan_single_directory(profile_override_dir(), true);
    for (const auto& [key, entry] : m_overrides) {
        discovered_entries[key] = entry.manifest_path;
    }

    remove_deleted_entries(discovered_entries);
}

void ShaderOverrideRegistry::scan_single_directory(const std::filesystem::path& dir, bool from_profile_dir) {
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
        auto existing = m_overrides.find(entry.key);

        if (existing == m_overrides.end()) {
            compile_or_refresh_entry(entry);
            m_overrides[entry.key] = std::move(entry);
            continue;
        }

        auto& current = existing->second;
        const bool profile_override_replaces_global = from_profile_dir && !current.from_profile_dir;
        const bool same_origin = current.manifest_path == entry.manifest_path;

        if (!profile_override_replaces_global && !same_origin && current.from_profile_dir && !from_profile_dir) {
            continue;
        }

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

    auto replacement_desc = record.owned_desc;

    if (vertex_entry != nullptr) {
        replacement_desc.vertex_shader = vertex_entry->compiled_bytecode;
        record.vertex_override_name = vertex_entry->name;
    }

    if (pixel_entry != nullptr) {
        replacement_desc.pixel_shader = pixel_entry->compiled_bytecode;
        record.pixel_override_name = pixel_entry->name;
    }

    replacement_desc.refresh_views();

    Microsoft::WRL::ComPtr<ID3D12PipelineState> replacement_pso{};
    HRESULT hr = E_FAIL;

    {
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
