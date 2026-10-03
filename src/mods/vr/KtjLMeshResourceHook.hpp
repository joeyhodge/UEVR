#pragma once

#include "KtjLMeshResources.hpp"

namespace uevr::ktjl::mesh {
inline std::shared_ptr<safetyhook::Allocator> make_hook_allocator() {
    // This fork cannot safely host our relocated instructions in its INT3 padding.
    // Keep this pool separate from global allocations that may already use it.
    return safetyhook::Allocator::create_private();
}

inline constexpr size_t hook_count = allocation_paths.size() + 2;
using Callbacks = std::array<safetyhook::MidHookFn, hook_count>;
struct Hooks {
    std::array<safetyhook::MidHook, hook_count> hooks{};
    std::optional<safetyhook::MidHook::Error> error{};
};

template<class Create>
Hooks prepare_hooks(uintptr_t base, const Callbacks& callbacks, Create&& create) {
    Hooks result{};
    constexpr auto rvas = [] {
        std::array<uintptr_t, hook_count> result{begin_rva};
        for (size_t i = 0; i < allocation_paths.size(); ++i) { result[i + 1] = allocation_paths[i].boundary; }
        result.back() = end_rva;
        return result;
    }();
    for (size_t i = 0; i < rvas.size(); ++i) {
        auto hook = create(reinterpret_cast<void*>(base + rvas[i]), callbacks[i], safetyhook::MidHook::StartDisabled);
        if (!hook) { result.error = hook.error(); return result; }
        result.hooks[i] = std::move(*hook);
    }
    return result;
}

enum class Activation { ready, failed, rollback_failed };
template<class Hook, size_t Count>
Activation enable_hooks(std::array<Hook, Count>& hooks) {
    for (size_t i = 0; i < hooks.size(); ++i) {
        if (!hooks[i].enable()) {
            bool rolled_back = true;
            while (i) { rolled_back = bool(hooks[--i].disable()) && rolled_back; }
            return rolled_back ? Activation::failed : Activation::rollback_failed;
        }
    }
    return Activation::ready;
}
}
