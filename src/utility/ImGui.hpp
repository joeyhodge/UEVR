#pragma once

#include <string_view>
#include <imgui.h>

namespace imgui {
bool is_point_intersecting_any(float x, float y);

inline void text_wrapped_unformatted(std::string_view text) {
    // Console help can contain printf directives; it is content, not a format.
    const auto begin = text.empty() ? "" : text.data();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(begin, begin + text.size());
    ImGui::PopTextWrapPos();
}
}
