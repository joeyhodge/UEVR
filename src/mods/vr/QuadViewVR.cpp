// Quad View: the parts of the VR mod that only exist for it.
//
//   - the console variables Quad View holds while it runs (and gives back),
//   - the per-tick small-actor culling driver,
//   - the per-frame layout / sharp-window snapshot accessors,
//   - the settings panel.
//
// The settings themselves are members of VR (VR.hpp, "Quad View" block); the
// engine hooks are in FFakeStereoRenderingHook.cpp; the portable math is in
// QuadView.hpp. See docs/quad-view.md and docs/quad-view-ue4-ue5.md.

#include <algorithm>
#include <cmath>
#include <optional>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <sdk/CVar.hpp>
#include <sdk/ConsoleManager.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <utility/String.hpp>

#include "Framework.hpp"
#include "../VR.hpp"
#include "QuadView.hpp"
#include "QuadViewCulling.hpp"

namespace {
// A float cvar that Quad View overrides while it runs, restoring the game's
// own value when Quad View (or the option) is turned off. The cvar is looked up
// lazily and again from time to time, since plugin modules register late.
struct QuadViewCVarOverride {
    const wchar_t* name;
    sdk::IConsoleVariable* variable{nullptr};
    uint32_t lookups{0};
    std::optional<float> saved{};
    std::optional<float> applied{};

    bool find() {
        if (variable != nullptr) {
            return true;
        }
        if (lookups++ % 300 != 0) {
            return false;
        }
        const auto console_manager = sdk::FConsoleManager::get();
        auto* object = console_manager != nullptr ? console_manager->find(name) : nullptr;
        if (object == nullptr || object->AsCommand() != nullptr) {
            return false;
        }
        variable = (sdk::IConsoleVariable*)object;
        return true;
    }

    bool set(float value) {
        wchar_t text[64]{};
        swprintf_s(text, L"%.9g", value);
        try {
            return variable->Set(text);
        } catch (...) {
            return false;
        }
    }

    // target: the value to hold, or nullopt to give the game its value back.
    void update(std::optional<float> target) {
        if (target) {
            if (!find()) {
                return;
            }
            const auto current = variable->TryGetFloat();
            if (!current) {
                return;
            }
            if (applied && same(*current, *target)) {
                return;
            }
            if (!saved) {
                saved = *current;
            }
            if (same(*current, *target)) {
                applied = *target; // the game already has this value: nothing to set
                spdlog::info("[QuadView] {} is already {}", utility::narrow(name), *target);
                return;
            }
            if (set(*target)) {
                applied = *target;
                spdlog::info("[QuadView] {} {} -> {}", utility::narrow(name), *current, *target);
            }
        } else if (saved && variable != nullptr) {
            // Give the value back only while it is still ours: if the game, a
            // plugin or the user changed it in the meantime, theirs stays.
            const auto current = variable->TryGetFloat();
            const bool still_ours = current && applied && same(*current, *applied);
            if (!still_ours) {
                spdlog::info("[QuadView] {} was changed by someone else; leaving it", utility::narrow(name));
            } else if (!same(*current, *saved)) {
                set(*saved);
                spdlog::info("[QuadView] {} restored to {}", utility::narrow(name), *saved);
            }
            saved.reset();
            applied.reset();
        }
    }

    static bool same(float a, float b) { return std::abs(a - b) <= 1e-9f + std::abs(b) * 1e-4f; }

    // Looked up and not there (after the first lookup attempt).
    bool missing() const { return variable == nullptr && lookups > 0; }

    // The cvar's current value, without overriding it (panel readouts).
    std::optional<float> read() {
        if (!find()) {
            return std::nullopt;
        }
        try {
            return variable->TryGetFloat();
        } catch (...) {
            return std::nullopt;
        }
    }

