#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>

namespace uevr::finch_aim {
inline bool finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline bool valid(const glm::quat& q) {
    const auto n = glm::dot(q, q);
    return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
        std::isfinite(n) && n >= 0.25f && n <= 4.0f;
}

inline std::optional<glm::quat> yaw_only(const glm::quat& q) {
    if (!valid(q)) { return {}; }
    const auto forward = glm::normalize(q) * glm::vec3{0, 0, 1};
    const auto horizontal = forward.x * forward.x + forward.z * forward.z;
    if (!finite(forward) || horizontal < 0.000001f) { return {}; }
    return glm::angleAxis(std::atan2(forward.x, forward.z), glm::vec3{0, 1, 0});
}

inline std::optional<glm::quat> engine_basis(glm::vec3 degrees) {
    if (!finite(degrees)) { return {}; }
    for (glm::length_t i = 0; i < 3; ++i) { degrees[i] = std::remainder(degrees[i], 360.0f); }
    const glm::quat q{glm::yawPitchRoll(glm::radians(-degrees.y), glm::radians(degrees.x), glm::radians(-degrees.z))};
    return valid(q) ? std::optional{glm::normalize(q)} : std::nullopt;
}

struct Plan {
    glm::vec3 world_degrees{};
    glm::quat world{}, next_offset{};
};

// This is side-effect-free. Recenter/decoupled-pitch state is committed only
// after Finch's native helper returns the rotation its camera caller consumes.
inline std::optional<Plan> plan(const glm::vec3& authored, const glm::quat& aim, const glm::quat& offset) {
    const auto base = engine_basis(authored);
    const auto yaw = base ? yaw_only(*base) : std::nullopt;
    const auto aim_yaw = yaw_only(aim);
    if (!yaw || !aim_yaw || !valid(offset)) { return {}; }
    Plan result;
    result.world = glm::normalize(*yaw * glm::normalize(offset) * glm::normalize(aim));
    float pitch{}, heading{}, roll{};
    glm::extractEulerAngleYXZ(glm::mat4_cast(result.world), heading, pitch, roll);
    result.world_degrees = glm::degrees(glm::vec3{pitch, -heading, -roll});
    result.next_offset = glm::inverse(*aim_yaw);
    return finite(result.world_degrees) && valid(result.world) && valid(result.next_offset) ? std::optional{result} : std::nullopt;
}

inline bool accepted(const Plan& expected, const glm::vec3& returned) {
    const auto actual = engine_basis(returned);
    return actual && valid(expected.world) && std::abs(glm::dot(*actual, expected.world)) >= 0.99999f;
}
} // namespace uevr::finch_aim
