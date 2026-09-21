#pragma once

#include <optional>
#include <safetyhook.hpp>
#include "KtjLCloudResources.hpp"

namespace uevr::ktjl::cloud {
enum class Boundary { predicate, resource_import };

struct ConsumerHook {
    safetyhook::MidHook hook{};
    Boundary boundary{Boundary::predicate};
    std::optional<safetyhook::MidHook::Error> primary_error{};
    std::optional<safetyhook::MidHook::Error> error{};
};

// Requires validate_code() first. Keep the proven boundary unless its exact
// conditional branch cannot be relocated; never fall back for unrelated errors.
template<typename Create>
ConsumerHook prepare_consumer_hook(uintptr_t base, safetyhook::MidHookFn callback, Create&& create) {
    ConsumerHook result{};
    auto primary = create(reinterpret_cast<void*>(base + hook_rva), callback, safetyhook::MidHook::StartDisabled);
    if (primary) {
        result.hook = std::move(*primary);
        return result;
    }
    result.primary_error = primary.error();
    result.error = primary.error();
    const auto& error = primary.error();
    if (error.type != safetyhook::MidHook::Error::BAD_INLINE_HOOK ||
        error.inline_hook_error.type != safetyhook::InlineHook::Error::IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE ||
        error.inline_hook_error.ip != reinterpret_cast<uint8_t*>(base + hook_rva + 2)) {
        return result;
    }
    result.boundary = Boundary::resource_import;
    auto fallback = create(reinterpret_cast<void*>(base + fallback_hook_rva), callback, safetyhook::MidHook::StartDisabled);
    if (fallback) {
        result.hook = std::move(*fallback);
        result.error.reset();
    } else {
        result.error = fallback.error();
    }
    return result;
}

inline bool consumer_enabled(const safetyhook::Context& ctx, Boundary boundary) {
    return boundary == Boundary::resource_import || (ctx.rax & 0xFF) != 0;
}

inline void skip_unsafe_import(safetyhook::Context& ctx, Boundary boundary, uintptr_t base) {
    if (boundary == Boundary::predicate) {
        ctx.rax &= ~uintptr_t{0xFF};
    } else {
        // RSI already holds the view state here. Use the game's loop-index
        // restoration before joining the original false-predicate continuation.
        ctx.rip = base + fallback_skip_rva;
    }
}
}
