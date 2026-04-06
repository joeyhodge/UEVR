#include "render/RenderAnalysisExport.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "Framework.hpp"

using json = nlohmann::json;

namespace {
std::string format_pointer(uintptr_t pointer) {
    std::ostringstream ss{};
    ss << "0x" << std::hex << std::uppercase << pointer;
    return ss.str();
}

const char* resource_backend_to_string(render::FrameResourceInspector::Backend backend) {
    switch (backend) {
    case render::FrameResourceInspector::Backend::D3D11:
        return "D3D11";
    case render::FrameResourceInspector::Backend::D3D12:
        return "D3D12";
    default:
        return "Unknown";
    }
}

const char* shader_backend_to_string(render::ShaderOverrideRegistry::Backend backend) {
    switch (backend) {
    case render::ShaderOverrideRegistry::Backend::D3D11:
        return "D3D11";
    case render::ShaderOverrideRegistry::Backend::D3D12:
        return "D3D12";
    default:
        return "Unknown";
    }
}

const char* shader_stage_to_string(render::ShaderOverrideRegistry::Stage stage) {
    switch (stage) {
    case render::ShaderOverrideRegistry::Stage::Vertex:
        return "VS";
    case render::ShaderOverrideRegistry::Stage::Pixel:
        return "PS";
    default:
        return "Unknown";
    }
}

std::string csv_escape(std::string_view value) {
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
}

std::filesystem::path make_bundle_dir() {
    const auto base_dir = Framework::get_persistent_dir("render_inspector") / "bundles";
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_s(&local_time, &now_time);

    std::ostringstream dir_name{};
    dir_name << std::put_time(&local_time, "%Y%m%d_%H%M%S");
    return base_dir / dir_name.str();
}

json to_json(const render::FrameResourceInspector::ResourceInfo& resource) {
    return {
        {"key", resource.key},
        {"pointer", format_pointer(resource.pointer)},
        {"backend", resource_backend_to_string(resource.backend)},
        {"name", resource.name},
        {"source", resource.source},
        {"type", resource.type},
        {"format", resource.format},
        {"resolution", resource.resolution},
        {"tags", resource.tags},
        {"width", resource.width},
        {"height", resource.height},
        {"first_seen_frame", resource.first_seen_frame},
        {"last_seen_frame", resource.last_seen_frame},
        {"seen_count", resource.seen_count},
        {"change_count", resource.change_count},
        {"is_depth", resource.is_depth},
        {"is_render_target", resource.is_render_target},
        {"is_ui", resource.is_ui},
        {"is_swapchain", resource.is_swapchain},
        {"is_eye", resource.is_eye},
        {"is_velocity_candidate", resource.is_velocity_candidate},
        {"is_rt_pool", resource.is_rt_pool},
        {"is_transient", resource.is_transient},
        {"is_recent", resource.is_recent}
    };
}

json to_json(const render::D3D12Diagnostics::HeapInfo& heap) {
    return {
        {"pointer", format_pointer(heap.pointer)},
        {"name", heap.name},
        {"source", heap.source},
        {"type", heap.type},
        {"total_descriptors", heap.total_descriptors},
        {"estimated_in_use", heap.estimated_in_use},
        {"first_seen_frame", heap.first_seen_frame},
        {"last_seen_frame", heap.last_seen_frame},
        {"bind_count", heap.bind_count},
        {"shader_visible", heap.shader_visible},
        {"transient", heap.transient},
        {"is_active", heap.is_active}
    };
}

json to_json(const render::D3D12Diagnostics::BindingEvent& event) {
    return {
        {"frame", event.frame},
        {"source", event.source},
        {"kind", event.kind},
        {"detail", event.detail}
    };
}

json to_json(const render::D3D12Diagnostics::BarrierEvent& event) {
    return {
        {"frame", event.frame},
        {"source", event.source},
        {"resource", format_pointer(event.resource)},
        {"type", event.type},
        {"before_state", event.before_state},
        {"after_state", event.after_state},
        {"subresource", event.subresource},
        {"note", event.note}
    };
}

json to_json(const render::D3D12Diagnostics::WarningEvent& event) {
    return {
        {"frame", event.frame},
        {"source", event.source},
        {"message", event.message}
    };
}

json to_json(const render::D3D12Diagnostics::BoundTargetInfo& target) {
    return {
        {"handle", format_pointer(target.handle)},
        {"resource", format_pointer(target.resource)},
        {"name", target.name},
        {"descriptor_type", target.descriptor_type}
    };
}

json to_json(const render::D3D12Diagnostics::CurrentBindContext& context) {
    json result{
        {"frame", context.frame},
        {"source", context.source},
        {"exact_this_frame", context.exact_this_frame},
        {"render_targets", json::array()}
    };

    for (const auto& target : context.render_targets) {
        result["render_targets"].push_back(to_json(target));
    }

    if (context.depth_target.has_value()) {
        result["depth_target"] = to_json(*context.depth_target);
    }

    return result;
}

json to_json(const render::ShaderOverrideRegistry::BoundShaderInfo& shader) {
    return {
        {"known", shader.known},
        {"backend", shader_backend_to_string(shader.backend)},
        {"stage", shader_stage_to_string(shader.stage)},
        {"original_pointer", format_pointer(shader.original_pointer)},
        {"bound_pointer", format_pointer(shader.bound_pointer)},
        {"hash", shader.hash},
        {"override_active", shader.override_active},
        {"override_name", shader.override_name},
        {"note", shader.note},
        {"last_bound_frame", shader.last_bound_frame}
    };
}

json to_json(const render::ShaderOverrideRegistry::D3D12PipelinePairInfo& pair) {
    return {
        {"frame", pair.frame},
        {"first_seen_frame", pair.first_seen_frame},
        {"last_seen_frame", pair.last_seen_frame},
        {"hit_count", pair.hit_count},
        {"original_pipeline_state", format_pointer(pair.original_pipeline_state)},
        {"bound_pipeline_state", format_pointer(pair.bound_pipeline_state)},
        {"pipeline_stream", pair.pipeline_stream},
        {"tracking_note", pair.tracking_note},
        {"vertex_shader", to_json(pair.vertex_shader)},
        {"pixel_shader", to_json(pair.pixel_shader)}
    };
}

json to_json(const render::ShaderOverrideRegistry::PsoRenderUsageInfo& usage) {
    return {
        {"render_target_name", usage.render_target_name},
        {"depth_target_name", usage.depth_target_name},
        {"render_target_key", usage.render_target_key},
        {"depth_target_key", usage.depth_target_key},
        {"hit_count", usage.hit_count},
        {"share", usage.share}
    };
}

json to_json(const render::ShaderOverrideRegistry::D3D12PsoAggregateInfo& aggregate) {
    json result{
        {"total_samples", aggregate.total_samples},
        {"sample_share", aggregate.sample_share},
        {"bind_count_with_known_targets", aggregate.bind_count_with_known_targets},
        {"first_seen_frame", aggregate.first_seen_frame},
        {"last_seen_frame", aggregate.last_seen_frame},
        {"original_pso", format_pointer(aggregate.original_pso)},
        {"last_bound_pso", format_pointer(aggregate.last_bound_pso)},
        {"pipeline_stream", aggregate.pipeline_stream},
        {"tracking_note", aggregate.tracking_note},
        {"vs_hash", aggregate.vs_hash},
        {"ps_hash", aggregate.ps_hash},
        {"vs_override", aggregate.vs_override},
        {"ps_override", aggregate.ps_override},
        {"likely_targets", json::array()}
    };

    for (const auto& usage : aggregate.likely_targets) {
        result["likely_targets"].push_back(to_json(usage));
    }

    return result;
}

json to_json(const render::ShaderOverrideRegistry::OverrideEntryInfo& entry) {
    return {
        {"key", entry.key},
        {"name", entry.name},
        {"backend", shader_backend_to_string(entry.backend)},
        {"stage", shader_stage_to_string(entry.stage)},
        {"target_hash", entry.target_hash},
        {"manifest_path", entry.manifest_path},
        {"source_path", entry.source_path},
        {"entry_point", entry.entry_point},
        {"profile", entry.profile},
        {"enabled", entry.enabled},
        {"compiled", entry.compiled},
        {"apply_supported", entry.apply_supported},
        {"from_profile_dir", entry.from_profile_dir},
        {"generation", entry.generation},
        {"status", entry.status},
        {"compiler", entry.compiler},
        {"last_error", entry.last_error}
    };
}

void write_json_file(const std::filesystem::path& path, const json& value) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << value.dump(2);
}
} // namespace

