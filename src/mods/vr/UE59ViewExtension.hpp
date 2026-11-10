#pragma once

#include <nlohmann/json.hpp>
#include <sdk/UE59Compatibility.hpp>

namespace uevr::ue59_extension {
inline bool valid_cache(const nlohmann::json& cached) noexcept {
    try {
        if (!cached.is_object() || !cached.contains("index_0_called") || !cached["index_0_called"].is_boolean()) { return false; }
        for (const auto key : {"ue59_schema", "is_active_this_frame_index", "begin_render_viewfamily_index",
            "pre_render_viewfamily_renderthread_index", "frame_count_offset"}) {
            if (!cached.contains(key) || !cached[key].is_number_integer() ||
                cached[key].get<int64_t>() < 0 || cached[key].get<uint64_t>() > UINT32_MAX) { return false; }
        }
        return cached["ue59_schema"].get<uint32_t>() == 1 && sdk::ue59::extension_mapping(
            cached["index_0_called"].get<bool>(), cached["is_active_this_frame_index"].get<uint32_t>(),
            cached["begin_render_viewfamily_index"].get<uint32_t>(),
            cached["pre_render_viewfamily_renderthread_index"].get<uint32_t>(), cached["frame_count_offset"].get<uint32_t>());
    } catch (...) { return false; }
}
} // namespace uevr::ue59_extension
