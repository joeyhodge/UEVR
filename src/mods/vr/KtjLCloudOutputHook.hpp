#pragma once

#include <safetyhook.hpp>
#include "KtjLCloudOutput.hpp"

namespace uevr::ktjl::cloud_output {
struct Hooks {
    safetyhook::MidHook producer{}, consumer{};
    std::optional<safetyhook::MidHook::Error> error{};
};

template<class Create>
Hooks prepare_hooks(uintptr_t base, safetyhook::MidHookFn producer, safetyhook::MidHookFn consumer, Create&& create) {
    Hooks h{};
    auto a = create(reinterpret_cast<void*>(base + producer_rva), producer, safetyhook::MidHook::StartDisabled);
    if (!a) { h.error = a.error(); return h; }
    h.producer = std::move(*a);
    auto b = create(reinterpret_cast<void*>(base + consumer_rva), consumer, safetyhook::MidHook::StartDisabled);
    if (!b) { h.error = b.error(); return h; }
    h.consumer = std::move(*b);
    return h;
}

template<class Hook>
bool enable_pair(Hook& producer, Hook& consumer) {
    if (!consumer.enable()) { return false; }
    if (!producer.enable()) { (void)consumer.disable(); return false; }
    return true;
}

inline void skip_unprepared_secondary(safetyhook::Context& ctx, uintptr_t base) {
    // Original false-predicate semantics: no output refs acquired. No prologue
    // has executed at this hook, so use the bare RET rather than its epilogue.
    ctx.rax = 0;
    ctx.rip = base + consumer_ret_rva;
}
}
