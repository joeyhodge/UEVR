#include <algorithm>
#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>
#include <utility/Logging.hpp>

#include <sdk/Utility.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/UObjectBase.hpp>
#include <sdk/UObject.hpp>
#include <sdk/UClass.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/FMalloc.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/APlayerController.hpp>

#include "QuadView.hpp"
#include "QuadViewCulling.hpp"

namespace uevr::quadview {
namespace {
constexpr uint32_t SCAN_BUDGET = 20000;    // object array entries looked at per frame
constexpr uint32_t BOUNDS_BUDGET = 256;    // GetActorBounds calls per frame
constexpr size_t MAX_HIDDEN = 8192;        // cap on actors hidden per frame

// TArray's memory layout.
struct RawArray {
    void* data;
    int32_t count;
    int32_t capacity;
};

bool is_live(const void* object, int32_t index) try {
    const auto objects = sdk::FUObjectArray::get();
    if (objects == nullptr || object == nullptr || index < 0 || index >= objects->get_object_count()) {
        return false;
    }
    const auto item = objects->get_object(index);
    return item != nullptr && (const void*)item->get_object() == object;
} catch (...) {
    return false;
}

bool is_live(sdk::UObject* object) {
    return object != nullptr && is_live(object, ((sdk::UObjectBase*)object)->get_internal_index());
}
}

ActorCulling& ActorCulling::get() {
    static ActorCulling instance{};
    return instance;
}

bool ActorCulling::setup() {
    if (m_setup_done) {
        return !m_setup_failed;
    }

    m_setup_done = true;
    m_setup_failed = true;

    m_actor_class = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.Actor");
    const auto controller_class = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.PlayerController");
    if (m_actor_class == nullptr || controller_class == nullptr) {
        m_problem = "Actor or PlayerController class not found";
        spdlog::warn("[QuadView][Culling] {}", m_problem);
        return false;
    }

    const auto hidden = controller_class->find_property(L"HiddenActors");
    if (hidden == nullptr) {
        m_problem = "PlayerController.HiddenActors not found";
        spdlog::warn("[QuadView][Culling] {}", m_problem);
        return false;
    }
    m_hidden_actors_offset = hidden->get_offset();

    m_bounds_fn = m_actor_class->find_function(L"GetActorBounds");
    const auto origin = m_bounds_fn != nullptr ? m_bounds_fn->find_property(L"Origin") : nullptr;
    const auto extent = m_bounds_fn != nullptr ? m_bounds_fn->find_property(L"BoxExtent") : nullptr;
    if (m_bounds_fn == nullptr || origin == nullptr || extent == nullptr) {
        m_problem = "Actor.GetActorBounds not found";
        spdlog::warn("[QuadView][Culling] {}", m_problem);
        return false;
    }

    m_origin_offset = origin->get_offset();
    m_extent_offset = extent->get_offset();
    m_bounds_params_size = (std::max<size_t>)((size_t)m_bounds_fn->get_properties_size(), m_extent_offset + 24);
    // FVector is 3 doubles in UE5 and 3 floats before; the parameters sit back to back.
    m_bounds_double = m_extent_offset >= m_origin_offset + 24;

    if (m_bounds_params_size > 0x200) {
        m_problem = "GetActorBounds parameters look wrong";
        spdlog::warn("[QuadView][Culling] {} (size {})", m_problem, m_bounds_params_size);
        return false;
    }

    spdlog::info("[QuadView][Culling] Ready: HiddenActors at 0x{:x}, GetActorBounds params {} bytes (origin 0x{:x}, extent 0x{:x}, {})",
        m_hidden_actors_offset, m_bounds_params_size, m_origin_offset, m_extent_offset, m_bounds_double ? "double" : "float");

    m_problem.clear();
    m_setup_failed = false;
    return true;
}

void ActorCulling::remove_candidate(size_t i) {
    const auto index = m_candidates[i].index;
    if (index >= 0 && (size_t)index < m_position_by_index.size()) {
        m_position_by_index[index] = 0;
    }

    if (i + 1 != m_candidates.size()) {
        m_candidates[i] = m_candidates.back();
        const auto moved = m_candidates[i].index;
        if (moved >= 0 && (size_t)moved < m_position_by_index.size()) {
            m_position_by_index[moved] = (int32_t)i + 1;
        }
    }

    m_candidates.pop_back();
}

void ActorCulling::scan_objects(uint32_t budget) try {
    const auto objects = sdk::FUObjectArray::get();
    if (objects == nullptr) {
        return;
    }

    const auto count = objects->get_object_count();
    if (count <= 0) {
        return;
    }

    if (m_position_by_index.size() < (size_t)count) {
        m_position_by_index.resize((size_t)count, 0);
    }

    for (uint32_t n = 0; n < budget; ++n) {
        if (m_scan_cursor >= count) {
            m_scan_cursor = 0;
            break; // one pass per frame at most
        }

        const auto index = m_scan_cursor++;
        const auto item = objects->get_object(index);
        const auto object = item != nullptr ? item->get_object() : nullptr;
        auto& position = m_position_by_index[index];

        if (position != 0) {
            if ((void*)m_candidates[position - 1].actor == (void*)object) {
                continue;
            }
            remove_candidate((size_t)position - 1);
        }

        if (object == nullptr) {
            continue;
        }

        const auto object_class = object->get_class();
        if (object_class == nullptr || !object_class->is_a(m_actor_class)) {
            continue;
        }

        // Class default objects are never in the world.
        if (object->get_full_name().find(L"Default__") != std::wstring::npos) {
            continue;
        }

        Candidate c{};
        c.actor = (sdk::UObject*)object;
        c.index = index;
        m_candidates.push_back(c);
        position = (int32_t)m_candidates.size();
    }
} catch (...) {
    SPDLOG_WARN_ONCE("[QuadView][Culling] Object scan failed");
}

bool ActorCulling::read_bounds(Candidate& c) try {
    alignas(16) uint8_t params[0x200]{};
    c.actor->process_event(m_bounds_fn, params);

    double o[3]{}, e[3]{};
    if (m_bounds_double) {
        std::memcpy(o, params + m_origin_offset, sizeof(o));
        std::memcpy(e, params + m_extent_offset, sizeof(e));
    } else {
        float of[3]{}, ef[3]{};
        std::memcpy(of, params + m_origin_offset, sizeof(of));
        std::memcpy(ef, params + m_extent_offset, sizeof(ef));
        for (int i = 0; i < 3; ++i) { o[i] = of[i]; e[i] = ef[i]; }
    }

    const double r = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    if (!std::isfinite(r) || !std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2])) {
        c.has_bounds = false;
        return false;
    }

    std::memcpy(c.origin, o, sizeof(o));
    c.radius = r;
    c.has_bounds = true;
    return true;
} catch (...) {
    c.has_bounds = false;
    return false;
}