    const char* status() const {
        if (applied) return "active";
        if (variable == nullptr && lookups > 0) return "not found in this game";
        return "off";
    }
};

QuadViewCVarOverride g_quad_foliage_min_screen_size{L"foliage.MinimumScreenSize"};
// Read only: the game's resolution scale, which shrinks all four views.
QuadViewCVarOverride g_quad_screen_percentage{L"r.ScreenPercentage"};
QuadViewCVarOverride g_quad_emitter_spawn_rate{L"r.EmitterSpawnRateScale"};
// UE4.26 on D3D11 hit a fatal DXGI_ERROR_INVALID_CALL reading an occlusion
// query (D3D11Query.cpp) on its first four-view frame.
QuadViewCVarOverride g_quad_ue4_occlusion_queries{L"r.AllowOcclusionQueries"};
// ...and still died seconds later on another query read: the GPU timing
// queries (GPU stats) are the other query users in UE4's D3D11 renderer.
QuadViewCVarOverride g_quad_ue4_gpu_stats{L"r.GPUStatsEnabled"};
// Quad View: four views mean four view states, and Unreal adapts exposure per
// view state. The sharp window sees a small slice of the scene, so it settles
// on a different brightness than the wide view around it. With the share
// exposure option on, both adaptation cvars are frozen while Quad View runs so
// all four views use one exposure (0 = fixed, no histogram adaptation).
QuadViewCVarOverride g_quad_shared_exposure{L"r.DefaultFeature.AutoExposure"};
QuadViewCVarOverride g_quad_eye_adaptation_quality{L"r.EyeAdaptationQuality"};
}

// Called once per frame from VR::update_hmd_state: applies or restores every
// console variable Quad View overrides.
void VR::update_quad_view_cvars() {
    // Quad View: each sharp view is its own small Unreal view, so the tonemapper
    // draws a vignette around it (a dark ring around the sharp area, and a darker
    // image where you look). r.Tonemapper.Quality below 2 removes the vignette
    // (and film grain) for every view; the original value comes back when
    // Quad View or this option is turned off.
    {
        const bool want = FFakeStereoRenderingHook::is_quad_view_active() && m_quad_view_disable_vignette->value();
        if (want) {
            const auto quality = sdk::get_cvar_int(L"Renderer", L"r.Tonemapper.Quality");
            if (quality && *quality >= 2) {
                if (!m_quad_view_saved_tonemapper_quality) {
                    m_quad_view_saved_tonemapper_quality = *quality;
                }
                sdk::set_cvar_int(L"Renderer", L"r.Tonemapper.Quality", 1);
                spdlog::info("[QuadView] Vignette off: r.Tonemapper.Quality {} -> 1", *quality);
            }
        } else if (m_quad_view_saved_tonemapper_quality) {
            // Restore only while the value is still the 1 we set.
            const auto quality = sdk::get_cvar_int(L"Renderer", L"r.Tonemapper.Quality");
            if (quality && *quality == 1) {
                sdk::set_cvar_int(L"Renderer", L"r.Tonemapper.Quality", *m_quad_view_saved_tonemapper_quality);
                spdlog::info("[QuadView] Vignette restored: r.Tonemapper.Quality -> {}", *m_quad_view_saved_tonemapper_quality);
            } else {
                spdlog::info("[QuadView] r.Tonemapper.Quality was changed by someone else; leaving it");
            }
            m_quad_view_saved_tonemapper_quality.reset();
        }
    }

    // Quad View: cull small objects in the wide views. foliage.MinimumScreenSize
    // is a share of the screen measured with each view's own projection, so a
    // threshold of N wide-view pixels culls only what is tiny in the periphery;
    // the narrow sharp views see the same object much bigger and keep it.
    {
        const bool quad = FFakeStereoRenderingHook::is_quad_view_active();
        std::optional<float> min_screen{};
        if (quad && m_quad_view_wide_min_pixels->value() >= 0.5f) {
            const auto layout = get_quad_view_layout();
            const auto wide_px = (float)(std::max)(1u, (std::min)(layout.views[0].w, layout.views[0].h));
            min_screen = m_quad_view_wide_min_pixels->value() / wide_px;
        }
        g_quad_foliage_min_screen_size.update(min_screen);

        std::optional<float> spawn{};
        if (quad && m_quad_view_cpu_particles->value() < 0.999f) {
            spawn = m_quad_view_cpu_particles->value();
        }
        g_quad_emitter_spawn_rate.update(spawn);

        // UE4: occlusion queries go off first, and the four views only start
        // once they have been off for a while, so no query issued for the old
        // two-view setup is still in flight when the views (and their view
        // states) change. Turning both on at once crashed Last Hope on inject.
        const bool ue4_quad_wanted = is_using_quad_view() && m_quad_view_allow_ue4->value() &&
            m_fake_stereo_hook != nullptr && !m_fake_stereo_hook->has_double_precision();
        // Without occlusion queries Unreal draws everything inside each view's
        // frustum, hidden or not, which costs a lot in dense scenes. The
        // "keep occlusion culling" option leaves them on for games that run
        // four views without the query crash.
        const bool occlusion_off = ue4_quad_wanted && !m_quad_view_ue4_keep_occlusion->value();
        g_quad_ue4_occlusion_queries.update(occlusion_off ? std::optional<float>{0.0f} : std::nullopt);
        g_quad_ue4_gpu_stats.update(ue4_quad_wanted ? std::optional<float>{0.0f} : std::nullopt);

        // Share exposure across the four views (see the cvar comments above).
        const bool share_exposure = quad && m_quad_view_shared_exposure->value();
        g_quad_shared_exposure.update(share_exposure ? std::optional<float>{0.0f} : std::nullopt);
        g_quad_eye_adaptation_quality.update(share_exposure ? std::optional<float>{0.0f} : std::nullopt);
        if (!ue4_quad_wanted) {
            m_quad_view_ue4_settle_frames = 0;
        } else if (!occlusion_off || g_quad_ue4_occlusion_queries.applied || g_quad_ue4_occlusion_queries.missing()) {
            if (m_quad_view_ue4_settle_frames < QUAD_VIEW_UE4_SETTLE_FRAMES) {
                ++m_quad_view_ue4_settle_frames;
                if (m_quad_view_ue4_settle_frames == QUAD_VIEW_UE4_SETTLE_FRAMES) {
                    spdlog::info("[QuadView] UE4: starting the four views (occlusion queries {})",
                        occlusion_off ? "off" : "left on");
                }
            }
        }
    }
}

