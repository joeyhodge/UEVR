#include <spdlog/spdlog.h>
#include <windows.h>

#include <algorithm>
#include <cwctype>

#include <utility/Scan.hpp>
#include <utility/String.hpp>

#include <sdk/FRenderTargetPool.hpp>
#include <sdk/EngineModule.hpp>
#include <sdk/threading/RHIThreadWorker.hpp>

#include "../VR.hpp"
#include "../../utility/Logging.hpp"
#include "RenderTargetPoolHook.hpp"

RenderTargetPoolHook* g_hook{nullptr};

RenderTargetPoolHook::RenderTargetPoolHook() {
    g_hook = this;
}

namespace {
inline void add_ref(IPooledRenderTarget* rt) {
    if (rt != nullptr) {
        rt->AddRef();
    }
}

inline void release_ref(IPooledRenderTarget* rt) {
    if (rt != nullptr) {
        rt->Release();
    }
}
} // namespace

void RenderTargetPoolHook::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    const auto want_depth = VR::get()->is_depth_enabled();
    const auto want_log = VR::get()->should_log_render_target_names();

    if (!m_attempted_hook) {
        m_wants_activate = true;
    }

    if (!m_attempted_hook && m_wants_activate) {
        m_attempted_hook = true;
        m_hooked = hook();
    }
}

bool RenderTargetPoolHook::hook() {
    SPDLOG_INFO("Attempting to hook RenderTargetPool::FindFreeElement");

    const auto is_ue5 = VR::get()->get_fake_stereo_hook()->has_double_precision();
    const auto find_free_element = sdk::FRenderTargetPool::get_find_free_element_fn(is_ue5);

    if (!find_free_element) {
        SPDLOG_ERROR("Failed to find FRenderTargetPool::FindFreeElement, cannot hook");
        return false;
    }

    /*if (VR::get()->get_fake_stereo_hook()->has_double_precision()) {
        spdlog::error("Render target pool hook is temporarily disabled on UE5, sorry :(");
        return false;
    }*/

    SPDLOG_INFO("Performing hook...");

    if (is_ue5) {
        m_find_free_element_hook = safetyhook::create_inline((void*)*find_free_element, find_free_element_hook_ue5);
    } else {
        m_find_free_element_hook = safetyhook::create_inline((void*)*find_free_element, find_free_element_hook);
    }

    if (m_find_free_element_hook) {
        SPDLOG_INFO("Successfully hooked RenderTargetPool::FindFreeElement");
    } else {
        SPDLOG_ERROR("Failed to hook RenderTargetPool::FindFreeElement");
    }

    return true;
}