namespace render {
RenderAnalysisExportResult RenderAnalysisExport::export_bundle(const RenderAnalysisExportInput& input) {
    RenderAnalysisExportResult result{};

    try {
        result.bundle_dir = make_bundle_dir();
        std::filesystem::create_directories(result.bundle_dir);

        const auto resources_json_path = result.bundle_dir / "resources.json";
        const auto dx12_json_path = result.bundle_dir / "dx12_diagnostics.json";
        const auto shader_pairs_json_path = result.bundle_dir / "shader_pairs.json";
        const auto pso_profiler_json_path = result.bundle_dir / "pso_profiler.json";
        const auto overrides_json_path = result.bundle_dir / "overrides.json";
        const auto resources_csv_path = result.bundle_dir / "resources.csv";
        const auto shader_pairs_csv_path = result.bundle_dir / "shader_pairs.csv";
        const auto pso_profiler_csv_path = result.bundle_dir / "pso_profiler.csv";
        const auto manifest_json_path = result.bundle_dir / "bundle_manifest.json";

        json resources_json = json::array();
        for (const auto& resource : input.resources) {
            resources_json.push_back(to_json(resource));
        }
        write_json_file(resources_json_path, resources_json);

        json dx12_json{
            {"available", input.d3d12.available},
            {"frame", input.d3d12.frame},
            {"device", format_pointer(input.d3d12.device)},
            {"swapchain", format_pointer(input.d3d12.swapchain)},
            {"command_queue", format_pointer(input.d3d12.command_queue)},
            {"render_width", input.d3d12.render_width},
            {"render_height", input.d3d12.render_height},
            {"display_width", input.d3d12.display_width},
            {"display_height", input.d3d12.display_height},
            {"proton_swapchain", input.d3d12.proton_swapchain},
            {"framegen_swapchain", input.d3d12.framegen_swapchain},
            {"active_cbv_srv_uav_heap", format_pointer(input.d3d12.active_cbv_srv_uav_heap)},
            {"active_sampler_heap", format_pointer(input.d3d12.active_sampler_heap)},
            {"descriptor_heap_sets_this_frame", input.d3d12.descriptor_heap_sets_this_frame},
            {"descriptor_heap_switches_this_frame", input.d3d12.descriptor_heap_switches_this_frame},
            {"resource_barriers_this_frame", input.d3d12.resource_barriers_this_frame},
            {"rtv_binds_this_frame", input.d3d12.rtv_binds_this_frame},
            {"transient_heap_creations_this_frame", input.d3d12.transient_heap_creations_this_frame},
            {"transient_resource_creations_this_frame", input.d3d12.transient_resource_creations_this_frame},
            {"transient_resource_bytes_this_frame", input.d3d12.transient_resource_bytes_this_frame},
            {"tracked_resource_bytes_total", input.d3d12.tracked_resource_bytes_total},
            {"tracked_transient_resource_bytes_total", input.d3d12.tracked_transient_resource_bytes_total},
            {"heaps", json::array()},
            {"recent_bindings", json::array()},
            {"recent_barriers", json::array()},
            {"recent_warnings", json::array()}
        };
        if (input.d3d12.current_bind_context.has_value()) {
            dx12_json["current_bind_context"] = to_json(*input.d3d12.current_bind_context);
        }
        for (const auto& heap : input.d3d12.heaps) {
            dx12_json["heaps"].push_back(to_json(heap));
        }
        for (const auto& event : input.d3d12.recent_bindings) {
            dx12_json["recent_bindings"].push_back(to_json(event));
        }
        for (const auto& event : input.d3d12.recent_barriers) {
            dx12_json["recent_barriers"].push_back(to_json(event));
        }
        for (const auto& event : input.d3d12.recent_warnings) {
            dx12_json["recent_warnings"].push_back(to_json(event));
        }
        write_json_file(dx12_json_path, dx12_json);

        json shader_pairs_json{
            {"frame", input.shaders.frame},
            {"total_samples", input.shaders.total_d3d12_pair_samples},
            {"distinct_pairs", json::array()}
        };
        for (const auto& pair : input.shaders.distinct_d3d12_pairs) {
            shader_pairs_json["distinct_pairs"].push_back(to_json(pair));
        }
        write_json_file(shader_pairs_json_path, shader_pairs_json);

        json pso_profiler_json{
            {"frame", input.shaders.frame},
            {"total_samples", input.shaders.total_d3d12_pso_samples},
            {"aggregates", json::array()}
        };
        for (const auto& aggregate : input.shaders.d3d12_pso_aggregates) {
            pso_profiler_json["aggregates"].push_back(to_json(aggregate));
        }
        write_json_file(pso_profiler_json_path, pso_profiler_json);

        json overrides_json = json::array();
        for (const auto& entry : input.shaders.overrides) {
            overrides_json.push_back(to_json(entry));
        }
        write_json_file(overrides_json_path, overrides_json);

        {
            std::ofstream csv{resources_csv_path, std::ios::binary | std::ios::trunc};
            csv << "key,pointer,backend,name,source,type,format,resolution,tags,width,height,first_seen_frame,last_seen_frame,seen_count,change_count,is_depth,is_render_target,is_ui,is_swapchain,is_eye,is_velocity_candidate,is_rt_pool,is_transient,is_recent\n";
            for (const auto& resource : input.resources) {
                csv << resource.key << ','
                    << csv_escape(format_pointer(resource.pointer)) << ','
                    << csv_escape(resource_backend_to_string(resource.backend)) << ','
                    << csv_escape(resource.name) << ','
                    << csv_escape(resource.source) << ','
                    << csv_escape(resource.type) << ','
                    << csv_escape(resource.format) << ','
                    << csv_escape(resource.resolution) << ','
                    << csv_escape(resource.tags) << ','
                    << resource.width << ','
                    << resource.height << ','
                    << resource.first_seen_frame << ','
                    << resource.last_seen_frame << ','
                    << resource.seen_count << ','
                    << resource.change_count << ','
                    << resource.is_depth << ','
                    << resource.is_render_target << ','
                    << resource.is_ui << ','
                    << resource.is_swapchain << ','
                    << resource.is_eye << ','
                    << resource.is_velocity_candidate << ','
                    << resource.is_rt_pool << ','
                    << resource.is_transient << ','
                    << resource.is_recent << '\n';
            }
        }

        {
            std::ofstream csv{shader_pairs_csv_path, std::ios::binary | std::ios::trunc};
            csv << "frame,first_seen_frame,last_seen_frame,hit_count,original_pso,bound_pso,pipeline_stream,tracking_note,vs_hash,ps_hash,vs_override,ps_override\n";
            for (const auto& pair : input.shaders.distinct_d3d12_pairs) {
                csv << pair.frame << ','
                    << pair.first_seen_frame << ','
                    << pair.last_seen_frame << ','
                    << pair.hit_count << ','
                    << csv_escape(format_pointer(pair.original_pipeline_state)) << ','
                    << csv_escape(format_pointer(pair.bound_pipeline_state)) << ','
                    << pair.pipeline_stream << ','
                    << csv_escape(pair.tracking_note) << ','
                    << csv_escape(pair.vertex_shader.hash) << ','
                    << csv_escape(pair.pixel_shader.hash) << ','
                    << csv_escape(pair.vertex_shader.override_name) << ','
                    << csv_escape(pair.pixel_shader.override_name) << '\n';
            }
        }

        {
            std::ofstream csv{pso_profiler_csv_path, std::ios::binary | std::ios::trunc};
            csv << "total_samples,sample_share,bind_count_with_known_targets,first_seen_frame,last_seen_frame,original_pso,last_bound_pso,pipeline_stream,tracking_note,vs_hash,ps_hash,vs_override,ps_override,top_render_target,top_depth_target,top_target_share\n";
            for (const auto& aggregate : input.shaders.d3d12_pso_aggregates) {
                const auto* top_target = !aggregate.likely_targets.empty() ? &aggregate.likely_targets.front() : nullptr;
                csv << aggregate.total_samples << ','
                    << std::fixed << std::setprecision(6) << aggregate.sample_share << ','
                    << aggregate.bind_count_with_known_targets << ','
                    << aggregate.first_seen_frame << ','
                    << aggregate.last_seen_frame << ','
                    << csv_escape(format_pointer(aggregate.original_pso)) << ','
                    << csv_escape(format_pointer(aggregate.last_bound_pso)) << ','
                    << aggregate.pipeline_stream << ','
                    << csv_escape(aggregate.tracking_note) << ','
                    << csv_escape(aggregate.vs_hash) << ','
                    << csv_escape(aggregate.ps_hash) << ','
                    << csv_escape(aggregate.vs_override) << ','
                    << csv_escape(aggregate.ps_override) << ','
                    << csv_escape(top_target != nullptr ? top_target->render_target_name : "") << ','
                    << csv_escape(top_target != nullptr ? top_target->depth_target_name : "") << ','
                    << (top_target != nullptr ? top_target->share : 0.0) << '\n';
            }
        }

        json manifest{
            {"schema_version", 1},
            {"profile_name", input.profile_name},
            {"backend", input.backend},
            {"frame", input.frame},
            {"files", json::array({
                "resources.json",
                "dx12_diagnostics.json",
                "shader_pairs.json",
                "pso_profiler.json",
                "overrides.json",
                "resources.csv",
                "shader_pairs.csv",
                "pso_profiler.csv"
            })}
        };
        write_json_file(manifest_json_path, manifest);

        result.files = {
            manifest_json_path,
            resources_json_path,
            dx12_json_path,
            shader_pairs_json_path,
            pso_profiler_json_path,
            overrides_json_path,
            resources_csv_path,
            shader_pairs_csv_path,
            pso_profiler_csv_path
        };
        result.succeeded = true;

        spdlog::info("[RenderAnalysisExport] Exported render analysis bundle to {}", result.bundle_dir.string());
        return result;
    } catch (const std::exception& e) {
        result.error = e.what();
        spdlog::error("[RenderAnalysisExport] Failed to export render analysis bundle: {}", result.error);
        return result;
    }
}
} // namespace render
