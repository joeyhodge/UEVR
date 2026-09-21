#pragma once

#include <safetyhook.hpp>
#include "KtjLShadowGather.hpp"

namespace uevr::ktjl::shadow {
struct Hooks {
    safetyhook::MidHook collection{}, gather{};
    std::optional<safetyhook::MidHook::Error> error{};
};

template<class Create>
Hooks prepare_hooks(uintptr_t base, safetyhook::MidHookFn collection, safetyhook::MidHookFn gather, Create&& create) {
    Hooks hooks{};
    auto first = create(reinterpret_cast<void*>(base + collect_rva), collection, safetyhook::MidHook::StartDisabled);
    if (!first) { hooks.error = first.error(); return hooks; }
    hooks.collection = std::move(*first);
    auto second = create(reinterpret_cast<void*>(base + gather_rva), gather, safetyhook::MidHook::StartDisabled);
    if (!second) { hooks.error = second.error(); return hooks; }
    hooks.gather = std::move(*second);
    return hooks;
}

enum class Activation { ready, collection_failed, gather_failed, rollback_failed };
template<class Hook>
Activation enable_pair(Hook& collection, Hook& gather) {
    if (!collection.enable()) { return Activation::collection_failed; }
    if (!gather.enable()) {
        return collection.disable() ? Activation::gather_failed : Activation::rollback_failed;
    }
    return Activation::ready;
}

inline void skip_duplicate(safetyhook::Context& ctx, uintptr_t base) {
    // This is an entry hook: no prologue has executed. Jump only to the bare
    // RET, not the epilogue that would unwind a frame we did not create.
    ctx.rip = base + gather_ret_rva;
}
}
