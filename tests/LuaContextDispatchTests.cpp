#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "ScriptContext.hpp"
#include "ScriptState.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename T>
bool register_callback(T) { return true; }

void log_message(const char*, ...) {}

struct MockSDK {
    static inline MockSDK* active{};
    UEVR_PluginVersion version{2, 40, 0};
    UEVR_PluginFunctions functions{};
    UEVR_PluginCallbacks callbacks{};
    UEVR_SDKCallbacks sdk_callbacks{};
    UEVR_SDKData sdk{};
    UEVR_LuaData lua{};
    UEVR_PluginInitializeParam params{};
    UEVR_Engine_TickCb tick{};
    UEVR_Slate_DrawWindow_RenderThreadCb slate{};

    MockSDK() {
        active = this;
        functions.log_info = log_message;
        functions.log_warn = log_message;
        functions.log_error = log_message;
        functions.remove_callback = [](void*) { return true; };
        callbacks.on_xinput_get_state = register_callback<UEVR_OnXInputGetStateCb>;
        callbacks.on_xinput_set_state = register_callback<UEVR_OnXInputSetStateCb>;
        sdk_callbacks.on_pre_engine_tick = [](UEVR_Engine_TickCb fn) { active->tick = fn; return true; };
        sdk_callbacks.on_post_engine_tick = register_callback<UEVR_Engine_TickCb>;
        sdk_callbacks.on_pre_slate_draw_window_render_thread = [](UEVR_Slate_DrawWindow_RenderThreadCb fn) {
            active->slate = fn;
            return true;
        };
        sdk_callbacks.on_post_slate_draw_window_render_thread = register_callback<UEVR_Slate_DrawWindow_RenderThreadCb>;
        sdk_callbacks.on_early_calculate_stereo_view_offset = register_callback<UEVR_Stereo_CalculateStereoViewOffsetCb>;
        sdk_callbacks.on_pre_calculate_stereo_view_offset = register_callback<UEVR_Stereo_CalculateStereoViewOffsetCb>;
        sdk_callbacks.on_post_calculate_stereo_view_offset = register_callback<UEVR_Stereo_CalculateStereoViewOffsetCb>;
        sdk_callbacks.on_pre_viewport_client_draw = register_callback<UEVR_ViewportClient_DrawCb>;
        sdk_callbacks.on_post_viewport_client_draw = register_callback<UEVR_ViewportClient_DrawCb>;
        sdk.callbacks = &sdk_callbacks;
        params.version = &version;
        params.functions = &functions;
        params.callbacks = &callbacks;
        params.sdk = &sdk;
        lua.add_additional_bindings = [](lua_State*) {};
        params.lua = &lua;
    }
};

struct Context {
    std::shared_ptr<sol::state> lua = std::make_shared<sol::state>();
    std::shared_ptr<uevr::ScriptContext> context;

    explicit Context(MockSDK& sdk) {
        lua->open_libraries(sol::lib::base);
        context = uevr::ScriptContext::create(lua, &sdk.params);
        context->setup_callback_bindings();
        (*lua)["callbacks"] = &sdk.sdk_callbacks;
    }

    void script(const char* text) {
        std::scoped_lock lock{context->get_mutex()};
        auto result = lua->safe_script(text, sol::script_pass_on_error);
        if (!result.valid()) {
            const sol::error error = result;
            throw std::runtime_error{error.what()};
        }
    }

    void check_errors() const {
        require(context->get_last_script_error().e.empty(), "production dispatcher reported a Lua callback error");
    }
};

void test_reentry(MockSDK& sdk) {
    unsigned int ticks{}, slates{};
    Context current{sdk};
    const auto tick = sdk.tick;
    (*current.lua)["record_tick"] = [&] { ++ticks; };
    (*current.lua)["nested_dispatch"] = [&] {
        ++slates;
        tick(nullptr, 0.01f);
    };
    current.script(R"(
        callbacks.on_pre_engine_tick(function() record_tick() end)
        callbacks.on_pre_slate_draw_window_render_thread(function() nested_dispatch() end)
    )");
    // This reenters the actual registry while the same Lua-state lock is held.
    sdk.slate(nullptr, nullptr);
    require(ticks == 1 && slates == 1, "same-thread nested dispatch completes exactly once");
    current.check_errors();
}