void ActorCulling::refresh_bounds(uint32_t budget) {
    for (uint32_t n = 0; n < budget && !m_candidates.empty(); ++n) {
        if (m_bounds_cursor >= m_candidates.size()) {
            m_bounds_cursor = 0;
        }

        auto& c = m_candidates[m_bounds_cursor];
        if (!is_live(c.actor, c.index)) {
            remove_candidate(m_bounds_cursor);
            continue;
        }

        read_bounds(c);
        ++m_bounds_cursor;
    }
}

void ActorCulling::pick_hidden() {
    m_to_hide.clear();
    m_last_hidden = 0;

    if (!m_enabled || m_min_pixels < 0.5 || m_px_per_tangent <= 0.0 || !m_have_view_location) {
        return;
    }

    for (const auto& c : m_candidates) {
        if (!c.has_bounds || c.radius <= 0.0) {
            continue; // actors with nothing to draw
        }

        const double dx = c.origin[0] - m_view_location[0];
        const double dy = c.origin[1] - m_view_location[1];
        const double dz = c.origin[2] - m_view_location[2];
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);

        if (wide_pixel_size(c.radius, d, m_px_per_tangent) < m_min_pixels) {
            m_to_hide.push_back(c.actor);
            if (m_to_hide.size() >= MAX_HIDDEN) {
                break;
            }
        }
    }
}

