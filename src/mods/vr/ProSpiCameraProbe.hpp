#pragma once
#include "ProSpiCameraTrace.hpp"
#include "ProSpiNativeCameraSource.hpp"
#include <sdk/ObjectIdentity.hpp>

namespace sdk { class UObjectBase; }

namespace uevr::prospi::trace {
// Game-thread only. No UObject is accessed by the recorder's writer/render threads.
class CameraProbe {
public:
    bool capture(sdk::UObjectBase* pcm, Camera& camera) noexcept;
    NativeSource capture_native(uintptr_t pcm) noexcept;
    void post_tick(Recorder& recorder, uint32_t thread) noexcept;
    void reset() noexcept;
private:
    bool resolve(sdk::UObjectBase* pcm);
    bool read(Pose& pose, bool& cut_known, bool& cut, uintptr_t& target) const;
    std::optional<sdk::object_liveness::Identity> m_identity;
    uintptr_t m_class{};
    int32_t m_cache{-1}, m_cache_bytes{}, m_location{-1}, m_rotation{-1}, m_fov{-1};
    int32_t m_aspect{-1}, m_timestamp{-1}, m_cut{-1}, m_target{-1};
    uint8_t m_cut_mask{};
    uint64_t m_last_post{};
    uint64_t m_last_post_session{}, m_last_post_camera{};
    bool m_previous_post_cut{};
    native::Layout m_native_layout{};
    bool m_native_attempted{}, m_native_supported{};
};
}