void test_lock_order(MockSDK& sdk) {
    std::latch game_owns_context{1}, render_entered{1};
    std::atomic<unsigned int> ticks{}, slates{};
    Context probe{sdk}, target{sdk};
    (*probe.lua)["render_entered"] = [&] { render_entered.count_down(); ++slates; };
    (*probe.lua)["record_tick"] = [&] { ++ticks; };
    (*target.lua)["record_slate"] = [&] { ++slates; };
    (*target.lua)["record_tick"] = [&] { ++ticks; };
    probe.script(R"(
        callbacks.on_pre_engine_tick(function() record_tick() end)
        callbacks.on_pre_slate_draw_window_render_thread(function() render_entered() end)
    )");
    target.script(R"(
        callbacks.on_pre_engine_tick(function() record_tick() end)
        callbacks.on_pre_slate_draw_window_render_thread(function() record_slate() end)
    )");
    const auto tick = sdk.tick;
    const auto slate = sdk.slate;
    // No sleeps: the old implementation holds the registry lock when the
    // probe signals, then waits on target while GameThread reenters the registry.
    std::thread game{[&] {
        std::scoped_lock lock{target.context->get_mutex()};
        game_owns_context.count_down();
        render_entered.wait();
        tick(nullptr, 0.01f);
    }};
    std::thread render{[&] {
        game_owns_context.wait();
        slate(nullptr, nullptr);
    }};
    game.join();
    render.join();
    require(ticks == 2 && slates == 2, "cross-thread context/registry inversion completes both dispatches");
    probe.check_errors();
    target.check_errors();
}

void test_membership(MockSDK& sdk) {
    std::vector<int> order;
    std::unique_ptr<Context> added;
    Context first{sdk}, second{sdk};
    (*first.lua)["visit_first"] = [&] {
        order.push_back(1);
        if (!added) {
            added = std::make_unique<Context>(sdk);
            (*added->lua)["visit_added"] = [&] { order.push_back(3); };
            added->script("callbacks.on_pre_engine_tick(function() visit_added() end)");
        }
    };
    (*second.lua)["visit_second"] = [&] { order.push_back(2); };
    first.script("callbacks.on_pre_engine_tick(function() visit_first() end)");
    second.script("callbacks.on_pre_engine_tick(function() visit_second() end)");
    sdk.tick(nullptr, 0.01f);
    require(order == std::vector<int>{1, 2}, "registration during a callback starts at the next dispatch");
    order.clear();
    sdk.tick(nullptr, 0.01f);
    require(order == std::vector<int>{1, 2, 3}, "snapshot preserves registration order");
    added.reset();
    order.clear();
    sdk.tick(nullptr, 0.01f);
    require(order == std::vector<int>{1, 2}, "expired registrations are pruned without skipping live entries");
    first.check_errors();
    second.check_errors();
}

void test_lifetime(MockSDK& sdk) {
    bool dropped{}, retained{};
    unsigned int calls{};
    Context first{sdk}, second{sdk};
    const std::weak_ptr<uevr::ScriptContext> weak_context = second.context;
    const std::weak_ptr<sol::state> weak_lua = second.lua;
    (*first.lua)["drop_owner"] = [&] {
        if (!dropped) {
            second.context.reset();
            second.lua.reset();
            retained = !weak_context.expired() && !weak_lua.expired();
            dropped = true;
        }
    };
    (*second.lua)["record_call"] = [&] { ++calls; };
    first.script("callbacks.on_pre_engine_tick(function() drop_owner() end)");
    second.script("callbacks.on_pre_engine_tick(function() record_call() end)");
    sdk.tick(nullptr, 0.01f);
    require(retained && calls == 1, "snapshot owns each context and its built-in Lua state until dispatch completes");
    require(weak_context.expired() && weak_lua.expired(), "dispatch releases its temporary ownership afterward");
    sdk.tick(nullptr, 0.01f);
    require(calls == 1, "a released context is not called on subsequent dispatches");
    first.check_errors();
}

void test_reset(MockSDK& sdk) {
    std::latch entered{1}, reset_requested{1}, release_render{1};
    std::atomic<bool> in_callback{};
    std::atomic<unsigned int> resets{};
    Context current{sdk};
    (*current.lua)["hold_render"] = [&] {
        in_callback = true;
        entered.count_down();
        release_render.wait();
        in_callback = false;
    };
    (*current.lua)["record_reset"] = [&] {
        require(!in_callback, "reset never runs concurrently in the same Lua state");
        ++resets;
    };
    current.script(R"(
        callbacks.on_pre_slate_draw_window_render_thread(function() hold_render() end)
        callbacks.on_script_reset(function() record_reset() end)
    )");
    const auto slate = sdk.slate;
    const auto context = current.context;
    std::thread render{[&] { slate(nullptr, nullptr); }};
    std::thread reset{[&] {
        entered.wait();
        reset_requested.count_down();
        context->script_reset();
    }};
    reset_requested.wait();
    const bool serialized = resets == 0;
    release_render.count_down();
    render.join();
    reset.join();
    require(serialized && resets == 1, "script reset retains the existing Lua-state serialization");
    current.check_errors();
}

void test_stress(MockSDK& sdk) {
    constexpr unsigned int iterations = 2000;
    Context current{sdk};
    current.script(R"(
        calls = 0
        callbacks.on_pre_engine_tick(function() calls = calls + 1 end)
        callbacks.on_pre_slate_draw_window_render_thread(function() calls = calls + 1 end)
    )");
    const auto tick = sdk.tick;
    const auto slate = sdk.slate;
    std::latch start{3};
    std::thread game{[&] {
        start.arrive_and_wait();
        for (unsigned int i = 0; i < iterations; ++i) { tick(nullptr, 0.01f); }
    }};
    std::thread render{[&] {
        start.arrive_and_wait();
        for (unsigned int i = 0; i < iterations; ++i) { slate(nullptr, nullptr); }
    }};
    std::thread owners{[&] {
        start.arrive_and_wait();
        for (unsigned int i = 0; i < 100; ++i) {
            auto lua = std::make_shared<sol::state>();
            auto context = uevr::ScriptContext::create(lua, &sdk.params);
        }
    }};
    game.join();
    render.join();
    owners.join();
    require(current.lua->get<unsigned int>("calls") == iterations * 2, "concurrent callbacks serialize without lost Lua updates");
    current.check_errors();
}

