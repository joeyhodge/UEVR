#pragma once

#include <cstdint>
#include <spdlog/spdlog.h>
#include <safetyhook/inline_hook.hpp>

namespace uevr::hooks {
inline void log_error(const char* stage, uintptr_t target, const safetyhook::InlineHook::Error& error) {
    if (error.type == safetyhook::InlineHook::Error::BAD_ALLOCATION) {
        SPDLOG_ERROR("{} at {:x}: SafetyHook error={} allocator_error={}", stage, target,
            static_cast<unsigned>(error.type), static_cast<unsigned>(error.allocator_error));
    } else {
        SPDLOG_ERROR("{} at {:x}: SafetyHook error={} instruction={:x}", stage, target,
            static_cast<unsigned>(error.type), reinterpret_cast<uintptr_t>(error.ip));
    }
}
}