// Called once per engine tick from VR::on_pre_engine_tick: feeds the wide-view
// small-actor culling (QuadViewCulling.hpp). `runtime` supplies the field of
// view the wide views are rendered with.
void VR::update_quad_view_culling(sdk::UGameEngine* engine, const VRRuntime* letterbox_runtime) {
    {
        const bool quad = FFakeStereoRenderingHook::is_quad_view_active();
        double px_per_tangent = 0.0;
        if (quad && letterbox_runtime != nullptr) {
            const auto* t = letterbox_runtime->rendered_tangents[0];
            const double width = (double)t[1] - (double)t[0];
            if (width > 0.01 && std::isfinite(width)) {
                px_per_tangent = (double)get_quad_view_layout().views[0].w / width;
            }
        }
        const bool cull = quad && m_quad_view_cull_actors->value();
        sdk::UObject* controller{nullptr};
        sdk::UObject* cull_world{nullptr};
        if (cull) {
            const auto world = engine != nullptr ? engine->get_world() : nullptr;
            cull_world = (sdk::UObject*)world;
            const auto gameplay = sdk::UGameplayStatics::get();
            controller = (world != nullptr && gameplay != nullptr) ? (sdk::UObject*)gameplay->get_player_controller(world, 0) : nullptr;
        }
        uevr::quadview::ActorCulling::get().on_pre_engine_tick(cull, m_quad_view_wide_min_pixels->value(), px_per_tangent, controller,
            cull_world);
    }
}