void RenderTargetPoolHook::on_post_find_free_element(
    sdk::FRenderTargetPool* pool, 
    sdk::FPooledRenderTargetDesc* desc, 
    TRefCountPtr<IPooledRenderTarget>* out, 
    const wchar_t* name)
{
    const auto want_depth = VR::get()->is_depth_enabled();
    const auto want_log = VR::get()->should_log_render_target_names();
    const auto want_capture = g_hook->m_wants_activate;

    if (!want_depth && !want_log && !want_capture) {
        return;
    }

    if (name != nullptr) {
        //SPDLOG_INFO("FRenderTargetPool::FindFreeElement called with name {}", utility::narrow(name));

        std::scoped_lock _{g_hook->m_mutex};

        if (out != nullptr && out->reference != nullptr) {
            auto* new_rt = out->reference;
            if (auto it = g_hook->m_render_targets.find(name); it != g_hook->m_render_targets.end()) {
                if (it->second != new_rt) {
                    release_ref(it->second);
                    it->second = new_rt;
                    add_ref(new_rt);
                }
            } else {
                g_hook->m_render_targets.emplace(name, new_rt);
                add_ref(new_rt);
            }
            VR::get()->set_force_skip_rt_size_override(true);
        } else {
            if (auto it = g_hook->m_render_targets.find(name); it != g_hook->m_render_targets.end()) {
                release_ref(it->second);
                g_hook->m_render_targets.erase(it);
            }
        }

        if (!g_hook->m_seen_names.contains(name)) {
            g_hook->m_seen_names.insert(name);
            if (want_log) {
                std::wstring lower{name};
                std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return std::towlower(c); });

                const auto interesting =
                    lower.find(L"temporal") != std::wstring::npos ||
                    lower.find(L"history") != std::wstring::npos ||
                    lower.find(L"bend") != std::wstring::npos ||
                    lower.find(L"aa") != std::wstring::npos;

                if (interesting) {
                    SPDLOG_WARN("FRenderTargetPool::FindFreeElement name (match): {}", utility::narrow(name));
                } else {
                    SPDLOG_INFO("FRenderTargetPool::FindFreeElement name: {}", utility::narrow(name));
                }
            }
        }
    }
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> RenderTargetPoolHook::get_best_color_texture(uint32_t min_width, uint32_t min_height, std::wstring* out_name) {
    std::scoped_lock _{m_mutex};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> best{};
    D3D11_TEXTURE2D_DESC best_desc{};
    uint64_t best_score = 0;
    std::wstring best_name{};

    for (const auto& [name, rt] : m_render_targets) {
        if (rt == nullptr || IsBadReadPtr(rt, sizeof(void*))) {
            continue;
        }

        const auto& tex = rt->item.texture.texture;
        if (tex == nullptr || IsBadReadPtr(tex, sizeof(void*))) {
            continue;
        }

        ID3D11Texture2D* native = (ID3D11Texture2D*)tex->get_native_resource();
        if (native == nullptr) {
            continue;
        }

        D3D11_TEXTURE2D_DESC desc{};
        native->GetDesc(&desc);

        if (desc.Width < min_width || desc.Height < min_height) {
            continue;
        }

        if ((desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0) {
            continue;
        }

        if ((desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0) {
            continue;
        }

        if ((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
            continue;
        }

        if (desc.SampleDesc.Count > 1) {
            continue;
        }

        std::wstring lower{name};
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return std::towlower(c); });

        if (lower.find(L"depth") != std::wstring::npos) {
            continue;
        }

        uint64_t score = static_cast<uint64_t>(desc.Width) * static_cast<uint64_t>(desc.Height);

        if (lower.find(L"bendpostprocessupscalefull") != std::wstring::npos) score += 3000000000ULL;
        if (lower.find(L"postoutput") != std::wstring::npos) score += 2000000000ULL;
        if (lower.find(L"postprocessoutput") != std::wstring::npos) score += 1500000000ULL;
        if (lower.find(L"upscale") != std::wstring::npos) score += 800000000ULL;
        if (lower.find(L"postprocess") != std::wstring::npos) score += 400000000ULL;
        if (lower.find(L"scenecolor") != std::wstring::npos) score += 200000000ULL;
        if (lower.find(L"fog") != std::wstring::npos) score += 100000000ULL;
        if (lower.find(L"ui") != std::wstring::npos) score -= 500000000ULL;

        if (score > best_score) {
            best_score = score;
            best = native;
            best_name = name;
            best_desc = desc;
        }
    }

    if (out_name != nullptr) {
        *out_name = best_name;
    }
    if (best_name != m_last_best_name) {
        if (best) {
            SPDLOG_INFO("[RenderTargetPoolHook] Selected color target: {} ({}x{})",
                        utility::narrow(best_name),
                        best_desc.Width, best_desc.Height);
        } else {
            SPDLOG_INFO("[RenderTargetPoolHook] Selected color target: <none>");
        }
    }
    m_last_best_name = best_name;

    return best;
}

bool RenderTargetPoolHook::find_free_element_hook(
    sdk::FRenderTargetPool* pool, sdk::FRHICommandListBase* cmd_list,
    sdk::FPooledRenderTargetDesc* desc, TRefCountPtr<IPooledRenderTarget>* out,
    const wchar_t* name, 
    uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10)
{
    SPDLOG_INFO_ONCE("FRenderTargetPool::FindFreeElement (UE4) called for the first time!");

    const auto result = g_hook->m_find_free_element_hook.call<bool>(pool, cmd_list, desc, out, name, a6, a7, a8, a9, a10);

    SPDLOG_INFO_ONCE("Finished calling FRenderTargetPool::FindFreeElement!");

    g_hook->on_post_find_free_element(pool, desc, out, name);

    return result;
}

bool RenderTargetPoolHook::find_free_element_hook_ue5(
    sdk::FRenderTargetPool* pool,
    sdk::FPooledRenderTargetDesc* desc,
    TRefCountPtr<IPooledRenderTarget>* out,
    const wchar_t* name,
    // these arent uintptrs, just defending against future changes to the size of the params
    uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10)
{
    SPDLOG_INFO_ONCE("FRenderTargetPool::FindFreeElement (UE5) called for the first time!");

    const auto result = g_hook->m_find_free_element_hook.call<bool>(pool, desc, out, name, a6, a7, a8, a9, a10);

    SPDLOG_INFO_ONCE("Finished calling FRenderTargetPool::FindFreeElement! (UE5)");

    g_hook->on_post_find_free_element(pool, desc, out, name);

    return result;
}