void ActorCulling::on_pre_engine_tick(bool enabled, double min_pixels, double px_per_tangent) {
    restore(); // never carry a swapped list into game code

    m_enabled = enabled && min_pixels >= 0.5;
    m_min_pixels = min_pixels;
    m_px_per_tangent = px_per_tangent;

    if (!m_enabled) {
        m_to_hide.clear();
        m_last_hidden = 0;
        return;
    }

    if (!setup()) {
        return;
    }

    try {
        const auto engine = sdk::UGameEngine::get();
        const auto world = engine != nullptr ? engine->get_world() : nullptr;
        const auto gameplay = sdk::UGameplayStatics::get();
        m_controller = (world != nullptr && gameplay != nullptr) ? (sdk::UObject*)gameplay->get_player_controller(world, 0) : nullptr;
    } catch (...) {
        m_controller = nullptr;
    }

    scan_objects(SCAN_BUDGET);
    refresh_bounds(BOUNDS_BUDGET);
    pick_hidden();
}

void ActorCulling::swap_in() {
    if (m_swapped || !m_enabled || m_to_hide.empty() || m_controller == nullptr || m_hidden_actors_offset == 0) {
        return;
    }

    if (!is_live(m_controller)) {
        m_controller = nullptr;
        return;
    }

    auto* const allocator = sdk::FMalloc::get();
    if (allocator == nullptr) {
        SPDLOG_WARN_ONCE("[QuadView][Culling] Engine allocator unavailable; not culling");
        return;
    }

    auto* const list = (RawArray*)((uintptr_t)m_controller + m_hidden_actors_offset);
    if (list->count < 0 || list->count > list->capacity || list->capacity > 1'000'000 ||
        (list->count > 0 && list->data == nullptr)) {
        SPDLOG_WARN_ONCE("[QuadView][Culling] HiddenActors looks wrong (count {}, capacity {}); not culling", list->count, list->capacity);
        return;
    }

    const size_t room = (size_t)list->count + m_to_hide.size();
    auto* const buffer = (sdk::UObject**)allocator->malloc(room * sizeof(void*), alignof(void*));
    if (buffer == nullptr) {
        return;
    }

    size_t n = 0;
    auto* const original = (sdk::UObject**)list->data;
    for (int32_t i = 0; i < list->count; ++i) {
        if (original[i] != nullptr) {
            buffer[n++] = original[i]; // the game's own hidden actors stay hidden
        }
    }

    const size_t own = n;
    for (auto* actor : m_to_hide) {
        // Actors can be destroyed (and collected) between the pre-tick and now.
        if (is_live(actor)) {
            buffer[n++] = actor;
        }
    }

    m_buffer = buffer;
    m_saved_data = list->data;
    m_saved_count = list->count;
    m_saved_capacity = list->capacity;
    m_swapped_controller = m_controller;

    list->data = buffer;
    list->count = (int32_t)n;
    list->capacity = (int32_t)room;
    m_swapped = true;
    m_last_hidden = n - own;

    SPDLOG_INFO_ONCE("[QuadView][Culling] First wide-view cull: hiding {} small actors", m_last_hidden);
}

void ActorCulling::restore() {
    if (!m_swapped) {
        return;
    }

    m_swapped = false;

    if (is_live(m_swapped_controller)) {
        auto* const list = (RawArray*)((uintptr_t)m_swapped_controller + m_hidden_actors_offset);
        if (list->data == m_buffer) {
            list->data = m_saved_data;
            list->count = m_saved_count;
            list->capacity = m_saved_capacity;
            if (auto* const allocator = sdk::FMalloc::get(); allocator != nullptr) {
                allocator->free(m_buffer);
            }
        } else {
            // The game changed the list while ours was in place; leave its version.
            SPDLOG_WARN_ONCE("[QuadView][Culling] HiddenActors was changed during the wide views; leaving it as the game left it");
        }
    }
    // A dead controller freed the buffer itself (it came from the engine allocator).

    m_buffer = nullptr;
    m_saved_data = nullptr;
    m_swapped_controller = nullptr;
}

void ActorCulling::on_view(int32_t slot, const double* view_location) {
    if (slot == 0 && view_location != nullptr &&
        std::isfinite(view_location[0]) && std::isfinite(view_location[1]) && std::isfinite(view_location[2])) {
        std::memcpy(m_view_location, view_location, sizeof(m_view_location));
        m_have_view_location = true;
    }

    if (slot == 0 || slot == 1) {
        swap_in();
    } else {
        restore();
    }
}

ActorCulling::Status ActorCulling::status() const {
    Status s{};
    s.ready = m_setup_done && !m_setup_failed;
    s.problem = m_problem;
    s.tracked = m_candidates.size();
    s.hidden = m_last_hidden;
    s.active = m_enabled && s.ready;
    return s;
}
}