void VR::draw_quad_view_ui() {
    ImGui::Indent();

    const ImVec4 warn{1.0f, 0.6f, 0.2f, 1.0f};
    const ImVec4 good{0.4f, 0.9f, 0.4f, 1.0f};

    if (m_extreme_compat_mode->value()) {
        ImGui::TextColored(warn, "Quad View is off: Extreme Compatibility Mode renders one view at a time.");
    }
    if (get_runtime() != nullptr && !get_runtime()->is_openxr()) {
        ImGui::TextColored(warn, "Quad View needs the OpenXR runtime. On OpenVR this game renders plain Native Stereo.");
    }
    if (is_sceneview_compatibility_enabled() || is_splitscreen_compatibility_enabled()) {
        ImGui::TextColored(warn, "Quad View is off while SceneView or Splitscreen compatibility is on.");
    }
    if (m_native_stereo_fix->value()) {
        // The fix only runs with the Native Stereo method, so it is inactive here.
        ImGui::TextColored(warn, "Native Stereo Fix is checked but only runs with Native Stereo. Games that need it\nmay not render correctly with Quad View.");
    }
    if (m_fake_stereo_hook != nullptr && !m_fake_stereo_hook->has_double_precision()) {
        m_quad_view_allow_ue4->draw("Allow on Unreal Engine 4 (experimental, may crash)");
        if (g_framework != nullptr && !g_framework->is_dx12()) {
            ImGui::TextColored(warn, "UE4 with D3D11 can crash with four views (D3D11 query error). If it does, use Native Stereo.");
        }
        if (m_quad_view_allow_ue4->value()) {
            m_quad_view_ue4_keep_occlusion->draw("UE4: keep occlusion culling (faster, may crash)");
            ImGui::TextDisabled(m_quad_view_ue4_keep_occlusion->value()
                ? "Occlusion queries stay on: hidden objects are skipped. Turn this off if the game crashes."
                : "Occlusion queries are off (crash workaround): Unreal draws hidden objects too, in all\nfour views. Try this option for more performance in dense scenes.");
        }
        if (!m_quad_view_allow_ue4->value()) {
            ImGui::TextColored(warn, "Unreal Engine 4 game: Quad View is off, this game renders plain stereo.");
        }
    }

    if (ImGui::TreeNodeEx("Sharp views (where you look)", ImGuiTreeNodeFlags_DefaultOpen)) {
        m_quad_view_focus_width->draw("Width with eye tracking (share of view)");
        m_quad_view_focus_height->draw("Height with eye tracking (share of view)");
        m_quad_view_fixed_width->draw("Width without eye tracking");
        m_quad_view_fixed_height->draw("Height without eye tracking");
        ImGui::TextDisabled(is_quad_view_gaze_available()
            ? "Using the eye-tracking size: the window follows your gaze."
            : "Using the size without eye tracking: the window stays put, so it is larger.");
        m_quad_view_focus_density->draw("Pixel density");
        m_quad_view_offset_x->draw("Offset left/right (+ = right)");
        m_quad_view_offset_y->draw("Offset down/up (+ = up)");
        m_quad_view_follow_gaze->draw("Follow gaze (eye tracking)");
        if (m_quad_view_follow_gaze->value()) {
            m_quad_view_gaze_smoothing_ms->draw("Gaze smoothing (ms, 0 = raw)");
            m_quad_view_gaze_dead_zone->draw("Gaze dead zone");

            if (!m_openxr->eye_gaze_supported) {
                ImGui::TextColored(warn, "Eye tracking: not started. Restart the game (or reinitialize the runtime) after turning gaze on.");
            } else if (m_openxr->is_quad_gaze_tracking()) {
                ImGui::TextColored(good, "Eye tracking: following your gaze");
            } else {
                ImGui::TextColored(warn, "Eye tracking: no gaze right now (eyes closed, or tracking not started)");
            }
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Wide views (periphery cost)", ImGuiTreeNodeFlags_DefaultOpen)) {
        m_quad_view_wide_scale->draw("Resolution (share of native)");
        m_quad_view_wide_lod_scale->draw("LOD scale (lower = simpler meshes)");
        m_quad_view_wide_min_pixels->draw("Cull objects smaller than (pixels, 0 = off)");
        ImGui::TextDisabled("Small object culling: %s. Covers foliage and instanced meshes; the sharp area keeps them.",
            g_quad_foliage_min_screen_size.status());
        if (m_quad_view_wide_min_pixels->value() >= 0.5f) {
            m_quad_view_cull_actors->draw("Also cull small actors: meshes, characters (experimental)");
            if (m_quad_view_cull_actors->value()) {
                const auto st = uevr::quadview::ActorCulling::get().status();
                if (!st.problem.empty()) {
                    ImGui::TextColored(warn, "Actor culling unavailable: %s", st.problem.c_str());
                } else {
                    ImGui::TextDisabled("Actor culling: %zu actors tracked, %zu hidden in the wide views", st.tracked, st.hidden);
                }
            }
        }
        m_quad_view_cpu_particles->draw("CPU particles (Cascade, whole image)");
        ImGui::TextDisabled("CPU particles: %s. Particles are simulated once for all views, so this also thins them where you look.",
            g_quad_emitter_spawn_rate.status());

        switch (FFakeStereoRenderingHook::s_quad_lod_status.load()) {
        case 0: ImGui::TextDisabled("LOD: learning this game's view layout..."); break;
        case 1: ImGui::TextColored(good, "LOD: active on the wide views"); break;
        case 2: ImGui::TextColored(warn, "LOD: not available in this game (view layout not recognised)"); break;
        case 4: ImGui::TextDisabled("LOD: off (LOD scale is 1.0)"); break;
        default: ImGui::TextDisabled("LOD: waiting for the first quad view frame"); break;
        }
        ImGui::TextDisabled("Nanite meshes ignore LOD; their detail already drops with the wide views' resolution.");
        ImGui::TreePop();
    }

    if (ImGui::TreeNodeEx("Blending", ImGuiTreeNodeFlags_DefaultOpen)) {
        m_quad_view_compose->draw("Compose into one image (enables edge blend)");
        if (m_quad_view_compose->value()) {
            m_quad_view_edge_blend->draw("Edge blend");
            m_quad_view_focus_shape->draw("Sharp area shape");
            m_quad_view_sharpen->draw("Sharpen the sharp views (0 = off)");
            m_quad_view_wide_bicubic->draw("Smooth wide-view upscaling (bicubic)");
            ImGui::TextDisabled("Bicubic upscaling hides the low resolution of the periphery better than plain\nfiltering, so the wide views can usually run at a lower resolution.");
            m_quad_view_show_outline->draw("Show sharp area outline");
            m_quad_view_debug_wide_only->draw("Debug: show wide views only (see the periphery quality)");
        }
        m_quad_view_debug_sharp_only->draw("Debug: submit the sharp layer only (wide layer off)");
        m_quad_view_disable_vignette->draw("Remove vignette (otherwise each sharp view gets its own dark edges)");
        m_quad_view_shared_exposure->draw("Share exposure across the four views (freezes auto-exposure)");
        if (m_quad_view_shared_exposure->value()) {
            m_quad_view_exposure_bias->draw("Brightness (composed image)");
        }
        m_quad_view_sharp_first->draw("Render sharp views first (experimental)");
        ImGui::TextDisabled("For games whose hair (Groom) only draws in the first two views. The left sharp view then drives\nexposure and shadows, which may look different. Takes effect on the next frame.");
        const bool composed = m_openxr->quad_composed.load();
        ImGui::TextDisabled(composed ? "Output: composed by UEVR (wide + sharp, blended)"
                                     : "Output: sharp views sent to the headset as an overlay layer (hard edge)");
        ImGui::TextDisabled("Each view adapts exposure to its own field of view; sharing freezes that so the sharp\nwindow and the wide periphery keep the same brightness.");
        ImGui::TreePop();
    }

    const auto runtime = get_runtime();
    if (runtime != nullptr && runtime->ready()) {
        const auto layout = get_quad_view_layout();
        ImGui::Text("Atlas %ux%u, %.0f%% of native stereo pixels", layout.atlas_w, layout.atlas_h,
            100.0 * uevr::quadview::pixel_share(layout, get_hmd_width(), get_hmd_height()));
        for (uint32_t i = 0; i < uevr::quadview::VIEW_COUNT; ++i) {
            const auto& r = layout.views[i];
            ImGui::Text("  %s %s: %ux%u at (%u, %u)", i % 2 == 0 ? "Left" : "Right", i < 2 ? "wide " : "sharp", r.w, r.h, r.x, r.y);
        }

        // The game's resolution scale shrinks all four views before Unreal
        // renders them; an upscaler (DLSS, FSR, TSR) can lower it further.
        if (const auto percentage = g_quad_screen_percentage.read(); percentage && *percentage > 0.0f) {
            const float k = *percentage / 100.0f;
            const auto wide_w = (uint32_t)(layout.views[0].w * k);
            const auto wide_h = (uint32_t)(layout.views[0].h * k);
            const auto sharp_w = (uint32_t)(layout.views[2].w * k);
            const auto sharp_h = (uint32_t)(layout.views[2].h * k);
            ImGui::Text("Rendered at r.ScreenPercentage %.0f%%: wide %ux%u, sharp %ux%u", *percentage, wide_w, wide_h, sharp_w, sharp_h);
            if (wide_w < 400) {
                ImGui::TextColored(warn, "The wide views render under 400 pixels across: the periphery will look blocky.\nRaise the wide Resolution or the game's resolution scale (and check DLSS/FSR modes).");
            }
        }
    }

    // Pixel share is not frame time: four views cost more CPU than two. Show
    // the real frame rate so settings can be compared with Native Stereo.
    const float fps = ImGui::GetIO().Framerate;
    if (fps > 0.0f) {
        ImGui::Text("Game frame rate: %.0f fps (%.1f ms)", fps, 1000.0f / fps);
        ImGui::TextDisabled("Compare with Native Stereo at the same spot. Console 'stat unit' splits the time into\ngame thread, render thread and GPU: a busy game or render thread means Quad View's\nextra views cost more CPU than the pixels they save.");
    }

    ImGui::Unindent();
}

bool VR::is_quad_view_gaze_available() const {
    return m_quad_view_follow_gaze->value() && has_quad_view_openxr() && m_openxr->eye_gaze_supported &&
        m_openxr->eye_gaze_space != XR_NULL_HANDLE;
}

bool VR::has_quad_view_openxr() const {
    const auto runtime = get_runtime();
    return m_openxr != nullptr && runtime != nullptr && runtime->is_openxr();
}

uevr::quadview::FrameSnapshot VR::get_quad_view_frame_live() const {
    // OpenXR places the window with each pose sample (gaze-following).
    if (has_quad_view_openxr()) {
        if (const auto current = m_openxr->get_quad_current()) {
            return *current;
        }
    }

    uevr::quadview::FrameSnapshot f{};
    f.layout = get_quad_view_layout();

    const auto runtime = get_runtime();
    const auto s = get_quad_view_settings();
    for (uint32_t eye = 0; eye < 2; ++eye) {
        // raw_projections holds the eye's real field of view as tangents: left, right, top, bottom.
        uevr::quadview::Tangents full{};
        if (runtime != nullptr) {
            const auto& p = runtime->raw_projections[eye];
            if (p[1] > p[0] && p[2] > p[3]) {
                full = {p[0], p[1], p[2], p[3]};
            }
        }
        f.focus[eye] = uevr::quadview::focus_window_for(full, s, 0.0, 0.0);
    }

    return f;
}

uevr::quadview::FrameSnapshot VR::get_quad_view_frame_game() const {
    // The frame whose pose the game thread is building views with. The render
    // thread may still be presenting an older frame, so this must not use the
    // render frame's snapshot.
    if (has_quad_view_openxr()) {
        if (const auto snapshot = m_openxr->get_game_quad_snapshot()) {
            return *snapshot;
        }
    }

    return get_quad_view_frame_live();
}

uevr::quadview::FrameSnapshot VR::get_quad_view_frame_render() const {
    // The frame being composed and presented: the same snapshot its views
    // were rendered with on the game thread.
    if (has_quad_view_openxr()) {
        if (const auto snapshot = m_openxr->get_render_quad_snapshot()) {
            return *snapshot;
        }
    }

    return get_quad_view_frame_live();
}
