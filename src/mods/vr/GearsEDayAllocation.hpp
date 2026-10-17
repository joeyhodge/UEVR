#pragma once

#include <atomic>

namespace uevr::gears_eday {

template<class TextureRef>
bool defer_to_engine_allocation(TextureRef*& target, TextureRef*& shader,
    bool& allocation_called, bool& hook_initialized, std::atomic_bool& replay_disabled) noexcept {
    // Never retain the engine's stack refs or replay its descriptor constructor.
    // Scene publication happens after Draw; dedicated Slate UI owns its texture.
    target = nullptr;
    shader = nullptr;
    allocation_called = false;
    hook_initialized = true;
    replay_disabled.store(true, std::memory_order_release);
    return false;
}

}