void test_owner_reset(MockSDK& sdk) {
    unsigned int calls{};
    bool retained{};
    Context first{sdk};
    auto owner = std::make_unique<uevr::ScriptState>(uevr::ScriptState::GarbageCollectionData{}, &sdk.params, false);
    const std::weak_ptr<uevr::ScriptContext> weak = owner->context();
    owner->lua()["record_call"] = [&] { ++calls; };
    auto result = owner->lua().safe_script(
        "uevr.sdk.callbacks.on_pre_engine_tick(function() record_call() end)", sol::script_pass_on_error);
    require(result.valid(), "built-in ScriptState registers its callback");
    result.abandon();
    (*first.lua)["drop_script_owner"] = [&] {
        if (owner) {
            owner.reset();
            retained = !weak.expired();
        }
    };
    first.script("callbacks.on_pre_engine_tick(function() drop_script_owner() end)");
    sdk.tick(nullptr, 0.01f);
    require(retained && calls == 0, "a snapshot retains storage but skips a destroyed ScriptState owner's callback");
    require(weak.expired(), "retired built-in context storage is released after dispatch");
    sdk.tick(nullptr, 0.01f);
    require(calls == 0, "retired scripts are not redispatched after reset");
    first.check_errors();
}

void test_queued_reset(MockSDK& sdk) {
    std::latch entered{1}, game_started{1};
    std::atomic<unsigned int> ticks{}, slates{};
    Context current{sdk};
    (*current.lua)["retire_during_callback"] = [&] {
        ++slates;
        entered.count_down();
        game_started.wait();
        current.context->retire_callbacks();
    };
    (*current.lua)["record_tick"] = [&] { ++ticks; };
    current.script(R"(
        callbacks.on_pre_engine_tick(function() record_tick() end)
        callbacks.on_pre_slate_draw_window_render_thread(function() retire_during_callback() end)
    )");
    const auto tick = sdk.tick;
    const auto slate = sdk.slate;
    std::thread render{[&] { slate(nullptr, nullptr); }};
    std::thread game{[&] {
        entered.wait();
        game_started.count_down();
        tick(nullptr, 0.01f);
    }};
    render.join();
    game.join();
    require(slates == 1 && ticks == 0, "queued callbacks recheck retirement under the existing Lua-state lock");
    current.check_errors();
}

void test_owner_retirement(MockSDK& sdk) {
    std::latch entered{1}, reset_started{1}, release{1};
    std::atomic<bool> destroyed{};
    auto owner = std::make_unique<uevr::ScriptState>(uevr::ScriptState::GarbageCollectionData{}, &sdk.params, false);
    const std::weak_ptr<uevr::ScriptContext> weak = owner->context();
    owner->lua()["hold_callback"] = [&] {
        entered.count_down();
        release.wait();
        require(!destroyed, "the script owner survives its running callback");
    };
    {
        auto result = owner->lua().safe_script(
            "uevr.sdk.callbacks.on_pre_slate_draw_window_render_thread(function() hold_callback() end)",
            sol::script_pass_on_error);
        require(result.valid(), "built-in ScriptState registers its render callback");
    }
    const auto slate = sdk.slate;
    std::thread render{[&] { slate(nullptr, nullptr); }};
    std::thread reset{[&] {
        entered.wait();
        reset_started.count_down();
        owner.reset();
        destroyed = true;
    }};
    reset_started.wait();
    const bool serialized = !destroyed;
    release.count_down();
    render.join();
    reset.join();
    require(serialized && destroyed && weak.expired(), "owner teardown waits for active callbacks and releases retired storage");
    slate(nullptr, nullptr);
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "pass a dispatch test case");
        MockSDK sdk;
        const std::string_view name{argv[1]};
        if (name == "reentry") { test_reentry(sdk); }
        else if (name == "lock-order") { test_lock_order(sdk); }
        else if (name == "membership") { test_membership(sdk); }
        else if (name == "lifetime") { test_lifetime(sdk); }
        else if (name == "reset") { test_reset(sdk); }
        else if (name == "owner-reset") { test_owner_reset(sdk); }
        else if (name == "queued-reset") { test_queued_reset(sdk); }
        else if (name == "owner-retirement") { test_owner_retirement(sdk); }
        else if (name == "stress") { test_stress(sdk); }
        else { throw std::runtime_error("unknown dispatch test case"); }
        std::cout << "Lua context dispatch passed: " << name << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
