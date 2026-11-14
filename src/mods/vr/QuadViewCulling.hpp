#pragma once

// Quad View: per-view culling of small actors in the wide (peripheral) views.
//
// Unreal only culls ordinary static and skeletal meshes by distance, never by
// size on screen, and its cvars apply to every view alike. It does however
// rebuild a hidden-component set for each view from the player controller's
// HiddenActors list (APlayerController::BuildHiddenComponentList, called from
// ULocalPlayer::CalcSceneView after the stereo view offset). So, while the
// wide views are being set up, the controller's list is swapped for one that
// also holds every actor smaller than N pixels in the wide views; for the
// sharp views the game's own list is put back. The sharp area therefore keeps
// everything, and the periphery skips objects too small to see there.
//
// All of this runs on the game thread. The swapped list is allocated with the
// engine's allocator, so the engine can free it safely if the controller dies
// while it is in place.

#include <cstdint>
#include <string>
#include <vector>

namespace sdk {
class UObject;
class UClass;
class UFunction;
}

namespace uevr::quadview {
class ActorCulling {
public:
    static ActorCulling& get();

    // Game thread, once per frame before the world ticks: finds actors,
    // refreshes their bounds, resolves the player controller, and picks the
    // actors to hide for this frame. `controller` is the local player's
    // PlayerController (or null).
    void on_pre_engine_tick(bool enabled, double min_pixels, double px_per_tangent, sdk::UObject* controller);

    // Game thread, from CalculateStereoViewOffset. slot 0/1 = wide views
    // (swap the hidden list in), anything else = put the game's list back.
    void on_view(int32_t slot, const double* view_location);
    void restore();

    struct Status {
        bool ready{false};
        std::string problem{};
        size_t tracked{0};
        size_t hidden{0};
        bool active{false};
    };
    Status status() const;

private:
    struct Candidate {
        sdk::UObject* actor{nullptr};
        int32_t index{-1};
        double origin[3]{};
        double radius{0.0};
        bool has_bounds{false};
    };

    bool setup();
    void scan_objects(uint32_t budget);
    void refresh_bounds(uint32_t budget);
    bool read_bounds(Candidate& c);
    void remove_candidate(size_t i);
    void pick_hidden();
    void swap_in();

    bool m_setup_done{false};
    bool m_setup_failed{false};
    std::string m_problem{};

    sdk::UClass* m_actor_class{nullptr};
    sdk::UFunction* m_bounds_fn{nullptr};
    size_t m_bounds_params_size{0};
    size_t m_origin_offset{0};
    size_t m_extent_offset{0};
    bool m_bounds_double{true};
    size_t m_hidden_actors_offset{0};

    std::vector<Candidate> m_candidates{};
    std::vector<int32_t> m_position_by_index{}; // object index -> position + 1 in m_candidates
    int32_t m_scan_cursor{0};
    size_t m_bounds_cursor{0};

    sdk::UObject* m_controller{nullptr};
    double m_view_location[3]{};
    bool m_have_view_location{false};
    double m_min_pixels{0.0};
    double m_px_per_tangent{0.0};
    bool m_enabled{false};
    std::vector<sdk::UObject*> m_to_hide{};

    // The swap currently in place.
    bool m_swapped{false};
    sdk::UObject* m_swapped_controller{nullptr};
    void* m_buffer{nullptr};
    void* m_saved_data{nullptr};
    int32_t m_saved_count{0};
    int32_t m_saved_capacity{0};
    size_t m_last_hidden{0};
};
}
